# 설치 패키지와 소스 트리를 각각 소비하는 빈 CMake 프로젝트를 검증한다. 이 스크립트는 ctest가
# 원본을 성공적으로 빌드한 뒤에만 실행되므로, --install은 방금 만든 static library와 export
# 파일을 대상으로 한다.
cmake_minimum_required(VERSION 3.21)

function(RunOrFail stage)
    execute_process(
            COMMAND ${ARGN}
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)

    if (NOT result STREQUAL "0")
        message(FATAL_ERROR
                "${stage} failed with exit code ${result}.\n"
                "stdout:\n${output}\n"
                "stderr:\n${error}")
    endif ()
endfunction()

# /MT[d] 소비자는 /MD[d] ServerCore 정적 라이브러리를 링크할 수 없어야 한다. 단순히
# "빌드가 실패했다"만 보면 컴파일러 탐색 실패도 통과할 수 있으므로 MSVC의 CRT 불일치
# 진단(LNK2038/RuntimeLibrary)까지 확인한다.
function(RunAndExpectMsvcRuntimeMismatch stage)
    execute_process(
            COMMAND ${ARGN}
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)

    if (result STREQUAL "0")
        message(FATAL_ERROR
                "${stage} unexpectedly succeeded. The consumer must not link a different MSVC CRT.\n"
                "stdout:\n${output}\n"
                "stderr:\n${error}")
    endif ()

    string(CONCAT diagnostic "${output}" "\n" "${error}")
    string(FIND "${diagnostic}" "LNK2038" lnk2038Position)
    string(FIND "${diagnostic}" "RuntimeLibrary" runtimeLibraryPosition)

    if (lnk2038Position EQUAL -1 OR runtimeLibraryPosition EQUAL -1)
        message(FATAL_ERROR
                "${stage} failed, but not with the expected MSVC CRT mismatch diagnostic.\n"
                "stdout:\n${output}\n"
                "stderr:\n${error}")
    endif ()
endfunction()

function(RunAndExpectMsvcRuntimePolicyFailure stage)
    execute_process(
            COMMAND ${ARGN}
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE error)

    if (result STREQUAL "0")
        message(FATAL_ERROR
                "${stage} unexpectedly succeeded. A legacy CMP0091 parent must be rejected.\n"
                "stdout:\n${output}\n"
                "stderr:\n${error}")
    endif ()

    string(CONCAT diagnostic "${output}" "\n" "${error}")
    string(FIND "${diagnostic}" "ServerCore requires CMP0091 NEW" policyDiagnosticPosition)

    if (policyDiagnosticPosition EQUAL -1)
        message(FATAL_ERROR
                "${stage} failed, but not with the expected ServerCore CMP0091 diagnostic.\n"
                "stdout:\n${output}\n"
                "stderr:\n${error}")
    endif ()
endfunction()

function(CreatePackageConsumerConfigureCommand outputVariable buildDirectory packagePrefix runtimeLibrary)
    set(command
            "${CMAKE_COMMAND}"
            -S "${consumerSourceDirectory}"
            -B "${buildDirectory}"
            -G "${SERVERCORE_PACKAGE_GENERATOR}")

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND command -A "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}")
    endif ()

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}" STREQUAL "")
        list(APPEND command -T "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}")
    endif ()

    if (NOT SERVERCORE_PACKAGE_MULTI_CONFIG)
        list(APPEND command
                "-DCMAKE_MAKE_PROGRAM=${SERVERCORE_PACKAGE_MAKE_PROGRAM}"
                "-DCMAKE_CXX_COMPILER=${SERVERCORE_PACKAGE_CXX_COMPILER}"
                "-DCMAKE_BUILD_TYPE=${SERVERCORE_PACKAGE_CONFIGURATION}")
    endif ()

    list(APPEND command
            "-DCMAKE_RC_COMPILER=${SERVERCORE_PACKAGE_RC_COMPILER}"
            "-DCMAKE_MT=${SERVERCORE_PACKAGE_MT}"
            "-DSERVERCORE_PACKAGE_PREFIX=${packagePrefix}"
            "-DSERVERCORE_PACKAGE_VERSION=${SERVERCORE_PACKAGE_VERSION}")

    if (SERVERCORE_PACKAGE_IS_MSVC AND NOT "${runtimeLibrary}" STREQUAL "")
        list(APPEND command "-DCMAKE_MSVC_RUNTIME_LIBRARY=${runtimeLibrary}")
    endif ()

    set("${outputVariable}" "${command}" PARENT_SCOPE)
endfunction()

