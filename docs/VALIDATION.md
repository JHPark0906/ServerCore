# 검증 결과

각 항목은 해당 날짜의 소스에 대한 기록입니다. 이전 기록의 HTTP 클라이언트·libcurl 검사는 당시 제공하던 기능의 검증이며, 현재 지원 범위를 뜻하지 않습니다.

## 2026-09-23: 0.2.0 최초 CI 실패 수정

`e8d7351`의 [GitHub CI](https://github.com/JHPark0906/ServerCore/actions/runs/35793549461)에서 로컬 MSVC·Clang/musl 검증으로 발견하지 못했던 두 문제를 확인했습니다.

- Ubuntu GCC의 `-Werror=misleading-indentation`이 C ABI의 한 줄 조건문 뒤에 이어진 독립 명령문 14곳을 거절해 Debug·Release·Rust 소스 빌드가 중단됐습니다. 조건문·출력 초기화·후속 호출을 별도 줄로 분리했습니다. 동작과 경고 설정은 유지합니다.
- Linux ASan은 `HostInputAdmission`의 작업이 내부 블록을 벗어난 지역 atomic을 읽는 `stack-use-after-scope`를 검출했습니다. 해제 신호는 작업 완료를 보증하지 않으므로 콜백이 공유 상태를 직접 보유하게 바꿨습니다. 조기 반환 시 작업 해제·수신 gate 정리와 같은 파일의 유사한 콜백 수명도 보완했습니다. 기존 입력 제한 시나리오는 유지합니다.

수정 후 Windows Release 빌드와 관련 Host·C ABI 검사 **10/10**이 통과했습니다. C ABI의 세 번역 단위는 기존 Clang/musl 도구로 엄격 경고 컴파일을 통과했습니다. 로컬에 GCC/glibc·Linux ASan 실행 환경은 없으므로 이 결과를 해당 환경의 통과로 간주하지 않습니다. 실제 GCC·ASan 및 전체 회귀 결과는 수정 커밋의 GitHub Actions 실행에서 확인합니다. 로컬 결과는 `build/backend-vs/ci-020-fix-results.xml`입니다.

## 2026-09-23: 0.2.0 배포 버전 검증

아래 서버 전용 점검을 마친 소스의 CMake·C++ 버전 API·Rust 패키지와 잠금 파일을 `0.2.0`으로 맞췄습니다. C ABI 버전은 1을 유지합니다. 공개 API 변경을 포함하는 0.x minor를 구분하도록 CMake 패키지의 호환 규칙을 `SameMinorVersion`으로 변경했습니다.

Windows Release를 다시 빌드하고 새 설치 경로에서 패키지 소비자를 구성·빌드했습니다. 구성 중 0.1.0 요청이 거절되고 0.2.0 EXACT 요청이 성공하는 것을 검사했으며, 소비자 실행·이전 이름 호환 검사 **2/2**, 버전 API·계층 검사 **3/3**이 통과했습니다. Rust는 Cargo가 네이티브 코드를 새로 빌드하는 소스 모드에서 offline·locked·전체 기능·전체 타깃 검사 **11/11**, 예제 3개 링크와 rustfmt가 통과했습니다.

버전 변경 후 전체 Windows·Linux 네이티브 검사를 다시 실행하지는 않았습니다. 구현에 대한 전체 결과와 환경 한계는 바로 아래 기록을 따릅니다. 이 항목은 GitHub 업로드 전 로컬 검증이며 원격 CI 결과는 포함하지 않습니다. 산출물은 `build/release-020-install`, `build/release-020-consumer`, Rust 로그는 `build/rust-v020-source-test.log`입니다.

## 2026-09-23: 서버 전용 범위와 레이어 점검

외부 HTTP 클라이언트의 C++·C·Rust 구현·헤더·feature·예제·검사와 CMake/CI 의존성을 제거했습니다. HTTP·WebSocket 서버, TCP/UDP, 작업 실행·취소, 스트리밍·파일 응답과 관측은 유지합니다. HTTP 관측 구현은 `src/Web`으로 옮기고, `ServerHost`의 Net 내부 송신 예산 주입은 공개 `Acceptor::SetSendQueueLimits`로 교체했습니다. 기존 Host 송신 예산은 유지합니다.

| 환경 | 검증 | 결과 |
| --- | --- | --- |
| Windows x64 MSVC Debug | 전체 CTest, 소스·설치 패키지·C ABI 소비와 계층 검사 | 205/205 통과 |
| Windows x64 MSVC Release | 전체 CTest, 소스·설치 패키지·C ABI 소비와 계층 검사 | 205/205 통과 |
| Linux x86_64 Clang 20.1.2·musl Debug, QEMU Linux 6.12.110 | 전체 네이티브 검사 | 201/201 통과 |
| 같은 Linux | 새 prefix의 정적 C ABI C11 소비자 구성·링크·실행, 계층 검사 | 통과 |
| Rust 1.89 Windows MSVC | Cargo가 네이티브 Release를 소스에서 빌드, offline 전체 기능·전체 타깃, 경고 오류 처리 | 11/11 통과 |
| Rust 1.89 Linux musl | 새 정적 라이브러리 링크·VM 실행 | 11/11 통과 |
| Rust workspace | 예제 3개 컴파일·링크, rustfmt | 통과 |

남은 네이티브 등록 검사는 201개입니다. Windows CTest는 등록 목록·계층 검사와 패키지 소비 2개를 더해 205개입니다. 이전 208개 네이티브 검사에서 HTTP 클라이언트 전용 7개를 제거했고, Rust도 해당 기능의 2개 단위 검사를 제거해 3개 단위·8개 loopback 검사를 유지합니다. 예제 harness의 종료는 실제 예제 서버 운영 검증으로 세지 않습니다.

계층 검사는 저장소 내부 직접 include 260개를 검사합니다. 별도 임시 fixture에서 Net → Web과 Execution → Export 위반이 거절되는 것도 확인했습니다. 모든 언어·플랫폼 분기의 의미를 분석하는 검사는 아니며 [아키텍처](ARCHITECTURE.md)의 범위와 UDP 내부 계약 예외를 따릅니다. 제거한 `SERVERCORE_BUILD_HTTP_CLIENT=ON` 요청은 이전 안내를 포함한 구성 오류로 거절됩니다.

새 Windows C ABI DLL은 클라이언트 export가 없고 OS·C++ 런타임 DLL만 사용합니다. Linux의 새 CMake cache·빌드 입력·설치 prefix에도 curl/TLS 패키지 참조가 없습니다. 남은 C ABI는 버전 1과 기존 수치·레이아웃을 유지하되 capability 마스크 값 `4`는 예약하고 반환하지 않습니다. 제거한 클라이언트 API의 바이너리 호환성을 약속하지 않으며 [이전 안내](API_MIGRATION.md)에 따라 새 설치 경로를 사용합니다.

Linux는 Zig 0.15.2 교차 빌드와 QEMU 커널 실행이며 Ubuntu 네이티브·glibc, sanitizer·장기 부하 검증은 포함하지 않습니다. GitHub 업로드·원격 CI 실행·커밋은 하지 않았습니다. 현재 문서의 로컬 링크와 `git diff --check`도 통과했습니다.

로그는 `build/backend-vs/windows-{debug,release}-backend-results.xml`, `build/backend-reject-client.log`, `build/linux-backend-{configure,build,vm,package,rust-vm,layers}.log`, `build/rust-server-only-source-test.log`, `build/rust-server-only-dll-{exports,dependents}.log`에 남겼습니다.

## 2026-09-22: 웹·게임 라이브러리 확장 2·4·5·6·7·8

현재 로컬 작업 트리에 제한된 JobRunner와 예약 완료·세션 작업 취소, 서버 drain, 연결 보호와 웹 정책, TCP·UDP 바이너리 봉투, 요청 스트리밍·파일 조건부 전송, 인스턴스 로그·지연 histogram·Prometheus 변환을 구현했습니다. 웹 확장은 기존 ABI 1 구조체를 바꾸지 않는 새 C 함수·descriptor와 안전한 Rust 소유 API로 연결했습니다. GitHub 업로드·패키지 게시·커밋은 수행하지 않았습니다.

| 환경 | 검증 | 결과 |
| --- | --- | --- |
| Windows x64 MSVC Debug | 전체 CTest, 설치·소스·선택적 클라이언트·C ABI 패키지 소비 | 212/212 통과 |
| 같은 Debug | 종료·스트리밍 수명 수정 후 Web·C ABI·운영·등록 목록 | 53/53 통과 |
| 같은 Debug | 최종 파일 시각·청크 취소/EOF 수정 후 관련 파일·업로드·정책·종료 | 9/9 통과 |
| Windows x64 MSVC Release | 최종 구현 전체 CTest와 패키지 소비 | 212/212 통과 |
| Linux x86_64 Clang·musl Debug, QEMU | 최종 구현 전체 네이티브 검사, HTTP 클라이언트·정적 C ABI 포함 | 208/208 통과 |
| 같은 Linux VM | CAbi 전용 설치 패키지의 C11 소비자 구성·링크·실행 | 통과 |
| Rust 1.89 Windows MSVC | 최종 Release DLL, 전체 기능·전체 타깃, 경고 오류 처리 | 13/13 통과 |
| Rust 1.89 Linux musl | 최종 정적 라이브러리 링크·VM 실행, 예제 4개 컴파일·링크 | 13/13 통과 |
| Rust workspace | rustfmt 검사 | 통과 |

네이티브 등록 검사는 이전 189개에서 208개로 늘었습니다. Windows CTest는 등록 목록 검사와 패키지 소비 3개를 더해 212개입니다. 새 검사와 기존 검사 확장은 다음을 확인합니다.

- 작업 수·선언 바이트 예산, 실행 중 캡처까지 유지되는 과금, 제어 작업 예산, 예약된 결과의 drain 중 전달과 강제 종료 후 거절, 세션 종료에 따른 대기·실행 작업 취소.
- TCP 바이너리의 NUL·비 UTF-8 왕복, UDP 등록 상한·토큰과 TCP 세션 수명, 프레임 조각의 도착 시각·절대 기한, 인증 기한·입력 속도·조각 상한, 여러 Host의 로거 독립성.
- 수락한 응답·게임 작업을 비우는 정상 종료, 기한 초과, C ABI의 정상 종료 대기를 즉시 Stop으로 중단하는 경쟁 경로, callback에서 잘못된 자기 종료 호출 거절.
- fixed/chunked 업로드, 꺼내 보관한 청크에 의한 수신 정지, 취소와 reader 해제, 정책의 HTTP 속성·공통 응답, 전역 및 라우트별 WebSocket 인증 결정이 101보다 먼저 실행되는 순서.
- 실제 파일의 206·HEAD·304·416 전송과 사전 조건 판정, 고정 histogram 경계·포화·누적 Prometheus 출력, C/Rust 소유 handle과 메트릭·로거.

검토 중 C ABI의 긴 graceful 대기가 즉시 Stop을 막던 잠금, 업로드 Cancel과 EOF의 경쟁, 100 Continue와 빠른 최종 응답의 순서, 플랫폼별 파일 시각 표현 차이를 수정했습니다. UDP 신규 테스트의 최초 실패는 수신 predicate를 두 번 평가해 성공한 datagram을 다시 읽으려던 테스트 오류였으며, 한 번의 성공을 그대로 반환하도록 고쳤습니다.

Linux는 Zig 0.15.2의 x86_64-musl 교차 빌드와 QEMU Linux 6.12.110 실행입니다. Ubuntu 네이티브, sanitizer, 장기 부하, 실제 ZISC·게임 서버 이식 검증은 포함하지 않습니다. 취소는 협력적이며 사용자 처리기를 강제 종료하지 않습니다. Rust의 기존 약 10ms readiness 대기 구현과 UDP·ServerHost 미매핑 범위는 이번 선택 항목에 포함하지 않았습니다.

결과는 `build/platform-web-vs/windows-debug-roadmap-full.log`, `windows-debug-roadmap-final-results.xml`, `windows-debug-roadmap-edge-results.xml`, `windows-release-roadmap-final-results.xml`, `build/linux-native-final-build.log`, `linux-native-final-vm.log`, `linux-native-final-package.log`, `linux-native-final-rust-vm.log`에 남겼습니다. Rust 검사는 5개 단위 검사와 8개 loopback 검사이며 예제 harness의 정상 종료를 실제 예제 서버 운영 검증으로 간주하지 않습니다.

## 2026-09-22: Rust·C ABI와 공통 운영 도구

로드맵 8~9의 선택적 C ABI 1, 안전한 Rust 래퍼와 Cargo/CMake 소비 경로, 비동기 콘솔·회전 파일 로거, 작업·HTTP 지표, HTTP 요청 종료 추적을 구현했습니다. 기존 네트워크 구현을 사용하며 C++에서 Rust 콜백을 호출하지 않습니다. 공개 계약은 [C ABI](C_ABI.md), [Rust](RUST.md), [운영 도구](OPERATIONS.md)에 있습니다.

| 환경 | 검증 | 결과 |
| --- | --- | --- |
| Windows x64 MSVC, Debug, HTTP 클라이언트·공유 C ABI 포함 | 전체 CTest, 기존 소스/설치·CRT 소비, 새 CAbi 전용 설치·순수 C 소비 | 193/193 통과 |
| 같은 Debug | 마지막 이벤트 해제 함수명 통일 후 C ABI 및 설치 소비 재검사 | 8/8 통과 |
| Windows x64 MSVC, Release | 전체 CTest와 설치·소스 소비 | 193/193 통과 |
| Linux x86_64 커널 VM, Clang·musl, Debug | HTTP 클라이언트·정적 C ABI 포함 전체 C++ 등록 검사 | 189/189 통과 |
| 같은 Linux VM | CAbi 전용 설치 후 `find_package(COMPONENTS CAbi)`로 연결한 C11 소비자 | 구성·빌드·실행 통과 |
| Rust 1.89, Windows MSVC 대상 | 최종 공유 DLL을 소비하는 전체 기능 Cargo 검사, 경고 오류 처리 | 10/10 통과 |
| Rust 1.89, Linux musl 대상 | 전체 기능·전체 타깃 정적 교차 링크 및 Linux VM 실행 | 10/10 통과, 예제 4개 컴파일 |
| Rust 1.89, Windows 소스 소비 | Cargo가 core-only 네이티브 Release를 빌드·CAbi 컴포넌트 설치·DLL 로딩 | 8/8 통과 |
| Rust workspace | rustfmt 검사 | 통과 |

새 C ABI 검사 7개는 입력 복사, 보관 이벤트의 바이트·개수 예산, handle 복제·마지막 해제, 요청/응답·WS·TCP 이벤트가 서버 종료보다 오래 살아 있는 경우, 큐 초과와 예약된 종료 이벤트, HTTP 클라이언트의 일시정지·재개·취소를 다룹니다. 순수 C 소비자는 C++ 언어를 활성화하지 않은 Windows 프로젝트로 공유 라이브러리를 사용하며, Linux 정적 소비자는 C로 컴파일한 뒤 C++ 링커와 전이 의존성을 사용합니다.

Rust 검사는 실제 loopback HTTP 패턴·70 KiB 응답 스트리밍·16 KiB 송신 상한, Future drop과 미완료 응답 해제, 연결 종료 취소 알림, masked WS 바이너리, raw TCP 바이트·종료 수명, 용량 대기 취소, 선택적 클라이언트 수신·취소를 포함합니다. Cargo 소스 소비는 libcurl 없이 성공했고, DLL은 각 build script의 OUT_DIR에만 준비하여 Cargo 실행 경로로 로딩했습니다. crates.io 게시나 바이너리 배포는 수행하지 않았습니다.

운영 검사 3개는 레벨 필터·제어 문자 이스케이프·파일 회전·출력 실패, 완료·실패·취소·시간 초과 집계, 지연 및 종료 후 보관 바이트, 요청 생성 예외의 종료 집계, 추적 큐 손실·콜백 예외·재진입을 확인합니다. 이벤트와 로그 본문의 실제 저장소를 해제한 뒤 예산을 반환하도록 검토·수정했고, C ABI 큐 종료 과정의 추가 메모리 할당도 제거했습니다.

Linux는 Zig 0.15.2의 x86_64-musl 교차 빌드와 QEMU Linux 6.12.110에서 실행했습니다. Ubuntu 네이티브, ASan·UBSan, 성능·고부하·장기 실행 검증은 이번 결과에 포함하지 않습니다. Rust readiness 알림은 약 10ms 주기의 대기 작업자를 사용하며 실시간 지연 상한을 보장하지 않습니다. C++ 전체 API의 Rust 매핑과 실제 ZISC/게임 서버 이식 완료를 뜻하지 않습니다.

결과 파일은 `build/platform-web-vs/windows-debug-cabi-final-results.xml`, `windows-debug-cabi-naming-results.xml`, `windows-release-cabi-final-results.xml`, `build/linux-cabi-vm.log`, `build/linux-rust-vm.log`, `build/linux-cabi-package.log`, `build/rust-source-validation.log`입니다. Windows·Ubuntu CI 정의를 보완했지만 원격 실행하거나 GitHub에 업로드하지 않았습니다.

## 2026-09-22: 비동기 HTTP·스트리밍·선택적 HTTP 클라이언트

로드맵 5~7의 HTTP 비동기 처리기, 응답 작성기·대용량 파일·SSE, WebSocket 송신 용량 알림과 선택적 libcurl 클라이언트를 구현했습니다. 기존 완성 응답 API는 동일한 처리기 풀·작성기·헤더 인코더를 사용합니다. 기본 코어를 소비할 때 libcurl을 요구하지 않는 설치 계약도 확인했습니다.

| 환경 | 검증 | 결과 |
| --- | --- | --- |
| Windows x64, MSVC, Debug | 전체 CTest와 설치·소스 소비 검사 | 182/182 통과 |
| 같은 Debug | 최종 수명·잠금·처리기 공유 수정 후 Web·등록 목록 재검사 | 35/35 통과 |
| Windows x64, MSVC, Release | 최종 구현의 전체 CTest와 설치·소스 소비 검사 | 182/182 통과 |
| Windows Debug·Release | 마지막 테스트 수명 가정 교정 후 클라이언트 종료 검사 | 각각 1/1 통과 |
| Linux x86_64 커널 VM, Clang·musl, Debug | 선택적 클라이언트 포함 전체 C++ 등록 검사 | 179/179 통과 |
| 같은 Linux VM | 설치·소스의 정식·호환 소비자, 웹 예제, 선택 모듈의 코어·클라이언트 소비자, TLS 기능 확인 | 8/8 통과 |
| Linux 교차 빌드 | 이동한 선택 모듈 설치 패키지 구성·링크 | 통과 |

검사는 다음 경계를 포함합니다.

- 소유 요청 문맥·경로 매개변수, 처리기 반환 이후 응답, I/O와 다른 요청의 진행, pipelining 순서와 뒤따르는 WebSocket upgrade.
- 완료·취소 후 보관한 문맥의 바이트 예산, 처리기 객체의 지속적인 상태, 응답 종료 시 이전 용량 대기 정리, 재진입과 WebSocket 동시 종료.
- HEAD·204·205·304, 고정 길이·chunked framing, SSE 필드 검증, 느린 수신자의 송신 상한과 정체 종료, 2 MiB를 넘는 바이너리 파일의 무결성과 HEAD.
- 파일 작업의 대기 중 취소, 경로·헤더 보유 예산, 호출자 여분 용량 제거, 제출 거절 시 응답 보존.
- HTTP 클라이언트 GET·바이너리 POST·인증 헤더, 1xx·중복 헤더·chunked 수신, 제한시간·취소·정지와 재개·예외·종료 후 콜백 해제.
- 실제 서버 비동기 처리기에서 플러그인 HTTP 응답을 기다리며 다른 요청을 처리하고, 서버 종료를 하위 클라이언트 요청의 취소로 전달하는 통합 경로.

Linux 최초 실행의 종료 검사 하나는 이동한 `std::function`의 원본이 항상 비워진다고 가정해 실패했습니다. 호출자 소유 원본을 명시적으로 해제한 뒤 라이브러리의 콜백 수명·보유 바이트·대기 수·활성 수를 각각 검사하도록 테스트를 교정했습니다. 생산 코드 변경 없이 Linux 전체 재실행이 통과했습니다.

Windows의 libcurl 8.22.0은 Schannel과 `/MDd`·`/MD`, Linux의 같은 libcurl은 mbedTLS 3.6.7을 사용했습니다. 두 SDK 모두 HTTP·HTTPS·비동기 DNS·thread-safe 초기화 기능을 확인했습니다. 이 의존성은 무시된 로컬 build 경로에만 있으며 배포 소스로 추가하지 않았습니다. 기본 코어의 외부 의존성은 늘지 않았습니다.

Linux는 Zig 0.15.2로 x86_64-musl 정적 실행 파일을 만들고 QEMU Linux 6.12.110에서 실행했습니다. Ubuntu 네이티브·ASan·UBSan·부하 측정·외부 HTTPS 인증서 연결 검증은 이번 결과에 포함하지 않습니다. 서버 TLS·파일 Range·Rust/C FFI는 이 구현의 지원 범위가 아닙니다. Ubuntu 선택 모듈 CI는 추가했지만 원격 실행하지 않았습니다.

로그는 `build/platform-web-vs/windows-debug-web-async-results.xml`, `windows-debug-web-final-results.xml`, `windows-release-web-async-results.xml`, `windows-debug-client-lifetime-results.xml`, `windows-release-client-lifetime-results.xml`, `build/linux-http-vm.log`, `build/linux-http-consumers-vm.log`, `build/linux-http-package-check.log`입니다. GitHub 업로드는 수행하지 않았습니다.

## 2026-09-22: 공통 실행·취소·TCP 흐름 제어

웹·게임 공통 계층의 의존성·소유권·오류·종료 계약을 정리하고, 제한된 `TaskExecutor`와 TCP `ConnectionFlowControl`을 추가했습니다. 수신 정지·재개는 Windows IOCP와 Linux epoll에 적용하며 송신 저장소·공유 예산·취소 가능한 용량 알림은 공통 구현을 사용합니다. raw TCP 수락기와 HTTP·WebSocket에도 전체 송신 예산을 적용했습니다.

| 환경 | 검증 | 결과 |
| --- | --- | --- |
| Windows x64, MSVC, Debug | 전체 CTest와 설치·소스 소비, deprecated·CRT 검사 | 162/162 통과 |
| Windows x64, MSVC, Release | 전체 CTest와 설치·소스 소비, deprecated·CRT 검사 | 162/162 통과 |
| Linux x86_64 커널 VM, Clang·musl, Debug | 전체 C++ 등록 검사 | 160/160 통과 |
| 같은 Linux VM | 설치·소스 소비자 각각의 정식·호환 실행 파일과 WEB.md 예제 | 5/5 통과 |

새 C++ 검사 16개는 작업 수·보유 바이트 제한, 부모·수동 취소와 마감 시간, 종료·완료 경쟁, 중첩 실행기의 자기 대기 방지, 부분 송신 저장소, 연결 간 공유 예산과 알림 누락 방지, 등록 해제·취소·종료·콜백 재진입, 수락 처리기 전후의 Pause/Resume, 콜백 버퍼·순서 보존, 정지 중 송신·EOF 종료와 공개 예산 설정을 검증합니다. 기존 웹 한도 검사에는 작은 전체 예산의 HTTP 응답·WebSocket handshake 거절과 충분한 예산의 정상 전송을 추가했습니다.

검토에서 확인한 중첩 취소 문맥 손실, 등록 중 소유자 파괴, 공유 송신 알림 중 다른 큐가 파괴될 때의 Closed 통지 누락을 수정하고 재현 검사를 포함했습니다. 최종 수명 수정 뒤 Debug 전체·Linux 전체와 소비자를 다시 검증했습니다. 공개 TaskExecutor·흐름 제어 헤더와 정식 API는 패키지 소비 검사에도 포함합니다.

결과 파일은 `build/platform-web-vs/windows-debug-flow-results.xml`·`windows-release-flow-results.xml`, `build/linux-flow-vm.log`, `build/linux-flow-consumers-vm.log`입니다. Linux는 Zig 0.15.2로 x86_64-musl 정적 실행 파일을 교차 빌드하고 QEMU Linux 6.12.110에서 실행했습니다. Ubuntu 네이티브·ASan·UBSan·성능 측정 결과는 포함하지 않습니다. 실행 중 작업 취소는 협력적이며 HTTP 비동기 완료·클라이언트·스트리밍·Rust 바인딩은 이번 범위에 포함하지 않습니다. GitHub 업로드는 수행하지 않았습니다.

## 2026-09-22: HTTP·WebSocket 패턴 라우팅

`RegisterRoutePattern`, `RegisterWebSocketPattern`, 소유된 경로 매개변수와 `PathParameter`를 추가했습니다. HTTP와 WebSocket은 같은 패턴 해석기를 사용하며, WebSocket의 `onOpenWithRequest`에서 새 연결과 요청을 함께 받을 수 있습니다. 기존 정확 일치 API와 `onOpen`은 유지합니다. 경로는 있지만 HTTP 메서드가 맞지 않으면 `405`와 `Allow`를 반환하고, 같은 경로에 등록된 일반 HTTP와 WebSocket 요청을 구분합니다.

| 환경 | 검증 | 결과 |
| --- | --- | --- |
| Windows x64, MSVC, Debug | 전체 CTest, 설치·소스 소비자와 deprecated 호환 검사 포함 | 146/146 통과 |
| Windows x64, MSVC, Release | 전체 CTest, 설치·소스 소비자와 deprecated 호환 검사 포함 | 146/146 통과 |
| Linux x86_64 커널 VM, Clang·musl, Debug | 전체 C++ 등록 검사 | 144/144 통과 |
| 같은 Linux VM | 설치·소스 소비자 각각의 정식·호환 실행 파일과 최신 `WEB.md` 예제 | 5/5 통과 |

새 C++ 검사 5개는 패턴 문법·중복 등록, 한 번의 percent decoding과 UTF-8 검증, 경로 우선순위와 메서드별 매개변수 이름, HEAD·405·Allow, HTTP·WebSocket 경로 공유, 요청 복사·이동과 pipelining의 매개변수 수명, 등록 생명주기를 검증합니다. 새 WebSocket 콜백의 요청 전달과 기존 콜백과의 동시 지정 거절도 포함합니다. 소비자 프로젝트는 기존 네 필드로 초기화한 `HttpRequest`와 `WebSocketCallbacks`를 경고 억제 없이 컴파일합니다.

Linux는 기존 Zig 0.15.2 툴체인으로 `x86_64-linux-musl` 정적 실행 파일을 교차 빌드하고 QEMU의 Linux 6.12.110 커널에서 실행했습니다. 이번 변경의 Ubuntu 네이티브·ASan·UBSan 검증은 포함하지 않습니다. Linux 테스트의 `noexcept` 검사는 암시적 문자열 변환을 제외하고 메서드 자체의 계약을 검사하도록 수정했습니다.

결과 파일은 `build/platform-web-vs/windows-debug-routing-results.xml`·`windows-release-routing-results.xml`, `build/linux-routing-vm.log`, `build/linux-routing-consumers-vm.log`입니다. 변경은 로컬 검증만 수행하며 GitHub에 게시하지 않았습니다.

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
