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
        Error.ResultFailureDoesNotConstructValue
        Error.ResultSupportsNonDefaultMoveOnlyValues
        Core.JobQueueOwnsMoveOnlyJobs
        Core.JobQueueRejectsEmptyLegacyJobs
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
        Logging.ReplacingPublishesTheNewLoggerBeforeDestroyingTheOld
        Clock.MillisecondsSinceProcessStartIsMonotonic
        Config.LoadsFlatSnapshot
        Config.ReportsMissingValuesAndFiles
        Config.ValidatesUtf8Paths
        Config.RejectsInvalidDocuments
        Config.RejectsInvalidIntegers)

set(SERVERCORE_PROTOCOL_TEST_NAMES
        Protocol.BinaryMessageEnvelope
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
        Protocol.JsonEntryPointsShareValidation
        Protocol.MessageEnvelopeValidationIsSymmetric
        Protocol.JsonDumpRejectsUnsafeValues
        Protocol.MessageRejectsUnsafeOutboundJson
        Protocol.MessageRejectsNonObjectBodies
        Protocol.JsonTextConstructorsKeepTheirType
        Protocol.ParseMessageMovesTheParsedBody
        Protocol.JsonObjectKeysResistHashFlooding
        Protocol.JsonNumbersUnderflowToSignedZero
        Protocol.JsonParseLimitsValueCount)

set(SERVERCORE_JOB_TEST_NAMES
        Runtime.RequestLimiterBudgetsAndLease
        Runtime.RequestLimiterExpiryAndConcurrency
        Runtime.RequestLimiterEvictsFullBucketsUnderPressure
        Runtime.TimerSchedulerAdmissionAndReschedule
        Runtime.TimerSchedulerRepeatsAndCancellation
        Runtime.TimerSchedulerShutdownAndExecutorPressure
        Runtime.TimerSchedulerSaturationBacksOffAndServesEarliestFirst
        Runtime.TaskGroupBoundsAndCollection
        Runtime.TaskGroupCancellationAndDeadlines
        Runtime.TaskGroupWaitAndShutdownRace
        Runtime.ChannelLimitsAndDrain
        Runtime.ChannelNotificationsAndCancellation
        Runtime.LatestValueVersionsAndRetirement
        Core.CompletionResetQuiesces
        Runtime.KeyedExecutorFifoAndParallelism
        Runtime.KeyedExecutorRoundRobin
        Runtime.KeyedExecutorLimitsAndCancellation
        Runtime.KeyedExecutorDeadlineShutdownAndReentrancy
        Runtime.TaskExecutorMoveOnlyOwnership
        Runtime.JobRunnerMoveOnlyOwnership
        Runtime.BoundedRunnerReservations
        Runtime.RunnerChargesExecutingCaptures
        Runtime.SessionTaskReturnsThroughReservation
        Runtime.SessionTaskRejectsEmptyLegacyWork
        Runtime.SessionTaskCancelsQueuedAndRunning
        Runtime.TaskCompletionCleanupAndRejection
        Runtime.TaskExecutorBoundedAdmission
        Runtime.TaskExecutorQueuedCancellation
        Runtime.TaskExecutorDeadlinesAndExceptions
        Runtime.TaskExecutorShutdownAndReentrancy
        Runtime.TaskExecutorCompletionRace
        Core.JobQueueRunsSnapshotOnly
        Core.JobQueueAcceptsConcurrentPosts
        Runtime.JobRunnerDrainsAcceptedJobsOnStop
        Runtime.JobRunnerReportsThreadAffinity
        Runtime.PeriodicRunnerInvokesThroughJobRunner
        Runtime.PeriodicRunnerCountsSkippedPeriods
        Runtime.PeriodicRunnerStopsSafelyAfterJobRunnerDestruction
        Runtime.PeriodicRunnerConcurrentStopsShareCompletedJoin
        Runtime.PeriodicRunnerRejectsUnboundedPeriod)

