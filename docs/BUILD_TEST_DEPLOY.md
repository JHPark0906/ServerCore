# 빌드·테스트·배포

ServerCore는 게임 서버가 링크하는 **Windows용 C++20 정적 라이브러리**다. 이 저장소를 빌드하면 `ServerCore.lib`와 선택적인 샘플·검사 실행 파일이 만들어진다. 실제 게임의 메시지 처리, 실행 진입점과 운영 설정은 ServerCore를 소비하는 서버 프로젝트가 제공한다.

아래 명령은 별도 설명이 없으면 저장소 루트에서 실행한다. 빌드 설정의 원본은 [CMakeLists.txt](../CMakeLists.txt), 프리셋은 [CMakePresets.json](../CMakePresets.json)이다. 런타임 구조와 지원 범위는 [아키텍처](ARCHITECTURE.md), [지원 범위와 제한](SUPPORT_AND_LIMITS.md)을 참고한다.

## 1. 준비할 도구

| 항목 | 현재 요구 사항 |
| --- | --- |
| 운영체제 | Windows. 네트워크 구현은 Winsock/IOCP를 사용하며 설치 패키지도 Windows 밖의 소비를 거절한다. |
| 컴파일러 | C++20을 지원하는 MSVC와 Windows SDK. Visual Studio 또는 Build Tools의 C++ 빌드 도구가 필요하다. |
| 아키텍처 | 이 문서와 검증 스크립트는 x64 환경을 사용한다. 프리셋 자체가 `-A x64`를 지정하는 것은 아니다. |
| CMake | 3.21 이상. `PROJECT_IS_TOP_LEVEL`과 버전 3 프리셋을 사용한다. |
| Ninja | 기본 `msvc-debug`, `msvc-release` 프리셋의 생성기다. |
| PowerShell | `scripts/VerifyBuild.ps1` 사용 시 5.1 이상. 직접 CMake 명령만 사용하는 빌드에는 필요하지 않다. |

서드파티 패키지 설치 단계는 없다. CMake에서 vcpkg·Conan·FetchContent를 사용하지 않으며, 검사도 저장소의 작은 C++ 하네스를 사용한다. Windows 시스템 라이브러리 `ws2_32`는 `ServerCore::ServerCore`를 통해 최종 실행 파일에 전이된다. 다른 엔진·게임 소스 트리는 빌드 입력으로 요구하지 않는다.

직접 프리셋을 쓸 때는 **x64 Native Tools Command Prompt** 또는 같은 MSVC x64 환경을 가져온 PowerShell에서 시작한다. Ninja 프리셋은 `CMAKE_CXX_COMPILER=cl`만 지정하므로 일반 터미널에서 `cl.exe`나 SDK 도구를 찾지 못할 수 있다. 현재 터미널의 `cl`, `cmake --version`, `ninja --version`으로 선택한 도구를 확인한다.

라이브러리는 MSVC에서 `/W4 /WX /permissive- /utf-8 /EHsc`로 컴파일한다. `/WX`는 라이브러리의 경고를 빌드 실패로 취급한다. 테스트와 샘플도 `/W4 /permissive- /utf-8 /EHsc`를 사용하지만 현재 각 대상에 `/WX`를 직접 설정하지는 않는다.

## 2. 기본 빌드: Ninja Debug·Release

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug

cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

각 구성에서 configure와 build가 성공한 뒤에만 다음 단계로 진행한다. CTest는 기존 실행 파일을 실행할 뿐 기본 빌드를 대신하지 않는다. 프리셋의 테스트 출력은 실패 시 상세 로그를 보여주도록 설정되어 있다.

Ninja는 이 프리셋에서 단일 구성 생성기다. Debug와 Release는 각각 다른 디렉터리를 사용하며, `--config Release`만 추가해서 Debug 프리셋을 Release로 바꾸는 방식으로 사용하지 않는다.

