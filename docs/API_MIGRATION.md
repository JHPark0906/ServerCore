# 공개 API 이전 안내

동작을 명확히 하는 이름과 인스턴스별 API를 정식 경계로 제공합니다. 아래 기존 API는 deprecation 경고를 냅니다. 이름 변경용 wrapper는 같은 구현으로 전달하며, 전역 로거는 별도 호환 API로만 남습니다. 삭제 시점은 아직 정하지 않았습니다.

| Deprecated API | 정식 API | 이유 |
| --- | --- | --- |
| `JobRunner::Stop()` | `JobRunner::RequestStop()` | 종료를 요청할 뿐 작업이나 외부 실행 스레드가 끝날 때까지 기다리지 않음 |
| `Core::SetGlobalLogger/GetGlobalLogger` | 서버별 `SetLogger`, 소유 `ILogger` | 여러 서버가 전역 로거를 덮어쓰는 의존성을 제거함. 전역 API는 호환용으로만 남으며 서버에 적용되지 않음 |
| `DatagramTransport::Send(id, bytes)` | `DatagramTransport::SendSerialized(id, bytes)` | 이미 직렬화한 JSON 봉투를 받으며 직렬화·JSON 검증을 수행하지 않음 |
| `HttpServer::Route(method, path, handler)` | `HttpServer::RegisterRoute(method, path, handler)` | 시작 전 라우트 등록이라는 동작을 명시 |
| `HttpServer::WebSocket(path, callbacks)` | `HttpServer::RegisterWebSocket(path, callbacks)` | 시작 전 WebSocket endpoint 등록이라는 동작을 명시 |

새 이름과 기존 이름은 같은 객체의 등록표·송신 순번·오류 처리·종료 상태를 공유합니다. 서로 독립적인 구현이나 별도 설정 모드는 아닙니다. 예를 들어 기존 이름으로 등록한 라우트를 새 이름으로 다시 등록하면 동일한 중복 등록 오류가 발생합니다.

## 종료 요청과 완료 대기

`Post`는 이제 작업 수·선언 바이트 상한을 적용하므로 기존 호출도 `WouldBlock`을 처리해야 합니다. 일반 작업 포화가 내부 세션 종료를 막지 않도록 Host는 별도 제어 예산을 사용합니다. 비동기 결과는 `SubmitSessionTask` 또는 `Lease::Reserve`로 먼저 완료 자리를 확보합니다. 무제한 큐 호환 모드는 제공하지 않습니다.

서버의 `BeginDrain/DrainStatus/StopGracefully`를 정상 종료 경로로 사용합니다. 기존 `Stop`의 즉시 취소 계약을 바꾸지 않습니다. 새로운 Web 정책 결정은 101 전 인증을 수행하며 `onOpen`에서 인증하던 애플리케이션은 `authorize` 또는 C/Rust 정책 이벤트로 옮깁니다. 요청 body 스트리밍은 명시적으로 스트리밍 라우트를 등록하고, 기존 라우트는 같은 파서 위의 buffered 경로를 사용합니다.

`JobRunner`는 스레드를 소유하지 않습니다. `RequestStop()`은 새 작업을 거절하고 이미 수락한 작업을 `RunUntilStopped()`가 끝까지 실행하도록 요청합니다. 작업 안에서도 호출할 수 있고 반복 호출도 안전합니다. 호출자는 실행 스레드를 join한 후 `JobRunner`를 파괴해야 합니다.

```cpp
ServerCore::Runtime::JobRunner runner;
std::thread worker([&runner] { runner.RunUntilStopped(); });
// runner.Post(...)로 작업을 넣고 반환 Status를 처리합니다.
runner.RequestStop();
worker.join();
```

스레드를 직접 소유하는 `IoContext`, `PeriodicRunner`, `ServerHost`와 종료 완료를 기다리는 `Acceptor`, `HttpServer`의 `Stop()`은 유지합니다. 이들의 종료 계약은 `JobRunner::RequestStop()`과 다릅니다.

## UDP 직렬화 경계