set(SERVERCORE_SESSION_TEST_NAMES
        SessionRegistry.RegistryIssuesValidDistinctIds
        SessionRegistry.RegistryRegistersFindsAndUnregistersSessions
        SessionRegistry.RegistryAllowsFailureCleanupFromAnyThread
        SessionRegistry.RegistryRejectsInvalidAndDuplicateRegistrations
        SessionRegistry.RegistryRejectsUnissuedAndReusedRegistrations
        SessionRegistry.RegistryDiscardsUnregisteredIssueReservations
        SessionRegistry.RegistrySerializesConcurrentReservationDiscards
        SessionRegistry.SessionMarksAuthenticationWithoutOwningIdentityPolicy
        SessionRegistry.ForEachUsesSnapshotSoVisitorMayRemoveSessions
        SessionRegistry.ForEachReportsSnapshotAllocationFailure)

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
        Runtime.SessionCapacityNotifications
        Runtime.HostBinaryAndUdpLifetime
        Runtime.DatagramRegistryAndBinary
        Runtime.HostInputAdmission
        Runtime.HostGracefulDrain
        Runtime.HostConcurrentIsolation
        Runtime.HostCapacityCallbackOutsideSessionLocks
        Runtime.HostControlLaneFitsSessionWork
        Runtime.HostDrainAcceptsGameContinuations
        Runtime.HostBinaryGracefulClose
        Runtime.HostBoundsFramesPerReceiveJob
        Runtime.HostRejectsUnsendableInputRate
        Runtime.HostObservationAndLogBoundaries
        Runtime.HostRejectsLateSetupUniformly
        Runtime.HostLimitsMessageFailureLogs
        Runtime.HostRejectsReceiveBudgetBelowOneReceive
        Runtime.HostLimitsJsonValuesPerMessage
        Transport.SendCapacityRetainsPartialStorage
        Transport.SendCapacitySharesBudgetWithoutMissedWake
        Transport.SendCapacityValidatesAndCancels
        Transport.SendCapacityCloseAndReentrancy
        Transport.SendCapacityResetWaitsForCallback
        Transport.PauseReceiveBeforeStart
        Transport.ResumeReceiveBeforeStart
        Transport.ReceiveFlowControlSerializesCallbacks
        Transport.SendWhileReceivePaused
        Transport.PausedPeerEofDisconnectsOnce
        Transport.SendQueueLimitsConfiguration
        Transport.SendQueueRetainsPartiallySentStorage
        Transport.AcceptorReusesHandlerStateAndReleasesOnStop
        Transport.SharedSendBudgetSerializesReservations
        Transport.IoContextStartsAndStops
        Transport.ListenRejectsInvalidEndpoint
        Transport.AcceptorCanListenAgainAfterStop
        Transport.AcceptorRetriesAfterAcceptResourceFailure
        Transport.ConnectionOutlivesIoContext
        Transport.PeerResetReportsErrorSource
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
        Transport.AcceptorStopDoesNotTimeOutActiveCompletionHandoff
        Transport.CloseAfterSendWithUnreadInputDeliversQueuedBytes)

# 프로세스의 descriptor 한도를 실제로 낮춰 accept4가 EMFILE을 돌려주게 하는 검사다. Windows에는
# 소켓 수를 이렇게 낮출 한도가 없으므로 Linux에서만 등록한다.
if (NOT WIN32)
    list(APPEND SERVERCORE_NET_TEST_NAMES Transport.AcceptorSurvivesDescriptorExhaustion)
else ()
    # IOCP worker가 수락 인계 중 IoContext의 잠금을 잡는 경로는 Windows 구현에만 있다.
    list(APPEND SERVERCORE_NET_TEST_NAMES Transport.IoContextStopDuringAcceptHandoffReturns
            Transport.AcceptorStopWaitsForBusyIoWorkers)
endif ()

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
        Runtime.DatagramSocketEndpointBoundaryConformance
        Runtime.DatagramSocketEmptyAndTruncatedDatagrams
        Runtime.DatagramBindLifecycleAndWinsockIsolation
        Runtime.DatagramRegistrationLifecycle
        Runtime.DatagramGenericRoundTripAndSendSequence
        Runtime.DatagramInvalidPacketsCannotRebindOrPoisonSequence
        Runtime.DatagramEndpointMigrationIsOptIn
        Runtime.DatagramSequenceJumpIsBounded
        Runtime.DatagramReceiverCallbacksCanSendUnregisterAndClose
        Runtime.DatagramAdmissionCallbacksRespectRegistrationLifetime
        Runtime.DatagramCallbackExceptionsEndPoll
        Runtime.DatagramBudgetCountsRejectedAndEmptyPackets
        Runtime.DatagramByteBudgetAndEmptyCallbacksPreservePendingPackets
        Runtime.DatagramTruncatedDatagramsRecover)

