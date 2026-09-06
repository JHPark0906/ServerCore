# 시험 실행 파일. 외부 시험 라이브러리에 의존하지 않는다.
#
# 왜 검사를 하나씩 add_test로 등록하는가:
# 실행 파일 하나를 시험 하나로 등록하면 ctest가 "1/1"만 뱉는다. 그러면 두 번 돌려 수를
# 비교할 수 없고, 검사가 늘었는지 줄었는지도 보이지 않는다. 하나씩 등록하면 "N/N"이 나온다.
# 실행 파일은 이름 하나를 인자로 받아 그 검사만 돌린다.
#
# 이름을 여기와 C++ 양쪽에 적어야 하는 것이 이 방식의 대가다. 그 어긋남은 아래
# RegisteredChecksMatchCtest 시험이 막는다. 한쪽에만 이름을 더하면 그 시험이 붉어진다.
#
# 새 검사를 더하는 순서: C++에 등록 객체를 만들고, 아래 목록에 같은 이름을 더한다.
set(SERVERCORE_TEST_NAMES
        Version.StringMatchesConstants
        Version.StringIsNotEmpty
        Error.OkStatusIsOk
        Error.FailStatusCarriesCodeAndMessage
        Error.UnimplementedStatusNamesWhat
        Error.ResultFromValueHoldsValue
        Error.ResultFromStatusHoldsStatus
        Error.ResultTakeStatusMovesFailure
        Error.ResultHoldsNonTrivialValue
        Error.ResultValueIsMutable
        ByteBuffer.NewBufferIsEmpty
        ByteBuffer.WriteThenPeekReturnsSameBytes
        ByteBuffer.ConsumeRemovesFromTheFront
        ByteBuffer.ConsumeEverythingLeavesItEmpty
        ByteBuffer.WriteBeyondCapacityIsPartial
        ByteBuffer.WriteToAFullBufferTakesNothing
        ByteBuffer.HeldBytesStayContiguous
        Logging.LoggerIsUsableBeforeInstall
        Logging.InstalledLoggerReceivesLevelAndMessage
        Logging.InstallingAgainReplacesTheLogger
        Clock.MillisecondsSinceProcessStartIsMonotonic
        Config.LoadsFlatSnapshot
        Config.ReportsMissingValuesAndFiles
        Config.ValidatesUtf8Paths
        Config.RejectsInvalidDocuments
        Config.RejectsInvalidIntegers)

set(SERVERCORE_PROTOCOL_TEST_NAMES
        Protocol.PreparedMessagesOwnImmutableJsonAndEnvelope
        Protocol.PreparedMessagesRejectInvalidInputs
        Protocol.PreparedMessagesAdaptLegacySessions
        Protocol.PreparedArrayReservesEnvelopeDepth
        Protocol.DatagramBoundariesAndWireOrder
        Protocol.FramingConformanceVectors
        Protocol.FramingRejectsInvalidLengths
        Protocol.FramingTransfersAndDiscardsCompletedBodies
        Protocol.FrameReaderReleaseStorageResetsReader
        Protocol.FrameCodecSharedApiIsNoThrow
        Protocol.MessagePreservesEnvelopeFields
        Protocol.MessagePreserves64BitIntegers
        Protocol.JsonRejectsUnrepresentableNumbers
        Protocol.MessageRejectsInvalidUtf8
        Protocol.JsonDumpRejectsUnsafeValues
        Protocol.MessageRejectsUnsafeOutboundJson
        Protocol.MessageRejectsNonObjectBodies)

set(SERVERCORE_JOB_TEST_NAMES
        Core.JobQueueRunsSnapshotOnly
        Core.JobQueueAcceptsConcurrentPosts
        Runtime.JobRunnerDrainsAcceptedJobsOnStop
        Runtime.JobRunnerReportsThreadAffinity
        Runtime.PeriodicRunnerInvokesThroughJobRunner
        Runtime.PeriodicRunnerCountsSkippedPeriods
        Runtime.PeriodicRunnerStopsSafelyAfterJobRunnerDestruction
        Runtime.PeriodicRunnerConcurrentStopsShareCompletedJoin)

