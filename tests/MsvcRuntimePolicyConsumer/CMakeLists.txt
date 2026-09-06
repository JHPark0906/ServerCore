# ServerCore를 add_subdirectory()로 넣는 부모가 CMP0091을 아직 모르던 CMake 정책으로
# CXX를 먼저 켠 경우를 재현한다. 그 상태에서는 ServerCore의 MSVC_RUNTIME_LIBRARY target
# 속성이 무시되므로, ServerCore는 구성 단계에서 명확하게 거절해야 한다.
cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED SERVERCORE_SOURCE_DIR OR "${SERVERCORE_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "SERVERCORE_SOURCE_DIR must name the ServerCore source tree.")
endif ()

project(ServerCoreLegacyMsvcRuntimePolicyConsumer LANGUAGES CXX)

add_subdirectory("${SERVERCORE_SOURCE_DIR}" ServerCore)