set(SERVERCORE_WEB_TEST_NAMES
        Web.GroupedPoliciesAndJson
        Web.CorsPreflightAndCredentials
        Web.TrustedProxyBoundaries
        Web.TrustedProxyRightAlignsForwardedFields
        Web.RequestFieldsAndEncoding
        Web.CookiesValidateAndPreserveDuplicates
        Web.MultipartArbitraryChunkBoundaries
        Web.MultipartBoundsAndOwnedEvents
        Web.MultipartRejectsMalformedAndLimits
        Web.WebSocketNegotiationAndFragments
        Web.WebSocketHeartbeatCorrelation
        Web.WebSocketCloseFromApplicationThread
        Web.WebSocketReceivePauseAndResume
        Web.WebSocketReceivePauseSuspendsHeartbeat
        Web.PolicyCallbacksDoNotStallDeadlines
        Web.ProgressEventsWakeWithoutMaintenance
        Web.EarlyWebSocketDataResumesAfterPolicy
        Web.PolicyResponsesKeepConnectionAlive
        Web.CorsPreflightReachesPolicyBeforeMethodCheck
        Web.StreamingUploadOutlivesRequestDeadline
        Web.WebSocketFragmentBackpressure
        Web.StreamingUploadBackpressure
        Web.StreamingBodyChargesPieceBookkeeping
        Web.RequestPolicyAndWebSocketGate
        Web.GracefulDrain
        Web.FileRangesAndConditionals
        Web.SendFileRejectsWindowsPathAliases
    Web.AsyncRequestOwnershipAndIoProgress
    Web.AsyncRetainedRequestBudget
        Web.AsyncPipelineOrderAndWebSocketUpgrade
        Web.AsyncCancellationAndShutdown
        Web.AsyncHandlerDeadline
        Web.StreamingFramingAndHead
        Web.StreamingSseOutlivesInboundIdle
        Web.StreamingBackpressureAndStall
        Web.StreamingFileIntegrityAndHead
    Web.StreamingResponseFraming
    Web.ResponseWaitLifetimeAndReentrancy
        Web.StreamingResponseRejectsInvalidHeaders
        Web.HttpChunkEncoding
        Web.SseEncoding
        Web.FileTaskQueuedCancellation
        Web.RoutePatternRegistration
        Web.RoutePatternDecodingAndOwnership
        Web.RoutePatternPrecedenceAndMethods
        Web.RoutePatternWebSocketCoexistence
        Web.RoutePatternLifecycle
        Web.HttpResponseSemantics
        Web.WebSocketSendValidationSymmetry
        Web.HttpParserConformance
        Web.HttpParserRejectsAmbiguousRequests
        Web.HttpParserReportsConsumedBytes
        Web.HttpParserInteroperability
        Web.Http10AndLenientRequestLines
        Web.PolicyHeadersKeepListFields
        Web.SilentConnectionsExpire
        Web.DrainBoundsIdleKeepAliveConnections
        Web.WebSocketProtocolConformance
        Web.HttpSocketIntegration
        Web.WebSocketSocketIntegration
        Web.LimitsAndShutdown
        Web.WebSocketRejectsProtocolErrors
        Web.ConnectionLimitAndActiveShutdown)