| 대상 | Debug 기본 출력 | Release 기본 출력 |
| --- | --- | --- |
| 정적 라이브러리 | `build/msvc-debug/ServerCore.lib` | `build/msvc-release/ServerCore.lib` |
| 검사 실행 파일 | `build/msvc-debug/tests/ServerCoreTests.exe` | `build/msvc-release/tests/ServerCoreTests.exe` |
| 최소 샘플 | `build/msvc-debug/samples/HelloServerCore.exe` | `build/msvc-release/samples/HelloServerCore.exe` |
| CTest 실행 로그 | `build/msvc-debug/Testing/Temporary/LastTest.log` | `build/msvc-release/Testing/Temporary/LastTest.log` |

최상위로 구성할 때 `SERVERCORE_BUILD_TESTS`와 `SERVERCORE_BUILD_SAMPLES`는 기본 ON이다. 라이브러리만 만들려면 별도 빌드 트리를 두거나 옵션을 명시한다.

```powershell
cmake -S . -B build/library-release -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release -DSERVERCORE_BUILD_TESTS=OFF -DSERVERCORE_BUILD_SAMPLES=OFF
cmake --build build/library-release --target ServerCore
```

옵션은 CMake 캐시에 남는다. 예전에 테스트를 끈 프리셋 디렉터리를 다시 사용하면 기본값 ON만으로 복구되지 않는다. 검사 대상을 되살릴 때는 해당 옵션을 `-DSERVERCORE_BUILD_TESTS=ON`으로 명시하여 다시 구성한다.

### Visual Studio 생성기를 쓰는 경우

Visual Studio 솔루션이 필요하면 Ninja 프리셋과 **다른 빌드 디렉터리**를 사용한다. 다음은 설치된 Visual Studio 2022를 사용하는 예다.

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Debug
ctest --test-dir build/vs2022 -C Debug --output-on-failure
cmake --build build/vs2022 --config Release
ctest --test-dir build/vs2022 -C Release --output-on-failure
```

다중 구성 생성기에서는 `--config`와 CTest의 `-C`로 구성을 지정한다. 출력은 `build/vs2022/Debug/ServerCore.lib`, `build/vs2022/tests/Debug/ServerCoreTests.exe`, `build/vs2022/samples/Debug/HelloServerCore.exe`처럼 구성 이름이 추가된다. Release도 같은 위치의 `Release` 디렉터리를 사용한다.

저장소의 최소 CMake 버전이 모든 신형 Visual Studio 생성기를 지원한다는 뜻은 아니다. 다른 Visual Studio 버전은 실제 `cmake --help`에 나타나는 생성기 이름과 그 버전을 지원하는 CMake를 사용한다. `VerifyBuild.ps1`은 이 Visual Studio 생성기 경로 대신 Ninja 프리셋을 사용한다.

## 3. 샘플이 확인하는 것

[HelloServerCore](../samples/HelloServerCore/main.cpp)는 `ServerCore::GetVersionString()`을 호출하고 `ServerCore <버전>`을 출력한 뒤 종료한다.

```powershell
.\build\msvc-debug\samples\HelloServerCore.exe
```

이 샘플의 목적은 시험 밖의 실행 파일이 공개 헤더와 라이브러리를 실제로 소비할 수 있는지 확인하는 것이다. TCP 포트를 열거나 Echo·Join 서버를 실행하지 않는다. 실제 전송과 서버 호스트 동작은 다음 절의 검사들이 담당한다. 게임 서버를 만들 때는 샘플 출력 프로그램에 접속하는 대신, 소비 프로젝트에서 `ServerHost`, `Dispatcher`와 세션 관찰자를 조립한다.

## 4. 검사 선택과 결과 확인

검사 목록과 CTest 등록은 [tests/CMakeLists.txt](../tests/CMakeLists.txt)에 있다. C++ 검사 하나를 CTest 한 개로 등록하므로 문제 영역을 이름으로 필터링할 수 있다.

```powershell
# 실행하지 않고 CTest에 등록된 목록 확인
ctest --preset msvc-debug -N

# 프레이밍·JSON만 실행
ctest --preset msvc-debug -R '^ServerCore\.Protocol\.'

# 실제 TCP 전송과 ServerHost 조립 검사
ctest --preset msvc-debug -R '^ServerCore\.(Transport\.|Runtime\.ServerHost)'

