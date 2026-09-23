cmake_minimum_required(VERSION 3.21)

foreach (requiredVariable IN ITEMS SERVERCORE_ABI_SOURCE_DIR SERVERCORE_ABI_BINARY_DIR
        SERVERCORE_ABI_TEST_ROOT SERVERCORE_ABI_GENERATOR SERVERCORE_ABI_CONFIGURATION
        SERVERCORE_ABI_MULTI_CONFIG SERVERCORE_ABI_CTEST_COMMAND SERVERCORE_ABI_LIBRARY
        SERVERCORE_ABI_RUNTIME)
    if (NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} was not provided to the shared ABI test.")
    endif ()
endforeach ()

if (NOT WIN32)
    message(FATAL_ERROR "The shared native ABI test requires Windows.")
endif ()

if (NOT SERVERCORE_ABI_MULTI_CONFIG)
    foreach (requiredVariable IN ITEMS SERVERCORE_ABI_CXX_COMPILER SERVERCORE_ABI_MAKE_PROGRAM)
        if (NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
            message(FATAL_ERROR "${requiredVariable} is required for a single-config generator.")
        endif ()
    endforeach ()
endif ()

# Only this test's private consumer builds may be removed. Resolve an existing
# root too, so a junction cannot redirect cleanup outside the producer build.
cmake_path(ABSOLUTE_PATH SERVERCORE_ABI_BINARY_DIR NORMALIZE OUTPUT_VARIABLE abiBinaryDirectory)
cmake_path(ABSOLUTE_PATH SERVERCORE_ABI_TEST_ROOT NORMALIZE OUTPUT_VARIABLE abiTestRoot)
file(REAL_PATH "${abiBinaryDirectory}" abiBinaryDirectory)
if (EXISTS "${abiTestRoot}")
    file(REAL_PATH "${abiTestRoot}" abiTestRoot)
else ()
    cmake_path(GET abiTestRoot PARENT_PATH abiTestParent)
    cmake_path(GET abiTestRoot FILENAME abiTestName)
    file(REAL_PATH "${abiTestParent}" abiTestParent)
    set(abiTestRoot "${abiTestParent}/${abiTestName}")
endif ()
cmake_path(IS_PREFIX abiBinaryDirectory "${abiTestRoot}" NORMALIZE abiRootInsideBuild)
if (NOT abiRootInsideBuild OR abiBinaryDirectory STREQUAL abiTestRoot)
    message(FATAL_ERROR "The ABI test root must be a child of the producer build directory: ${abiTestRoot}")
endif ()
file(REMOVE_RECURSE "${abiTestRoot}")
file(MAKE_DIRECTORY "${abiTestRoot}")

function(AbiRunOrFail stage logPath)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE commandResult
            OUTPUT_VARIABLE commandOutput ERROR_VARIABLE commandError)
    file(WRITE "${logPath}" "${commandOutput}\n${commandError}")
    if (NOT "${commandResult}" STREQUAL "0")
        message(FATAL_ERROR "${stage} failed (${commandResult}).\n${commandOutput}\n${commandError}")
    endif ()
endfunction()

