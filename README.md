# ServerCore

Windows와 Linux에서 웹·게임 백엔드를 만들기 위한 **C++23 서버 라이브러리**입니다. Windows IOCP·Linux epoll 기반 네트워크, HTTP/1.1·WebSocket, 메시지 처리와 제한된 작업 실행을 제공하며 애플리케이션이 그 위에 자신의 규칙을 구현합니다.

현재 버전은 **0.2.0**입니다. C++와 선택적 C ABI를 하나의 정적 라이브러리 또는 DLL/SO로 빌드합니다. Rust에서는 같은 C ABI를 사용하는 소유권 기반 래퍼를 제공합니다. 프로젝트 코드는 [MIT-0](LICENSE) 라이선스이며 개발 과정에서 생성형 AI의 도움을 받았습니다.

표준 라이브러리·컴파일러 런타임·OS API만 사용하고 서드파티 라이브러리에 의존하지 않습니다. Rust workspace에도 crates.io 의존성이 없으며 Tokio 같은 비동기 실행기를 강제하지 않습니다.

## 제공하는 기능과 범위

| 영역 | 제공하는 기능 |
| --- | --- |
| 네트워크 | IPv4·IPv6 TCP 수락, 부분 송수신, 연결 수명, 비차단 UDP와 제한된 수신·송신 예산 |
| 웹 | HTTP/1.1, 정확·패턴 라우팅과 경로 매개변수, 비동기 처리기, 요청·응답 스트리밍, SSE, 파일 전송·단일 Range·ETag |
| WebSocket | 업그레이드 전 인증, subprotocol 협상, text·binary·분할 송신, Ping/Pong heartbeat, 흐름 제어 |
| HTTP 도구 | 요청 제한, CORS·공통 헤더·요청 ID·JSON 오류, 명시적 신뢰 프록시, query·form·cookie·multipart 파싱 |
| 게임 통신 | 길이 프레임, JSON 봉투 또는 바이너리, 세션·디스패치, 준비된 메시지 재사용, 토큰·순번 기반 UDP |
| 실행 | 작업·바이트 상한, 협력적 취소·마감 시간, 키별 직렬 실행, 타이머·작업 그룹, FIFO·최신 값 채널 |
| 게임 실행 | 논리 틱·지연·catch-up 제한, 세션 작업 복귀, 송신 전 FIFO·최신 값·만료·배치 처리 |
| 파일·관측 | 제한된 바이너리 입출력, 임시 파일·원자적 교체, 비동기 콘솔·회전 로그, 메트릭·Prometheus 변환·drain 상태 |
| 언어 연동 | 네이티브 C++ API, 불투명 handle 기반 C ABI 1, Rust 소유 객체와 표준 `Future` |

외부 HTTP 클라이언트, TCP outbound connect, DNS 해석, TLS/DTLS, HTTP/2·HTTP/3, 압축·암호화, macOS 백엔드는 포함하지 않습니다. TLS 종료나 외부 서비스 호출은 소비 애플리케이션의 구성 요소로 연결합니다.

프로세스 실행·플러그인 발견·복구, DB·계정 인증, 방·매치메이킹·AOI·월드 저장, 게임 스키마, UI·VRM 검증과 자동 배포도 애플리케이션 책임입니다. 라이브러리는 서버의 `main`이나 특정 게임 규칙을 소유하지 않습니다.

내부 계층은 [공개 헤더](include/ServerCore)와 [구현](src)에서 Core·Net·Protocol·Session·Dispatch·Runtime·Web·Observability·C로 구분합니다. Runtime의 Host가 모듈을 조립하고 Web은 Net과 공통 실행기를 사용합니다. C 어댑터는 네이티브 API 위에 놓입니다. 이 구분은 소스 의존 방향이며 계층마다 DLL을 만들지 않습니다.

## 빌드와 테스트

CMake 3.21 이상과 C++23의 `std::expected`·`std::move_only_function`을 제공하는 컴파일러·표준 라이브러리가 필요합니다. 구성 단계에서 해당 기능을 검사합니다. 프리셋은 Ninja를 사용합니다.