list(APPEND SERVERCORE_TEST_NAMES
        Core.NumericEndpointBoundaries
        Core.AtomicFilePublication
        Runtime.LogicalTickPoliciesAndCancellation
        Runtime.TickRunnerRearmsOnFixedRateGrid
        Runtime.DatagramReadinessAndCancellation
        Runtime.DatagramExecutorPressureAndStop
        Runtime.DatagramPumpRearmDoesNotWaitOnMonitor
        Runtime.DatagramPumpCountsBatchFailures
        Runtime.DatagramSendRateLimits
        Runtime.OutboundFifoLatestAndBatch
        Runtime.OutboundExpiryAndReentrancy
        Runtime.OutboundCapacityWakeBeforeArmIsKept
        Runtime.OutboundQueueFitsOneSchedulerSlot
        Observability.StructuredRecordsAndBounds
        Observability.StructuredLoggerOwnershipAndDrain
        Observability.ObservationAvailabilityAndLifecycle
        Core.AtomicFileFailureAndCleanup
        Core.AtomicFileSyncFailureIsTerminal
        Protocol.BinaryWireAndBounds
        Protocol.BinaryVersionNegotiation
        Core.CidrMasksAndFamilies
        Transport.Ipv6ConnectionEndpoints
        Transport.Ipv6OnlyAndDualStack
        Transport.Ipv6DatagramEndpoints
        Runtime.Ipv6DatagramSessionEndpoints
        Observability.LoggerFilteringAndRotation
        Observability.LoggerRotationFailureKeepsArchives
        Observability.LoggerStartCanRetryAfterFailure
        Observability.HistogramAndPrometheus
        Observability.TaskMetrics
        Observability.HttpMetricsAndTracing
        ${SERVERCORE_PROTOCOL_TEST_NAMES}
        ${SERVERCORE_JOB_TEST_NAMES}
        ${SERVERCORE_SESSION_TEST_NAMES}
        ${SERVERCORE_DISPATCH_TEST_NAMES}
        ${SERVERCORE_NET_TEST_NAMES}
        ${SERVERCORE_HOST_TEST_NAMES}
        ${SERVERCORE_DATAGRAM_TEST_NAMES}
        ${SERVERCORE_WEB_TEST_NAMES})

add_executable(ServerCoreTests
        Core/EndpointTest.cpp
        Core/AtomicFileTest.cpp
        Runtime/GameExecutionTest.cpp
        Runtime/DatagramExecutionTest.cpp
        Observability/ObservationTest.cpp
        Protocol/BinaryIOTest.cpp
        Net/EndpointTest.cpp
        Runtime/RequestLimiterTest.cpp
        Web/PolicyTest.cpp
        Web/WebDataTest.cpp
        TestHarness.cpp
        Observability/OperationsTest.cpp
        Web/InputLifecycleTest.cpp
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
        Runtime/TaskExecutorTest.cpp
        Runtime/SessionTaskTest.cpp
        Runtime/ChannelTest.cpp
        Runtime/KeyedExecutorTest.cpp
        Runtime/TimerSchedulerTest.cpp
        Runtime/TaskGroupTest.cpp
        Runtime/GameHostExtensionsTest.cpp
        Protocol/BinaryMessageTest.cpp
        Runtime/ServerHostTest.cpp
        Runtime/DatagramTransportTest.cpp
        Session/SessionRegistryTest.cpp
        Net/TransportTest.cpp
        Net/FlowControlTest.cpp
        Net/ReceiveFlowControlTest.cpp
        Web/WebTest.cpp
        Web/RoutingTest.cpp)
target_sources(ServerCoreTests PRIVATE Web/AsyncStreamingTest.cpp Web/StreamingEncodingTest.cpp)

if (SERVERCORE_BUILD_C_API)
    target_sources(ServerCoreTests PRIVATE C/WebAbiTest.cpp C/NetAbiTest.cpp C/ReadinessAbiTest.cpp C/RuntimeAbiTest.cpp
        C/EndpointAbiTest.cpp C/WebDataAbiTest.cpp C/WebPolicyAbiTest.cpp C/UtilityAbiTest.cpp
        C/ObservationAbiTest.cpp C/DatagramAbiTest.cpp C/GameExecutionAbiTest.cpp)
    list(APPEND SERVERCORE_TEST_NAMES CAbi.WebHttpOwnershipAndStreaming CAbi.WebSocketOwnershipAndOverflow
        CAbi.EndpointValues CAbi.TcpIpv6Endpoints CAbi.RequestDataOwnedViews CAbi.MultipartOwnedEventsAndFailure
        CAbi.UtilityOwnedHandles
        CAbi.ObservationContracts
        CAbi.LogicalTicksOwnership
        CAbi.OutboundOwnershipAndBatch
        CAbi.DatagramOwnedEventsAndBackpressure
        CAbi.DatagramCodecAndValidation
        CAbi.PolicyOwnershipAndLimits CAbi.PolicyPreflightSocket
        CAbi.WebPolicyBeforeUpgrade CAbi.WebStreamingBodyOwnership CAbi.WebDrainMetricsAndLogger
            CAbi.TcpOwnershipAndTerminal CAbi.TcpRetainedEventOverflow
            CAbi.ReadinessLifetimeAndRaces CAbi.ReadinessWebAndTcp
            CAbi.RuntimeWorkAndNotifications CAbi.RuntimeTimersAndGroups
            CAbi.RuntimeDeferredOwnerDestruction CAbi.RuntimeShutdown)
    # C ABI 구현은 ServerCoreTestHooks에 들어 있다. ServerCore::CAbi는 설치되는 ServerCore를 끌어오므로
    # 여기서 링크하지 않는다(같은 심볼이 두 번 들어온다).
