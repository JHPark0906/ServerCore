# ServerCore

Windows 게임 서버의 구조와 구현을 학습하기 위한 **C++20 서버 코어 라이브러리**입니다. TCP/IOCP와 비차단 UDP 전송, 메시지 프레이밍과 JSON, 세션, 디스패치, 작업 실행과 자원 예산을 제공하며, 게임별 백엔드가 그 위에 자신의 규칙을 구현합니다.

개발 과정에서 생성형 AI의 도움을 받은 프로젝트입니다.

현재 라이브러리 버전은 `0.1.0`입니다. 다른 엔진·게임 저장소 없이 빌드할 수 있는 정적 라이브러리이며, 특정 게임의 규칙이나 게임 서버 진입점을 포함하지 않습니다. 프로젝트 코드는 [MIT-0](LICENSE) 라이선스로 제공합니다.

## 문서 안내

| 문서 | 내용 |
| --- | --- |
| [아키텍처](docs/ARCHITECTURE.md) | 모듈 경계, 소유권, 부팅·메시지·종료 흐름 |
| [프로토콜](docs/PROTOCOL.md) | 프레임 형식, JSON 봉투, 디스패치와 오류 계약 |
| [빌드·테스트·배포](docs/BUILD_TEST_DEPLOY.md) | 요구 사항, CMake, 소스·설치 패키지 소비, 검증 |
| [지원 범위와 설정](docs/SUPPORT_AND_LIMITS.md) | 기본값, 설정 파일, 자원 상한, 미지원 기능 |
| [검증 결과](docs/VALIDATION.md) | 검증한 소스·도구 환경, 빌드와 테스트 결과 |

## 핵심 기능

- **비동기 TCP 전송:** Windows IOCP, AcceptEx, 겹침 수신·송신, 부분 송신 처리와 연결 수명 관리.
- **스트림 프레이밍:** 4바이트 길이 머리와 UTF-8 JSON 본문. 나뉘어 도착하거나 연속으로 도착한 프레임을 처리합니다.
- **공용 메시지 봉투:** `type`, `body`, 선택적 `seq`와 `error`를 파싱·직렬화합니다. UTF-8, JSON 값, 본문 형식과 크기를 검증합니다.
- **준비된 송신 값:** `PreparedJsonValue`와 `PreparedMessage`를 소유 값으로 만들고 여러 수신자에게 재사용합니다. 실제 NetworkSession의 `SendPrepared`는 JSON을 다시 직렬화하지 않고 기존 프레임·송신 큐에 넣습니다.
- **비차단 UDP 전송:** `Runtime::DatagramTransport`가 소켓, 세션별 토큰, 순번·재전송 입력 거절, endpoint 재바인딩과 제한된 수신 pump를 제공합니다. 공유 `DatagramCodec`의 최대 1,200바이트 형식을 사용하며 게임 메시지 정책은 소비자가 정합니다.
- **세션과 처리기:** 세션 ID 발급, 연결·인증·종료 상태, 타입별 처리기와 본문 크기 제한, 시작 전 등록표 동결.
- **실행 문맥:** 게임 처리를 직렬화하는 JobRunner와 PeriodicRunner. 선택적으로 JSON 파싱만 별도 worker에서 처리하며 같은 세션의 순서를 유지합니다.
- **자원 제한:** 연결 수, 프레임 크기, 수신·파싱·송신 대기량에 상한을 적용합니다. 유휴 세션과 마지막 송신을 기다리는 세션의 종료 기한도 설정할 수 있습니다.
- **관측과 실패 처리:** `Status`/`Result<T>`, 주입 가능한 로거, 활성 세션·프레임 수·대기량 스냅숏을 제공합니다.
- **라이브러리 소비:** 소스 트리의 `add_subdirectory`와 설치 패키지의 `find_package`에서 같은 `ServerCore::ServerCore` 타깃을 사용합니다.

서버 프로세스의 `main`, 주소·포트의 명령행 옵션, 플레이어 목록과 방, 입장 조건, 게임 메시지의 스키마는 소비 프로젝트가 정합니다. 아래 예제에서는 작은 Echo 서버로 이 경계를 보여 줍니다.

## 저장소 구조

| 위치 | 책임 |
| --- | --- |
| [include/ServerCore](include/ServerCore) | 소비자가 포함하는 공개 API. Core·Net·Protocol·Session·Dispatch·Runtime으로 구분 |
| [src](src) | 공개 API 구현과 내부 전송·파싱 상태. Windows 헤더와 소켓 구현을 내부에 둠 |
| [tests](tests) | C++ 회귀, 실제 TCP·UDP 통합과 소스/설치 패키지 소비 검사 |
| [cmake](cmake) | 설치 패키지의 Config/Targets 구성 |
| [scripts](scripts) | 개발 빌드 검증 스크립트 |
| [docs](docs) | 프로토콜·설정·빌드 계약 |

