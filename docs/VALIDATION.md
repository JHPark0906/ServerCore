# 검증 결과

## 2026-09-21: 공개 API 일관성과 deprecated 호환성

`JobRunner::RequestStop`, `DatagramTransport::SendSerialized`, `HttpServer::RegisterRoute`와 `RegisterWebSocket`을 정식 진입점으로 추가했습니다. 기존 네 이름은 `[[deprecated]]`를 붙인 외부 정의로 유지하며 새 구현에 전달합니다. 내부 호출과 일반 검사·문서 예제는 새 이름으로 이전했습니다. 변경 이유와 WebSocket 송신 UTF-8 오류의 `InvalidArgument` 교정은 [API_MIGRATION.md](API_MIGRATION.md)에 기록했습니다.

Windows Debug·Release 전체 CTest는 각각 **141/141 통과**했습니다(Debug 236.03초, Release `--parallel 4` 213.11초). 최종 빌드 출력에 경고는 없습니다. Linux x86_64 커널 VM의 전체 C++ 검사는 **139/139 통과**했습니다. 같은 Linux VM에서 설치 패키지·소스 직접 포함 방식의 정식 API 소비자 두 개, deprecated 호환 소비자 두 개와 `WEB.md` 예제도 **5/5 통과**했습니다.

소비자 검증은 공개 헤더를 포함한 정식 API를 경고 억제 없이 컴파일하고, 호환 소비자에만 deprecation 경고 억제를 적용합니다. 각 소비 방식에서 네 기존 API를 별도 컴파일하여 deprecation 경고 종류와 올바른 대체 이름을 확인했습니다. Windows Debug·Release와 Linux에서 각각 **8/8** 컴파일 실패 검사를 통과했습니다. 단순 컴파일 실패는 성공으로 인정하지 않습니다. 기존·새 API의 공통 등록 상태와 오류, `Send`의 `noexcept`, 종료 요청 뒤 수락한 작업의 실행도 확인했습니다.

도구와 실행 환경은 아래 Linux·웹 추가 기록과 같습니다. Linux는 Clang·musl 교차 빌드 및 실제 Linux 커널 VM 검증이며, Ubuntu·glibc 네이티브 CI와 ASan·UBSan 실행 결과는 포함하지 않습니다. 결과 파일은 `build/platform-web-vs/windows-debug-api-results.xml`·`windows-release-api-results.xml`, `build/linux-api-vm.log`와 `build/linux-api-consumers-vm.log`입니다.

## 2026-09-21: 중복 구현과 플랫폼·프로토콜 비대칭 점검

Linux·웹 기능을 추가한 뒤 같은 작업 트리에서 공통 계약을 비교하고 다음을 수정했습니다.

| 확인한 문제 | 수정 |
| --- | --- |
| Windows 수락기가 연결마다 콜백을 복사해 mutable 캡처의 상태가 유지되지 않음 | 두 플랫폼이 등록된 같은 콜백을 호출하고 인계 중 소유권을 보존 |
| Linux 수락기는 Stop 이후에도 콜백 캡처를 보관 | 진행 중인 인계가 끝난 뒤 두 플랫폼 모두 해제하고, 캡처 소멸은 상태 잠금 밖에서 수행 |
| Linux 연결 시작·송신 실패의 종료 통지는 상세 진단 문자열을 버림 | 호출 결과와 종료 통지에 진단을 보존하고, 복사 할당 실패 시 무할당 PlatformError로 전환 |
| TCP 플랫폼마다 송신 큐·부분 전송·메모리 반환 규칙을 별도 구현 | 공통 SendQueue로 통합. Windows의 커널 버퍼 참조가 끝나기 전에는 보관 메모리를 반환하지 않음 |
| IPv4 주소, 설정 경로, JSON 문서·봉투와 원자적 자원 예산 검증 중복 | 공통 검증·계산 경로를 사용하며 원래 계층별 오류 코드는 유지 |
| HEAD의 오류 응답에 본문이 붙고, 205·304 응답의 길이 처리가 잘못됨 | 정상·오류 응답의 HEAD 처리와 상태별 본문·Content-Length 정책을 통일 |
| 일반 응답의 Date와 426의 Upgrade 관련 필수 헤더 누락 | Date 자동 생성, 요청·응답의 공통 Upgrade 문법 검증과 올바른 426 응답 |
| WebSocket 송신 API마다 종료·크기·UTF-8 검증 순서가 다름 | 종료 상태와 크기를 먼저 검사한 후 텍스트 검증 |

등록된 C++ 회귀 검사는 131개에서 **139개**로 늘었습니다. 추가한 8개는 콜백 상태·수명, 부분 송신 보관 메모리, UDP 주소·빈 패킷·잘림 처리, JSON 진입점·봉투 규칙 일치, HTTP 응답 의미와 WebSocket 송신 검증을 확인합니다. 기존 동시성·세션 종료·공유 예산 롤백 검사도 전체 실행에 포함했습니다.

