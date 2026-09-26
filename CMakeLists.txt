# ServerCore — 게임 서버 백엔드의 공용 코어 라이브러리.
# 각 게임 프로젝트가 이 라이브러리를 링크해 자신의 서버 백엔드를 그 위에 올린다.
#
# 최소 버전을 3.21로 두는 이유:
#   - PROJECT_IS_TOP_LEVEL(3.21)을 써서, 다른 프로젝트가 우리를 포함했을 때
#     시험이 그쪽 빌드에 딸려 들어가지 않게 한다.
#   - CMakePresets v3(3.21)를 쓴다.
#   - C++23 표준 선택과 cxx_std_23은 3.20부터 지원한다.
cmake_minimum_required(VERSION 3.21)

# MSVC runtime target 속성은 언어를 켜기 전에 CMP0091이 NEW여야만 작동한다. 최상위
# ServerCore 구성에서는 아래 project()보다 먼저 이 선택을 명시한다.
cmake_policy(SET CMP0091 NEW)

project(ServerCore
        VERSION 0.3.0
        DESCRIPTION "웹 서버와 게임 서버용 공용 코어 라이브러리"
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
find_package(Threads REQUIRED)

if (WIN32)
    set(ServerCorePlatformSources
            src/Net/Acceptor.cpp
            src/Net/Connection.cpp
            src/Net/DatagramSocket.cpp
            src/Net/IoContext.cpp
            src/Net/WinsockInternal.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ServerCorePlatformSources
            src/Net/Linux/Acceptor.cpp
            src/Net/Linux/Connection.cpp
            src/Net/Linux/DatagramSocket.cpp
            src/Net/Linux/IoContext.cpp)
else ()
    message(FATAL_ERROR "ServerCore supports Windows and Linux.")
endif ()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Check the compiler AND its selected standard library, including cross builds.
# CMake selects the appropriate /std: mode for MSVC; do not hard-code a preview
# flag or assume support from a compiler's version number alone.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCxx23.cmake")
ServerCoreCheckCxx23Support()

# 소비자(게임 프로젝트)가 우리를 add_subdirectory로 포함하면 시험은 꺼진다.
option(SERVERCORE_BUILD_TESTS   "ServerCore의 시험을 빌드한다"   ${PROJECT_IS_TOP_LEVEL})
# C and C++ APIs now share one binary. Reject obsolete scripts rather than
# silently accepting an option that no longer controls a separate artifact.
if (DEFINED SERVERCORE_C_API_SHARED)
    message(FATAL_ERROR
            "SERVERCORE_C_API_SHARED has been removed: C++ and C ABI now use one ServerCore binary. "
            "Use SERVERCORE_BUILD_SHARED and clear the old cache entry with -U SERVERCORE_C_API_SHARED.")
endif ()
option(SERVERCORE_BUILD_SHARED "Build the unified C++ and optional C ABI as a shared library" OFF)
option(SERVERCORE_BUILD_C_API "Build the C ABI used by Rust and other languages" OFF)

# Diagnose stale build scripts instead of silently dropping a requested feature.
if (SERVERCORE_BUILD_HTTP_CLIENT)
    message(FATAL_ERROR
            "The HTTP client has been removed from ServerCore. Remove SERVERCORE_BUILD_HTTP_CLIENT "
            "from your build configuration and provide outbound HTTP in the consuming application.")
endif ()

# ---------------------------------------------------------------------------
# 라이브러리
# ---------------------------------------------------------------------------
# PublicHeaderCompileCheck.cpp는 공개 헤더 전부를 한 번역 단위에 함께 넣어 라이브러리의 경고
# 설정으로 컴파일한다. 헤더 하나하나의 자기완결은 여기서 증명되지 않는다. 그것은 시험 빌드에서
# tests/CMakeLists.txt가 헤더마다 번역 단위를 만들어 확인한다(ServerCore.Architecture.PublicHeadersCompile).
# 새 공개 헤더를 더하면 그 파일에도 한 줄 더한다. 빠뜨리면 같은 시험이 실패한다.
if (SERVERCORE_BUILD_SHARED)
    set(ServerCoreLibraryKind SHARED)
else ()
    set(ServerCoreLibraryKind STATIC)
endif ()
set(ServerCoreSources
        src/Core/Assert.cpp
        src/Core/ByteBuffer.cpp
        src/Core/Clock.cpp
        src/Core/CompletionSubscription.cpp
        src/Core/Endpoint.cpp
        src/Core/Config.cpp
        src/Core/Error.cpp
        src/Core/JobQueue.cpp
        src/Core/Logging.cpp
        src/Core/Utf8.cpp
        src/Core/Version.cpp
        src/Dispatch/Dispatcher.cpp
        src/Observability/AsyncLogger.cpp
        src/Observability/Prometheus.cpp
        ${ServerCorePlatformSources}
        src/Net/ConnectionFlowControl.cpp
        src/Net/SendBudgetInternal.cpp
        src/Net/SendQueueInternal.cpp
        src/Protocol/Framing.cpp
        src/Protocol/BinaryMessage.cpp
        src/Protocol/BinaryIO.cpp
        src/Core/AtomicFile.cpp
        src/Observability/ServerObservation.cpp
        src/Net/DatagramReadiness.cpp
        src/Runtime/TickRunner.cpp
        src/Runtime/OutboundQueue.cpp
        src/Protocol/Json.cpp
        src/Protocol/Message.cpp
        src/Runtime/DatagramTransport.cpp
        src/Runtime/JobRunner.cpp
        src/Runtime/KeyedExecutor.cpp
        src/Runtime/RequestLimiter.cpp
        src/Runtime/ParseWorkerPool.cpp
        src/Runtime/PeriodicRunner.cpp
        src/Runtime/ServerHost.cpp
        src/Runtime/ServerHostMetrics.cpp
        src/Runtime/ServerHostSessions.cpp
        src/Runtime/ServerHostLifecycle.cpp
        src/Runtime/ServerHostNetworkSession.cpp
        src/Runtime/ServerHostOptions.cpp
        src/Runtime/ServerHostTestHooks.cpp
        src/Runtime/TaskExecutor.cpp
        src/Runtime/TaskGroup.cpp
        src/Runtime/TimerScheduler.cpp
        src/Runtime/SessionTask.cpp
        src/Session/SessionRegistry.cpp
        src/Web/HttpServer.cpp
        src/Web/HttpPolicy.cpp
        src/Web/TrustedProxy.cpp
        src/Web/RequestData.cpp
        src/Web/Multipart.cpp
        src/Web/HttpObservationInternal.cpp
        src/Web/HttpResponseInternal.cpp
        src/Web/HttpStreaming.cpp
        src/Web/ResponseEncoding.cpp
        src/Web/RoutePattern.cpp
        src/Web/WebProtocol.cpp
        src/PublicHeaderCompileCheck.cpp)

# Both API surfaces use the same implementation and binary. The interface target
# exposes only the C declarations and link requirements, without propagating the
# native C++ compiler/STL ABI requirements to C or Rust consumers.
if (SERVERCORE_BUILD_C_API)
    list(APPEND ServerCoreSources src/C/Types.cpp src/C/Web.cpp src/C/Net.cpp
            src/C/Channel.cpp src/C/Runtime.cpp src/C/Endpoint.cpp src/C/RequestLimiter.cpp
            src/C/WebData.cpp src/C/WebPolicy.cpp src/C/BinaryIO.cpp src/C/Files.cpp
            src/C/Datagram.cpp src/C/GameExecution.cpp)
endif ()

if (MSVC)
    # ServerCore의 공개 API는 std::string, std::function, shared_ptr 등을 넘긴다. 따라서
    # 정적 라이브러리를 만든 CRT와 소비자가 쓰는 CRT는 같아야 한다. 위의 CMP0091 확인 뒤 이
    # target 속성은 Debug에서는 /MDd, 그 밖에서는 /MD를 만든다. 소비자 target까지 강제하지는
    # 않는다. 다른 CRT를 고르면 MSVC의 LNK2038 진단으로 링크를 막아, 라이브러리 경계를 넘는
    # 힙·표준 라이브러리 혼용을 조기에 발견하게 한다.
    set(ServerCoreMsvcRuntimeLibrary "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif ()

# 설치되는 제품 라이브러리(ServerCore)와 시험만 링크하는 훅 포함 라이브러리(ServerCoreTestHooks)가
# 같은 소스를 같은 규칙으로 빌드하도록, target 속성은 전부 이 함수 한 곳에서 붙인다.
function(ServerCoreConfigureLibrary target)
    if (SERVERCORE_BUILD_SHARED)
        target_compile_definitions(${target} PUBLIC SERVERCORE_SHARED PRIVATE SERVERCORE_BUILDING_LIBRARY)
        # Native C++ ABI requires a matching release/toolchain; do not promise a
        # stable class layout across future library versions.
        set_target_properties(${target} PROPERTIES VERSION "${PROJECT_VERSION}"
                SOVERSION "${PROJECT_VERSION}" DEBUG_POSTFIX "d")
        if (MSVC)
            target_compile_definitions(${target} PUBLIC
                    SERVERCORE_MSVC_DEBUG=$<IF:$<CONFIG:Debug>,1,0>
                    SERVERCORE_MSVC_VERSION=${MSVC_VERSION})
        endif ()
        if (SERVERCORE_BUILD_C_API)
            target_compile_definitions(${target} PUBLIC SC_CABI_SHARED PRIVATE SC_CABI_BUILD)
        endif ()
    endif ()

    target_include_directories(${target}
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
            # 층 안에서만 쓰는 헤더는 src 아래에 있다. 소비자에게 보이면 안 되므로 PRIVATE이고,
            # 그래서 전송 층의 구현이 "Net/WinsockInternal.h"처럼 층 이름부터 적을 수 있다.
            PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_link_libraries(${target} PUBLIC Threads::Threads)
    if (WIN32)
        target_link_libraries(${target} PUBLIC ws2_32 PRIVATE bcrypt)
    endif ()

    # Allow the static core to be embedded in shared application libraries.
    set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)

    target_compile_features(${target} PUBLIC cxx_std_23)

    if (MSVC)
        set_property(TARGET ${target} PROPERTY
                MSVC_RUNTIME_LIBRARY "${ServerCoreMsvcRuntimeLibrary}")

        # /utf-8 — 원본이 UTF-8이고 주석이 한국어다. 없으면 MSVC가 시스템 코드페이지로 읽어 깨진다.
        # /EHsc — 구현은 예외를 API 밖으로 내보내지 않지만, 자원 고갈을 Status로 바꾸기 위해 내부에서
        # catch를 쓴다. 예외 모델은 소비자에게 강제하지 않고, 이 target의 번역 단위에만 명시한다.
        # /permissive- — 표준 준수 모드.
        # /WX는 켜 둔다. 소비자 쪽 기준도 "/W4 경고 0"이고, "나중에 켜자"는 스위치는 켜지지 않는다.
        # 구현이 들어오면서 미사용 매개변수 경고(C4100) 같은 것이 걸리기 시작하면
        # 규칙으로 끄지 말고 그 자리에서 처리한다.
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
        target_compile_options(${target} PUBLIC /utf-8)
    else ()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif ()
endfunction()

add_library(ServerCore ${ServerCoreLibraryKind} ${ServerCoreSources})
add_library(ServerCore::ServerCore ALIAS ServerCore)
ServerCoreConfigureLibrary(ServerCore)
if (SERVERCORE_BUILD_SHARED)
    # 설치되는 ELF 공유 라이브러리도 Windows DLL처럼 SERVERCORE_API·SC_API로 표시한 것만 내보낸다
    # (BUILD-2). 내부 구현이 다른 DSO와 섞이지 않고, 상위 프로젝트의 가시성 설정에 C ABI가 휘둘리지
    # 않는다. MSVC에서는 아무 효과가 없다. 내보낸 집합은 ServerCore.CMake.SharedLibraryExportSet이
    # 확인하고, 설치 패키지 소비 시험들이 이 규칙 아래에서 링크한다.
    #
    # 시험 전용 ServerCoreTestHooks에는 걸지 않는다. 시험은 src 아래의 내부 클래스까지 직접 만든다.
    # Itanium C++ ABI는 가상 함수가 있는 클래스의 vtable을 key function이 정의된 라이브러리 번역
    # 단위에만 두므로, 클래스 전체가 아니라 멤버에만 표시한 내부 클래스(예:
    # Web::Detail::HttpResponseState)를 시험이 만들면 숨겨진 vtable에 닿지 못해 링크가 실패한다.
    # MSVC는 필요한 번역 단위마다 vtable을 COMDAT으로 내므로 Windows에서는 드러나지 않았다.
    # 시험용 target의 export 표면은 제품 계약이 아니므로 기본 가시성으로 둔다.
    set_target_properties(ServerCore PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
endif ()

if (SERVERCORE_BUILD_C_API)
    add_library(ServerCoreCAbi INTERFACE)
    add_library(ServerCore::CAbi ALIAS ServerCoreCAbi)
    set_target_properties(ServerCoreCAbi PROPERTIES EXPORT_NAME CAbi)
    target_include_directories(ServerCoreCAbi INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(ServerCoreCAbi INTERFACE "$<LINK_ONLY:ServerCore>")
    if (SERVERCORE_BUILD_SHARED)
        target_compile_definitions(ServerCoreCAbi INTERFACE SC_CABI_SHARED)
    endif ()
endif ()

# 경쟁 검사 몇몇은 구현 안의 시험 전용 hook(SERVERCORE_ENABLE_TEST_HOOKS, TestAccess 함수들)으로
# worker를 결정적으로 멈춘다. 그 hook은 설치되는 ServerCore에 넣지 않는다(BUILD-1). 시험을 빌드할
# 때만 같은 소스로 ServerCoreTestHooks를 따로 만들고 시험 실행 파일만 그것을 링크한다. 이 target은
# 어떤 export 집합에도 속하지 않으므로 설치되지 않는다. 설치 결과물에 hook이 없다는 것은
# ServerCore.CMake.InstalledLibraryHasNoTestHooks가 확인한다.
if (SERVERCORE_BUILD_TESTS)
    add_library(ServerCoreTestHooks ${ServerCoreLibraryKind} ${ServerCoreSources})
    ServerCoreConfigureLibrary(ServerCoreTestHooks)
    target_compile_definitions(ServerCoreTestHooks PRIVATE SERVERCORE_ENABLE_TEST_HOOKS=1)
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
        # Before 1.0, a new minor version may change public APIs.
        COMPATIBILITY SameMinorVersion)

set(ServerCoreNativeInstallComponent Unspecified)
if (SERVERCORE_BUILD_C_API)
    # The C ABI is part of the real ServerCore binary in both link modes.
    # Its component therefore includes that binary and its imported target.
    set(ServerCoreNativeInstallComponent CAbi)
endif ()
install(TARGETS ServerCore
        EXPORT ServerCoreTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${ServerCoreNativeInstallComponent}
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${ServerCoreNativeInstallComponent}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${ServerCoreNativeInstallComponent})

if (SERVERCORE_BUILD_C_API)
    install(TARGETS ServerCoreCAbi EXPORT ServerCoreCAbiTargets)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/ServerCore/C"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ServerCore" COMPONENT CAbi)
    install(EXPORT ServerCoreCAbiTargets FILE ServerCoreCAbiTargets.cmake
            NAMESPACE ServerCore:: DESTINATION "${ServerCoreCMakeInstallDir}" COMPONENT CAbi)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ServerCoreConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/ServerCoreConfigVersion.cmake"
            DESTINATION "${ServerCoreCMakeInstallDir}" COMPONENT CAbi)
endif ()

# include 아래는 라이브러리가 약속하는 공개 표면 전체다. 확장자 목록을 따로 관리하면 새 공개
# 헤더가 설치에서 빠질 수 있으므로, 그 트리를 있는 그대로 설치한다.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT ${ServerCoreNativeInstallComponent})

install(EXPORT ServerCoreTargets
        FILE ServerCoreTargets.cmake
        NAMESPACE ServerCore::
        DESTINATION "${ServerCoreCMakeInstallDir}" COMPONENT ${ServerCoreNativeInstallComponent})

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
