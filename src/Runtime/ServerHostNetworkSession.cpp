// NetworkSession의 수신 batch 처리와 비정상 종료 정리다.
#include "Runtime/ServerHostNetworkSession.h"

namespace ServerCore::Runtime
{
namespace
{
// 동기 파싱에서 수신 작업 하나가 dispatch할 frame 수의 상한이다. 수신 batch 하나에 작은 frame이
// 수만 개 들어 있어도 한 세션이 runner를 독점하지 않고, 남은 frame은 다음 작업이 이어 처리한다.
// 이 교대는 Runtime.HostBoundsFramesPerReceiveJob이 고정한다.
constexpr std::size_t MaximumFramesPerReceiveJob = 64;
}

thread_local SessionLockThreadContext gSessionLocks;

void ServerHost::State::NetworkSession::ConsumePendingBytesOnRunner()
{
    const std::shared_ptr<ServerHost::State> host = mHost.lock();
    if (host == nullptr || !host->mJobRunner.IsCurrentThread())
    {
        return;
    }

    mDispatchBudget = MaximumFramesPerReceiveJob;
    if (mDeferredReceiveChunks != 0)
    {
        // 앞 작업이 한도 때문에 남긴 frame을 새 batch보다 먼저 처리한다.
        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (host->IsAccepting() && state != ServerCore::Session::SessionState::Closing &&
            state != ServerCore::Session::SessionState::Closed)
        {
            DispatchCompletedFramesOnRunner(host);
        }
        if (HasUndispatchedFramesOnRunner())
        {
            Core::Status posted = Core::Status::Ok();
            try
            {
                const std::shared_ptr<NetworkSession> self = shared_from_this();
                posted =
                    host->mJobRunner.PostControl([self]() { self->ConsumePendingBytesOnRunner(); });
            }
            catch (...)
            {
                posted = Core::Status::AllocationFailure();
            }
            if (!posted.IsOk())
            {
                ReleaseDeferredReceiveBatchOnRunner();
                FailAndDisconnect(std::move(posted));
            }
            return;
        }
        ReleaseDeferredReceiveBatchOnRunner();
    }

    std::vector<std::byte> batch;
    std::vector<ReceiveChunk> chunks;
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        batch.swap(mPendingReceiveBytes);
        chunks.swap(mPendingReceiveChunks);
    }

    if (batch.empty())
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        mReceiveJobQueued = false;
        return;
    }

    const ServerCore::Session::SessionState state = mSessionState.load(std::memory_order_acquire);
    if (!host->IsAccepting() || state == ServerCore::Session::SessionState::Closing ||
        state == ServerCore::Session::SessionState::Closed)
    {
        const std::size_t batchSize = batch.size();
        const auto chunkCount = chunks.size();
        std::vector<ReceiveChunk>().swap(chunks);
        std::vector<std::byte>().swap(batch);
        CompleteReceiveBatchOnRunner(batchSize, chunkCount);
        DiscardQueuedReceiveBytes();
        return;
    }

    const std::size_t batchSize = batch.size();
    const auto chunkCount = chunks.size();
    std::size_t offset = 0;
    for (const auto& chunk : chunks)
    {
        mCurrentBatchReceivedAt = chunk.receivedAt;
        ConsumeBytesOnRunner(std::span(batch).subspan(offset, chunk.bytes));
        offset += chunk.bytes;
    }
    std::vector<ReceiveChunk>().swap(chunks);
    // 다른 I/O thread가 반환된 Host 예산으로 새 수신 vector를 만들기 전에, 이 batch의 실제
    // payload 저장소부터 해제한다.
    std::vector<std::byte>().swap(batch);
    if (HasUndispatchedFramesOnRunner())
    {
        mDeferredReceiveBytes = batchSize;
        mDeferredReceiveChunks = chunkCount;
    }
    else
    {
        CompleteReceiveBatchOnRunner(batchSize, chunkCount);
    }

    bool queueContinuation = false;
    bool discardQueuedBytes = false;
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        const ServerCore::Session::SessionState afterConsume =
            mSessionState.load(std::memory_order_acquire);
        if (!host->IsAccepting() || afterConsume == ServerCore::Session::SessionState::Closing ||
            afterConsume == ServerCore::Session::SessionState::Closed)
        {
            discardQueuedBytes = true;
        }
        else if (!mPendingReceiveBytes.empty() || mDeferredReceiveChunks != 0)
        {
            // 한 배치만 처리하고 줄 끝으로 다시 예약한다. 바쁜 한 연결이 JobRunner를 계속
            // 점유하지 않으면서도, 세션마다 대기 작업은 항상 하나뿐이다.
            queueContinuation = true;
        }
        else
        {
            mReceiveJobQueued = false;
        }
    }

    if (discardQueuedBytes)
    {
        ReleaseDeferredReceiveBatchOnRunner();
        DiscardQueuedReceiveBytes();
        return;
    }

    if (queueContinuation)
    {
        Core::Status posted = Core::Status::Ok();
        try
        {
            const std::shared_ptr<NetworkSession> self = shared_from_this();
            posted =
                host->mJobRunner.PostControl([self]() { self->ConsumePendingBytesOnRunner(); });
        }
        catch (const std::bad_alloc&)
        {
            posted = Core::Status::AllocationFailure();
        }
        catch (...)
        {
            posted = Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }
        if (!posted.IsOk())
        {
            ReleaseDeferredReceiveBatchOnRunner();
            FailAndDisconnect(std::move(posted));
        }
    }
}