# 설치 패키지와 add_subdirectory 소비 검사
ctest --preset msvc-debug -R '^ServerCore\.CMake\.InstallAndSourceTreeConsume$'

# 검사 실행 파일의 내부 등록 목록과 단일 검사 직접 실행
.\build\msvc-debug\tests\ServerCoreTests.exe --list
.\build\msvc-debug\tests\ServerCoreTests.exe Protocol.MessagePreserves64BitIntegers
```

직접 실행할 때는 CTest가 덧붙이는 `ServerCore.` 접두사를 제외한 내부 검사 이름을 넘긴다. 인수 없이 실행하면 사용법을 출력하고 코드 2로 종료하며, 모든 검사를 자동 실행하지 않는다. `ServerCore.Harness.RegisteredChecksMatchCtest`는 C++ 등록표와 CMake의 검사 목록이 일치하는지 확인한다.

현재 검사가 다루는 축은 다음과 같다.

| 영역 | 대표 검증 |
| --- | --- |
| Core | 오류·결과 값, 버퍼 경계, 설정 파일과 UTF-8, 로깅, 단조 시각, 작업 큐 |
| Protocol | 공유 프레이밍 벡터, 부분 프레임과 크기 제한, JSON/UTF-8 오류, 64비트 정수, 봉투 검증 |
| Session·Dispatch | ID 발급·등록·해제, 관찰 중 제거, 등록 동결, 라우팅과 본문 제한, 알 수 없는 타입 정책 |
| Transport | 실제 loopback 통신, 분할 수신, 동시 송신, 송신 예산, close-after-send, IOCP worker와 종료 순서 |
| Runtime | JobRunner·PeriodicRunner, ServerHost 조립, 유휴·종료 기한, 파싱 순서, 공유 수신·파싱·송신 예산, 종료 fallback |
| CMake 소비 | 소스 트리 소비, 설치·이동한 패키지 소비, CRT와 CMP0091 계약 |

[PublicHeaderCompileCheck.cpp](../src/PublicHeaderCompileCheck.cpp)는 라이브러리에 항상 포함된다. 목록에 있는 공개 헤더를 한 번역 단위에서 함께 컴파일하므로 테스트를 끈 빌드에도 공개 선언 컴파일 확인이 남는다. 각 헤더를 독립된 번역 단위로 하나씩 검사하는 방식은 아니며, 새 공개 헤더는 이 파일의 목록에도 추가해야 한다.

### TCP 검사와 시간 제한

Transport·ServerHost 검사에는 CTest `TIMEOUT 60`과 `RESOURCE_LOCK ServerCore.TcpPorts`가 있다. 같은 CTest 실행 안에서는 해당 자원을 요구하는 검사끼리 겹치지 않는다. 별도 터미널의 다른 CTest 프로세스까지 잠그지는 않으므로 동일 빌드 트리나 같은 포트 블록을 사용하는 검사를 동시에 실행하지 않는다.

포트 기준값은 configure 시 `CMAKE_BUILD_TYPE`이 정확히 `Debug`이면 17000, 그 외에는 17050이다. 각 검사는 여기에 자신이 사용하는 offset을 더하고 `127.0.0.1`에서 통신한다. 따라서 기본 Ninja Debug·Release는 서로 다른 블록이지만, 일반적인 Visual Studio 다중 구성 트리는 `CMAKE_BUILD_TYPE`이 비어 있어 **Debug·Release 모두 17050 기준**을 쓴다. 다중 구성의 검증은 순차 실행한다.

60초는 정상이 걸리는 시간을 약속하는 값이 아니라 멈춘 검사를 종료할 상한이다. 특정 검사 이름이 마지막으로 남았는지, 포트를 다른 프로세스가 점유했는지, 구성과 실행 파일이 맞는지 확인한다. 회귀 검사는 장시간 부하나 최대 연결 수의 측정을 대신하지 않는다. 실제 처리량은 소비 서버의 메시지·방송 정책을 포함한 별도 부하에서 측정한다.

## 5. 검증 스크립트

[scripts/VerifyBuild.ps1](../scripts/VerifyBuild.ps1)은 MSVC 환경 준비부터 프리셋 configure → build → CTest까지 연결한다. 일반 PowerShell에서 실행할 수 있다.

```powershell
# 기본: Debug와 Release 모두 구성·빌드·검사
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/VerifyBuild.ps1