endif ()

target_include_directories(ServerCoreTests
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src)

# 시험은 고정 포트 블록을 쓰지 않는다. TCP 리스너가 필요하면 운영체제에게 빈 번호를 받는다
# (SocketTestSupport.h의 ServerCoreTest::FreeLoopbackTcpPort). 그래서 여러 빌드 트리·구성이 동시에
# 시험을 돌려도, 이 기계의 다른 프로그램이 어떤 포트를 쓰고 있어도 부딪히지 않는다.
target_compile_definitions(ServerCoreTests
        PRIVATE
        SERVERCORE_ENABLE_TEST_HOOKS=1
        SERVERCORE_PROTOCOL_VECTOR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Protocol/FramingVectors.txt")

# 시험은 hook이 든 ServerCoreTestHooks를 링크한다. 설치되는 ServerCore에는 hook이 없다(BUILD-1).
target_link_libraries(ServerCoreTests PRIVATE ServerCoreTestHooks)
if (WIN32 AND SERVERCORE_BUILD_SHARED)
    add_custom_command(TARGET ServerCoreTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:ServerCoreTestHooks> $<TARGET_FILE_DIR:ServerCoreTests>)
endif ()

if (MSVC)
    # 시험 실행 파일도 ServerCore 정적 라이브러리와 같은 CRT로 링크한다. 이 값은 최상위
    # CMake cache의 기본값과 독립적으로 ServerCore target 계약을 따른다.
    set_property(TARGET ServerCoreTests PROPERTY
            MSVC_RUNTIME_LIBRARY "${ServerCoreMsvcRuntimeLibrary}")
    target_compile_options(ServerCoreTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif ()

foreach (test_name IN LISTS SERVERCORE_TEST_NAMES)
    add_test(NAME ServerCore.${test_name} COMMAND ServerCoreTests ${test_name})
    # ServerCoreTest::Skip이 기록된 검사는 SkipExitCode(TestHarness.h, 77)로 끝난다. ctest는 그것을
    # 통과가 아니라 건너뜀으로 센다(BUILD-4: IPv6가 없는 환경의 IPv6 검사).
    set_tests_properties(ServerCore.${test_name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach ()

# 포트를 여는 검사에만 시간 제한을 건다. 60초는 정상 경로보다 한참 넉넉하다. 이 값은 검사가
# 얼마나 걸리는지를 말하는 것이 아니라, 매달린 것을 언제 실패로 판정할지를 말한다.
foreach (test_name IN LISTS SERVERCORE_NET_TEST_NAMES SERVERCORE_HOST_TEST_NAMES SERVERCORE_WEB_TEST_NAMES)
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

set_tests_properties(ServerCore.Transport.Ipv6ConnectionEndpoints ServerCore.Transport.Ipv6OnlyAndDualStack
    ServerCore.Transport.Ipv6DatagramEndpoints ServerCore.Runtime.Ipv6DatagramSessionEndpoints
    PROPERTIES TIMEOUT 60 RESOURCE_LOCK ServerCore.TcpPorts)

foreach (test_name IN LISTS SERVERCORE_JOB_TEST_NAMES)
    set_tests_properties(ServerCore.${test_name} PROPERTIES TIMEOUT 60)
endforeach ()

set_tests_properties(ServerCore.Runtime.DatagramPumpRearmDoesNotWaitOnMonitor
    ServerCore.Runtime.DatagramPumpCountsBatchFailures
    PROPERTIES TIMEOUT 60 RESOURCE_LOCK ServerCore.UdpPorts)
set_tests_properties(ServerCore.Runtime.LogicalTickPoliciesAndCancellation
    ServerCore.Runtime.TickRunnerRearmsOnFixedRateGrid
    ServerCore.Runtime.OutboundFifoLatestAndBatch ServerCore.Runtime.OutboundExpiryAndReentrancy
    ServerCore.Runtime.OutboundCapacityWakeBeforeArmIsKept
    ServerCore.Runtime.OutboundQueueFitsOneSchedulerSlot
    PROPERTIES TIMEOUT 60)
set_tests_properties(ServerCore.Runtime.DatagramReadinessAndCancellation
    ServerCore.Runtime.DatagramExecutorPressureAndStop ServerCore.Runtime.DatagramSendRateLimits
    PROPERTIES TIMEOUT 60 RESOURCE_LOCK ServerCore.UdpPorts)

foreach(test_name IN LISTS SERVERCORE_TEST_NAMES)
    if (test_name MATCHES "^(CAbi|Observability)\\.")
        set_tests_properties(ServerCore.${test_name} PROPERTIES TIMEOUT 60 RESOURCE_LOCK ServerCore.TcpPorts)
    endif ()
endforeach ()

# 이 시험이 위 목록과 실행 파일의 등록표가 같은지 본다. 어느 한쪽에만 있는 이름이 있으면
# 그 이름을 적어 실패한다.
add_test(NAME ServerCore.Harness.RegisteredChecksMatchCtest
        COMMAND ServerCoreTests --verify-list ${SERVERCORE_TEST_NAMES})

add_test(NAME ServerCore.Architecture.LayerBoundaries
        COMMAND "${CMAKE_COMMAND}" "-DSERVERCORE_LAYER_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckLayers.cmake")

# 공개 헤더가 저마다 스스로 필요한 것을 모두 포함하는지(자기완결) 본다. src/PublicHeaderCompileCheck.cpp는
# 공개 헤더 전부를 한 번역 단위에 넣으므로, 뒤의 헤더는 앞에서 들어온 선언에 기대도 통과한다. 그래서
# include/ServerCore 아래 헤더마다 그 헤더 하나만 포함하는 번역 단위를 만들어 C++로 컴파일하고, C ABI
# 헤더는 C 소비자처럼 C로 한 번 더 컴파일한다. 자기완결이 아닌 헤더가 있으면 빌드가 그 번역 단위에서
# 멈춘다.
#
# 목록은 손으로 적지 않고 include 트리를 그대로 훑는다. 손으로 적으면 새 헤더를 빠뜨려도 신호가 없다.
# 번역 단위 이름은 헤더 경로의 '/'를 '.'로 바꾼 것이다. 파일 이름이 같은 헤더(C/Endpoint.h와
# Core/Endpoint.h)도 목적 파일이 겹치지 않고, 아래 시험이 목적 파일 이름에서 헤더를 되짚을 수 있다.
file(GLOB_RECURSE ServerCorePublicHeaders CONFIGURE_DEPENDS
        RELATIVE "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/include/ServerCore/*.h")
if (NOT ServerCorePublicHeaders)
    message(FATAL_ERROR "No public headers under ${PROJECT_SOURCE_DIR}/include/ServerCore.")
endif ()
set(ServerCoreHeaderCheckRoot "${CMAKE_CURRENT_BINARY_DIR}/PublicHeaders")
set(ServerCoreHeaderCheckCxxSources)
set(ServerCoreHeaderCheckCSources)
foreach (header IN LISTS ServerCorePublicHeaders)
    string(REPLACE "/" "." stem "${header}")
    # file(CONFIGURE)는 내용이 같으면 다시 쓰지 않으므로, 재구성만으로 다시 컴파일되지 않는다.
    file(CONFIGURE OUTPUT "${ServerCoreHeaderCheckRoot}/Cxx/${stem}.cpp"
            CONTENT "#include \"${header}\"\n")
    list(APPEND ServerCoreHeaderCheckCxxSources "${ServerCoreHeaderCheckRoot}/Cxx/${stem}.cpp")
    if (SERVERCORE_BUILD_C_API AND header MATCHES "^ServerCore/C/")
        file(CONFIGURE OUTPUT "${ServerCoreHeaderCheckRoot}/C/${stem}.c"
                CONTENT "#include \"${header}\"\n")
        list(APPEND ServerCoreHeaderCheckCSources "${ServerCoreHeaderCheckRoot}/C/${stem}.c")
    endif ()
endforeach ()
add_library(ServerCorePublicHeadersCxx OBJECT ${ServerCoreHeaderCheckCxxSources})
target_link_libraries(ServerCorePublicHeadersCxx PRIVATE ServerCore)
set(ServerCoreHeaderCheckObjects
        "set(SERVERCORE_HEADERS_CXX_OBJECTS \"$<TARGET_OBJECTS:ServerCorePublicHeadersCxx>\")\n")
if (SERVERCORE_BUILD_C_API)
    enable_language(C)
    add_library(ServerCorePublicHeadersC OBJECT ${ServerCoreHeaderCheckCSources})
    # C 소비자처럼 C ABI target만 링크한다. 네이티브 C++ 요구 사항은 전이되지 않는다.
    target_link_libraries(ServerCorePublicHeadersC PRIVATE ServerCore::CAbi)
    string(APPEND ServerCoreHeaderCheckObjects
            "set(SERVERCORE_HEADERS_C_OBJECTS \"$<TARGET_OBJECTS:ServerCorePublicHeadersC>\")\n")
endif ()
file(GENERATE OUTPUT "${ServerCoreHeaderCheckRoot}/Objects-$<CONFIG>.cmake"
        CONTENT "${ServerCoreHeaderCheckObjects}")

# 빌드가 통과했다는 것만으로는 "헤더를 하나씩 컴파일했다"를 말할 수 없다. 이 시험이 시험 시점의 소스
# 트리에서 헤더 목록을 다시 읽어, 헤더마다 그 번역 단위의 목적 파일이 있는지와 그 수가 공개 헤더 수와
# 같은지를 본다. PublicHeaderCompileCheck.cpp가 공개 헤더를 빠짐없이 포함하는지도 여기서 본다.
add_test(NAME ServerCore.Architecture.PublicHeadersCompile
        COMMAND "${CMAKE_COMMAND}"
        "-DSERVERCORE_HEADERS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DSERVERCORE_HEADERS_OBJECTS=${ServerCoreHeaderCheckRoot}/Objects-$<CONFIG>.cmake"
        "-DSERVERCORE_HEADERS_C_API=${SERVERCORE_BUILD_C_API}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/PublicHeadersTest.cmake")

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
        "-DSERVERCORE_PACKAGE_SHARED=${SERVERCORE_BUILD_SHARED}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CMakePackageTest.cmake")
# cmake --install은 producer build 디렉터리에 install_manifest.txt를 쓰므로, 같은 build를 설치하는
# 시험끼리는 ServerCore.InstallTree로 직렬화한다.
set_tests_properties(ServerCore.CMake.InstallAndSourceTreeConsume PROPERTIES TIMEOUT 600
        RESOURCE_LOCK ServerCore.InstallTree)

if (WIN32 AND MSVC AND SERVERCORE_BUILD_SHARED)
    add_test(NAME ServerCore.CMake.SharedAbiCompatibility COMMAND "${CMAKE_COMMAND}"
            "-DSERVERCORE_ABI_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DSERVERCORE_ABI_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DSERVERCORE_ABI_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/SharedAbiConsumer-$<CONFIG>"
            "-DSERVERCORE_ABI_GENERATOR=${CMAKE_GENERATOR}"
            "-DSERVERCORE_ABI_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DSERVERCORE_ABI_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DSERVERCORE_ABI_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DSERVERCORE_ABI_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DSERVERCORE_ABI_CONFIGURATION=$<CONFIG>"
            "-DSERVERCORE_ABI_MULTI_CONFIG=${ServerCorePackageMultiConfig}"
            "-DSERVERCORE_ABI_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            "-DSERVERCORE_ABI_LIBRARY=$<TARGET_LINKER_FILE:ServerCore>"
            "-DSERVERCORE_ABI_RUNTIME=$<TARGET_FILE:ServerCore>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/SharedAbiTest.cmake")
    set_tests_properties(ServerCore.CMake.SharedAbiCompatibility PROPERTIES TIMEOUT 240)
endif ()

if (SERVERCORE_BUILD_C_API AND SERVERCORE_BUILD_SHARED)
    add_test(NAME ServerCore.CMake.CAbiPackageConsume COMMAND "${CMAKE_COMMAND}"
            "-DSERVERCORE_PACKAGE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DSERVERCORE_PACKAGE_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DSERVERCORE_PACKAGE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/CAbiPackageConsumer-$<CONFIG>"
            "-DSERVERCORE_PACKAGE_GENERATOR=${CMAKE_GENERATOR}"
            "-DSERVERCORE_PACKAGE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DSERVERCORE_PACKAGE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DSERVERCORE_PACKAGE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DSERVERCORE_PACKAGE_CONFIGURATION=$<CONFIG>"
            "-DSERVERCORE_PACKAGE_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            "-DSERVERCORE_PACKAGE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DSERVERCORE_PACKAGE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CAbiPackageTest.cmake")
    set_tests_properties(ServerCore.CMake.CAbiPackageConsume PROPERTIES TIMEOUT 180
            RESOURCE_LOCK ServerCore.InstallTree)
endif ()

# 설치 결과물의 심볼을 dumpbin(MSVC) 또는 nm으로 직접 읽는다. 도구가 없으면 건너뛰지 않고 실패한다.
if (MSVC)
    get_filename_component(ServerCoreCompilerDirectory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(SERVERCORE_DUMPBIN dumpbin HINTS "${ServerCoreCompilerDirectory}")
    set(ServerCoreSymbolTool "${SERVERCORE_DUMPBIN}")
    set(ServerCoreSymbolToolKind dumpbin)
else ()
    set(ServerCoreSymbolTool "${CMAKE_NM}")
    set(ServerCoreSymbolToolKind nm)
endif ()
if (WIN32 AND SERVERCORE_BUILD_SHARED)
    set(ServerCoreInstalledLibraryDirectory "${CMAKE_INSTALL_BINDIR}")
else ()
    set(ServerCoreInstalledLibraryDirectory "${CMAKE_INSTALL_LIBDIR}")
endif ()
add_test(NAME ServerCore.CMake.InstalledLibraryHasNoTestHooks COMMAND "${CMAKE_COMMAND}"
        -DSERVERCORE_SYMBOLS_MODE=hooks
        "-DSERVERCORE_SYMBOLS_BINARY_DIR=${PROJECT_BINARY_DIR}"
        "-DSERVERCORE_SYMBOLS_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/LibrarySymbols-$<CONFIG>"
        "-DSERVERCORE_SYMBOLS_CONFIGURATION=$<CONFIG>"
        "-DSERVERCORE_SYMBOLS_HOOKS_FILE=$<TARGET_FILE:ServerCoreTestHooks>"
        "-DSERVERCORE_SYMBOLS_INSTALLED_RELATIVE=${ServerCoreInstalledLibraryDirectory}/$<TARGET_FILE_NAME:ServerCore>"
        "-DSERVERCORE_SYMBOLS_TOOL=${ServerCoreSymbolTool}"
        "-DSERVERCORE_SYMBOLS_TOOL_KIND=${ServerCoreSymbolToolKind}"
        "-DSERVERCORE_SYMBOLS_SHARED=${SERVERCORE_BUILD_SHARED}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/LibrarySymbolsTest.cmake")
set_tests_properties(ServerCore.CMake.InstalledLibraryHasNoTestHooks PROPERTIES TIMEOUT 180
        RESOURCE_LOCK ServerCore.InstallTree)

if (SERVERCORE_BUILD_SHARED)
    add_test(NAME ServerCore.CMake.SharedLibraryExportSet COMMAND "${CMAKE_COMMAND}"
            -DSERVERCORE_SYMBOLS_MODE=exports
            "-DSERVERCORE_SYMBOLS_PRODUCT_FILE=$<TARGET_FILE:ServerCore>"
            "-DSERVERCORE_SYMBOLS_C_API=${SERVERCORE_BUILD_C_API}"
            "-DSERVERCORE_SYMBOLS_C_HEADER_DIR=${PROJECT_SOURCE_DIR}/include/ServerCore/C"
            "-DSERVERCORE_SYMBOLS_TOOL=${ServerCoreSymbolTool}"
            "-DSERVERCORE_SYMBOLS_TOOL_KIND=${ServerCoreSymbolToolKind}"
            "-DSERVERCORE_SYMBOLS_SHARED=${SERVERCORE_BUILD_SHARED}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/LibrarySymbolsTest.cmake")
    set_tests_properties(ServerCore.CMake.SharedLibraryExportSet PROPERTIES TIMEOUT 120)
endif ()