set(SERVERCORE_SESSION_TEST_NAMES
        SessionRegistry.RegistryIssuesValidDistinctIds
        SessionRegistry.RegistryRegistersFindsAndUnregistersSessions
        SessionRegistry.RegistryAllowsFailureCleanupFromAnyThread
        SessionRegistry.RegistryRejectsInvalidAndDuplicateRegistrations
        SessionRegistry.RegistryRejectsUnissuedAndReusedRegistrations
        SessionRegistry.RegistryDiscardsUnregisteredIssueReservations
        SessionRegistry.RegistrySerializesConcurrentReservationDiscards
        SessionRegistry.SessionMarksAuthenticationWithoutOwningIdentityPolicy
        SessionRegistry.ForEachUsesSnapshotSoVisitorMayRemoveSessions)

set(SERVERCORE_DISPATCH_TEST_NAMES
        Dispatch.RegisterRoutesAndEnforcesRawBodyLimit
        Dispatch.PassesEnvelopeContextToHandler
        Dispatch.FreezeRejectsLateRegistration
        Dispatch.FreezeSerializesConcurrentRegistration
        Dispatch.DispatchAndRegisterAreSafeBeforeFreeze
        Dispatch.UnknownTypePolicyControlsDisconnect
        Dispatch.HandlerStatusIsPropagatedWithoutDisconnect
        Dispatch.ErrorEnvelopeWithoutBodyIsNotRouted
        Dispatch.DispatchDoesNotHideSessionContractViolations)

# 실제로 포트를 여는 검사들이다. 위 목록과 나눠 둔 이유는 아래에서 이것들에만 TIMEOUT을
# 걸기 때문이다. 매달리는 것이 전송 층의 대표적 실패 형태이고, 시간 제한이 없으면 검증이
# 멈춘 채 남는다. 포트를 열지 않는 검사에까지 제한을 걸 이유는 없다.
set(SERVERCORE_NET_TEST_NAMES
        Transport.SharedSendBudgetSerializesReservations
        Transport.IoContextStartsAndStops
        Transport.ListenRejectsInvalidEndpoint
        Transport.AcceptorCanListenAgainAfterStop
        Transport.EchoRoundTrip
        Transport.DisconnectIsNotifiedExactlyOnce
        Transport.LargePayloadCrossesReceiveBoundaries
        Transport.SendAfterCloseIsRefused
        Transport.CloseDiscardsQueuedSendBytes
        Transport.SendQueueLimitRejectsWholeOverflowAndRecovers
        Transport.SharedSendBudgetRejectsAcrossConnectionsAndRecovers
        Transport.CloseAfterSendDrainsQueuedBytes
        Transport.CloseAfterSendOnEmptyConnectionCloses
        Transport.ConcurrentSendsDoNotInterleave
        Transport.MultiWorkerDeliversDifferentConnectionsConcurrently
        Transport.DisconnectWaitsForReceiveCallback
        Transport.LateObserverReceivesPriorDisconnect
        Transport.AcceptorStopWaitsForActiveCompletionHandoff
        Transport.AcceptorStopDoesNotTimeOutActiveCompletionHandoff)

# ServerHost는 실제 TCP 수락부터 세션·프레임·디스패치 조립까지 확인한다. 이것도 포트를
# 열므로 전송 층 검사와 같은 시간 제한을 적용한다.
set(SERVERCORE_HOST_TEST_NAMES
        Runtime.ServerHostPreparedSendsPreserveFramingLimitsAndMetrics
        Runtime.ServerHostRoutesSplitFramesAndStops
        Runtime.ServerHostRejectsRestartAfterStop
        Runtime.ServerHostRejectsInvalidOptions
        Runtime.ServerHostSharesConfiguredSendBudgetAcrossSessions
        Runtime.ServerHostEnforcesConcurrentSessionLimit
        Runtime.ServerHostConfiguresFromConfig
        Runtime.ServerHostRejectsInvalidConfig
        Runtime.ServerHostBindsRegistryBeforePrepostedWork
        Runtime.ServerHostSendsFinalFrameBeforeDisconnect
        Runtime.ServerHostDisconnectsIdleSessions
        Runtime.ServerHostBoundsGracefulCloseWithoutIdleTimeout
        Runtime.ServerHostFailedStartSerializesOwnerResetWithStop
        Runtime.ServerHostConcurrentStopsWaitForBlockedHandler
        Runtime.ServerHostReportsMetrics
        Runtime.ServerHostParseWorkersPreserveSessionHandlerOrder
        Runtime.ServerHostFallbackFinalizerReleasesQueuedParseWork
        Runtime.ServerHostFallbackFinalizerAllowsReentrantDisconnect
        Runtime.ServerHostFallbackCloseNotificationCanStopHost
        Runtime.ServerHostFallbackCloseNotificationStopsStartingHost
        Runtime.ServerHostStopWaitsForCloseNotification
        Runtime.ServerHostBoundsTotalPendingParseBytes
        Runtime.ServerHostRollsBackParseBytesWhenTotalTaskLimitRejects
        Runtime.ServerHostBoundsPendingReceiveBytes
        Runtime.ServerHostBoundsTotalPendingReceiveBytes)