# 한 구성만 검사
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/VerifyBuild.ps1 -Configuration Release

# 해당 프리셋 빌드 트리를 삭제하고 다시 시작
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/VerifyBuild.ps1 -Configuration Debug -Clean

# 빌드만 확인: 검사 결과는 PASS 대신 SKIPPED
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/VerifyBuild.ps1 -SkipTests
```

`-VsInstallPath`로 Visual Studio 설치 루트를 명시할 수도 있다. 생략하면 `vswhere`로 C++ 도구가 있는 설치를 찾고, 경로에 `2022`가 있는 설치를 우선한다. 이후 `vcvars64.bat`의 x64 환경을 현재 프로세스로 가져온다. CMake·CTest·Ninja는 PATH에서 먼저 찾으며 일부 Visual Studio·CLion 번들 경로도 fallback으로 확인한다. 따라서 실제 선택된 CMake와 MSVC 툴셋은 스크립트 머리말을 기준으로 기록한다.

스크립트는 저장소 루트로 작업 디렉터리를 맞추고 configure 뒤 `CMakeCache.txt`의 `CMAKE_HOME_DIRECTORY`까지 확인한다. configure가 실패하면 build와 test를, build가 실패하면 test를 건너뛰어 오래된 바이너리가 대신 통과하지 않도록 한다. `-Clean`은 선택한 `build/msvc-debug` 또는 `build/msvc-release` 트리를 삭제하므로 그 안의 로그·임시 설치 결과도 다시 만들어진다.

결과는 종료 코드와 `PASS / FAIL / SKIPPED` 요약을 함께 읽는다. FAIL이 있으면 코드 1이지만, `-SkipTests`나 CTest 탐색 실패로 검사 단계가 SKIPPED여도 현재 스크립트는 코드 0을 반환한다. **종료 코드 0만으로 두 구성의 전체 검사 통과를 판정하지 않는다.** Git 커밋 번호가 찍혀도 미커밋 변경이 없는지까지 보증하는 것은 아니므로 검증에 사용한 소스 상태도 함께 기록한다.

### 설치·소스 소비 회귀의 내용

`ServerCore.CMake.InstallAndSourceTreeConsume`은 [CMakePackageTest.cmake](../tests/CMakePackageTest.cmake)를 실행하며 180초 제한이 있다. 일반 C++ 검사와 달리 이 검사는 내부에서 작은 별도 프로젝트를 구성·빌드·실행한다.

1. `add_subdirectory()`로 ServerCore를 포함한 소비자를 구성·빌드·실행한다. 테스트·샘플이 부모에 기본으로 딸려오지 않는지도 확인한다.
2. 현재 빌드의 설치 결과를 전용 prefix에 만들고 다른 경로로 이동한다.
3. 이동한 prefix만 `find_package(... NO_DEFAULT_PATH)`로 찾아 소비자를 구성·빌드·실행한다. 개발기에 남아 있는 다른 설치본으로 우연히 통과하는 것을 방지한다.
4. MSVC에서는 CXX 활성화 이전에 CMP0091을 NEW로 선택하지 않은 오래된 부모가 명확한 오류로 거절되는지 검사한다.
5. producer의 전역 CRT 기본값을 `/MT[d]`로 주어도 ServerCore 대상이 `/MD[d]`를 유지하는지 확인한다. 일치하는 소비자는 실행하고, 불일치하는 소비자는 단순 실패뿐 아니라 `LNK2038`와 `RuntimeLibrary` 진단까지 확인한다.

중간 파일은 현재 빌드 트리의 `tests/PackageConsumer` 아래에 두며 매회 해당 전용 디렉터리를 다시 준비한다. 같은 트리에서 이 검사를 중복 실행하지 않는다. 검사 소비자의 C++20 코드, 공개 헤더, 버전 함수와 잘못된 endpoint의 오류 반환을 통해 target의 사용 요구 사항과 실제 링크를 확인한다. 설치 압축 파일이나 운영 서버 설치 프로그램을 생성하는 검사는 아니다.

## 6. 다른 프로젝트에서 소스 트리로 소비하기

예를 들어 다음처럼 프로젝트를 배치한다.

```text
workspace/
  ServerCore/
  MyServer/
    CMakeLists.txt
    main.cpp