bool ServerHost::State::NetworkSession::HasUndispatchedFramesOnRunner()
{
    if (mParseAccounting != nullptr)
    {
        // parse worker 모드의 완결 frame은 parse 예산이 따로 붙들고 완료 작업이 하나씩 넘긴다.
        return false;
    }
    const ServerCore::Session::SessionState state = mSessionState.load(std::memory_order_acquire);
    if (state == ServerCore::Session::SessionState::Closing ||
        state == ServerCore::Session::SessionState::Closed)
    {
        return false;
    }
    const std::lock_guard<std::mutex> guard(mParseStateMutex);
    return !mFinalized.load(std::memory_order_acquire) && mFrameReader.CompletedFrameCount() != 0;
}

void ServerHost::State::NetworkSession::ReleaseDeferredReceiveBatchOnRunner()
{
    if (mDeferredReceiveChunks == 0)
    {
        return;
    }
    const std::size_t bytes = std::exchange(mDeferredReceiveBytes, 0);
    const std::size_t chunks = std::exchange(mDeferredReceiveChunks, 0);
    CompleteReceiveBatchOnRunner(bytes, chunks);
}

void ServerHost::State::NetworkSession::CompleteReceiveBatchOnRunner(
    const std::size_t batchSize, const std::size_t chunks)
{
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        SERVERCORE_ASSERT(mBufferedReceiveBytes >= batchSize,
            "completed receive bytes exceeded the NetworkSession budget");
        mBufferedReceiveBytes -= batchSize;
        mBufferedReceiveChunks -= chunks;
    }

    if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
    {
        host->ReleasePendingReceiveBytes(batchSize);
    }
}

void ServerHost::State::NetworkSession::DiscardQueuedReceiveBytes()
{
    std::size_t queuedBytes = 0;
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        queuedBytes = mPendingReceiveBytes.size();
        SERVERCORE_ASSERT(mBufferedReceiveBytes >= queuedBytes,
            "discarded receive bytes exceeded the NetworkSession budget");
        mBufferedReceiveBytes -= queuedBytes;
        mBufferedReceiveChunks -= mPendingReceiveChunks.size();
        std::vector<ReceiveChunk>().swap(mPendingReceiveChunks);
        std::vector<std::byte>().swap(mPendingReceiveBytes);
        mReceiveJobQueued = false;
    }

    if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
    {
        host->ReleasePendingReceiveBytes(queuedBytes);
    }
}

