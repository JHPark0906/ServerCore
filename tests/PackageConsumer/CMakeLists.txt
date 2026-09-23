cmake_minimum_required(VERSION 3.21)

project(ServerCorePackageConsumer LANGUAGES CXX)

# The linked target must raise this baseline to C++23 without consumer flags.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERVERCORE_PACKAGE_PREFIX OR "${SERVERCORE_PACKAGE_PREFIX}" STREQUAL "" OR
        NOT DEFINED SERVERCORE_PACKAGE_VERSION OR "${SERVERCORE_PACKAGE_VERSION}" STREQUAL "")
    message(FATAL_ERROR
            "SERVERCORE_PACKAGE_PREFIX and SERVERCORE_PACKAGE_VERSION must describe an installed ServerCore package.")
endif ()

# NO_DEFAULT_PATH가 없으면 개발기 어딘가에 남은 다른 ServerCore package를 찾을 수 있다.
# 이 consumer는 바로 앞 단계가 만든 install prefix만으로 패키지가 완결되는지를 본다.
# Pre-1.0 API changes must not silently satisfy a previous minor's request.
if (SERVERCORE_PACKAGE_VERSION MATCHES "^0\\.([0-9]+)\\." AND CMAKE_MATCH_1 GREATER 0)
    math(EXPR previousMinor "${CMAKE_MATCH_1} - 1")
    find_package(ServerCore "0.${previousMinor}.0" QUIET CONFIG
            NO_DEFAULT_PATH PATHS "${SERVERCORE_PACKAGE_PREFIX}")
    if (ServerCore_FOUND)
        message(FATAL_ERROR "A pre-1.0 package must reject a previous minor version.")
    endif ()
endif ()
find_package(ServerCore "${SERVERCORE_PACKAGE_VERSION}" EXACT CONFIG REQUIRED
        NO_DEFAULT_PATH
        PATHS "${SERVERCORE_PACKAGE_PREFIX}")

add_executable(ServerCorePackageConsumer main.cpp)

# 소비자가 C++ 표준·Winsock·include 경로를 따로 반복하지 않도록 모두 패키지 target에서 전이된다.
target_link_libraries(ServerCorePackageConsumer PRIVATE ServerCore::ServerCore)

enable_testing()
add_test(NAME ServerCorePackageConsumer.Runs COMMAND ServerCorePackageConsumer)
include("${CMAKE_CURRENT_LIST_DIR}/../ApiConsumerChecks.cmake")
ServerCoreAddConsumerApiChecks(ServerCorePackageConsumer)