Windows는 MSVC와 Windows SDK가 필요합니다. x64 Native Tools 개발 환경에서 저장소 루트에서 실행합니다.

```powershell
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

Ubuntu는 GCC 13 이상과 대응 libstdc++, CMake·Ninja를 준비한 뒤 실행합니다. GCC 버전 숫자뿐 아니라 위 표준 라이브러리 기능을 만족해야 합니다.

```bash
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

Debug는 `msvc-debug`·`linux-debug`, 공유 빌드는 `msvc-shared-release`·`linux-shared-release`를 사용합니다. 공유 Debug 프리셋도 있습니다. 기본 프리셋은 정적 C++ 빌드이며 C ABI가 필요하면 구성 명령에 `-DSERVERCORE_BUILD_C_API=ON`을 추가합니다.

| CMake 옵션 | 기본값 | 역할 |
| --- | --- | --- |
| `SERVERCORE_BUILD_SHARED` | `OFF` | 전체 라이브러리의 정적·공유 방식 선택 |
| `SERVERCORE_BUILD_C_API` | `OFF` | 같은 라이브러리에 C ABI 구현 추가 |
| `SERVERCORE_BUILD_TESTS` | 최상위 빌드에서 `ON` | C++·통합·패키지 소비 검사 빌드 |

두 API를 포함하는 Release 공유 라이브러리를 직접 구성하고 설치하는 예입니다. Windows에서는 같은 MSVC 개발 환경에서 실행합니다.

```sh
cmake -S . -B build/shared -G Ninja -DCMAKE_BUILD_TYPE=Release -DSERVERCORE_BUILD_SHARED=ON -DSERVERCORE_BUILD_C_API=ON
cmake --build build/shared
ctest --test-dir build/shared --output-on-failure
cmake --install build/shared --prefix stage/shared
```

| 산출물 | Windows Release / Debug | Linux Release / Debug |
| --- | --- | --- |
| 정적 아카이브 | `ServerCore.lib` / `ServerCore.lib` | `libServerCore.a` / `libServerCore.a` |
| 공유 런타임 | `ServerCore.dll` / `ServerCored.dll` | `libServerCore.so.0.2.0` / `libServerCored.so.0.2.0` |
| 공유 링크 파일 | import `ServerCore.lib` / `ServerCored.lib` | `libServerCore.so` / `libServerCored.so` |

정적·공유 및 Debug·Release에는 별도 빌드 디렉터리와 설치 prefix를 사용합니다. Windows의 정적 아카이브와 import library는 확장자가 같아 서로 대체할 수 없습니다. DLL은 설치 prefix의 `bin`, 링크 파일은 `lib`에 설치합니다. Linux SO는 `lib`에 설치합니다.

공유 배포에는 실행 파일과 대응 DLL/SO가 필요합니다. Windows에서는 DLL을 실행 파일 옆에 두거나 `PATH`로 찾게 하고, Linux에서는 RPATH나 운영체제 로더 경로를 설정합니다. 헤더·import library·CMake 파일은 개발용입니다. 정적 ServerCore를 사용해도 컴파일러·OS 런타임 의존성까지 없어지는 것은 아닙니다.

별도 `ServerCoreCAbi` 바이너리와 `SERVERCORE_C_API_SHARED` 옵션은 제거했습니다. 이전 옵션이 캐시에 남았다면 `cmake -U SERVERCORE_C_API_SHARED -S . -B <빌드 디렉터리>`로 제거하고 `SERVERCORE_BUILD_SHARED`를 지정합니다.

## C++에서 사용하기

소스를 함께 빌드하는 소비 프로젝트의 예입니다. `main.cpp`와 ServerCore 저장소 경로는 프로젝트에 맞춥니다.

```cmake
cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
project(MyServer LANGUAGES CXX)

add_subdirectory(../ServerCore servercore-build)
add_executable(MyServer main.cpp)
target_link_libraries(MyServer PRIVATE ServerCore::ServerCore)
if(MSVC)
    set_property(TARGET MyServer PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
```