공개 API에서 실행 계층까지는 `ServerHost → FrameReader/ParseMessage → Dispatcher → 게임 처리기 → Session` 순서로 읽을 수 있습니다. IOCP와 JobRunner의 실제 스레드·수명 관계는 [아키텍처](docs/ARCHITECTURE.md)에 설명합니다.

## 빠른 시작

Windows, C++20을 지원하는 MSVC와 Windows SDK, CMake 3.21 이상, Ninja가 필요합니다. **x64 Native Tools Command Prompt** 또는 같은 도구 환경을 설정한 PowerShell에서 저장소 루트로 이동하여 실행합니다.

```powershell
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

Debug는 위 세 명령의 preset 이름을 `msvc-debug`로 바꿉니다. 개발 환경 설정, 전체 검증 스크립트, 출력 경로와 패키지 설치는 [빌드 문서](docs/BUILD_TEST_DEPLOY.md)를 참고합니다.

## 서버 프로젝트에서 사용하기

다른 저장소에서 소스를 함께 빌드하려면 다음처럼 연결합니다. 예시는 두 저장소가 형제 디렉터리에 있고 소비 프로젝트에 `main.cpp`가 있는 경우입니다. MSVC의 CRT 선택 정책은 C++ 언어를 켜기 전에 설정합니다.

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

ServerCore를 설치한 뒤에는 `find_package(ServerCore CONFIG REQUIRED)`로 같은 타깃을 얻을 수 있습니다. 위 예의 `add_subdirectory`를 그 호출로 바꾸고 소비 프로젝트를 구성할 때 설치 prefix를 `CMAKE_PREFIX_PATH`로 전달합니다.

```powershell
# 앞 절의 Release 프리셋 빌드가 끝난 뒤, 저장소 루트에서 실행
cmake --install build/msvc-release --prefix stage/msvc-release
```

[CMake 소비 예제](docs/BUILD_TEST_DEPLOY.md)를 따라 `ServerCore::ServerCore`에 연결한 실행 타깃의 `main.cpp`를 다음처럼 작성할 수 있습니다. 이 예제는 `127.0.0.1:17891`을 열고, `Echo` 요청의 객체 본문과 `seq`를 `EchoReply`로 돌려줍니다. Enter를 누르면 메인 스레드에서 서버를 정리합니다.

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

    const Core::Status configured = host.Configure(options);
    if (!configured.IsOk())
    {
        std::cerr << configured.Message() << '\n';
        return 1;
    }

    const Core::Status registered = host.GetDispatcher().Register(
        "Echo",
        [](const std::shared_ptr<Session::Session>& session,
           const Protocol::Message& message) -> Core::Status
        {
            try
            {
                return session->Send(Protocol::MessageFields{
                    "EchoReply", message.Body(), message.Sequence(), nullptr });
            }
            catch (...)
            {
                return Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
            }
        },
        4096);
    if (!registered.IsOk())
    {
        std::cerr << registered.Message() << '\n';
        return 1;
    }

    const Core::Status started = host.Start();
    if (!started.IsOk())
    {
        std::cerr << started.Message() << '\n';
        return 1;
    }

    std::cout << "Listening on 127.0.0.1:17891. Press Enter to stop.\n";
    std::cin.get();
    host.Stop();
    return 0;
}
```

송신할 JSON은 예를 들어 `{"type":"Echo","seq":1,"body":{"text":"hello"}}`입니다. 실제 TCP 전송에서는 이 문자열 앞에 **JSON 바이트 수를 나타내는 4바이트 little-endian 길이**를 붙입니다. 일반 텍스트나 HTTP 요청을 그대로 보내는 형식이 아닙니다. 세부 규격과 인코딩 API는 [프로토콜 문서](docs/PROTOCOL.md)에 있습니다.

처리기는 Start 전에 등록합니다. 이 예제의 `4096`은 Echo의 **raw body** 상한이고, `8192`는 봉투 전체 JSON의 상한입니다. 등록된 처리기에 들어오는 `Body()`는 객체이며, 요청에서 빌린 참조는 처리기 호출을 넘어 보관하지 않습니다. `Send` 성공은 송신 큐 수락을 뜻하며 상대의 수신 확인을 뜻하지 않습니다.

운영 로그가 필요하면 Start 전에 `host.SetLogger(...)`로 `Core::ILogger` 구현을 넣습니다. 기본 로거는 출력을 버립니다. 별도 관리 스레드에서 종료를 요청하는 프로그램은 `host.Run()`으로 메인 스레드를 대기시킬 수도 있습니다. Host가 소유한 worker나 메시지 처리기 안에서 `Stop()`으로 자기 스레드를 기다리면 안 됩니다.

## 설정과 관측