Windows Debug·Release 전체 CTest는 각각 **141/141 통과**(Debug 191.20초, Release 188.28초)했습니다. 최종 빌드 출력에 경고는 없습니다. 동일한 Clang·musl 교차 빌드와 Linux 가상 머신에서 최종 변경을 포함한 C++ 검사는 **139/139 통과**했습니다. Windows의 나머지 2개는 등록 목록 일치와 설치·소스 패키지 소비 검사입니다. 결과 파일은 `build/platform-web-vs/windows-debug-audit-results.xml`·`windows-release-audit-results.xml`과 `build/linux-audit-vm.log`입니다.

Windows IOCP와 Linux epoll의 커널 처리 방식, TCP의 고정 포트와 UDP의 임시 포트 허용, 원시 UDP의 빈 패킷과 응용 메시지의 빈 payload 금지, 메시지 수신 오류 `InvalidFormat`과 잘못된 송신 인자 `InvalidArgument`는 목적에 따른 차이이므로 유지했습니다. 기존 TCP 길이 프레임과 UDP datagram의 wire format은 변경하지 않았습니다. Ubuntu 네이티브 CI·ASan·UBSan은 이번 로컬 검증에 포함하지 않았습니다.

## 2026-09-21: Linux·HTTP·WebSocket 추가

Windows IOCP 구현을 유지하면서 Linux epoll·POSIX 구현과 `Web::HttpServer`를 추가한 작업 트리를 검증했습니다. 기존 TCP·UDP·설정 파일·런타임 검사와 새 HTTP·WebSocket 검사를 함께 실행했습니다.

| 환경 | 검증 | 결과 |
| --- | --- | --- |
| Windows x64, MSVC 19.50.35728, Debug | 전체 빌드와 CTest, 설치·소스 소비 프로젝트 포함 | 133/133 통과, 185.52초 |
| Windows x64, MSVC 19.50.35728, Release | 전체 빌드와 CTest, 설치·소스 소비 프로젝트 포함 | 133/133 통과, 187.75초 |
| Linux x86_64 가상 머신, Clang 20.1.2·musl, Debug | 실제 Linux 커널에서 등록된 C++ 검사 전체 실행 | 131/131 통과 |
| 같은 Linux 가상 머신 | 설치 패키지 소비, 소스 트리 소비, `WEB.md` 예제 실행 | 3/3 통과 |

Windows는 CMake 4.2.3-msvc3, Visual Studio 18 2026 생성기와 Windows SDK 10.0.26100.0을 사용했습니다. Debug `/MDd`, Release `/MD`로 빌드했으며 최종 빌드 출력에 경고가 없습니다. Linux는 Windows에서 Zig 0.15.2의 Clang으로 `x86_64-linux-musl` 정적 실행 파일을 교차 빌드한 뒤, QEMU의 Alpine Linux 3.22 virt 커널과 loopback 네트워크를 사용하는 임시 머신에서 실행했습니다. QEMU·Zig는 검증 도구이며 라이브러리의 빌드·실행 의존성이 아닙니다.

Linux의 131개는 C++ 등록 검사 수입니다. Windows CTest의 133개에는 등록 목록 일치 검사와 CMake 패키지 검사도 포함되므로 서로 같은 분모가 아닙니다. Linux 설치·소스 소비 프로젝트는 별도로 구성·링크하고 가상 머신에서 실행했습니다. 문서의 웹 예제도 그대로 추출해 컴파일한 뒤 리스너 시작과 종료를 확인했습니다.

새 웹 검사는 다음을 포함합니다.

- HTTP/1.1 파싱·실제 소켓 왕복, 지속 연결·pipelining·HEAD·chunked 본문·100-continue.
- 모호한 요청 길이와 잘못된 헤더 거절, 본문·응답·연결 상한, 요청·유휴·응답 종료 기한.
- WebSocket handshake, 마스킹·UTF-8·opcode·길이 검증, 메시지 분할·재조립, ping/pong·close.
- 처리기 예외, 활성 연결이 있는 서버 종료와 연결 슬롯 회수.

Linux 실행에서 Windows 체크아웃의 CRLF 테스트 벡터를 발견해, 벡터 리더가 LF와 CRLF를 모두 읽도록 수정했습니다. 이후 Linux 전체와 Windows Release 전체를 통과했고, Windows Debug의 해당 검사도 다시 통과했습니다.

빌드 출력은 `build/platform-web-vs`, `build/linux-cross`, 소비 프로젝트는 `build/linux-package-consumer`와 `build/linux-source-consumer`에 있습니다. Windows JUnit 결과는 `build/platform-web-vs/windows-debug-results.xml`·`windows-release-results.xml`, Linux 실행 로그는 `build/linux-vm.log`·`build/linux-consumers-vm.log`입니다. 이 파일들은 로컬 검증 산출물이며 배포 소스에 포함하지 않습니다.

