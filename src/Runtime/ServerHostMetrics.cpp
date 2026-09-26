// ServerHost의 운영 지표 스냅숏과 계수다.
#include "Runtime/ServerHostNetworkSession.h"

namespace ServerCore::Runtime
{
Core::Result<ServerMetricsSnapshot> ServerHost::State::SnapshotMetrics() const
{
    const bool onRunner = mJobRunner.IsCurrentThread();
    bool stopped = false;
    {
        const std::lock_guard guard(mLifecycleMutex);
        stopped = mLifecycle == Lifecycle::Stopped;
    }
    // Stop이 끝난 Host는 runner가 없고 세션도 모두 정리됐으므로 어느 스레드에서나 읽게 한다.
    if (!onRunner && !stopped)
    {
        return Core::Result<ServerMetricsSnapshot>::FromStatus(
            Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost::SnapshotMetrics() must run in the ServerHost JobRunner context"));
    }

    try
    {
        ServerMetricsSnapshot snapshot;
        {
            const std::lock_guard guard(mLifecycleMutex);
            snapshot.lifecycle =
                mLifecycle == Lifecycle::Stopped ? Observability::Lifecycle::Stopped
                : (mLifecycle == Lifecycle::Stopping || mDraining.load())
                    ? Observability::Lifecycle::Draining
                : mLifecycle == Lifecycle::Running ? Observability::Lifecycle::Running
                                                   : Observability::Lifecycle::Created;
        }
        snapshot.configuredIoWorkerThreadCount =
            static_cast<std::uint32_t>(mOptions.ioWorkerThreadCount);
        snapshot.configuredParseWorkerThreadCount =
            static_cast<std::uint32_t>(mOptions.parseWorkerThreadCount);
        snapshot.pendingReceiveBytes = mPendingReceiveBytes.load(std::memory_order_acquire);
        snapshot.pendingJobCount = mJobRunner.PendingCount();
        snapshot.jobs = mJobRunner.GetMetrics();
        if (onRunner && snapshot.jobs.outstandingJobs != 0)
        {
            // runner에서 부르는 호출자는 언제나 실행 중인 작업 하나 안에 있다. 그 자신을 남은 일로 세면
            // drain 진행 관측이 0에 닿지 못한다.
            --snapshot.jobs.outstandingJobs;
        }
        snapshot.pendingParseBytes = mPendingParseBytes.load(std::memory_order_acquire);
        snapshot.pendingParseTaskCount = mPendingParseTasks.load(std::memory_order_acquire);
        snapshot.receivedFrameCount = mReceivedFrameCount.load(std::memory_order_relaxed);
        snapshot.queuedSendFrameCount = mQueuedSendFrameCount.load(std::memory_order_relaxed);
        snapshot.errorCount = mErrorCount.load(std::memory_order_relaxed);
        snapshot.skippedPeriodCount = mJobRunner.PeriodicSkippedCount();

        if (!onRunner)
        {
            return Core::Result<ServerMetricsSnapshot>::FromValue(std::move(snapshot));
        }
        snapshot.sessionSendQueues.reserve(mRegistry.Count());
        Core::Status visited = mRegistry.ForEach(
            [&snapshot](const std::shared_ptr<Session::Session>& session)
            {
                // ServerHost가 Registry에 넣는 구현은 NetworkSession 하나뿐이다. Session API에 전송 계층
                // 지표를 새로 새지 않기 위해 이 L5 조립 지점에서만 구체 구현을 읽는다.
                const std::shared_ptr<NetworkSession> networkSession =
                    std::dynamic_pointer_cast<NetworkSession>(session);
                SERVERCORE_ASSERT(networkSession != nullptr,
                    "ServerHost registry contained a session it did not create");
                if (networkSession == nullptr)
                {
                    return;
                }
                snapshot.sessionSendQueues.push_back(
                    SessionSendQueueSnapshot{ session->Id(), networkSession->QueuedSendBytes() });
                const auto retained = networkSession->RetainedSendBytes();
                const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
                snapshot.retainedSendBytes = retained > maximum - snapshot.retainedSendBytes
                                                 ? maximum
                                                 : snapshot.retainedSendBytes + retained;
            });
        if (!visited.IsOk())
        {
            return Core::Result<ServerMetricsSnapshot>::FromStatus(std::move(visited));
        }
        std::sort(snapshot.sessionSendQueues.begin(), snapshot.sessionSendQueues.end(),
            [](const SessionSendQueueSnapshot& left, const SessionSendQueueSnapshot& right)
            { return static_cast<std::uint64_t>(left.id) < static_cast<std::uint64_t>(right.id); });
        snapshot.activeSessionCount = snapshot.sessionSendQueues.size();
        return Core::Result<ServerMetricsSnapshot>::FromValue(std::move(snapshot));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<ServerMetricsSnapshot>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& error)
    {
        return Core::Result<ServerMetricsSnapshot>::FromStatus(PlatformFailureFrom(error));
    }
}

void ServerHost::State::RecordReceivedFrame() noexcept
{
    mReceivedFrameCount.fetch_add(1, std::memory_order_relaxed);
}

void ServerHost::State::RecordQueuedSendFrame() noexcept
{
    mQueuedSendFrameCount.fetch_add(1, std::memory_order_relaxed);
}

void ServerHost::State::RecordError(const Core::Status& status) noexcept
{
    if (status.IsOk() || status.Code() == Core::ErrorCode::Closed ||
        status.Code() == Core::ErrorCode::Timeout || status.Code() == Core::ErrorCode::WouldBlock)
    {
        return;
    }

    mErrorCount.fetch_add(1, std::memory_order_relaxed);
}
}
