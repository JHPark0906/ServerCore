// ServerHost의 시작·drain·정지와 세션 수명 통지다.
#include "Runtime/ServerHostNetworkSession.h"

namespace ServerCore::Runtime
{
namespace
{
struct CloseNotificationThreadContext
{
    const void* host = nullptr;
    const CloseNotificationThreadContext* previous = nullptr;
};

thread_local const CloseNotificationThreadContext* gCloseNotificationContext = nullptr;

/// <summary>현재 스레드가 이 Host의 OnSessionClosed 안에 중첩된 깊이다.</summary>
[[nodiscard]] std::size_t CurrentCloseNotificationDepth(const void* const host) noexcept
{
    std::size_t depth = 0;
    for (const CloseNotificationThreadContext* context = gCloseNotificationContext;
        context != nullptr; context = context->previous)
    {
        if (context->host == host)
        {
            ++depth;
        }
    }
    return depth;
}

/// <summary>다른 Host까지 중첩될 수 있는 종료 통지의 스레드 문맥을 되돌린다.</summary>
class ScopedCloseNotificationContext final
{
public:
    explicit ScopedCloseNotificationContext(const void* const host) noexcept
        : mContext{ host, gCloseNotificationContext }
    {
        gCloseNotificationContext = &mContext;
    }

    ~ScopedCloseNotificationContext() { gCloseNotificationContext = mContext.previous; }

    ScopedCloseNotificationContext(const ScopedCloseNotificationContext&) = delete;
    ScopedCloseNotificationContext& operator=(const ScopedCloseNotificationContext&) = delete;

private:
    CloseNotificationThreadContext mContext;
};
}

Core::Status ServerHost::State::StartJobRunner()
{
    try
    {
        const std::shared_ptr<State> self = shared_from_this();
        std::shared_ptr<std::promise<Core::Status>> bound =
            std::make_shared<std::promise<Core::Status>>();
        std::future<Core::Status> result = bound->get_future();
        mJobThread = std::thread(
            [self, bound]()
            {
                // JobRunner가 대기열을 보기 전에 Registry를 이 실행 스레드에 묶는다. Start 전에
                // 받아 둔 Lease 작업도 여기보다 먼저 실행될 수 없으므로, 첫 작업부터 세션 표를
                // 안전하게 쓸 수 있다.
                const auto publishFailure = [&bound](Core::Status failure) noexcept
                {
                    try
                    {
                        bound->set_value(std::move(failure));
                    }
                    catch (...)
                    {
                        // 이미 성공 결과를 넘긴 뒤 RunUntilStopped가 실패한 경우와, promise 자체의
                        // 예외 모두 아래 Stop으로 수렴한다. 시작 호출자는 parent 참조를 놓았으므로
                        // 아직 결과가 없었다면 broken_promise로라도 반드시 깨어난다.
                    }
                };

                try
                {
                    Core::Status boundStatus = self->mRegistry.BindToCurrentThread();
                    const bool boundSuccessfully = boundStatus.IsOk();
                    bound->set_value(std::move(boundStatus));
                    if (!boundSuccessfully)
                    {
                        self->mJobRunner.RequestStop();
                        return;
                    }

                    self->mJobRunner.RunUntilStopped();
                }
                catch (const std::bad_alloc&)
                {
                    publishFailure(Core::Status::AllocationFailure());
                    self->mJobRunner.RequestStop();
                }
                catch (...)
                {
                    publishFailure(
                        Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError));
                    self->mJobRunner.RequestStop();
                }
            });

        // worker만 promise를 소유하게 한다. set_value 자체가 예외를 내더라도 worker 종료 시
        // future가 broken_promise로 준비되어 Start()가 영구히 기다리지 않는다.
        bound.reset();
        return result.get();
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::system_error& error)
    {
        return PlatformFailureFrom(error);
    }
    catch (const std::future_error& error)
    {
        return PlatformFailureFrom(error);
    }
}

