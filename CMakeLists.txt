# ServerCore — 게임 서버 백엔드의 공용 코어 라이브러리.
# 각 게임 프로젝트가 이 라이브러리를 링크해 자신의 서버 백엔드를 그 위에 올린다.
#
# 최소 버전을 3.21로 두는 이유:
#   - PROJECT_IS_TOP_LEVEL(3.21)을 써서, 다른 프로젝트가 우리를 포함했을 때
#     시험이 그쪽 빌드에 딸려 들어가지 않게 한다.
#   - CMakePresets v3(3.21)를 쓴다.
cmake_minimum_required(VERSION 3.21)

# MSVC runtime target 속성은 언어를 켜기 전에 CMP0091이 NEW여야만 작동한다. 최상위
# ServerCore 구성에서는 아래 project()보다 먼저 이 선택을 명시한다.
cmake_policy(SET CMP0091 NEW)

project(ServerCore
        VERSION 0.1.0
        DESCRIPTION "게임 서버 백엔드용 공용 코어 라이브러리"
        LANGUAGES CXX)

# MSVC runtime target 속성은 MSVC ABI 언어를 처음 켤 때 CMP0091이 NEW여야만 작동한다.
# 이 트리가 최상위면 위의 명시적 정책 선택이 그 조건을 만들고, 부모가 먼저 CXX를 켠
# add_subdirectory 소비에서는 부모가 같은 정책을 먼저 골라야 한다. OLD 상태를 조용히
# 받아들이면 아래의 /MD[d] 계약이 적용되지 않으므로 즉시 원인을 알린다.
if (MSVC AND "${CMAKE_MSVC_RUNTIME_LIBRARY_DEFAULT}" STREQUAL "")
    message(FATAL_ERROR
            "ServerCore requires CMP0091 NEW before the first project() or enable_language() "
            "command that enables an MSVC-ABI language. Set cmake_policy(SET CMP0091 NEW) "
            "before the parent project's project() call.")
