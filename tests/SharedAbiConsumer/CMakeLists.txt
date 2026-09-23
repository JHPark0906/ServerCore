cmake_minimum_required(VERSION 3.21)

project(ServerCoreSharedAbiConsumer LANGUAGES CXX)

if (NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The shared native ABI fixture requires Windows and MSVC.")
endif ()

foreach (requiredVariable IN ITEMS SERVERCORE_ABI_INCLUDE_DIR SERVERCORE_ABI_LIBRARY
        SERVERCORE_ABI_RUNTIME SERVERCORE_ABI_RUNTIME_LIBRARY)
    if (NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} was not supplied to the ABI consumer.")
    endif ()
endforeach ()

if (NOT EXISTS "${SERVERCORE_ABI_INCLUDE_DIR}/ServerCore/Core/Version.h" OR
        NOT EXISTS "${SERVERCORE_ABI_LIBRARY}" OR NOT EXISTS "${SERVERCORE_ABI_RUNTIME}")
    message(FATAL_ERROR "The ABI consumer requires the producer's headers, import library, and DLL.")
endif ()

add_executable(ServerCoreSharedAbiConsumer main.cpp)
target_compile_features(ServerCoreSharedAbiConsumer PRIVATE cxx_std_23)
set_target_properties(ServerCoreSharedAbiConsumer PROPERTIES
        CXX_EXTENSIONS OFF
        MSVC_RUNTIME_LIBRARY "${SERVERCORE_ABI_RUNTIME_LIBRARY}")
target_compile_options(ServerCoreSharedAbiConsumer PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
target_include_directories(ServerCoreSharedAbiConsumer PRIVATE "${SERVERCORE_ABI_INCLUDE_DIR}")

# Deliberately bypass the package target and its compile definitions. This models
# a manual import-library consumer and catches a configuration-fallback DLL too.
target_compile_definitions(ServerCoreSharedAbiConsumer PRIVATE SERVERCORE_SHARED)
if (DEFINED SERVERCORE_ABI_ITERATOR_LEVEL)
    target_compile_definitions(ServerCoreSharedAbiConsumer PRIVATE
            "_ITERATOR_DEBUG_LEVEL=${SERVERCORE_ABI_ITERATOR_LEVEL}")
endif ()
target_link_libraries(ServerCoreSharedAbiConsumer PRIVATE "${SERVERCORE_ABI_LIBRARY}")

add_custom_command(TARGET ServerCoreSharedAbiConsumer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${SERVERCORE_ABI_RUNTIME}" "$<TARGET_FILE_DIR:ServerCoreSharedAbiConsumer>"
        VERBATIM)

enable_testing()
add_test(NAME ServerCoreSharedAbiConsumer.Runs COMMAND ServerCoreSharedAbiConsumer)