`SendSerialized(id, prepared.Bytes())`에 주는 바이트는 JSON 봉투입니다. TCP 길이 접두사나 UDP 머리를 직접 붙이지 않습니다. `Protocol::SerializeMessage()` 또는 `PreparedMessage`를 사용해 만들고 UDP payload 상한을 지킵니다. 이 호출의 성공은 OS의 datagram 수락이며, 비동기 TCP의 로컬 송신 큐 수락이나 상대의 수신 확인과 같지 않습니다.

`SendSerialized`는 기존 `Send`와 마찬가지로 JSON을 다시 파싱하지 않습니다. 등록되지 않았거나 준비되지 않은 경로, 빈/초과 payload, OS 오류의 판정 순서와 송신 성공 시에만 증가하는 순번 계약도 유지합니다.

## HTTP 패턴 라우팅과 메서드 응답

`HttpServer::RegisterRoutePattern(method, pattern, handler)`와 `RegisterWebSocketPattern(pattern, callbacks)`를 추가했습니다. `RegisterRoute`와 `RegisterWebSocket`은 원래 경로의 정확 일치 API로 유지하며 deprecated 처리하지 않습니다. `{id}`를 캡처하려는 라우트만 명시적으로 패턴 API를 사용합니다. 등록은 모두 `Start()` 전으로 제한됩니다.

