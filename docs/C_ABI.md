# C ABI와 외부 언어 연동

`ServerCore::CAbi`는 C++ 객체·예외·STL 컨테이너를 외부 언어로 넘기지 않는 선택적 네이티브 경계입니다. Windows IOCP와 Linux epoll 위의 동일한 HTTP·WebSocket·raw TCP 구현을 사용합니다. Rust에서는 [안전한 래퍼](RUST.md)를 우선 사용합니다.

## 빌드와 소비

```sh
cmake -S . -B build/cabi -DSERVERCORE_BUILD_C_API=ON -DSERVERCORE_BUILD_TESTS=ON
cmake --build build/cabi --config Release
ctest --test-dir build/cabi -C Release --output-on-failure
cmake --install build/cabi --config Release --component CAbi --prefix install/cabi
```

기본은 공유 라이브러리입니다. 코어가 DLL/SO 안에 들어가므로 공유 C ABI 소비자는 C++ 헤더를 필요로 하지 않습니다. 서드파티 라이브러리 의존성은 없지만 해당 플랫폼의 C++·OS 런타임은 필요합니다. Windows에서는 빌드에 맞는 Microsoft C++ 런타임을 사용합니다.

별도 CMake 프로젝트는 `find_package(ServerCore CONFIG REQUIRED COMPONENTS CAbi)`와 `target_link_libraries(app PRIVATE ServerCore::CAbi)`를 사용합니다. `CMAKE_PREFIX_PATH`에 설치 prefix를 지정합니다. `SC_CABI_SHARED`는 CMake 타깃이 전달합니다. CMake 밖에서 Windows DLL을 소비할 때에는 직접 정의해야 합니다.

`SERVERCORE_C_API_SHARED=OFF`는 정적 아카이브를 만듭니다. 정적 소비자는 코어, C++ 런타임과 플랫폼 라이브러리도 연결합니다. 이는 코어가 포함된 공유 라이브러리를 사용하는 배포 계약과 다릅니다. CMake의 전체 설치 및 C++ 링커를 사용하거나 [Rust 정적 링크 설정](RUST.md)을 따릅니다. 서로 다른 아키텍처·C 런타임의 바이너리 혼용은 지원하지 않습니다.

## 버전·포인터·소유권

- 헤더는 `ServerCore/C/{Types,Web,Net,Observability}.h`입니다. `sc_abi_version()`과 `sc_capabilities()`로 ABI 버전 및 지원 기능을 확인합니다. 유지되는 API는 ABI 1의 기존 필드 순서·크기·상태 값과 capability 값을 보존합니다. 업로드·정책·종료 확장은 `SC_CAP_WEB_EXTENSIONS`, 로그·메트릭은 `SC_CAP_OBSERVABILITY`이며 별도 구조체와 함수로 추가했습니다.
- 옵션·응답 헤더는 `*_init(&value, sizeof(value))`로 초기화합니다. 조회용 view에는 호출 전에 `abi_version=SC_ABI_VERSION`, `struct_size=sizeof(view)`를 넣습니다. 잘못된 버전이나 작은 구조체는 `SC_INVALID_ARGUMENT`입니다.
- 입력 포인터는 호출 중 유효해야 합니다. 비동기 사용에 필요한 문자열·본문은 네이티브 구현이 복사합니다. 본문은 바이너리이며 NUL 종료 문자열이 아닙니다. 빈 슬라이스에만 NULL을 사용할 수 있습니다. HTTP 수신 헤더 값은 UTF-8이 아닐 수도 있으므로 바이트로 읽습니다.
- 반환한 handle은 대응하는 `*_destroy`로 정확히 한 번 해제합니다. `free`나 외부 언어의 할당 해제 함수를 사용하지 않습니다. view는 해당 이벤트가 살아 있는 동안만 빌립니다. 이벤트는 서버·연결 종료 뒤에도 읽을 수 있습니다.
- 한 handle의 메서드는 동시 호출할 수 있지만 해제와 같은 handle의 사용은 겹치면 안 됩니다. `*_retain`은 독립적으로 해제할 수 있는 handle을 만듭니다. 마지막 소유 handle의 해제 규칙이 실제 취소·연결 종료 시점을 정합니다.
- 오류는 정수 `sc_status`로 반환하며 C++ 예외는 경계를 넘지 않습니다. 성공은 로컬 수락을 뜻하며 상대의 수신 확인이 아닙니다.

## 이벤트와 종료

웹 서버는 라우트를 등록한 뒤 시작하고 `sc_web_server_next`로 HTTP 요청 또는 열린 WebSocket을 받습니다. HTTP 이벤트에서 응답 handle을 얻고 작은 응답은 `complete`, 스트리밍은 `start` → `write` → `finish`를 호출합니다. 알 수 없는 본문 길이는 chunked 응답으로 전송합니다. SSE는 적절한 콘텐츠 형식과 SSE 형식의 바이트를 같은 스트리밍 API로 보냅니다. 파일도 호출자가 제한된 크기로 읽어 전송하며 파일 전체를 복사할 필요가 없습니다.