Core::Status ServerHost::State::Start()
{
    std::optional<ServerHostOptions> optionsSnapshot;
    {
        const std::lock_guard<std::mutex> guard(mLifecycleMutex);
        if (mLifecycle == Lifecycle::Starting || mLifecycle == Lifecycle::Running)
        {
            return Core::Status::Fail(
                Core::ErrorCode::AlreadyExists, "ServerHost is already started");
        }
        if (mLifecycle == Lifecycle::Stopping || mLifecycle == Lifecycle::Stopped)
        {
            return Core::Status::Fail(
                Core::ErrorCode::Closed, "ServerHost cannot be started again");
        }
        if (!mConfigured)
        {
            return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost requires ServerHostOptions before Start()");
        }

        try
        {
            optionsSnapshot.emplace(mOptions);
        }
        catch (const std::bad_alloc&)
        {
            return Core::Status::AllocationFailure();
        }
        catch (const std::exception& error)
        {
            return PlatformFailureFrom(error);
        }

        if (mOptions.payloadMode == Protocol::PayloadMode::Binary && !mBinaryHandler)
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        mLifecycle = Lifecycle::Starting;
    }

    const ServerHostOptions& options = *optionsSnapshot;

    try
    {
        try
        {
            // 종료 자체가 새 메모리를 요구하지 않도록, 수락할 수 있는 세션의 소유 슬롯을 포트를
            // 열기 전에 모두 확보한다. Stop은 이 배열을 직접 훑어 Connection을 닫는다. Registry에
            // 들어가기 전 끊긴 세션도 OnDisconnected까지 살아 있어야 하므로 슬롯이 강하게 소유한다.
            const std::lock_guard<std::mutex> lifecycleGuard(mLifecycleMutex);
            mSessionSlots.resize(options.maxConcurrentSessions);
            mMessageLogLimiter =
                std::make_shared<MessageLogLimiter>(options.maxMessageFailureLogsPerSecond);
            mDispatcher.SetLogger(
                mLogger ? std::make_shared<RateLimitedLogger>(mLogger, mMessageLogLimiter)
                        : std::shared_ptr<Core::ILogger>());
            mFreeSessionSlots.reserve(options.maxConcurrentSessions);
            // 낮은 번호부터 쓰도록 역순으로 쌓는다. 종료 순회 순서는 슬롯 배열 순서다.
            for (std::size_t index = options.maxConcurrentSessions; index != 0; --index)
            {
                mFreeSessionSlots.push_back(index - 1);
            }
            mIo = std::make_unique<Net::IoContext>();
            mAcceptor = std::make_unique<Net::Acceptor>();
            mAcceptor->SetLogger(mLogger);
            if (options.parseWorkerThreadCount != 0)
            {
                mParsePool = std::make_unique<ParseWorkerPool>();
            }
        }
        catch (const std::bad_alloc&)
        {
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }

        Core::Status sendLimitsConfigured = mAcceptor->SetSendQueueLimits(
            { Net::SendQueueLimitBytes, options.maxTotalSendQueueCapacityBytes });
        if (!sendLimitsConfigured.IsOk())
        {
            CompleteFailedStart();
            return sendLimitsConfigured;
        }

        Core::Status ioStarted = Core::Status::Ok();
        try
        {
            ioStarted = mIo->Start(options.ioWorkerThreadCount);
        }
        catch (const std::bad_alloc&)
        {
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }
        catch (const std::exception& error)
        {
            CompleteFailedStart();
            return PlatformFailureFrom(error);
        }
        if (!ioStarted.IsOk())
        {
            CompleteFailedStart();
            return ioStarted;
        }

        Core::Status runnerStarted = StartJobRunner();
        if (!runnerStarted.IsOk())
        {
            CompleteFailedStart();
            return runnerStarted;
        }

        if (mParsePool != nullptr)
        {
            Core::Status parseStarted = mParsePool->Start(options.parseWorkerThreadCount);
            if (!parseStarted.IsOk())
            {
                CompleteFailedStart();
                return parseStarted;
            }
        }

        {
            try
            {
                const std::weak_ptr<State> self = weak_from_this();
                mSessionTimeoutRunner = std::make_unique<PeriodicRunner>(
                    mJobRunner.AcquireControlLease(), SessionTimeoutScanPeriod(options),
                    [self]()
                    {
                        if (const std::shared_ptr<State> host = self.lock())
                        {
                            host->CloseExpiredSessionsOnRunner();
                        }
                    });
            }
            catch (const std::bad_alloc&)
            {
                CompleteFailedStart();
                return Core::Status::AllocationFailure();
            }
            catch (const std::exception& error)
            {
                CompleteFailedStart();
                return PlatformFailureFrom(error);
            }

            Core::Status timeoutTimerStarted = mSessionTimeoutRunner->Start();
            if (!timeoutTimerStarted.IsOk())
            {
                CompleteFailedStart();
                return timeoutTimerStarted;
            }
        }

        try
        {
            const std::weak_ptr<State> self = weak_from_this();
            mAcceptor->SetConnectionHandler(
                [self](std::shared_ptr<Net::Connection> connection)
                {
                    if (const std::shared_ptr<State> host = self.lock())
                    {
                        host->OnConnectionAccepted(std::move(connection));
                        return;
                    }

                    connection->Close();
                });
        }
        catch (const std::bad_alloc&)
        {
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }

        try
        {
            auto endpoint = Core::IpEndpoint::Parse(options.listenAddress, options.port);
            Core::Status listened = endpoint.IsOk() ? mAcceptor->Listen(endpoint.Value(),
                                                          options.acceptBacklog, options.ipv6Only)
                                                    : std::move(endpoint).TakeStatus();
            if (!listened.IsOk())
            {
                CompleteFailedStart();
                return listened;
            }

            // 이 시점부터 등록표는 읽기 전용이다. 수락 전이므로 첫 메시지가 등록 중인 표를 볼 수 없다.
            mDispatcher.Freeze();
            mAccepting.store(true, std::memory_order_release);

            Core::Status accepting = mAcceptor->Start(*mIo);
            if (!accepting.IsOk())
            {
                mAccepting.store(false, std::memory_order_release);
                CompleteFailedStart();
                return accepting;
            }
        }
        catch (const std::bad_alloc&)
        {
            mAccepting.store(false, std::memory_order_release);
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }
        catch (const std::exception& error)
        {
            mAccepting.store(false, std::memory_order_release);
            CompleteFailedStart();
            return PlatformFailureFrom(error);
        }
        catch (...)
        {
            mAccepting.store(false, std::memory_order_release);
            CompleteFailedStart();
            return Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        // 수락기가 실제로 동작하지만 Lifecycle은 아직 Starting인 좁은 경계를 고정한다.
        WaitBeforeHostRunningForTest();
#endif

        mPort.store(options.port, std::memory_order_release);
        mRunning.store(true, std::memory_order_release);
        bool stopAfterStart = false;
        {
            const std::lock_guard<std::mutex> guard(mLifecycleMutex);
            mLifecycle = Lifecycle::Running;
            stopAfterStart = mStopRequestedDuringStart;
            mStartSucceeded = !stopAfterStart;
            mLifecycleChanged.notify_all();
        }

        // 수락기가 시작된 아주 짧은 Starting 구간에도 연결 종료 callback이 올 수 있다. 그
        // callback에서 Stop을 요청했다면 callback 자신을 기다리지 말고 여기서 부팅을 끝낸 뒤
        // 정상 종료 순서를 대신 수행한다.
        if (stopAfterStart)
        {
            Stop();
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        }

        return Core::Status::Ok();
    }
    catch (const std::bad_alloc&)
    {
        mAccepting.store(false, std::memory_order_release);
        CompleteFailedStart();
        return Core::Status::AllocationFailure();
    }
    catch (...)
    {
        mAccepting.store(false, std::memory_order_release);
        CompleteFailedStart();
        return Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
    }
}

void ServerHost::State::CompleteFailedStart() noexcept
{
    mAccepting.store(false, std::memory_order_release);
    mRunning.store(false, std::memory_order_release);
    mPort.store(0, std::memory_order_release);

    // PeriodicRunner는 JobRunner에 일을 넣을 수 있으므로, 실행자를 멈추기 전에 timer thread를
    // 먼저 거둔다. 이미 큐에 든 콜백은 mAccepting=false를 보고 아무 세션도 건드리지 않는다.
    if (mSessionTimeoutRunner != nullptr)
    {
        mSessionTimeoutRunner->Stop();
        mSessionTimeoutRunner.reset();
    }

    if (mAcceptor != nullptr)
    {
        mAcceptor->Stop();
    }

    if (mParsePool != nullptr)
    {
        mParsePool->StopAndDiscard();
    }

    // Start()가 mAccepting=true를 공개한 뒤 수락기 준비에서 실패할 수도 있다. 그 짧은 사이에
    // 만들어진 세션도 정상 Stop과 같은 순서로 닫고, overlapped 완료가 모두 돌아와 slot을
    // 반납할 때까지 I/O를 먼저 내리지 않는다.
    CloseAllSessions();
    {
        std::unique_lock<std::mutex> guard(mLifecycleMutex);
        mLifecycleChanged.wait(guard,
            [this]() { return mOutstandingSessions == 0 && mInFlightCloseNotifications == 0; });
    }

    if (mIo != nullptr)
    {
        mIo->Stop();
    }

    mJobRunner.RequestStop();
    if (mJobThread.joinable())
    {
        mJobThread.join();
    }

    // Starting을 기다리기 전 Stop()도 I/O·parse 소유자를 읽어 자기 실행 문맥을 검사한다.
    // 정상 종료와 같은 잠금 안에서 파기해야 실패 정리와 그 읽기 사이의 경합이 없다.
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
    if (const auto gate = GetFailedStartOwnerGateForTest())
    {
        gate->WaitBeforeFailedStartOwnerReset();
    }
#endif
    mAcceptor.reset();
    mIo.reset();
    mParsePool.reset();
    mLifecycle = Lifecycle::Stopped;
    mLifecycleChanged.notify_all();
}

Core::Status ServerHost::State::BeginDrain()
{
    Net::Acceptor* acceptor = nullptr;
    {
        std::unique_lock guard(mLifecycleMutex);
        if (mJobRunner.IsCurrentThread() || (mIo && mIo->IsCurrentThreadIoThread()) ||
            (mParsePool && mParsePool->IsCurrentThread()) ||
            CurrentCloseNotificationDepth(this) != 0)
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        if (mLifecycle == Lifecycle::Stopped)
            return Core::Status::Ok();
        if (mLifecycle != Lifecycle::Running)
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        if (mDraining.load(std::memory_order_acquire))
        {
            mLifecycleChanged.wait(guard, [this] { return !mDrainStarting; });
            return Core::Status::Ok();
        }
        mDrainStarting = true;
        mDraining.store(true, std::memory_order_release);
        mRunning.store(false, std::memory_order_release);
        mPort.store(0, std::memory_order_release);
        // 게임 작업 수락은 여기서 닫지 않는다. drain 중 처리되는 이미 받은 요청의 후속 작업(Post,
        // SubmitSessionTask의 Reserve, 관측)이 이어져야 하므로 AdvanceDrain이 송신 drain을 시작할 때 닫는다.
        acceptor = mAcceptor.get();
    }
    if (acceptor)
        acceptor->Stop();
    for (std::size_t index = 0; index < mSessionSlots.size(); ++index)
    {
        std::shared_ptr<NetworkSession> session;
        {
            const std::lock_guard guard(mLifecycleMutex);
            session = mSessionSlots[index];
        }
        if (session)
            session->StopReceiving();
    }
    {
        const std::lock_guard guard(mLifecycleMutex);
        mDrainStarting = false;
    }
    mLifecycleChanged.notify_all();
    AdvanceDrain();
    return Core::Status::Ok();
}

Core::Status ServerHost::State::DrainStatus() const
{
    const std::lock_guard guard(mLifecycleMutex);
    if (mLifecycle == Lifecycle::Stopped)
        return Core::Status::Ok();
    if (mLifecycle == Lifecycle::Ready || mLifecycle == Lifecycle::Starting)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    if (mDraining.load(std::memory_order_acquire) && mDrainSendsStarted &&
        mOutstandingSessions == 0 && mInFlightCloseNotifications == 0)
        return Core::Status::Ok();
    return Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock);
}