공유 소스 빌드는 `add_subdirectory` 전에 `SERVERCORE_BUILD_SHARED=ON`을 지정합니다. 설치 패키지 소비는 위 `add_subdirectory`를 `find_package(ServerCore CONFIG REQUIRED)`로 바꾸고 구성 시 `-DCMAKE_PREFIX_PATH=<설치 prefix>`를 전달합니다. 두 방식 모두 `ServerCore::ServerCore`를 사용하며 C++23 조건이 전파됩니다.

네이티브 C++ 공유 API는 **같은 라이브러리 릴리스·헤더·아키텍처·컴파일러 툴셋·STL·CRT·빌드 구성**을 요구합니다. MSVC는 `/MD[d]`와 기본 iterator level(Debug 2, Release 0)을 사용합니다. 제공 타깃과 헤더의 ABI 검사를 제거하거나 소비자에서 `SERVERCORE_BUILDING_LIBRARY`를 정의하지 않습니다. C++ ABI는 툴체인·릴리스를 넘는 안정성을 보장하지 않으며 Windows DLL을 Ubuntu에서 재사용할 수 없습니다.

다음 Echo 서버는 `127.0.0.1:17891`에서 길이 프레임 프로토콜을 받습니다.

```cpp
#include <ServerCore/Runtime/ServerHost.h>
#include <iostream>
#include <memory>

int main()
{
    using namespace ServerCore;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = 17891;
    options.maxBodySize = 8192;

    auto status = host.Configure(options);
    if (!status.IsOk()) {
        std::cerr << status.Message() << '\n';
        return 1;
    }
    status = host.GetDispatcher().Register(
        "Echo",
        [](const std::shared_ptr<Session::Session>& session,
           const Protocol::Message& message) -> Core::Status {
            try {
                return session->Send(Protocol::MessageFields{
                    "EchoReply", message.Body(), message.Sequence(), nullptr });
            } catch (...) {
                return Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
            }
        },
        4096);
    if (!status.IsOk()) {
        std::cerr << status.Message() << '\n';
        return 1;
    }
    status = host.Start();
    if (!status.IsOk()) {
        std::cerr << status.Message() << '\n';
        return 1;
    }
    std::cout << "Listening on 127.0.0.1:17891. Press Enter to stop.\n";
    std::cin.get();
    host.Stop();
}
```

요청 예는 `{"type":"Echo","seq":1,"body":{"text":"hello"}}`입니다. TCP에서는 UTF-8 JSON 바이트 수를 나타내는 **4바이트 little-endian 길이**를 앞에 붙입니다. 위 `4096`은 Echo의 raw body 상한, `8192`는 전체 JSON 상한입니다. HTTP·WebSocket은 [Web::HttpServer](include/ServerCore/Web/HttpServer.h)로 별도 구성합니다.

## C에서 사용하기

`SERVERCORE_BUILD_C_API=ON`으로 만든 설치 패키지를 소비합니다. 공개 헤더는 [include/ServerCore/C](include/ServerCore/C)에 있습니다.

```cmake
cmake_minimum_required(VERSION 3.21)
project(MyCServer LANGUAGES C)
find_package(ServerCore CONFIG REQUIRED COMPONENTS CAbi)
add_executable(MyCServer main.c)
target_link_libraries(MyCServer PRIVATE ServerCore::CAbi)
```

공유 라이브러리는 위처럼 C++ 언어를 활성화하지 않고 소비할 수 있습니다. 정적 링크에는 C++ 링커·런타임과 플랫폼 라이브러리가 필요합니다. `ServerCore::CAbi`는 C 헤더·정의·링크 조건을 제공하는 INTERFACE 타깃이며 네이티브 C++ 컴파일 조건을 전파하지 않습니다. 실제 빌드 타깃은 `ServerCore`, 파일 경로 조회는 `$<TARGET_FILE:ServerCore::ServerCore>`를 사용합니다. `cmake --install ... --component CAbi`도 통합 라이브러리와 필요한 소비 설정을 설치합니다.