`ServerHostOptions` 또는 UTF-8 `key = value` 파일을 읽은 `Core::Config`로 서버를 구성합니다. 기본 주소는 `127.0.0.1`, 기본 연결 상한은 256이며 포트는 반드시 지정해야 합니다. 외부 인터페이스에 열려면 소비 프로그램이 `listenAddress`를 명시합니다.

설정상 연결 수는 최대 65,536까지 허용하지만 프레임 크기와 곱한 저장소 예산도 만족해야 합니다. **기본 64 KiB 본문 상한을 그대로 두고 연결 수만 65,536으로 올리면 Configure가 거절합니다.** 8 KiB 본문과 65,536 연결을 구성하는 예제는 [설정 문서](docs/SUPPORT_AND_LIMITS.md)에 있습니다.

`SnapshotMetrics()`는 JobRunner 문맥에서 호출합니다. 통계는 서로 다른 동기화 경계에서 읽은 관측값이며, 송신 성공·오류 0·빈 큐 중 어느 하나도 게임의 응답성이나 전체 메시지 전달을 보증하지 않습니다.

## 검증 범위

CTest는 기반 자료형, 설정, JSON·프레이밍, 세션, 디스패치, 작업 실행, 실제 TCP·UDP와 ServerHost의 수명·예산·종료 경로, CMake 소비를 검사합니다. 테스트 분류와 명령은 [빌드·테스트·배포](docs/BUILD_TEST_DEPLOY.md), 실행 환경과 결과는 [검증 결과](docs/VALIDATION.md)에 정리합니다.

회귀 테스트의 성공이나 설정상 연결 상한은 실제 게임의 동시 플레이 수와 처리량을 보장하지 않습니다. 성능을 판단할 때는 소비 서버의 메시지 크기·빈도·방송 대상 수·처리기 비용을 포함한 부하에서 지연 분포, 대기열과 자원 사용량을 함께 측정해야 합니다.

## TCP와 UDP의 책임 경계

ServerHost의 네트워크 세션은 **Windows IPv4 TCP/IOCP**입니다. 별도의 [Runtime::DatagramTransport](include/ServerCore/Runtime/DatagramTransport.h)는 비차단 IPv4 UDP 소켓과 세션별 토큰·순번·endpoint를 관리합니다. `Protocol/DatagramCodec.h`의 magic·128비트 토큰·big-endian 순번으로 된 28바이트 머리를 그대로 사용합니다.

소비자는 신뢰 가능한 제어 채널에서 `RegisterSession`의 토큰을 전달하고 세션 종료 시 `UnregisterSession`을 호출합니다. `Poll(admission, receiver)`는 JSON 봉투를 한 번 파싱하고 게임의 admission이 허용한 뒤에만 순번·endpoint·준비 상태를 갱신합니다. 콜백은 내부 잠금 밖에서 실행됩니다. 수신 호출은 소비자의 실행 문맥에서 직렬화하며 기본 한도는 호출당 4,096회 수신 시도와 약 1 MiB입니다. heartbeat, 유실 복구, AOI와 틱별 송신 예산은 게임이 정합니다.

`PreparedMessage::Size()`는 JSON 봉투 바이트만 나타내며, `PreparedJsonValue::Size()`는 항목 하나의 바이트 수입니다. 소비자가 전송량을 계산할 때 TCP 머리 4바이트 또는 DatagramCodec 머리 28바이트를 더해야 하며, 이는 IP·UDP/TCP 커널 헤더와 재전송 트래픽을 포함한 링크 대역폭과 다릅니다. 자세한 소유권·검증·기본 Session 위임 경로는 [프로토콜 문서](docs/PROTOCOL.md#61-준비된-json과-봉투-재사용)를 참고합니다.

## 지원 범위

현재 라이브러리는 TLS/DTLS, IPv6, DNS 연결, 자동 재접속, 계정 인증, DB, 매치메이킹, 서버 권위 물리, 방·관심 영역 분할을 제공하지 않습니다. 인증 상태 전이 API는 자격 증명을 검증하지 않습니다.

공개 헤더는 WinSock 구조체를 노출하지 않지만, 라이브러리 전체가 다른 운영체제에서 빌드되는 것은 아닙니다. MSVC에서는 라이브러리와 소비 실행 파일이 동일한 `/MD` 또는 `/MDd` CRT 계약을 따라야 합니다. 자세한 계약과 제약은 [지원 범위와 설정](docs/SUPPORT_AND_LIMITS.md)을 기준으로 확인합니다.

## 라이선스

프로젝트 코드는 [MIT No Attribution (MIT-0)](LICENSE) 라이선스를 따릅니다. 사용·수정·재배포·상업적 이용을 허용하며, 재배포 시 저작권 고지 유지나 수정한 소스 공개를 요구하지 않습니다. 보증과 책임에 관한 조건은 라이선스 원문을 따릅니다.