Core::Status ServerHost::State::StopGracefully(const std::chrono::steady_clock::time_point deadline)
{
    auto begun = BeginDrain();
    if (!begun.IsOk())
        return begun;
    bool expired = false;
    while (!DrainStatus().IsOk())
    {
        AdvanceDrain();
        if (DrainStatus().IsOk())
            break;
        std::unique_lock guard(mLifecycleMutex);
        if (std::chrono::steady_clock::now() >= deadline)
        {
            expired = true;
            break;
        }
        mLifecycleChanged.wait_until(guard,
            std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
    }
    Stop();
    return expired ? Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout)
                   : Core::Status::Ok();
}

void ServerHost::State::AdvanceDrain()
{
    {
        const std::lock_guard guard(mLifecycleMutex);
        // The periodic control callback itself occupies one slot; external drain polling does not.
        if (!mDraining.load(std::memory_order_acquire) || mDrainStarting || mDrainSendsStarted ||
            mLifecycle != Lifecycle::Running ||
            mPendingReceiveBytes.load(std::memory_order_acquire) != 0 ||
            mPendingParseTasks.load(std::memory_order_acquire) != 0 ||
            mJobRunner.OutstandingCount() > (mJobRunner.IsCurrentThread() ? 1u : 0u))
            return;
        mDrainSendsStarted = true;
        // 받은 입력과 그 후속 작업이 모두 끝난 시점이다. 이 뒤의 게임 작업은 닫히는 세션에 닿을 수 없다.
        mJobRunner.CloseAdmission();
    }
    for (std::size_t index = 0; index < mSessionSlots.size(); ++index)
    {
        std::shared_ptr<NetworkSession> session;
        {
            const std::lock_guard guard(mLifecycleMutex);
            session = mSessionSlots[index];
        }
        if (session)
            session->DrainSends();
    }
    mLifecycleChanged.notify_all();
}