function(CreateSourceTreeConsumerConfigureCommand outputVariable buildDirectory)
    set(command
            "${CMAKE_COMMAND}"
            -S "${sourceTreeConsumerSourceDirectory}"
            -B "${buildDirectory}"
            -G "${SERVERCORE_PACKAGE_GENERATOR}")

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND command -A "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}")
    endif ()

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}" STREQUAL "")
        list(APPEND command -T "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}")
    endif ()

    if (NOT SERVERCORE_PACKAGE_MULTI_CONFIG)
        list(APPEND command
                "-DCMAKE_MAKE_PROGRAM=${SERVERCORE_PACKAGE_MAKE_PROGRAM}"
                "-DCMAKE_CXX_COMPILER=${SERVERCORE_PACKAGE_CXX_COMPILER}"
                "-DCMAKE_BUILD_TYPE=${SERVERCORE_PACKAGE_CONFIGURATION}")
    endif ()

    list(APPEND command
            "-DCMAKE_RC_COMPILER=${SERVERCORE_PACKAGE_RC_COMPILER}"
            "-DCMAKE_MT=${SERVERCORE_PACKAGE_MT}"
            "-DSERVERCORE_SOURCE_DIR=${SERVERCORE_PACKAGE_SOURCE_DIR}")

    set("${outputVariable}" "${command}" PARENT_SCOPE)
endfunction()

foreach (requiredVariable IN ITEMS
        SERVERCORE_PACKAGE_SOURCE_DIR
        SERVERCORE_PACKAGE_BINARY_DIR
        SERVERCORE_PACKAGE_TEST_ROOT
        SERVERCORE_PACKAGE_GENERATOR
        SERVERCORE_PACKAGE_MAKE_PROGRAM
        SERVERCORE_PACKAGE_CXX_COMPILER
        SERVERCORE_PACKAGE_RC_COMPILER
        SERVERCORE_PACKAGE_MT
        SERVERCORE_PACKAGE_CONFIGURATION
        SERVERCORE_PACKAGE_MULTI_CONFIG
        SERVERCORE_PACKAGE_IS_MSVC
        SERVERCORE_PACKAGE_CTEST_COMMAND
        SERVERCORE_PACKAGE_VERSION)
    if (NOT DEFINED ${requiredVariable} OR "${${requiredVariable}}" STREQUAL "")
        message(FATAL_ERROR "${requiredVariable} was not provided to the CMake consumer test.")
    endif ()
endforeach ()

# 매 회차의 install·source tree·package consumer build를 지우는 대상은 현재 build tree 아래의 이
# 전용 디렉터리만이다. CMake의 경로 정규화 뒤에도 그 관계가 성립하는지 확인하고 나서만 재귀
# 삭제한다.
cmake_path(ABSOLUTE_PATH SERVERCORE_PACKAGE_BINARY_DIR NORMALIZE
        OUTPUT_VARIABLE packageBinaryDirectory)
cmake_path(ABSOLUTE_PATH SERVERCORE_PACKAGE_TEST_ROOT NORMALIZE
        OUTPUT_VARIABLE packageTestRoot)
cmake_path(IS_PREFIX packageBinaryDirectory "${packageTestRoot}" NORMALIZE testRootIsInsideBuild)

if (NOT testRootIsInsideBuild OR packageBinaryDirectory STREQUAL packageTestRoot)
    message(FATAL_ERROR
            "The package consumer test root must be a child of the ServerCore build directory. "
            "build=${packageBinaryDirectory}, root=${packageTestRoot}")
endif ()

file(REMOVE_RECURSE "${packageTestRoot}")

set(packageInstallStage "${packageTestRoot}/install-stage")
set(packagePrefix "${packageTestRoot}/install-relocated")
set(consumerBuildDirectory "${packageTestRoot}/consumer-build")
set(consumerSourceDirectory "${SERVERCORE_PACKAGE_SOURCE_DIR}/tests/PackageConsumer")
set(sourceTreeConsumerBuildDirectory "${packageTestRoot}/source-tree-consumer-build")
set(sourceTreeConsumerSourceDirectory
        "${SERVERCORE_PACKAGE_SOURCE_DIR}/tests/SourceTreeConsumer")
set(legacyPolicyConsumerSourceDirectory
        "${SERVERCORE_PACKAGE_SOURCE_DIR}/tests/MsvcRuntimePolicyConsumer")

if (SERVERCORE_PACKAGE_IS_MSVC)
    if ("${SERVERCORE_PACKAGE_CONFIGURATION}" STREQUAL "Debug")
        set(matchingMsvcRuntimeLibrary MultiThreadedDebugDLL)
        set(mismatchedMsvcRuntimeLibrary MultiThreadedDebug)
    else ()
        set(matchingMsvcRuntimeLibrary MultiThreadedDLL)
        set(mismatchedMsvcRuntimeLibrary MultiThreaded)
    endif ()
endif ()

