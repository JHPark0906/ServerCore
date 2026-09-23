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
    get_target_property(serverCoreKind ServerCore::ServerCore TYPE)
    if (DEFINED SERVERCORE_EXPECT_SHARED)
        if ((SERVERCORE_EXPECT_SHARED AND NOT serverCoreKind STREQUAL "SHARED_LIBRARY") OR
                (NOT SERVERCORE_EXPECT_SHARED AND NOT serverCoreKind STREQUAL "STATIC_LIBRARY"))
            message(FATAL_ERROR "ServerCore native library kind differs from the requested package.")
        endif ()
    endif ()
    get_target_property(serverCoreFeatures ServerCore::ServerCore INTERFACE_COMPILE_FEATURES)
    if (NOT "cxx_std_23" IN_LIST serverCoreFeatures)
        message(FATAL_ERROR "ServerCore::ServerCore must export the PUBLIC cxx_std_23 requirement.")
    endif ()
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
    if (WIN32 AND serverCoreKind STREQUAL "SHARED_LIBRARY")
        foreach(consumer IN ITEMS "${canonicalTarget}" "${legacyTarget}")
            add_custom_command(TARGET "${consumer}" POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:ServerCore::ServerCore> $<TARGET_FILE_DIR:${consumer}>)
        endforeach()
    endif ()
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