Ubuntu 24.04의 네이티브 GCC 빌드 및 Clang ASan·UBSan 검사는 `.github/workflows/build.yml`에 추가했지만, 이번 로컬 작업에서는 해당 CI를 실행하지 않았습니다. 위 Linux 결과는 Ubuntu·glibc 환경의 검증 결과가 아닙니다. 부하 성능·장기 운영·외부 프로토콜 적합성 도구 검증과 TLS는 이번 검증 범위에 포함하지 않습니다.

## 2026-09-07: UDP 추가 당시의 검증 기록

2026-09-07에 공통 UDP 전송을 ServerCore로 옮긴 소스를 독립된 빌드 디렉터리에서 검증했습니다. 기존 공개 소스에 `Runtime::DatagramTransport`, 내부 Net 소켓과 회귀 검사를 추가한 입력이며, 게임·엔진 소스나 에셋을 요구하지 않습니다.

### 환경

| 항목 | 확인한 값 |
| --- | --- |
| 플랫폼 | Windows x64 |
| 컴파일러 | MSVC 19.50.35728.0 |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.2.3-msvc3 |
| 생성기 | Visual Studio 18 2026, x64 |
| CRT | Debug `/MDd`, Release `/MD` |

라이브러리의 실제 생성된 컴파일 설정에서 `/W4 /WX`와 구성별 CRT를 확인했으며 두 구성의 빌드 경고는 0개입니다. Windows 시스템 라이브러리 `ws2_32`와 `bcrypt`를 링크하며 제3자 패키지 설치 단계는 없습니다.

### 빌드와 회귀 테스트

`cmake -S . -B build/vs -G "Visual Studio 18 2026" -A x64 -DSERVERCORE_BUILD_TESTS=ON`으로 구성했습니다. Debug·Release의 전체 빌드가 성공한 뒤 `ctest --test-dir build/vs -C <구성> --output-on-failure --no-tests=error`로 각 구성을 순서대로 검사하고 JUnit 결과를 확인했습니다.

| 구성 | 전체 빌드 | CTest | CTest 실행 시간 |
| --- | --- | --- | --- |
| Debug | 통과, 경고 0개 | 125/125 | 130.99초 |
| Release | 통과, 경고 0개 | 125/125 | 129.88초 |

총 **250/250 통과**이며 실패하거나 건너뛴 검사는 없습니다. 기존 115개에 다음 범위의 UDP 검사 10개를 추가했습니다.

- 임시 포트·배타적 바인딩, 반복 종료·재바인딩과 독립적인 Winsock 수명입니다.
- 세션 토큰 등록·폐기·재등록, 일반 JSON 봉투 왕복과 실제 송신 성공의 순번·바이트 지표입니다.
- 알 수 없는 토큰·재전송·손상 JSON·허용되지 않은 고순번 메시지의 거절과 정상 경로 회복입니다.
- endpoint 재바인딩, 콜백의 송신·등록 해제·종료 재진입, admission 중 등록 교체와 예외 경계입니다.
- 빈 datagram·잘린 패킷·초과 길이 입력과 수신 시도·바이트 예산입니다.

`ServerCore.CMake.InstallAndSourceTreeConsume`은 새 공개 API를 포함하는 별도 소비 프로젝트를 소스의 `add_subdirectory`와 이동한 설치 패키지의 `find_package`로 빌드·실행했습니다. CRT 불일치와 잘못된 CMP0091 부모 정책의 거절 검사도 포함합니다. 공개 전송 헤더는 새 UDP 검사와 패키지 소비 코드에서 첫 번째 헤더로 포함해 독립적인 선언 컴파일을 확인했습니다.

SummitServer도 같은 ServerCore 소스로 Debug·Release를 빌드하고 기존 8개 검사를 각각 통과했습니다. schema 6, UDP Hello/Ready·이동과 기존 TCP, 토큰 폐기·손상 패킷 거절·주소 재바인딩의 통신 계약을 유지했습니다.

### 결과의 범위

위 시간은 회귀 검사의 소요 시간이며 처리량이나 네트워크 지연 벤치마크가 아닙니다. 이번 변경에서는 별도의 부하 성능 실험을 실행하지 않았습니다. 소켓 버퍼 포화, CSPRNG 실패와 메모리 할당 실패를 강제로 주입한 검사는 포함하지 않습니다.

명령과 테스트 구분은 [빌드·테스트·배포](BUILD_TEST_DEPLOY.md), 지원 범위는 [지원 범위와 설정](SUPPORT_AND_LIMITS.md)을 참고합니다.