캡처는 새 `HttpRequest::pathParameters`가 소유하며 `PathParameter(name)`으로 조회합니다. 패턴 경로는 세그먼트별로 한 번 percent-decode하고 검증하지만 정확 일치 경로의 표기는 바꾸지 않습니다. 우선순위, 중복 구조의 `AlreadyExists`, 디코딩과 매개변수 수명은 [웹 API 계약](WEB.md#패턴과-경로-매개변수)을 참고합니다.

`WebSocketCallbacks` 끝에 선택적 `onOpenWithRequest(connection, request)`를 추가했습니다. 연결이 열린 뒤 경로 매개변수와 요청 헤더가 필요할 때 사용합니다. 기존 `onOpen(connection)`은 그대로 유효하며 deprecated가 아닙니다. 둘 중 하나만 지정하며, 둘을 함께 지정한 정확 일치·패턴 WebSocket 등록은 `InvalidArgument`입니다. 요청과 내부 view는 새 콜백 실행 동안만 유효하므로 이후 사용할 값은 복사합니다. 기존 네 콜백의 필드 순서는 유지하고 새 필드는 빈 콜백으로 기본 초기화합니다. `HttpRequest`와 `WebSocketCallbacks`에 저장 필드가 추가되므로 정적 라이브러리와 소비 프로그램을 함께 다시 빌드해야 합니다.

기존에 다른 메서드로 등록된 HTTP 경로를 요청하면 `404`였던 동작을 `405 Method Not Allowed`와 `Allow` 헤더로 교정했습니다. `GET`이 등록되어 있으면 `Allow`에는 자동 HEAD 지원도 반영합니다. 명시적 `HEAD` 처리기가 있으면 `GET` fallback보다 우선합니다. 가장 구체적인 경로를 먼저 고르므로 그 경로에 요청 메서드가 없다고 덜 구체적인 패턴으로 내려가지는 않습니다. 경로 자체가 없으면 계속 `404`입니다.

HTTP와 WebSocket을 같은 경로에 등록한 경우 일반 HTTP 요청은 HTTP 처리기로, upgrade 요청은 WebSocket 처리기로 보냅니다. WebSocket 경로만 있고 upgrade를 하지 않은 요청은 기존 `400` 동작을 유지합니다.

## 비동기 HTTP·스트리밍 이전

모든 HTTP 처리기를 I/O 스레드에서 제한된 worker 풀로 옮겼습니다. `RegisterRoute`·`RegisterRoutePattern`은 작은 완성 응답용으로 유효하며 deprecated가 아닙니다. 내부에서 동일한 요청 문맥·응답 작성기에 위임하므로 이전 실행 경로를 별도로 유지하지 않습니다. I/O thread-local이나 특정 스레드 호출을 가정한 코드는 수정해야 합니다.

외부 응답이나 작업을 기다리는 라우트는 `RegisterAsyncRoute`·`RegisterAsyncRoutePattern`으로 바꾸고 전달된 소유 문맥을 보관합니다. 응답 완료에는 `Complete`, 스트리밍에는 `Start` → `Write` → `Finish`를 사용합니다. 연결 종료 취소 토큰을 하위 작업과 연결하고 `WouldBlock` 처리 시 등록 handle의 수명을 유지합니다. HTTP worker와 요청·pipeline 예산, 처리·송신 정체 기한도 새 `HttpServerOptions` 필드로 설정합니다.

`maxResponseBodyBytes`는 기존 완성 응답에만 적용합니다. 큰 파일과 SSE는 제한된 조각을 쓰며 송신 큐 상한을 늘려 전체 본문을 넣지 않습니다. `GetWebSocketFlowControl`은 기존 WebSocket 가상 함수 변경 없이 공통 송신 용량 알림을 노출합니다. 공개 구조체에 필드가 추가되어 소비자 재빌드가 필요합니다.

Rust/C 연동은 별도 선택 타깃 `ServerCore::CAbi`와 Cargo 래퍼로 제공합니다. 상세 수명과 이전 예제는 [WEB.md](WEB.md), [C_ABI.md](C_ABI.md)를 따릅니다.

## HTTP 클라이언트 제거

ServerCore의 범위를 웹·게임 서버의 수신·응답과 실행 기반으로 정리하면서 외부 HTTP 클라이언트와 libcurl 의존성을 제거했습니다. 백엔드가 외부 서비스에 HTTP 요청을 보낼 필요가 없다는 뜻은 아닙니다. 해당 전송 구현과 의존성은 소비 애플리케이션이 선택하며, ServerCore의 비동기 응답 문맥·취소 기능과 연결할 수 있습니다.

- C++의 `ServerCore/Web/HttpClient.h`, `Web::HttpClient`와 관련 요청·응답 타입, CMake `ServerCore::HttpClient` 타깃 및 `HttpClient` 패키지 컴포넌트를 제거했습니다. `SERVERCORE_BUILD_HTTP_CLIENT` 설정과 관련 링크를 소비 프로젝트에서 삭제합니다. 이전 설정을 `ON`으로 전달하면 지원 기능으로 무시하지 않고 명확한 구성 오류로 거절합니다.
- C의 `ServerCore/C/HttpClient.h`와 HTTP 클라이언트·요청·이벤트 API를 제거했습니다. 나머지 상태 값·구조체·capability 값은 유지하며, 기존 클라이언트 capability 값 `4`는 예약하고 재사용하지 않습니다. 유지되는 서버 API의 C ABI 버전은 1입니다.
- Rust의 `servercore::client`, `servercore-sys::client`, `http-client` feature와 `http_client` 예제를 제거했습니다. Cargo feature 요청과 해당 import를 삭제하고 외부 요청을 애플리케이션의 구현으로 이전합니다.

제거한 클라이언트 API에는 deprecated 어댑터를 남기지 않습니다. 이를 사용하는 소스·바이너리는 이전과 재빌드가 필요합니다. 기존 클라이언트 포함 C ABI 바이너리는 외부 의존성을 유지하므로 새 서버 전용 바이너리로 교체합니다. CMake 설치는 이전 파일을 자동으로 지우지 않으므로 새 설치 prefix를 사용해 삭제된 헤더·타깃 파일이 남지 않게 합니다. HTTP 서버·WebSocket·TCP 수락·요청/응답 스트리밍의 API는 유지합니다.

## Rust·C ABI와 운영 API 추가

C ABI 1은 새 외부 언어 계약이며 C++ 객체를 직접 노출하지 않습니다. 기존 C++ API를 deprecate하지 않습니다. `Observability::AsyncLogger`는 기존 `ILogger`의 구현이고 `TaskExecutor::GetMetrics`, `HttpServer::GetMetrics`, `SetRequestTraceHandler`는 선택적으로 사용합니다. 로그·추적 큐가 가득 차면 작업 스레드를 기다리게 하지 않고 손실을 집계합니다. C++ 소비자는 같은 툴체인·CRT로 재빌드하며 C/Rust 사용자는 ABI 버전과 모듈 capability를 확인합니다.

## WebSocket 오류 코드 교정

호출자가 `SendText()` 또는 `Close()`에 넘긴 UTF-8이 잘못되면 `InvalidArgument`를 반환하도록 교정했습니다. 앞선 구현의 `InvalidFormat`에 분기하던 코드는 `InvalidArgument`로 옮깁니다. 이는 `JsonValue::Dump()`와 `SerializeMessage()`의 잘못된 송신 인자 규칙과 같습니다. 종료된 연결은 `Closed`, 크기 상한 초과는 `TooLarge`를 먼저 반환하며, 상대가 보내온 잘못된 UTF-8은 계속 WebSocket 종료 코드 1007로 처리합니다. 기존 가상 함수의 시그니처나 슬롯은 바꾸지 않습니다.

## 호환성 범위

### 공통 실행·흐름 제어 추가

`TaskExecutor`를 대기열·보유 메모리가 제한된 병렬 작업의 정식 API로 추가했습니다. 직렬 상태 변경과 drain이 필요한 `JobRunner`는 계속 유효합니다. 두 실행기의 종료 의미가 다르므로 기존 이름을 새 풀에 단순 위임하지 않습니다. `ErrorCode::Cancelled = 12`를 추가했으며 기존 오류 값은 바꾸지 않았습니다.

`ConnectionFlowControl`은 ServerCore TCP의 수신 정지·재개와 보유 송신량·용량 알림을 제공합니다. `QueuedSendBytes()`는 미송신량 지표로 유지하고 실제 메모리 여유 판단에는 새 `RetainedSendBytes()`와 `WaitForSendCapacity()`를 사용합니다. 사용자 정의 Connection은 별도 capability를 구현해야 이 확장을 제공합니다. 지원하지 않으면 조회가 빈 포인터를 반환합니다.

모든 `Acceptor`에 기본 256 MiB 공유 송신 예산을 적용하고 `SetSendQueueLimits()`로 구성합니다. 기존 Host 전체 예산은 유지합니다. `HttpServerOptions` 끝에는 `maxTotalSendQueueCapacityBytes`를 추가했으며 기본값은 256 MiB입니다. 전체 송신 상한 때문에 기존에 수락하던 송신도 `WouldBlock`이 될 수 있습니다. HTTP는 완성 응답을 큐에 넣을 수 없으면 연결을 닫고, WebSocket 데이터 송신은 실패 상태를 반환합니다. 상한을 끄는 호환 모드는 제공하지 않습니다.

유효한 API에 불필요한 deprecated 표시를 붙이지 않지만, 앞으로도 의미가 충돌하는 API는 대체 API와 이전 안내를 제공해 변경할 수 있습니다. 가상 인터페이스나 기존 구현을 영구 동결하는 정책은 아닙니다. 상세 스레드·취소·소유권 계약은 [실행·취소·흐름 제어](EXECUTION_AND_FLOW_CONTROL.md)를 따릅니다.

### 소스·바이너리 호환

위에서 제거를 명시한 HTTP 클라이언트 이외의 기존 API를 호출하는 소스는 유지되지만, `/WX` 또는 `-Werror`로 deprecation 경고를 오류로 취급하는 프로젝트는 위 표에 따라 이전해야 합니다. 라이브러리의 정상 빌드와 예제는 정식 API만 사용합니다. 호환성 검사에서만 deprecation 경고를 제한적으로 억제하고, 별도 컴파일 검사로 각 기존 API의 경고가 실제 발생하는지도 확인합니다.

`Session`에는 기본 구현이 있는 `GetCancellationToken`과 `SendBinary` 가상 함수가 추가되었습니다. 기존 사용자 정의 세션 소스는 유지되지만 C++ 가상 테이블 ABI는 바뀌므로 라이브러리와 소비자를 함께 재빌드합니다. 즉시 세션 취소와 바이너리 송신을 사용하려면 해당 기능을 구현해야 합니다. `Connection::Send`와 `SendWithOutcome`의 관계는 유지합니다. `JsonValue::Parse`와 `ParseBytes`도 입력 형태를 명확히 구분하는 이름으로 유지합니다.

앞선 HEAD 응답·WebSocket 프로토콜 검증·플랫폼별 수명 관리 수정은 잘못된 동작을 바로잡은 것입니다. 이를 재현하는 legacy 모드는 제공하지 않습니다. 오류 코드의 수치, 설정 구조체의 필드 순서, TCP/UDP 프레임 형식은 변경하지 않았습니다. 정적 라이브러리 소비자는 같은 툴체인·CRT로 다시 빌드하는 기존 배포 계약을 따릅니다.