# 설치 결과물을 보지 않는 별도 부모 프로젝트가 ServerCore를 add_subdirectory()로 넣고, 같은
# 공개 target 이름으로 configure·build·실행까지 할 수 있어야 한다. fixture는 tests/samples가
# 부모 프로젝트에 딸려오지 않는지도 자체적으로 확인한다.
CreateSourceTreeConsumerConfigureCommand(
        sourceTreeConsumerConfigureCommand
        "${sourceTreeConsumerBuildDirectory}")

RunOrFail("Configuring ServerCore source-tree consumer" ${sourceTreeConsumerConfigureCommand})

RunOrFail("Building ServerCore source-tree consumer"
        "${CMAKE_COMMAND}"
        --build "${sourceTreeConsumerBuildDirectory}"
        --config "${SERVERCORE_PACKAGE_CONFIGURATION}")

RunOrFail("Running ServerCore source-tree consumer"
        "${SERVERCORE_PACKAGE_CTEST_COMMAND}"
        --test-dir "${sourceTreeConsumerBuildDirectory}"
        -C "${SERVERCORE_PACKAGE_CONFIGURATION}"
        --output-on-failure)

RunOrFail("Installing ServerCore package"
        "${CMAKE_COMMAND}"
        --install "${packageBinaryDirectory}"
        --config "${SERVERCORE_PACKAGE_CONFIGURATION}"
        --prefix "${packageInstallStage}")

# export 파일에 build-time prefix가 남지 않았는지 보기 위해 설치를 완료한 뒤 다른 경로로 옮긴다.
file(RENAME "${packageInstallStage}" "${packagePrefix}")

CreatePackageConsumerConfigureCommand(
        consumerConfigureCommand
        "${consumerBuildDirectory}"
        "${packagePrefix}"
        "")

RunOrFail("Configuring ServerCore package consumer" ${consumerConfigureCommand})

RunOrFail("Building ServerCore package consumer"
        "${CMAKE_COMMAND}"
        --build "${consumerBuildDirectory}"
        --config "${SERVERCORE_PACKAGE_CONFIGURATION}")

RunOrFail("Running ServerCore package consumer"
        "${SERVERCORE_PACKAGE_CTEST_COMMAND}"
        --test-dir "${consumerBuildDirectory}"
        -C "${SERVERCORE_PACKAGE_CONFIGURATION}"
        --output-on-failure)