CMake 밖에서 Windows DLL을 사용할 때는 `SC_CABI_SHARED`를 정의합니다. C ABI는 불투명 handle·정수 상태·버전과 크기가 지정된 구조체를 사용하고 C++ 객체·예외·STL을 넘기지 않습니다. `sc_abi_version()`·`sc_capabilities()`로 지원을 확인하고, 반환 자원은 대응하는 `*_destroy`로 해제합니다. 상세 소유권·동시성 조건은 각 C 헤더에 명시합니다.

## Rust에서 사용하기

[rust](rust)의 `servercore-sys`는 C ABI 선언·빌드, `servercore`는 안전한 소유 객체와 표준 `Future`를 제공합니다. Rust 1.75 이상과 위 네이티브 빌드 도구가 필요합니다. 소비 프로젝트의 Cargo.toml에는 로컬 경로를 지정합니다.

```toml
[dependencies]
servercore = { path = "../ServerCore/rust/servercore" }
```

저장소 루트에서 예제를 실행할 수 있습니다.

```sh
cargo test --manifest-path rust/Cargo.toml --workspace --all-targets --locked --offline
cargo run --manifest-path rust/Cargo.toml --offline -p servercore --example http_echo -- 8080
```

`SERVERCORE_CABI_DIR`가 없으면 Cargo가 저장소 소스로 **C ABI를 포함한 Release 공유 ServerCore**를 빌드합니다. Cargo Debug에서도 네이티브 소스 빌드는 Release입니다. 소스 빌드에는 전체 저장소가 필요하고, crate만 복사한 경우에는 prebuilt 라이브러리를 지정합니다.

| 환경 변수 | 용도 |
| --- | --- |
| `SERVERCORE_CABI_DIR` | prebuilt 라이브러리의 `lib` 디렉터리. C ABI를 포함한 빌드여야 함 |
| `SERVERCORE_NATIVE_LIBRARY` | prebuilt basename `ServerCore` 또는 `ServerCored` 명시. 기본은 이 순서로 탐색 |
| `SERVERCORE_CABI_STATIC=1` | 소스·prebuilt 모두 정적 링크 선택 |
| `SERVERCORE_NATIVE_LIBS` | 정적 링크에 필요한 순서 있는 세미콜론 구분 라이브러리 목록 |
| `SERVERCORE_NATIVE_SEARCH` | 추가 네이티브 라이브러리 디렉터리, 세미콜론 구분 |
| `SERVERCORE_CMAKE_GENERATOR`, `SERVERCORE_CMAKE_TOOLCHAIN` | 소스 빌드 생성기·cross compile 도구 체인 |
| `SERVERCORE_CMAKE_JOBS` | 네이티브 빌드 병렬도, 기본 2 |

Windows prebuilt 공유 빌드는 import `.lib` 옆 또는 형제 `bin` 디렉터리에 대응 DLL을 둡니다. Cargo가 선택한 DLL 하나를 자체 출력에 준비하므로 `cargo run/test`에서 찾을 수 있습니다. 직접 배포한 실행 파일에는 앞 절의 런타임 배포가 필요합니다. Linux prebuilt 예:

```bash
export SERVERCORE_CABI_DIR=/absolute/path/to/servercore/lib
export LD_LIBRARY_PATH="$SERVERCORE_CABI_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cargo test --manifest-path rust/Cargo.toml --workspace --locked --offline
```

Windows MSVC 개발 PowerShell에서 정적 소스 빌드를 선택하는 예입니다.

```powershell
$env:SERVERCORE_CABI_DIR = $null
$env:SERVERCORE_CABI_STATIC = '1'
$env:SERVERCORE_NATIVE_LIBS = 'dylib=ws2_32;dylib=bcrypt'
$env:CARGO_TARGET_DIR = "$PWD/build/rust-static"
cargo build --manifest-path rust/Cargo.toml --workspace --examples --locked --offline
```