endif ()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 소비자(게임 프로젝트)가 우리를 add_subdirectory로 포함하면 시험은 꺼진다.
option(SERVERCORE_BUILD_TESTS   "ServerCore의 시험을 빌드한다"   ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# 라이브러리
# ---------------------------------------------------------------------------
# PublicHeaderCompileCheck.cpp는 공개 헤더 전부를 포함하는 번역 단위여서,
# 빌드의 성공이 곧 "공개 헤더 전부가 컴파일된다"의 증거가 된다.
# 새 공개 헤더를 더하면 그 파일에도 한 줄 더한다.
add_library(ServerCore STATIC
        src/Core/Assert.cpp
        src/Core/ByteBuffer.cpp
        src/Core/Clock.cpp
        src/Core/Config.cpp
        src/Core/Error.cpp
        src/Core/JobQueue.cpp
        src/Core/Logging.cpp
        src/Core/Utf8.cpp
        src/Core/Version.cpp
        src/Dispatch/Dispatcher.cpp
        src/Net/Acceptor.cpp
        src/Net/Connection.cpp
        src/Net/DatagramSocket.cpp
        src/Net/IoContext.cpp
        src/Net/WinsockInternal.cpp
        src/Protocol/Framing.cpp
        src/Protocol/Json.cpp
        src/Protocol/Message.cpp
        src/Runtime/DatagramTransport.cpp
        src/Runtime/JobRunner.cpp
        src/Runtime/ParseWorkerPool.cpp
        src/Runtime/PeriodicRunner.cpp
        src/Runtime/ServerHost.cpp
        src/Session/SessionRegistry.cpp
        src/PublicHeaderCompileCheck.cpp)

add_library(ServerCore::ServerCore ALIAS ServerCore)

target_include_directories(ServerCore
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        # 층 안에서만 쓰는 헤더는 src 아래에 있다. 소비자에게 보이면 안 되므로 PRIVATE이고,
        # 그래서 전송 층의 구현이 "Net/WinsockInternal.h"처럼 층 이름부터 적을 수 있다.
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 전송 층이 Winsock을 쓴다. 정적 라이브러리라 이 의존은 우리를 링크하는 쪽까지 따라가야
# 하므로 PRIVATE이 아니라 PUBLIC이다.
target_link_libraries(ServerCore PUBLIC ws2_32 PRIVATE bcrypt)

target_compile_features(ServerCore PUBLIC cxx_std_20)

if (MSVC)
    # ServerCore의 공개 API는 std::string, std::function, shared_ptr 등을 넘긴다. 따라서
    # 정적 라이브러리를 만든 CRT와 소비자가 쓰는 CRT는 같아야 한다. 위의 CMP0091 확인 뒤 이
    # target 속성은 Debug에서는 /MDd, 그 밖에서는 /MD를 만든다. 소비자 target까지 강제하지는
    # 않는다. 다른 CRT를 고르면 MSVC의 LNK2038 진단으로 링크를 막아, 라이브러리 경계를 넘는
    # 힙·표준 라이브러리 혼용을 조기에 발견하게 한다.
    set(ServerCoreMsvcRuntimeLibrary "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_property(TARGET ServerCore PROPERTY
            MSVC_RUNTIME_LIBRARY "${ServerCoreMsvcRuntimeLibrary}")

    # /utf-8 — 원본이 UTF-8이고 주석이 한국어다. 없으면 MSVC가 시스템 코드페이지로 읽어 깨진다.
    # /EHsc — 구현은 예외를 API 밖으로 내보내지 않지만, 자원 고갈을 Status로 바꾸기 위해 내부에서
    # catch를 쓴다. 예외 모델은 소비자에게 강제하지 않고, 이 target의 번역 단위에만 명시한다.
    # /permissive- — 표준 준수 모드.
    # /WX는 켜 둔다. 소비자 쪽 기준도 "/W4 경고 0"이고, "나중에 켜자"는 스위치는 켜지지 않는다.
    # 구현이 들어오면서 미사용 매개변수 경고(C4100) 같은 것이 걸리기 시작하면
    # 규칙으로 끄지 말고 그 자리에서 처리한다.
    target_compile_options(ServerCore PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_compile_options(ServerCore PUBLIC /utf-8)
else ()
    target_compile_options(ServerCore PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif ()

# 전역 parse 예산의 경쟁 검사는 예약을 잡은 worker를 결정적으로 멈춰야 한다. 이 내부 hook은
# 시험 target을 만들 때만 ServerCore 구현에 넣고, PUBLIC 사용 요구 사항이나 설치 패키지에는
# 전혀 내보내지 않는다.
if (SERVERCORE_BUILD_TESTS)
    target_compile_definitions(ServerCore PRIVATE SERVERCORE_ENABLE_TEST_HOOKS=1)
endif ()

# ---------------------------------------------------------------------------
# 설치 패키지
# ---------------------------------------------------------------------------
# 소스 트리 소비자는 add_subdirectory()와 위의 별칭을 쓰고, 설치한 라이브러리를 쓰는 서버는
# find_package(ServerCore CONFIG REQUIRED)로 같은 ServerCore::ServerCore를 얻는다. 두 경로가
# 다른 target 이름을 만들면 SummitServer와 다음 서버 프로젝트가 소비 방법마다 분기하게 된다.
set(ServerCoreCMakeInstallDir "${CMAKE_INSTALL_LIBDIR}/cmake/ServerCore")

configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ServerCoreConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ServerCoreConfig.cmake"
        INSTALL_DESTINATION "${ServerCoreCMakeInstallDir}")

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ServerCoreConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion)

install(TARGETS ServerCore
        EXPORT ServerCoreTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# include 아래는 라이브러리가 약속하는 공개 표면 전체다. 확장자 목록을 따로 관리하면 새 공개
# 헤더가 설치에서 빠질 수 있으므로, 그 트리를 있는 그대로 설치한다.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

install(EXPORT ServerCoreTargets
        FILE ServerCoreTargets.cmake
        NAMESPACE ServerCore::
        DESTINATION "${ServerCoreCMakeInstallDir}")

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ServerCoreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ServerCoreConfigVersion.cmake"
        DESTINATION "${ServerCoreCMakeInstallDir}")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/ServerCore")

# ---------------------------------------------------------------------------
# 시험
# ---------------------------------------------------------------------------
if (SERVERCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()