if (SERVERCORE_PACKAGE_IS_MSVC)
    # target 속성이 최초 CXX enable 이후에는 무시된다는 CMake 정책 제약을 명시적으로
    # 진단하는지 확인한다. 이 fixture는 CMP0091이 없던 3.14 정책의 부모를 재현한다.
    set(legacyPolicyConsumerBuildDirectory "${packageTestRoot}/legacy-runtime-policy-consumer-build")
    set(legacyPolicyConsumerConfigureCommand
            "${CMAKE_COMMAND}"
            -S "${legacyPolicyConsumerSourceDirectory}"
            -B "${legacyPolicyConsumerBuildDirectory}"
            -G "${SERVERCORE_PACKAGE_GENERATOR}")

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND legacyPolicyConsumerConfigureCommand
                -A "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}")
    endif ()

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}" STREQUAL "")
        list(APPEND legacyPolicyConsumerConfigureCommand
                -T "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}")
    endif ()

    if (NOT SERVERCORE_PACKAGE_MULTI_CONFIG)
        list(APPEND legacyPolicyConsumerConfigureCommand
                "-DCMAKE_MAKE_PROGRAM=${SERVERCORE_PACKAGE_MAKE_PROGRAM}"
                "-DCMAKE_CXX_COMPILER=${SERVERCORE_PACKAGE_CXX_COMPILER}"
                "-DCMAKE_BUILD_TYPE=${SERVERCORE_PACKAGE_CONFIGURATION}")
    endif ()

    list(APPEND legacyPolicyConsumerConfigureCommand
            "-DCMAKE_RC_COMPILER=${SERVERCORE_PACKAGE_RC_COMPILER}"
            "-DCMAKE_MT=${SERVERCORE_PACKAGE_MT}"
            "-DSERVERCORE_SOURCE_DIR=${SERVERCORE_PACKAGE_SOURCE_DIR}")

    RunAndExpectMsvcRuntimePolicyFailure(
            "Configuring ServerCore below a legacy MSVC runtime policy parent"
            ${legacyPolicyConsumerConfigureCommand})

    # CMake의 기본값도 현재는 /MD[d]다. 그래서 이 회귀는 기본값만 관찰하지 않고, producer에
    # 의도적으로 /MT[d] 기본값을 주어도 ServerCore target 속성이 /MD[d]를 고정하는지를 본다.
    # tests/samples를 끈 별도 producer라, 이 검사는 외부 소비자의 전역 CMake 설정을 흉내 내되
    # 현재 시험 실행 파일의 CRT 계약을 흔들지 않는다.
    set(runtimeOverrideProducerBuildDirectory "${packageTestRoot}/runtime-override-producer-build")
    set(runtimeOverrideInstallStage "${packageTestRoot}/runtime-override-install-stage")
    set(runtimeOverridePackagePrefix "${packageTestRoot}/runtime-override-install-relocated")

    set(runtimeOverrideProducerConfigureCommand
            "${CMAKE_COMMAND}"
            -S "${SERVERCORE_PACKAGE_SOURCE_DIR}"
            -B "${runtimeOverrideProducerBuildDirectory}"
            -G "${SERVERCORE_PACKAGE_GENERATOR}")

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND runtimeOverrideProducerConfigureCommand -A "${SERVERCORE_PACKAGE_GENERATOR_PLATFORM}")
    endif ()

    if (NOT "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}" STREQUAL "")
        list(APPEND runtimeOverrideProducerConfigureCommand -T "${SERVERCORE_PACKAGE_GENERATOR_TOOLSET}")
    endif ()

    if (NOT SERVERCORE_PACKAGE_MULTI_CONFIG)
        list(APPEND runtimeOverrideProducerConfigureCommand
                "-DCMAKE_MAKE_PROGRAM=${SERVERCORE_PACKAGE_MAKE_PROGRAM}"
                "-DCMAKE_CXX_COMPILER=${SERVERCORE_PACKAGE_CXX_COMPILER}"
                "-DCMAKE_BUILD_TYPE=${SERVERCORE_PACKAGE_CONFIGURATION}")
    endif ()

    list(APPEND runtimeOverrideProducerConfigureCommand
            "-DCMAKE_RC_COMPILER=${SERVERCORE_PACKAGE_RC_COMPILER}"
            "-DCMAKE_MT=${SERVERCORE_PACKAGE_MT}"
            -DSERVERCORE_BUILD_TESTS=OFF
            -DSERVERCORE_BUILD_SAMPLES=OFF
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=${mismatchedMsvcRuntimeLibrary}")

    RunOrFail("Configuring ServerCore with an incompatible global MSVC runtime"
            ${runtimeOverrideProducerConfigureCommand})

    RunOrFail("Building ServerCore with an incompatible global MSVC runtime"
            "${CMAKE_COMMAND}"
            --build "${runtimeOverrideProducerBuildDirectory}"
            --config "${SERVERCORE_PACKAGE_CONFIGURATION}"
            --target ServerCore)

    RunOrFail("Installing ServerCore built with an incompatible global MSVC runtime"
            "${CMAKE_COMMAND}"
            --install "${runtimeOverrideProducerBuildDirectory}"
            --config "${SERVERCORE_PACKAGE_CONFIGURATION}"
            --prefix "${runtimeOverrideInstallStage}")

    file(RENAME "${runtimeOverrideInstallStage}" "${runtimeOverridePackagePrefix}")

    set(matchingConsumerBuildDirectory "${packageTestRoot}/matching-runtime-consumer-build")
    CreatePackageConsumerConfigureCommand(
            matchingConsumerConfigureCommand
            "${matchingConsumerBuildDirectory}"
            "${runtimeOverridePackagePrefix}"
            "${matchingMsvcRuntimeLibrary}")

    RunOrFail("Configuring matching-runtime ServerCore package consumer"
            ${matchingConsumerConfigureCommand})

    RunOrFail("Building matching-runtime ServerCore package consumer"
            "${CMAKE_COMMAND}"
            --build "${matchingConsumerBuildDirectory}"
            --config "${SERVERCORE_PACKAGE_CONFIGURATION}")

    RunOrFail("Running matching-runtime ServerCore package consumer"
            "${SERVERCORE_PACKAGE_CTEST_COMMAND}"
            --test-dir "${matchingConsumerBuildDirectory}"
            -C "${SERVERCORE_PACKAGE_CONFIGURATION}"
            --output-on-failure)

    set(mismatchedConsumerBuildDirectory "${packageTestRoot}/mismatched-runtime-consumer-build")
    CreatePackageConsumerConfigureCommand(
            mismatchedConsumerConfigureCommand
            "${mismatchedConsumerBuildDirectory}"
            "${runtimeOverridePackagePrefix}"
            "${mismatchedMsvcRuntimeLibrary}")

    RunOrFail("Configuring mismatched-runtime ServerCore package consumer"
            ${mismatchedConsumerConfigureCommand})

    RunAndExpectMsvcRuntimeMismatch("Building mismatched-runtime ServerCore package consumer"
            "${CMAKE_COMMAND}"
            --build "${mismatchedConsumerBuildDirectory}"
            --config "${SERVERCORE_PACKAGE_CONFIGURATION}")
endif ()