set(SERVERCORE_DATAGRAM_TEST_NAMES
        Runtime.DatagramBindLifecycleAndWinsockIsolation
        Runtime.DatagramRegistrationLifecycle
        Runtime.DatagramGenericRoundTripAndSendSequence
        Runtime.DatagramInvalidPacketsCannotRebindOrPoisonSequence
        Runtime.DatagramReceiverCallbacksCanSendUnregisterAndClose
        Runtime.DatagramAdmissionCallbacksRespectRegistrationLifetime
        Runtime.DatagramCallbackExceptionsEndPoll
        Runtime.DatagramBudgetCountsRejectedAndEmptyPackets
        Runtime.DatagramByteBudgetAndEmptyCallbacksPreservePendingPackets
        Runtime.DatagramTruncatedDatagramsRecover)

list(APPEND SERVERCORE_TEST_NAMES
        ${SERVERCORE_PROTOCOL_TEST_NAMES}
        ${SERVERCORE_JOB_TEST_NAMES}
        ${SERVERCORE_SESSION_TEST_NAMES}
        ${SERVERCORE_DISPATCH_TEST_NAMES}
        ${SERVERCORE_NET_TEST_NAMES}
        ${SERVERCORE_HOST_TEST_NAMES}
        ${SERVERCORE_DATAGRAM_TEST_NAMES})

add_executable(ServerCoreTests
        TestHarness.cpp
        Core/ByteBufferTest.cpp
        Core/ClockTest.cpp
        Core/ConfigTest.cpp
        Core/LoggingTest.cpp
        Core/ErrorTest.cpp
        Core/JobQueueTest.cpp
        Core/VersionTest.cpp
        Dispatch/DispatcherTest.cpp
        Protocol/ProtocolTest.cpp
        Runtime/RuntimeTest.cpp
        Runtime/ServerHostTest.cpp
        Runtime/DatagramTransportTest.cpp
        Session/SessionRegistryTest.cpp
        Net/TransportTest.cpp)

target_include_directories(ServerCoreTests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src)

# 검사마다 다른 포트를 쓰고 Debug와 Release도 서로 다른 블록을 쓴다. 같은 포트를 잇달아
# 쓰면 앞 회차가 남긴 연결이 TIME_WAIT에 있는 동안 다음 bind가 실패할 수 있고, 그러면
# 코드가 아니라 앞 회차 때문에 붉어진다.
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SERVERCORE_TEST_PORT_BASE 17000)
else ()
    set(SERVERCORE_TEST_PORT_BASE 17050)
endif ()