청크 크기와 송신 예산을 지키고 `SC_WOULD_BLOCK`이면 capacity wait를 등록합니다. 알림은 예약이 아니므로 다시 송신할 때 경합으로 `WouldBlock`이 나올 수 있습니다. 한 연결의 대기 등록은 하나이며 여러 생산자는 애플리케이션에서 직렬화합니다. `sc_wait_destroy`는 네이티브 알림을 정리하고 기다린 뒤 반환합니다. 대기 취소는 연결 자체를 종료하지 않습니다.

응답 handle을 얻지 않고 HTTP 이벤트를 버리거나 마지막 미완료 응답 handle을 해제하면 응답을 중단합니다. 응답 handle을 얻은 뒤에는 요청 이벤트를 먼저 해제해도 됩니다. 마지막 WebSocket/TCP handle을 해제하면 연결을 닫습니다. 서버의 `stop/destroy`는 대기를 깨우고 네이티브 작업자를 합류합니다. 남아 있는 이벤트는 계속 읽을 수 있고 남아 있는 응답·연결의 송신은 종료 상태를 반환합니다.

모든 `next/accept/wait`의 제한시간은 밀리초입니다. 0은 비차단(`WouldBlock`), `UINT32_MAX`는 종료될 때까지 대기입니다. 유한 대기의 `Timeout`은 작업을 취소하지 않습니다. 포인터를 파괴하려면 먼저 그 포인터로 대기 중인 호출을 종료·합류해야 합니다.

Rust/C 콜백을 C++ I/O 스레드에서 호출하지 않습니다. 이벤트 큐·사용자가 이미 꺼내 보관한 이벤트를 함께 계산합니다. HTTP/WS 열림 이벤트의 `max_event_count/bytes`, 모든 WebSocket 메시지의 `max_ws_event_count/bytes`, TCP 메시지의 `max_event_count/bytes`는 **서버 전체 상한**입니다. 네이티브 요청·파서·송신 예산도 별도로 적용됩니다. 컨테이너·할당기의 부가 저장소는 이벤트 개수와 연결 개수로 제한되며 바이트 상한의 정확한 RSS 보장은 아닙니다.

HTTP 이벤트 예산 초과는 503, WS 메시지 예산 초과는 종료 요청, TCP 메시지 예산 초과는 `TooLarge` 종료입니다. TCP·WS 종료 이벤트는 연결 수락 시 별도 공간을 확보하여 큐 포화 뒤에도 전달합니다. 닫힌 연결 handle이나 종료 이벤트를 보관하면 해당 연결의 예약도 유지됩니다. WS 종료 이벤트에는 코드만 제공하며 상대의 종료 이유 문자열은 제공하지 않습니다.

## HTTP 클라이언트 제거에 따른 이전

외부 HTTP 요청을 소비 애플리케이션의 책임으로 옮기면서 `ServerCore/C/HttpClient.h`와 HTTP 클라이언트·요청·이벤트 API를 제거했습니다. 제거한 API를 사용하는 소비자는 애플리케이션이 선택한 HTTP 구현으로 이전하고 다시 빌드해야 합니다. 기존 클라이언트 capability 값 `4`는 예약하며 재사용하지 않고, `sc_capabilities()`는 이 비트를 반환하지 않습니다. 나머지 C ABI의 수치와 레이아웃은 유지하므로 서버 API의 ABI 버전은 계속 1입니다. 이전 클라이언트 심벌에 의존하는 바이너리와의 호환성을 보장하는 것은 아닙니다. 새 설치 prefix를 사용해 예전 헤더·타깃 파일이 남지 않게 하며, 상세 이전 범위는 [API_MIGRATION.md](API_MIGRATION.md)를 따릅니다.

## 요청 스트리밍과 인증 정책

`sc_web_server_stream_route`는 전체 업로드를 기다리지 않고 요청 헤더 이벤트를 전달합니다. 이벤트의 `request.body`는 비어 있습니다. 이벤트를 해제하기 전에 `sc_web_event_body`와 `sc_web_event_response`를 각각 얻습니다. `sc_http_body_read`는 소유 청크, 깨끗한 EOF(`SC_OK`와 NULL), 대기(`SC_WOULD_BLOCK`), 오류를 구별합니다. 청크 바이트는 `sc_body_chunk_destroy`까지 유효하며, 꺼낸 청크도 수신 용량을 차지합니다. 청크를 보관하면 추가 업로드 수신이 제한됩니다. 청크는 서버·reader 종료 뒤에도 읽을 수 있습니다.

`sc_web_server_set_body_limits`는 시작 전에 reader 버퍼와 전체 스트리밍 본문 상한을 설정합니다. 전체 상한은 기존 `sc_web_options.max_body_bytes` 이상이어야 합니다. `max_chunk_bytes`는 생산하는 요청 청크에도 적용됩니다. 고정 길이와 chunked 업로드 모두 같은 reader를 사용합니다. 깨끗한 EOF 전에 마지막 reader handle을 해제하거나 reader를 얻지 않은 이벤트를 버리면 업로드와 응답을 중단합니다. 파서가 EOF까지 완료한 뒤의 reader 해제는 안전합니다.