void ServerHost::State::NetworkSession::DiscardUnregisteredId() noexcept
{
    if (mRegistered.load(std::memory_order_acquire))
    {
        return;
    }

    if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
    {
        try
        {
            // 이미 Register()가 예약을 소비했거나 다른 종료 경로가 먼저 버렸다면
            // InvalidArgument가 정상이다. 이 함수의 목적은 예약 메모리를 남기지 않는 것이며
            // 번호 자체는 어떤 경우에도 재사용되지 않는다.
            (void)host->mRegistry.DiscardIssuedId(mId);
        }
        catch (...)
        {
            // I/O 종료 경로에서 예외를 내보내면 worker를 잃는다. 예약 정리 실패는 다음 Host
            // 수명까지 남을 수 있지만, 종료와 outstanding 계수 정리는 계속해야 한다.
        }
    }
}

void ServerHost::State::NetworkSession::FinalizeAfterRunnerFailure() noexcept
{
    // Deferred callers reach here only after any outbound/notification critical section exits.
    const std::shared_ptr<ServerHost::State> host = mHost.lock();
    if (host == nullptr)
    {
        return;
    }

    bool registered = false;
    bool opened = false;
    {
        const std::lock_guard<std::mutex> guard(mFinalizationMutex);

        // OnSessionOpened 안에서 동기 Disconnect가 일어나고 finalizer Post까지 실패할 수 있다.
        // callback의 순서를 뒤집거나 같은 mutex에 재진입하지 않고, OpenOnRunner가 callback을
        // 반환한 직후 이 정리를 다시 수행하게 한다.
        if (mLifecycleNotificationDepth != 0)
        {
            mFinalizeAfterNotification = true;
            return;
        }
        if (mFinalized.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        registered = mRegistered.exchange(false, std::memory_order_acq_rel);
        opened = mOpened.exchange(false, std::memory_order_acq_rel);
    }

    // 외부 게임 코드가 닫힌 Session shared_ptr를 계속 들고 있어도 completed payload와 그
    // Host aggregate reservation은 finalization 시점에 끝나야 한다. runner와 경합할 수 있으므로
    // FrameReader를 포함한 parser 상태는 전용 잠금 아래 비운다. worker가 가진 한 건은 별도의
    // ParseReservation이 끝날 때 반납된다.
    RequestCancellationIfClosing();
    DiscardQueuedParseFrames();

    // 이 경로는 JobRunner 자체가 더는 정리 작업을 받지 못하는 비정상 경계다. Unregister의
    // thread-safe 정리 경계로 닫힌 세션을 목록에서 빼고, 등록 전이었다면 발급 예약만 버린다.
    // 어느 쪽이든 Host의 preallocated slot과 outstanding 계수까지 정확히 한 번 정리한다.
    if (registered)
    {
        try
        {
            const Core::Status unregistered = host->mRegistry.Unregister(mId);
            if (!unregistered.IsOk() && unregistered.Code() != Core::ErrorCode::NotFound)
            {
                host->LogStatus(Core::LogLevel::Warn, unregistered, mId);
            }
        }
        catch (...)
        {
            Core::ReportAssertFailure("SessionRegistry::Unregister() did not throw", __FILE__,
                __LINE__, "fallback session finalization could not update the session registry");
        }
    }
    else
    {
        DiscardUnregisteredId();
    }

    const std::shared_ptr<NetworkSession> keepAlive = weak_from_this().lock();
    SERVERCORE_ASSERT(
        keepAlive != nullptr, "a finalizing NetworkSession lost all shared ownership");
    host->FinishNetworkSession(this, opened);

    if (opened)
    {
        try
        {
            host->NotifyClosed(mId, TakeCloseReason());
        }
        catch (...)
        {
            Core::ReportAssertFailure("fallback close notification did not throw", __FILE__,
                __LINE__, "fallback session finalization could not notify a session close");
        }
    }
}
}