function(AbiConfigure caseName configuration runtimeLibrary)
    set(buildDirectory "${abiTestRoot}/${caseName}")
    set(command "${CMAKE_COMMAND}"
            -S "${SERVERCORE_ABI_SOURCE_DIR}/tests/SharedAbiConsumer" -B "${buildDirectory}"
            -G "${SERVERCORE_ABI_GENERATOR}"
            "-DSERVERCORE_ABI_INCLUDE_DIR=${SERVERCORE_ABI_SOURCE_DIR}/include"
            "-DSERVERCORE_ABI_LIBRARY=${SERVERCORE_ABI_LIBRARY}"
            "-DSERVERCORE_ABI_RUNTIME=${SERVERCORE_ABI_RUNTIME}"
            "-DSERVERCORE_ABI_RUNTIME_LIBRARY=${runtimeLibrary}")
    if (DEFINED SERVERCORE_ABI_GENERATOR_PLATFORM AND
            NOT "${SERVERCORE_ABI_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND command -A "${SERVERCORE_ABI_GENERATOR_PLATFORM}")
    endif ()
    if (DEFINED SERVERCORE_ABI_GENERATOR_TOOLSET AND
            NOT "${SERVERCORE_ABI_GENERATOR_TOOLSET}" STREQUAL "")
        list(APPEND command -T "${SERVERCORE_ABI_GENERATOR_TOOLSET}")
    endif ()
    if (NOT SERVERCORE_ABI_MULTI_CONFIG)
        list(APPEND command "-DCMAKE_BUILD_TYPE=${configuration}"
                "-DCMAKE_CXX_COMPILER=${SERVERCORE_ABI_CXX_COMPILER}"
                "-DCMAKE_MAKE_PROGRAM=${SERVERCORE_ABI_MAKE_PROGRAM}")
    endif ()
    foreach (tool IN ITEMS RC_COMPILER MT)
        if (DEFINED SERVERCORE_ABI_${tool} AND NOT "${SERVERCORE_ABI_${tool}}" STREQUAL "")
            list(APPEND command "-DCMAKE_${tool}=${SERVERCORE_ABI_${tool}}")
        endif ()
    endforeach ()
    list(APPEND command ${ARGN})
    AbiRunOrFail("Configuring ${caseName}" "${abiTestRoot}/${caseName}-configure.log" ${command})
endfunction()

function(AbiExpectBuildFailure caseName configuration requiredDiagnostic)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${abiTestRoot}/${caseName}"
            --config "${configuration}" --target ServerCoreSharedAbiConsumer
            RESULT_VARIABLE buildResult OUTPUT_VARIABLE buildOutput ERROR_VARIABLE buildError)
    set(buildLog "${buildOutput}\n${buildError}")
    file(WRITE "${abiTestRoot}/${caseName}-build.log" "${buildLog}")
    if ("${buildResult}" STREQUAL "0")
        message(FATAL_ERROR "${caseName} unexpectedly accepted an incompatible shared native ABI.")
    endif ()
    if (NOT buildLog MATCHES "${requiredDiagnostic}")
        message(FATAL_ERROR
                "${caseName} failed without its required ABI diagnostic.\n${buildLog}")
    endif ()
    message(STATUS "${caseName}: rejected by the expected shared native ABI diagnostic")
endfunction()

if (SERVERCORE_ABI_CONFIGURATION STREQUAL "Debug")
    set(matchingRuntime MultiThreadedDebugDLL)
    set(staticRuntime MultiThreadedDebug)
    set(oppositeConfiguration Release)
    set(oppositeRuntime MultiThreadedDLL)
    set(oppositeIterator 0)
    set(oppositeDebug 0)
    set(matchingDebug 1)
else ()
    set(matchingRuntime MultiThreadedDLL)
    set(staticRuntime MultiThreaded)
    set(oppositeConfiguration Debug)
    set(oppositeRuntime MultiThreadedDebugDLL)
    set(oppositeIterator 2)
    set(oppositeDebug 1)
    set(matchingDebug 0)
endif ()

# A known-good build/run makes missing headers, import libraries, DLLs, tools,
# and ordinary link dependencies test failures rather than negative-test passes.
AbiConfigure(matching "${SERVERCORE_ABI_CONFIGURATION}" "${matchingRuntime}")
AbiRunOrFail("Building matching ABI consumer" "${abiTestRoot}/matching-build.log"
        "${CMAKE_COMMAND}" --build "${abiTestRoot}/matching"
        --config "${SERVERCORE_ABI_CONFIGURATION}" --target ServerCoreSharedAbiConsumer)
AbiRunOrFail("Running matching ABI consumer" "${abiTestRoot}/matching-run.log"
        "${SERVERCORE_ABI_CTEST_COMMAND}" --test-dir "${abiTestRoot}/matching"
        -C "${SERVERCORE_ABI_CONFIGURATION}" --output-on-failure --no-tests=error)

AbiConfigure(static-crt "${SERVERCORE_ABI_CONFIGURATION}" "${staticRuntime}")
AbiExpectBuildFailure(static-crt "${SERVERCORE_ABI_CONFIGURATION}"
        "ServerCore C\\+\\+ ABI RuntimeLibrary mismatch")

# These consumers receive no producer Debug/toolset metadata. The real import
# library must reject their ABI at link time through its configuration anchor.
AbiConfigure(opposite-configuration "${oppositeConfiguration}" "${oppositeRuntime}")
AbiExpectBuildFailure(opposite-configuration "${oppositeConfiguration}"
        "LNK(2001|2019)[^\r\n]*ServerCoreNativeAbi_Msvc[0-9]+_Iter${oppositeIterator}_Debug${oppositeDebug}")

# Level 1 is supported by MSVC for both CRT configurations, but differs from
# ServerCore's Debug=2 and Release=0. Thus this must reach the native linker.
AbiConfigure(iterator-mismatch "${SERVERCORE_ABI_CONFIGURATION}" "${matchingRuntime}"
        -DSERVERCORE_ABI_ITERATOR_LEVEL=1)
AbiExpectBuildFailure(iterator-mismatch "${SERVERCORE_ABI_CONFIGURATION}"
        "LNK(2001|2019)[^\r\n]*ServerCoreNativeAbi_Msvc[0-9]+_Iter1_Debug${matchingDebug}")
