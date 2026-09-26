cmake_minimum_required(VERSION 3.21)
project(ServerCoreCAbiConsumer LANGUAGES C)
find_package(ServerCore CONFIG REQUIRED COMPONENTS CAbi PATHS "${SERVERCORE_PACKAGE_PREFIX}" NO_DEFAULT_PATH)
get_target_property(cAbiKind ServerCore::CAbi TYPE)
get_target_property(coreKind ServerCore::ServerCore TYPE)
if (NOT cAbiKind STREQUAL "INTERFACE_LIBRARY" OR NOT coreKind STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "The shared C ABI must use an interface target backed by the single ServerCore library.")
endif ()
add_executable(CAbiConsumer main.c)
target_link_libraries(CAbiConsumer PRIVATE ServerCore::CAbi)
# Types.h promises that the headers compile as C90, C99 and C11 without
# extensions. GCC and Clang enforce that here with -pedantic-errors. MSVC
# accepts C11-only constructs such as a repeated typedef silently even with
# /Za, so on MSVC these targets only show that the headers compile at all.
# Imported include directories are system directories by default, where GCC
# and Clang suppress pedantic diagnostics; NO_SYSTEM_FROM_IMPORTED keeps them.
foreach (standard IN ITEMS 90 99 11)
    add_library(CAbiHeadersC${standard} OBJECT Headers.c)
    target_link_libraries(CAbiHeadersC${standard} PRIVATE ServerCore::CAbi)
    set_target_properties(CAbiHeadersC${standard} PROPERTIES
            C_STANDARD ${standard} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
            NO_SYSTEM_FROM_IMPORTED ON)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(CAbiHeadersC${standard} PRIVATE -pedantic-errors -Wall -Wextra -Werror)
    endif ()
endforeach ()
get_property(consumerLanguages GLOBAL PROPERTY ENABLED_LANGUAGES)
if ("CXX" IN_LIST consumerLanguages)
    message(FATAL_ERROR "The shared C ABI consumer must not require enabling C++.")
endif ()
if (WIN32)
    add_custom_command(TARGET CAbiConsumer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ServerCore::ServerCore> $<TARGET_FILE_DIR:CAbiConsumer>
            VERBATIM)
endif ()
enable_testing()
add_test(NAME CAbiConsumer.Runs COMMAND CAbiConsumer)
