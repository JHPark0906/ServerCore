include("${CMAKE_CURRENT_LIST_DIR}/ApiConsumer/DeprecatedApis.cmake")

function(ServerCoreSetStrictConsumerWarnings target)
    # Imported include directories must also be checked as ordinary headers.
    set_property(TARGET "${target}" PROPERTY NO_SYSTEM_FROM_IMPORTED TRUE)
    if (MSVC)
        target_compile_options("${target}" PRIVATE /W4 /WX /permissive- /EHsc)
    else ()
        target_compile_options("${target}" PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif ()
endfunction()

function(ServerCoreAddConsumerApiChecks canonicalTarget)
    ServerCoreSetStrictConsumerWarnings("${canonicalTarget}")

    set(legacyTarget "${canonicalTarget}LegacyCompatibility")
    add_executable("${legacyTarget}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ApiConsumer/LegacyCompatibility.cpp")
    target_link_libraries("${legacyTarget}" PRIVATE ServerCore::ServerCore)
    ServerCoreSetStrictConsumerWarnings("${legacyTarget}")
    # Only this compatibility fixture deliberately calls deprecated declarations.
    # All other warnings remain errors, and canonical consumers suppress nothing.
    if (MSVC)
        target_compile_options("${legacyTarget}" PRIVATE /wd4996)
    else ()
        target_compile_options("${legacyTarget}" PRIVATE -Wno-deprecated-declarations)
    endif ()
    add_test(NAME "${legacyTarget}.Runs" COMMAND "${legacyTarget}")
    set_tests_properties("${legacyTarget}.Runs" PROPERTIES TIMEOUT 30)

    foreach (probe IN LISTS SERVERCORE_DEPRECATED_API_PROBES)
        string(REPLACE "|" ";" fields "${probe}")
        list(GET fields 0 suffix)
        list(GET fields 1 selector)
        set(probeTarget "ServerCoreDeprecated${suffix}")
        add_library("${probeTarget}" OBJECT EXCLUDE_FROM_ALL
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ApiConsumer/DeprecatedApiProbe.cpp")
        target_link_libraries("${probeTarget}" PRIVATE ServerCore::ServerCore)
        target_compile_definitions("${probeTarget}" PRIVATE "SERVERCORE_DEPRECATED_API=${selector}")
        ServerCoreSetStrictConsumerWarnings("${probeTarget}")
    endforeach ()
endfunction()