정적 prebuilt는 `SERVERCORE_CABI_DIR`에 `ServerCore.lib` 또는 `libServerCore.a`가 있는 디렉터리를 지정하고 같은 정적 설정을 유지합니다. GNU/Linux에서는 보통 `SERVERCORE_NATIVE_LIBS='dylib=stdc++'`가 필요하며 실제 툴체인의 런타임·링커에 맞춥니다. 목록에 ServerCore 아카이브를 다시 넣지 않습니다. MSVC 네이티브 빌드는 `/MD`이므로 Rust의 `+crt-static`을 추가하는 방식은 지원하지 않습니다. 공유로 돌아갈 때 정적 관련 변수를 해제하고, 링크 방식 변경에는 별도 Cargo 출력 디렉터리를 사용합니다.

HTTP·WebSocket·raw TCP·UDP와 공통 실행·파일·관측을 감싸지만 C++ `ServerHost`·세션 등록표·JSON 디스패처 전체를 매핑하지는 않습니다. raw TCP 바이트에는 자동 프레이밍이 없습니다. 실행기와 애플리케이션 프로토콜은 소비자가 선택합니다. [Rust 예제](rust/servercore/examples)는 `http_echo`, `websocket_echo`, `tcp_echo`, `multipart_upload`, `runtime_channels`입니다.

## 수명·흐름 제어·종료

- 처리기·라우트·정책·로거는 시작 전에 등록합니다. 요청에서 빌린 참조는 소유 요청·이벤트보다 오래 보관하지 않습니다.
- 송신 성공은 로컬 큐 수락입니다. `WouldBlock`이면 용량 알림이나 비동기 송신을 사용하고 제한된 조각으로 보냅니다. 수신 완료나 원격 처리 완료를 뜻하지 않습니다.
- 큐는 작업 수·바이트 상한을 적용합니다. 소비자가 별도로 복사해 보관하는 데이터는 애플리케이션의 메모리 예산으로 관리합니다.
- 작업 취소는 협력적이며 실행 중인 사용자 코드를 강제로 중단하지 않습니다. Rust의 불완전한 응답·연결 handle 해제는 해당 취소·종료 계약에 연결됩니다.
- `BeginDrain/DrainStatus/StopGracefully`는 새 수락을 막고 진행 중 작업의 완료를 기다립니다. 기한 후 남은 연결을 취소하며 일반 `Stop`은 즉시 종료 경로입니다.
- 종료·객체 해제는 worker join 때문에 대기할 수 있습니다. 서버가 소유한 worker·처리기에서 자기 스레드를 기다리는 `Stop`을 호출하지 않습니다. `JobRunner`는 실행 스레드를 소유하지 않으므로 `RequestStop` 후 호출자가 join합니다.
- 실행 중 DLL/SO 교체·언로드는 지원하지 않습니다. 로거·메트릭은 인스턴스별이며 기본 로거는 출력을 버립니다.

## 최근 검증

2026-09-24 Windows x64, MSVC 19.50.35728·CMake 4.2.3 기준입니다.

| 구성 | 결과 |
| --- | --- |
| 공유 Debug / Release | 각각 CTest 278/278 통과 |
| 정적 Debug | CTest 276/276 통과 |
| Rust 1.89: 공유 소스 / 공유 Debug prebuilt / 정적 소스 | 각 42/42 통과, 예제 5개 컴파일·링크 |
| 위 Rust 세 구성의 `runtime_channels` | `ready` 출력 후 정상 종료 |

설치·소스 소비, 순수 C 공유 소비, 네이티브 C++ ABI 불일치 거절, 통합 DLL의 C++·C 심벌과 실행 파일 의존성을 확인했습니다. 최신 통합 변경의 **Linux/GCC·ASan/UBSan 실행 검증은 하지 않았습니다**. Linux 구현과 CI 구성이 있다는 사실을 최신 Linux 실행 성공으로 해석하지 않습니다.

테스트는 [tests](tests), 빌드 자동화는 [scripts](scripts)에 있습니다. 회귀 테스트와 설정상 연결 상한은 실제 동시 플레이 수·처리량을 보장하지 않습니다. 소비 서버의 메시지 크기·빈도·방송 대상 수·처리기 비용을 반영한 부하에서 지연·대기열·자원 사용량을 측정합니다.