void ServerHost::State::Stop()
{
    Net::Acceptor* acceptor = nullptr;
    Net::IoContext* io = nullptr;
    const std::size_t callerCloseNotificationDepth = CurrentCloseNotificationDepth(this);
    {
        std::unique_lock<std::mutex> guard(mLifecycleMutex);

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        if (const auto gate = GetFailedStartOwnerGateForTest())
        {
            gate->OnStopOwnerRead();
        }
#endif
        // Starting을 기다리기 전에 현재 실행 문맥부터 거절한다. Start 전에 받아 둔 Lease 작업이나
        // 시작 도중 들어온 I/O callback이 여기서 기다리면 Start 실패 정리의 join과 서로 막힌다.
        SERVERCORE_ASSERT(!mJobRunner.IsCurrentThread(),
            "ServerHost::Stop() cannot run from its JobRunner thread");
        if (mIo != nullptr)
        {
            SERVERCORE_ASSERT(!mIo->IsCurrentThreadIoThread(),
                "ServerHost::Stop() cannot run from one of its I/O threads");
        }
        if (mParsePool != nullptr)
        {
            SERVERCORE_ASSERT(!mParsePool->IsCurrentThread(),
                "ServerHost::Stop() cannot run from one of its parse worker threads");
        }
        SERVERCORE_ASSERT(mInFlightCloseNotifications >= callerCloseNotificationDepth,
            "ServerHost close-notification thread depth exceeded its lifecycle count");

        while (mLifecycle == Lifecycle::Starting)
        {
            if (callerCloseNotificationDepth != 0)
            {
                // Start 실패 정리도 이 callback 계수가 0이 되기를 기다린다. callback이 여기서
                // Starting 종료를 기다리면 서로 막히므로 요청만 남기고 Start 호출자가 이어서
                // 정상 Stop을 수행하게 한다.
                mStopRequestedDuringStart = true;
                return;
            }
            mLifecycleChanged.wait(guard);
        }
        mLifecycleChanged.wait(guard, [this] { return !mDrainStarting; });

        if (mLifecycle == Lifecycle::Ready)
        {
            return;
        }
        if (mLifecycle == Lifecycle::Stopped)
        {
            if (callerCloseNotificationDepth == 0)
            {
                mLifecycleChanged.wait(
                    guard, [this]() { return mInFlightCloseNotifications == 0; });
            }
            return;
        }
        if (mLifecycle == Lifecycle::Stopping)
        {
            // 다른 Stop은 이 callback의 반환을 기다린다. 여기서 그 Stop을 기다리면 순환하므로
            // callback 문맥은 이미 진행 중인 종료 요청에 합류한 것으로 보고 곧바로 돌아간다.
            if (callerCloseNotificationDepth != 0)
            {
                return;
            }
            mLifecycleChanged.wait(guard, [this]()
                { return mLifecycle == Lifecycle::Stopped && mInFlightCloseNotifications == 0; });
            return;
        }

        mLifecycle = Lifecycle::Stopping;
        mAccepting.store(false, std::memory_order_release);
        mRunning.store(false, std::memory_order_release);
        mPort.store(0, std::memory_order_release);
        acceptor = mAcceptor.get();
        io = mIo.get();
    }

    // Stop 뒤 새 timeout callback이 JobRunner에 들어가면 종료 작업과 순서가 섞인다. timer를 먼저
    // 멈추면 이미 들어간 callback도 mAccepting=false 경로로 no-op이 된다.
    if (mSessionTimeoutRunner != nullptr)
    {
        mSessionTimeoutRunner->Stop();
        mSessionTimeoutRunner.reset();
    }

    // Acceptor는 I/O가 살아 있을 때 먼저 drain해야 한다. 그 뒤부터는 OnConnectionAccepted가
    // 새 세션을 등록하지 못하고, 이미 만든 후보만 JobRunner에서 닫히기를 기다리면 된다.
    if (acceptor != nullptr)
    {
        acceptor->Stop();
    }

    // 파서 worker의 완료가 JobRunner 종료 뒤에 post되지 않게 먼저 입력을 닫고 worker를 거둔다.
    // Stop이 이미 mAccepting=false를 보였으므로 진행 중인 파싱 결과도 게임 handler로 가지 않는다.
    if (mParsePool != nullptr)
    {
        mParsePool->StopAndDiscard();
    }

    // 수락 handler가 모두 빠진 뒤이므로 슬롯에는 이 시점의 모든 NetworkSession이 들어 있다.
    // startup 때 미리 확보한 슬롯을 직접 훑어, 종료 작업 enqueue나 Registry snapshot 할당 없이
    // 스레드 안전한 Disconnect를 시작한다.
    CloseAllSessions();

    {
        std::unique_lock<std::mutex> guard(mLifecycleMutex);
        mLifecycleChanged.wait(guard,
            [this, callerCloseNotificationDepth]()
            {
                return mOutstandingSessions == 0 &&
                       mInFlightCloseNotifications <= callerCloseNotificationDepth;
            });
    }

    // NetworkSession은 OnDisconnected가 온 뒤에야 outstanding에서 빠진다. 즉 여기서는 각
    // Connection의 overlapped 작업도 끝났으므로 I/O를 멈춰도 완료 수명은 남지 않는다.
    if (io != nullptr)
    {
        io->Stop();
    }

    mJobRunner.RequestStop();
    if (mJobThread.joinable())
    {
        mJobThread.join();
    }
    if (mLogger && mMessageLogLimiter)
    {
        mMessageLogLimiter->Flush(*mLogger, true);
    }

    // Lifecycle이 Stopping인 동안 다른 Stop() 호출자는 이 잠금에서 현재 I/O·parse worker 소유자를
    // 읽어 자기 스레드가 금지된 문맥인지 확인한다. 따라서 그 포인터들을 파기하고 Stopped를 알리는
    // 전이를 하나의 임계 구역으로 묶어, 두 번째 호출자가 이미 파기한 객체를 역참조하지 않게 한다.
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    mAcceptor.reset();
    mIo.reset();
    mParsePool.reset();
    mLifecycle = Lifecycle::Stopped;
    mLifecycleChanged.notify_all();
}