```

`MyServer/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
project(MyServer LANGUAGES CXX)

set(SERVERCORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ServerCore"
    CACHE PATH "ServerCore source tree")
set(SERVERCORE_BUILD_TESTS OFF CACHE BOOL "Build ServerCore tests")
set(SERVERCORE_BUILD_SAMPLES OFF CACHE BOOL "Build ServerCore samples")
add_subdirectory("${SERVERCORE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ServerCore")

add_executable(MyServer main.cpp)
target_link_libraries(MyServer PRIVATE ServerCore::ServerCore)
if(MSVC)
    set_property(TARGET MyServer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
```

두 소비 예제에서 사용할 최소 `main.cpp`:

```cpp
#include "ServerCore/Core/Version.h"
#include <iostream>

int main()
{
    std::cout << "ServerCore " << ServerCore::GetVersionString() << '\n';
    return 0;
}
```

MSVC x64 환경에서 `MyServer` 디렉터리로 이동한 뒤 실행한다.

```powershell
cmake -S . -B build/debug -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
.\build\debug\MyServer.exe
```

`ServerCore::ServerCore`를 링크하면 공개 include 경로, C++20 기능 요구, `ws2_32`, MSVC `/utf-8`이 전이된다. `src` 내부 include 경로와 내부 테스트 hook 정의는 전이되지 않는다. 소스 트리 포함 시 테스트·샘플의 기본값은 OFF지만 기존 CMake 캐시에 이미 다른 값이 있으면 그 값이 유지된다. 예제의 `CACHE` 설정도 사용자의 기존 값을 강제로 덮어쓰지 않는다.

## 7. 설치 패키지로 소비하기

설치 대상은 공개 헤더, 정적 라이브러리, CMake package 파일과 MIT-0 라이선스다. 기본 설치 레이아웃은 다음과 같다. `GNUInstallDirs` 변수를 바꾸면 경로도 바뀐다.

```text
<prefix>/
  include/ServerCore/...
  lib/ServerCore.lib
  lib/cmake/ServerCore/
    ServerCoreConfig.cmake
    ServerCoreConfigVersion.cmake
    ServerCoreTargets.cmake
    ServerCoreTargets-<configuration>.cmake
  share/doc/ServerCore/LICENSE
```

배포용 설치본은 내부 테스트 hook이 필요하지 않으므로 tests와 samples를 끈 별도 트리에서 만든다. ServerCore 저장소 루트에서:

```powershell
cmake -S . -B build/package-release -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release -DSERVERCORE_BUILD_TESTS=OFF -DSERVERCORE_BUILD_SAMPLES=OFF
cmake --build build/package-release --target ServerCore
$serverCoreInstallRelease = Join-Path (Get-Location).Path out/install-release
cmake --install build/package-release --prefix "$serverCoreInstallRelease"

cmake -S . -B build/package-debug -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Debug -DSERVERCORE_BUILD_TESTS=OFF -DSERVERCORE_BUILD_SAMPLES=OFF
cmake --build build/package-debug --target ServerCore
$serverCoreInstallDebug = Join-Path (Get-Location).Path out/install-debug
cmake --install build/package-debug --prefix "$serverCoreInstallDebug"
```

Debug와 Release는 **다른 prefix에 설치한다.** 현재 라이브러리에 Debug 접미사나 구성별 `lib` 하위 경로가 없어 둘 다 `lib/ServerCore.lib`로 설치된다. 같은 prefix에 순서대로 설치하면 라이브러리가 덮어써져 구성별 export 파일과 실제 바이너리가 맞지 않을 수 있다.

앞 절과 같은 `MyServer/main.cpp`를 두고, 이번에는 `MyServer/CMakeLists.txt`를 다음으로 사용한다.

```cmake
cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
project(MyServer LANGUAGES CXX)

find_package(ServerCore CONFIG REQUIRED)
add_executable(MyServer main.cpp)
target_link_libraries(MyServer PRIVATE ServerCore::ServerCore)
if(MSVC)
    set_property(TARGET MyServer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
```

소스 트리 예제와 같은 이웃 디렉터리 배치에서 `MyServer` 루트의 PowerShell로 다음을 실행한다. 설치 위치를 먼저 해석해 CMake에 넘기므로 상대 경로 해석이 빌드 디렉터리와 섞이지 않는다.

```powershell
$serverCorePrefix = (Resolve-Path ../ServerCore/out/install-release).Path
cmake -S . -B build/release -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$serverCorePrefix"
cmake --build build/release
.\build\release\MyServer.exe
```

`CMAKE_PREFIX_PATH`에는 `include`나 `.lib`가 아닌 설치 prefix를 넘긴다. 대안으로 `ServerCore_DIR`에 `lib/cmake/ServerCore`를 지정할 수 있다. Debug 소비자는 Debug 빌드와 `out/install-debug`를 짝지어 사용한다.

현재 package 버전은 CMake 프로젝트의 `0.1.0`이며 버전 파일은 `SameMajorVersion` 규칙을 사용한다. 특정 산출물이 필요하면 `find_package(ServerCore 0.1.0 EXACT CONFIG REQUIRED)`로 고정할 수 있다. 이 버전 선택 규칙이 임의의 컴파일러·CRT·표준 라이브러리 ABI 호환을 보증하는 것은 아니다.

## 8. CRT·ABI와 실제 배포 경계

ServerCore는 정적 라이브러리이지만 MSVC CRT는 Debug에서 `/MDd`, 그 밖의 구성에서 `/MD`를 선택한다. **정적 라이브러리와 정적 CRT는 다른 선택**이다. 기본 빌드에서 `ServerCore.dll`은 만들어지지 않으며 ServerCore 코드는 소비 실행 파일에 링크된다. 그래도 `/MD` 실행 파일에는 그 도구 집합에 맞는 동적 MSVC 런타임이 실행 환경에 있어야 한다.

공개 API에서 `std::string`, `std::function`, `std::shared_ptr`와 컨테이너 등 C++ 객체를 주고받는다. 소비자와 라이브러리는 아키텍처, Debug/Release, CRT, 컴파일러·표준 라이브러리 ABI를 일치시켜 사용한다. 임의의 바이너리 ABI 호환 계층이나 C API가 있는 패키지는 아니다. 소비자를 `/MT[d]`로 바꿔 연결하면 현재 MSVC 소비 회귀가 기대하는 `LNK2038 RuntimeLibrary` 불일치 대상이 된다. 오류를 링크 옵션으로 감추기보다 양쪽 빌드 설정을 맞춘다.

`MSVC_RUNTIME_LIBRARY`가 작동하려면 MSVC ABI 언어를 처음 활성화하는 `project()` 또는 `enable_language()` **이전**에 CMP0091이 NEW여야 한다. ServerCore 하위 디렉터리에 들어온 뒤 정책을 바꾸는 것만으로 이미 활성화된 부모 언어 설정을 되돌릴 수 없다. 소스 소비 예제에서 정책 줄을 부모의 `project()` 위에 둔 이유다. ServerCore는 자기 대상의 CRT를 정하지만 소비 실행 파일의 CRT까지 자동으로 강제하지는 않는다.

개발용으로 설치 패키지를 전달할 때는 `include`, `lib`, `lib/cmake/ServerCore`를 함께 보관한다. 실행 사용자에게 배포할 때는 소비 프로젝트가 만든 Release 실행 파일과 그 프로젝트의 필요한 설정·데이터 및 런타임 의존성을 기준으로 구성한다. CMake의 `cmake --install`은 ServerCore 개발 패키지를 설치할 뿐 게임 서버 실행 파일, Windows 서비스 등록, 방화벽 규칙, 인증서, 런타임 설치 프로그램을 함께 만들지 않는다. 현재 저장소에는 CPack 기반 배포 압축/설치 프로그램 생성 단계도 없다.

`build/`, `out/`, `cmake-build-*` 및 일반 바이너리·로그는 [.gitignore](../.gitignore)에 제외되어 있다. Git에 소스를 올리는 것과 빌드 산출물을 배포하는 것은 별도 작업이다. 검증 기록에는 구성·툴체인·소스 상태와 실제 실행한 검사 범위를 남긴다.

## 9. 문제 해결

| 증상 | 확인할 내용 |
| --- | --- |
| `cl.exe`를 찾지 못하거나 C++ 컴파일러 검사 실패 | x64 MSVC 개발 환경에서 실행했는지, C++ 도구와 Windows SDK가 설치되었는지 확인한다. 일반 PowerShell에서는 검증 스크립트의 환경 준비 경로를 사용할 수 있다. |
| Ninja 생성기를 찾지 못함 | Ninja가 PATH에 있는지 확인한다. 검증 스크립트는 찾은 Ninja를 `CMAKE_MAKE_PROGRAM`으로 넘긴다. |
| 요청한 Visual Studio 생성기가 없음 | 현재 `cmake --help`의 생성기 목록과 설치된 Visual Studio 버전을 맞춘다. 최소 CMake 3.21만으로 모든 새 Visual Studio 지원을 가정하지 않는다. |
| 기존 캐시와 생성기·컴파일러가 다름 | 같은 build 디렉터리에 Ninja와 Visual Studio 설정을 섞지 않는다. 별도 디렉터리에서 새로 구성한다. |
| `ServerCore requires CMP0091 NEW` | 부모의 첫 CXX 활성화 이전으로 정책 설정을 옮긴다. ServerCore 아래에서만 설정하지 않는다. |
| `LNK2038`의 `RuntimeLibrary` 불일치 | `/MDd`·`/MD`와 Debug·Release 조합을 확인한다. 같은 prefix에 다른 구성의 `.lib`를 덮어쓰지 않았는지도 확인한다. |
| `find_package`가 실패하거나 예상 밖 설치본을 찾음 | 설치 prefix와 `ServerCore_DIR`, 캐시의 기존 탐색 결과를 확인한다. 설치 패키지는 Windows용이며 헤더만 복사한 디렉터리는 완전한 패키지가 아니다. |
| CTest 목록이 비어 있음 | `SERVERCORE_BUILD_TESTS=ON`으로 구성했는지 확인한다. 최상위가 아닌 소비 프로젝트에서는 기본 OFF다. |
| 검사 이름을 직접 실행했는데 찾지 못함 | `ServerCoreTests.exe --list`의 내부 이름을 사용한다. CTest 접두사 `ServerCore.`는 직접 실행 인수에서 제외한다. |
| TCP 검사의 bind 실패·시간 초과 | 다른 프로세스의 포트 점유와 중복 CTest 실행을 확인한다. 다중 구성의 Debug·Release는 기본 포트 기준값을 공유한다. |
| 설치 소비 회귀가 컴파일러·SDK 탐색에서 실패 | 이 검사는 내부에서 새로운 프로젝트를 빌드한다. CTest만 일반 터미널에서 실행하여 MSVC 환경을 잃지 않았는지 확인한다. |
| 검증 스크립트 종료 코드는 0인데 전체 검사를 확인할 수 없음 | 요약에 SKIPPED가 있는지 확인한다. `-SkipTests` 또는 CTest 탐색 실패는 현재 코드 0과 함께 SKIPPED를 남길 수 있다. |
| 한글 소스가 깨지거나 MSVC 문자 인코딩 경고 | target의 `/utf-8` 설정과 원본 파일 인코딩을 확인한다. ServerCore의 `/utf-8`은 소비 대상에도 전이된다. |

프로토콜 호환성 문제는 [프레이밍·메시지 문서](PROTOCOL.md), 서버의 메모리 예산과 동시성 제한은 [지원 범위와 제한](SUPPORT_AND_LIMITS.md)에서 이어서 확인한다.