`sc_web_server_enable_policy`는 선택된 라우트에 적용하는 전역 정책을, `sc_web_server_websocket_policy`는 해당 WebSocket 라우트의 별도 정책을 등록합니다. 스트리밍 라우트는 헤더 단계에서 정책을 실행하며, 일반 버퍼링 라우트는 제한된 본문을 받은 뒤 handler 호출 전에 실행합니다. 프로토콜 오류·없는 경로·지원하지 않는 메서드는 정책 전에 거절합니다. `SC_WEB_POLICY`에서 요청·헤더·라우트 파라미터를 읽고 `sc_web_event_decision`을 얻습니다. `allow`는 복사한 응답 헤더와 애플리케이션 속성을 전달하며, `reject`는 HTTP 응답으로 거절합니다. WebSocket은 이 결정 전에는 `101`을 보내지 않습니다. 전역 정책과 WebSocket 정책을 함께 등록했다면 두 이벤트를 순서대로 허용해야 합니다. 마지막 미결정 handle이나 미수령 정책 이벤트를 해제하면 중단합니다. 대기 제한시간은 기존 `handler_timeout_ms`이고 disconnect·timeout은 `sc_request_decision_cancelled`로 확인합니다.

정책 속성은 대소문자를 구분하는 애플리케이션 키이며 HTTP 필드 정규화 대상이 아닙니다. 허용된 HTTP 요청의 `sc_web_event_attributes`로 조회합니다. 열린 WebSocket 이벤트는 원래 요청만 제공하고 정책 속성은 제공하지 않습니다. 정책 응답 헤더에는 네이티브 프레이밍·핸드셰이크 필드를 덮어쓸 수 없는 제한이 적용됩니다. 인증 토큰, Origin, 애플리케이션 권한 검사는 호출자가 구현합니다.

## 정상 종료와 관측

`sc_web_server_begin_drain`은 신규 수락을 중지하고 이미 수락한 HTTP 작업을 마치게 합니다. WebSocket은 1001 종료를 시작합니다. `sc_web_server_drain_status`는 비차단 상태 조회입니다. `sc_web_server_stop_gracefully`는 지정한 시간까지 기다리고 네이티브 작업자를 합류하며, 만료하면 남은 작업을 강제 중단하고 `SC_TIMEOUT`을 반환합니다. 일반 `stop/destroy`는 즉시 중단 방식입니다. graceful 대기 중에도 이미 받은 응답 handle로 처리할 수 있습니다.

`sc_web_server_get_metrics`는 고정 필드의 요청·연결·송신·handler 통계와 12개 지연 분포 구간을 반환합니다. 조회 순간은 필드 전체에 대해 원자적이지 않습니다. 구간 값은 누적이 아니며 나노초 상한은 헤더에 정의되어 있습니다. `sc_web_server_metrics_prometheus`는 고정 이름과 라벨, 초 단위 누적 histogram으로 내보내는 소유 문자열을 반환합니다. `sc_owned_text_destroy`까지 서버와 독립적으로 유효합니다. 토큰·URL·사용자별 라벨을 자동 생성하지 않습니다.

`sc_logger_create`는 크기가 제한된 비동기 로그 인스턴스를 시작합니다. UTF-8 파일 경로, 콘솔 사용, 로그 수준, 큐·바이트·메시지 크기 및 회전 파일 수를 설정합니다. `try_write`는 수준 필터를 적용하고 수락한 메시지 바이트를 복사하며 용량·크기 오류를 반환합니다. `sc_web_server_set_logger`는 시작 전에 해당 서버에 인스턴스를 연결하며 전역 logger를 바꾸지 않습니다. 서버가 logger를 별도로 소유하므로 호출자의 logger handle을 먼저 해제해도 됩니다. `sc_logger_stop`과 마지막 네이티브 소유자 해제는 출력을 비우고 worker를 합류하므로 파일·콘솔 I/O 지연의 엄격한 시간 상한은 없습니다. async 실행기의 worker에서 호출하지 않습니다.

## 현재 경계

이 ABI는 HTTP 서버·요청/응답 스트리밍·정책·WebSocket·정상 종료·서버 메트릭·로그 인스턴스·raw TCP 수락/송수신/흐름 제어를 제공합니다. 외부 HTTP 요청은 소비 애플리케이션이 처리합니다. C++ 게임 `ServerHost`, JSON 디스패처, UDP, 독립 `TaskExecutor`와 추적 callback API의 외부 언어 래퍼는 아직 제공하지 않습니다. HTTP handler executor 통계는 서버 snapshot에 포함됩니다. Rust 게임 서버는 raw TCP 바이트 위에 프레이밍과 게임 로직을 둘 수 있습니다. C++와 다른 전송 구현을 별도로 유지하지 않습니다.

일반 WebSocket 열림 이벤트는 업그레이드 후 전달됩니다. 업그레이드 전 인증에는 위 정책 등록 API를 사용합니다. 서버 내장 TLS, 범용 outbound TCP, HTTP/2·HTTP/3은 기존 지원 범위와 같습니다.