target_compile_definitions(ServerCoreTests
        PRIVATE
        SERVERCORE_ENABLE_TEST_HOOKS=1
        SERVERCORE_TEST_PORT_BASE=${SERVERCORE_TEST_PORT_BASE}
        SERVERCORE_PROTOCOL_VECTOR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Protocol/FramingVectors.txt")

target_link_libraries(ServerCoreTests PRIVATE ServerCore::ServerCore)

if (MSVC)
    # 시험 실행 파일도 ServerCore 정적 라이브러리와 같은 CRT로 링크한다. 이 값은 최상위
    # CMake cache의 기본값과 독립적으로 ServerCore target 계약을 따른다.
    set_property(TARGET ServerCoreTests PROPERTY
            MSVC_RUNTIME_LIBRARY "${ServerCoreMsvcRuntimeLibrary}")
    target_compile_options(ServerCoreTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif ()

foreach (test_name IN LISTS SERVERCORE_TEST_NAMES)
    add_test(NAME ServerCore.${test_name} COMMAND ServerCoreTests ${test_name})
endforeach ()

# 포트를 여는 검사에만 시간 제한을 건다. 60초는 정상 경로보다 한참 넉넉하다. 이 값은 검사가
# 얼마나 걸리는지를 말하는 것이 아니라, 매달린 것을 언제 실패로 판정할지를 말한다.
foreach (test_name IN LISTS SERVERCORE_NET_TEST_NAMES SERVERCORE_HOST_TEST_NAMES)
    set_tests_properties(ServerCore.${test_name} PROPERTIES
            TIMEOUT 60
            RESOURCE_LOCK ServerCore.TcpPorts)
endforeach ()

# UDP checks use ephemeral loopback ports and bounded receive waits.
foreach (test_name IN LISTS SERVERCORE_DATAGRAM_TEST_NAMES)
    set_tests_properties(ServerCore.${test_name} PROPERTIES
            TIMEOUT 60
            RESOURCE_LOCK ServerCore.UdpPorts)
endforeach ()

# 이 시험이 위 목록과 실행 파일의 등록표가 같은지 본다. 어느 한쪽에만 있는 이름이 있으면
# 그 이름을 적어 실패한다.
add_test(NAME ServerCore.Harness.RegisteredChecksMatchCtest
        COMMAND ServerCoreTests --verify-list ${SERVERCORE_TEST_NAMES})

# 설치한 정적 라이브러리와 소스 트리는 모두 별도 CMake 프로젝트에서 같은 target 이름으로 소비할
# 수 있어야 한다. 이 검사는 C++ 등록 검사와 성질이 달라 목록 동기화 시험에는 넣지 않는다. build
# 아래의 전용 경로에서 소스 트리 소비와 설치 prefix 소비를 각각 구성·빌드·실행한다.
if (CMAKE_CONFIGURATION_TYPES)
    set(ServerCorePackageMultiConfig TRUE)
else ()
    set(ServerCorePackageMultiConfig FALSE)
endif ()

if (MSVC)
    set(ServerCorePackageIsMsvc TRUE)
else ()
    set(ServerCorePackageIsMsvc FALSE)
endif ()

# 보통 설치·소비 경로와, producer에 /MT[d] 전역값을 줘도 ServerCore가 /MD[d]를 유지하는
# 경로를 함께 확인한다. 후자는 의도적으로 다른 CRT 소비자가 LNK2038로 거절되는지도 본다.
add_test(NAME ServerCore.CMake.InstallAndSourceTreeConsume
        COMMAND "${CMAKE_COMMAND}"
        "-DSERVERCORE_PACKAGE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DSERVERCORE_PACKAGE_BINARY_DIR=${PROJECT_BINARY_DIR}"
        "-DSERVERCORE_PACKAGE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/PackageConsumer"
        "-DSERVERCORE_PACKAGE_GENERATOR=${CMAKE_GENERATOR}"
        "-DSERVERCORE_PACKAGE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DSERVERCORE_PACKAGE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DSERVERCORE_PACKAGE_RC_COMPILER=${CMAKE_RC_COMPILER}"
        "-DSERVERCORE_PACKAGE_MT=${CMAKE_MT}"
        "-DSERVERCORE_PACKAGE_CONFIGURATION=$<CONFIG>"
        "-DSERVERCORE_PACKAGE_MULTI_CONFIG=${ServerCorePackageMultiConfig}"
        "-DSERVERCORE_PACKAGE_IS_MSVC=${ServerCorePackageIsMsvc}"
        "-DSERVERCORE_PACKAGE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DSERVERCORE_PACKAGE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DSERVERCORE_PACKAGE_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        "-DSERVERCORE_PACKAGE_VERSION=${ServerCore_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CMakePackageTest.cmake")
set_tests_properties(ServerCore.CMake.InstallAndSourceTreeConsume PROPERTIES TIMEOUT 180)
