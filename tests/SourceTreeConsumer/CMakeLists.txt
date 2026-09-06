# ServerCore를 소스 트리로 소비하는 부모 프로젝트다. MSVC target runtime 속성이 유효하려면 CXX를
# 처음 켜는 project()보다 먼저 CMP0091을 NEW로 골라야 한다.
cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)

if (NOT DEFINED SERVERCORE_SOURCE_DIR OR "${SERVERCORE_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "SERVERCORE_SOURCE_DIR must name the ServerCore source tree.")
endif ()

project(ServerCoreSourceTreeConsumer LANGUAGES CXX)

add_subdirectory("${SERVERCORE_SOURCE_DIR}" ServerCore)

if (NOT TARGET ServerCore::ServerCore)
    message(FATAL_ERROR "ServerCore source-tree consumption did not provide ServerCore::ServerCore.")
endif ()

# ServerCore는 하위 프로젝트일 때 자신만의 tests를 기본값으로 켜면 안 된다. target 유입을
# 직접 확인해야 option의 기본값만 우연히 false인 상황을 통과시키지 않는다.
if (TARGET ServerCoreTests)
    message(FATAL_ERROR "ServerCore source-tree consumption unexpectedly added tests.")
endif ()

add_executable(ServerCoreSourceTreeConsumer
        "${CMAKE_CURRENT_LIST_DIR}/../PackageConsumer/main.cpp")

# 소비자가 C++ 표준·Winsock·include 경로를 따로 반복하지 않도록 모두 source-tree target에서
# 전이되어야 한다.
target_link_libraries(ServerCoreSourceTreeConsumer PRIVATE ServerCore::ServerCore)

enable_testing()
add_test(NAME ServerCoreSourceTreeConsumer.Runs COMMAND ServerCoreSourceTreeConsumer)