int ServerHost::State::Run()
{
    std::unique_lock<std::mutex> guard(mLifecycleMutex);
    const bool stoppedAfterStart = mLifecycle == Lifecycle::Stopped && mStartSucceeded;
    if (mLifecycle != Lifecycle::Running && mLifecycle != Lifecycle::Stopping && !stoppedAfterStart)
    {
        return 1;
    }

    mLifecycleChanged.wait(guard,
        [this]() { return mLifecycle == Lifecycle::Stopped && mInFlightCloseNotifications == 0; });
    return 0;
}

bool ServerHost::State::IsRunning() const noexcept
{
    return mRunning.load(std::memory_order_acquire);
}

std::uint16_t ServerHost::State::Port() const noexcept
{
    return mPort.load(std::memory_order_acquire);
}

void ServerHost::State::FinishCloseNotification() noexcept
{
    {
        const std::lock_guard<std::mutex> guard(mLifecycleMutex);
        SERVERCORE_ASSERT(mInFlightCloseNotifications != 0,
            "ServerHost completed a close notification that it did not count");
        --mInFlightCloseNotifications;
    }
    mLifecycleChanged.notify_all();
}

void ServerHost::State::NotifyOpened(const std::shared_ptr<Session::Session>& session)
{
    if (const std::shared_ptr<Session::ISessionObserver> observer = mSessionObserver.lock())
    {
        try
        {
            observer->OnSessionOpened(session);
        }
        catch (...)
        {
            Core::ReportAssertFailure("OnSessionOpened() did not throw", __FILE__, __LINE__,
                "a ServerHost session observer threw from OnSessionOpened()");
        }
    }
}

void ServerHost::State::NotifyAuthenticated(const std::shared_ptr<Session::Session>& session)
{
    if (const std::shared_ptr<Session::ISessionObserver> observer = mSessionObserver.lock())
    {
        try
        {
            observer->OnSessionAuthenticated(session);
        }
        catch (...)
        {
            Core::ReportAssertFailure("OnSessionAuthenticated() did not throw", __FILE__, __LINE__,
                "a ServerHost session observer threw from OnSessionAuthenticated()");
        }
    }
}

void ServerHost::State::NotifyClosed(const Session::SessionId id, Core::Status reason)
{
    {
        const ScopedCloseNotificationContext notificationContext(this);
        if (const std::shared_ptr<Session::ISessionObserver> observer = mSessionObserver.lock())
        {
            try
            {
                observer->OnSessionClosed(id, std::move(reason));
            }
            catch (...)
            {
                Core::ReportAssertFailure("OnSessionClosed() did not throw", __FILE__, __LINE__,
                    "a ServerHost session observer threw from OnSessionClosed()");
            }
        }
    }
    FinishCloseNotification();
}
}
