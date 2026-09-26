#pragma once

// ServerHost가 연결 하나마다 만드는 NetworkSession의 정의다. ServerHost 구현 파일들만 include한다.
#include "Runtime/ServerHostState.h"

namespace ServerCore::Runtime
{
class ServerHost::State::NetworkSession final : public ServerCore::Session::Session,
                                                public Net::IConnectionObserver,
                                                public std::enable_shared_from_this<NetworkSession>
{
private:
    /// <summary>세션별 parse reservation과 Host 전체 reservation을 함께 반납하는 공유 장부다.</summary>
    /// <remarks>
    /// worker task는 NetworkSession을 살려 두지 않는다. 이 장부만 shared_ptr로 들고 완료 뒤에
    /// 반납하므로, 원격 종료 뒤 늦은 parse 완료가 닫힌 세션이나 Registry를 만질 수 없다.
    /// </remarks>
    class ParseAccounting
    {
    public:
        ParseAccounting(std::weak_ptr<ServerHost::State> host, const std::size_t byteLimit,
            const std::size_t taskLimit) noexcept
            : mHost(std::move(host))
            , mByteLimit(byteLimit)
            , mTaskLimit(taskLimit)
        {
        }

        ~ParseAccounting()
        {
            const std::size_t bytes = mPendingBytes.exchange(0, std::memory_order_acq_rel);
            const std::size_t tasks = mPendingTasks.exchange(0, std::memory_order_acq_rel);
            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                // ParseWorkerPool은 Host 소멸 전에 join되므로, 이 최후 정리는 비정상 JobRunner
                // 실패 경로의 잔여분뿐이다.
                host->ReleasePendingParseWork(bytes, tasks);
            }
        }

        [[nodiscard]] bool TryReserve(
            const std::size_t byteCount, const std::size_t taskCount) noexcept
        {
            if (!TryReserveLocal(mPendingBytes, mByteLimit, byteCount))
            {
                return false;
            }
            if (!TryReserveLocal(mPendingTasks, mTaskLimit, taskCount))
            {
                ReleaseLocal(mPendingBytes, byteCount);
                return false;
            }

            const std::shared_ptr<ServerHost::State> host = mHost.lock();
            if (host != nullptr && host->TryReservePendingParseWork(byteCount, taskCount))
            {
                return true;
            }

            ReleaseLocal(mPendingBytes, byteCount);
            ReleaseLocal(mPendingTasks, taskCount);
            return false;
        }

        void Release(const std::size_t byteCount, const std::size_t taskCount) noexcept
        {
            ReleaseLocal(mPendingBytes, byteCount);
            ReleaseLocal(mPendingTasks, taskCount);
            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->ReleasePendingParseWork(byteCount, taskCount);
            }
        }

    private:
        [[nodiscard]] static bool TryReserveLocal(std::atomic<std::size_t>& pending,
            const std::size_t limit, const std::size_t amount) noexcept
        {
            return Core::Detail::TryReserveBudget(pending, limit, amount);
        }

        static void ReleaseLocal(
            std::atomic<std::size_t>& pending, const std::size_t amount) noexcept
        {
            Core::Detail::ReleaseBudget(
                pending, amount, "released parse work exceeded the NetworkSession budget");
        }

        std::weak_ptr<ServerHost::State> mHost;
        std::size_t mByteLimit;
        std::size_t mTaskLimit;
        std::atomic<std::size_t> mPendingBytes{ 0 };
        std::atomic<std::size_t> mPendingTasks{ 0 };
    };

    /// <summary>FrameReader가 본문 vector를 복사하기 전에 parse reservation을 확보한다.</summary>
    /// <remarks>
    /// 한 Receive batch 안의 여러 완결 frame을 Append 뒤에 한꺼번에 예약하면, 작은 frame이
    /// 많은 경우 실제 deque 할당이 task 상한을 순간적으로 넘을 수 있다. 이 객체는 callback
    /// 안에서 프레임 하나씩 먼저 예약하고, 복사에 실패한 마지막 frame만 Reconcile에서 되돌린다.
    /// </remarks>
    class ParseAdmission
    {
    public:
        explicit ParseAdmission(ParseAccounting& accounting) noexcept
            : mAccounting(accounting)
        {
        }

        ~ParseAdmission()
        {
            // FrameReader::Append()가 Status 설명 문자열을 만들다가 OOM을 던지는 식으로
            // ReleaseNotStored까지 돌아오지 못해도, callback 안에서 먼저 잡은 reservation은
            // 이 범위가 끝날 때 반드시 반납한다.
            if (!mSettled)
            {
                mAccounting.Release(mReservedBytes, mReservedTasks);
            }
        }

        [[nodiscard]] static bool Admit(void* const context, const std::size_t byteCount) noexcept
        {
            ParseAdmission* const admission = static_cast<ParseAdmission*>(context);
            if (admission == nullptr || !admission->mAccounting.TryReserve(byteCount, 1))
            {
                return false;
            }

            // ParseAccounting의 설정 상한이 size_t보다 훨씬 작으므로 이 합계는 overflow하지 않는다.
            admission->mReservedBytes += byteCount;
            ++admission->mReservedTasks;
            return true;
        }

        void ReleaseNotStored(const std::size_t storedBytes, const std::size_t storedTasks) noexcept
        {
            SERVERCORE_ASSERT(mReservedBytes >= storedBytes,
                "FrameReader stored more parse bytes than it admitted");
            SERVERCORE_ASSERT(mReservedTasks >= storedTasks,
                "FrameReader stored more parse tasks than it admitted");
            mAccounting.Release(mReservedBytes - storedBytes, mReservedTasks - storedTasks);
            mSettled = true;
        }

    private:
        ParseAccounting& mAccounting;
        std::size_t mReservedBytes = 0;
        std::size_t mReservedTasks = 0;
        bool mSettled = false;
    };

    /// <summary>예약한 frame 하나와 그 parse 결과를 worker와 JobRunner 사이에서 함께 붙든다.</summary>
    /// <remarks>
    /// raw body와 parsed Message를 예산 장부와 별도 lambda capture로 두면 capture 멤버의 소멸
    /// 순서는 C++가 정하지 않는다. 그러면 실행되지 않고 버린 작업에서 예산이 실제 payload보다
    /// 먼저 풀릴 수 있다. 이 객체의 소멸자는 payload를 명시적으로 먼저 놓고 마지막에 예산을
    /// 반납해 어느 큐에서 파기되더라도 같은 순서를 지킨다.
    /// </remarks>
    class ParseWorkItem
    {
    public:
        ParseWorkItem(std::shared_ptr<ParseAccounting> accounting, std::vector<std::byte> body,
            const Protocol::JsonParseLimits limits) noexcept
            : mAccounting(std::move(accounting))
            , mByteCount(body.size())
            , mBody(std::move(body))
            , mLimits(limits)
        {
        }

        ~ParseWorkItem()
        {
            mMessage.reset();
            std::vector<std::byte>().swap(mBody);
            if (mAccounting != nullptr)
            {
                mAccounting->Release(mByteCount, 1);
            }
        }

        ParseWorkItem(const ParseWorkItem&) = delete;
        ParseWorkItem& operator=(const ParseWorkItem&) = delete;

        void Parse()
        {
            SERVERCORE_ASSERT(!mMessage.has_value(), "a parse work item was parsed more than once");
            mMessage.emplace(Protocol::ParseMessage(mBody, mLimits));
            // parsed Message가 필요한 값을 모두 소유한 뒤에는 wire body를 즉시 놓는다. 이 객체가
            // completion queue에 머무는 동안 예산은 계속 유지된다.
            std::vector<std::byte>().swap(mBody);
        }

        [[nodiscard]] Core::Result<Protocol::Message>& Message()
        {
            SERVERCORE_ASSERT(mMessage.has_value(), "a parse work item has no completed result");
            return *mMessage;
        }

    private:
        std::shared_ptr<ParseAccounting> mAccounting;
        std::size_t mByteCount;
        std::vector<std::byte> mBody;
        Protocol::JsonParseLimits mLimits;
        std::optional<Core::Result<Protocol::Message>> mMessage;
    };

    /// <summary>동기 Connection 호출이 돌아올 때까지 fallback OnSessionClosed를 미룬다.</summary>
    // Send/Disconnect의 지역 잠금보다 먼저 생성해야 한다. 역순 소멸로 outbound 잠금을 놓은
    // 뒤에만 종료 관찰자를 부를 수 있어, 관찰자가 다시 Send/Disconnect해도 교착하지 않는다.
    class FinalizationDeferral
    {
    public:
        explicit FinalizationDeferral(NetworkSession& session) noexcept
            : mSession(session)
            , mActive(session.BeginFinalizationDeferral())
        {
        }

        ~FinalizationDeferral()
        {
            mSession.RequestCancellationIfClosing();
            if (mActive)
            {
                mSession.CompleteLifecycleNotification();
            }
        }

        FinalizationDeferral(const FinalizationDeferral&) = delete;
        FinalizationDeferral& operator=(const FinalizationDeferral&) = delete;

    private:
        NetworkSession& mSession;
        bool mActive;
    };

public:
    NetworkSession(std::weak_ptr<ServerHost::State> host, const ServerCore::Session::SessionId id,
        std::shared_ptr<Net::Connection> connection, const std::uint32_t maxBodySize,
        const std::uint32_t maxPendingReceiveBytes, const std::uint32_t maxPendingParseBytes,
        const std::uint32_t maxPendingParseTasks)
        : mHost(std::move(host))
        , mId(id)
        , mConnection(std::move(connection))
        , mLastReceivedMilliseconds(Core::MillisecondsSinceProcessStart())
        , mFrameReader(maxBodySize)
        , mMaxBodySize(maxBodySize)
        , mMaxPendingReceiveBytes(maxPendingReceiveBytes)
    {
        SERVERCORE_ASSERT(mConnection != nullptr, "NetworkSession was given a null connection");
        if (maxPendingParseTasks != 0)
        {
            mParseAccounting = std::make_shared<ParseAccounting>(mHost,
                static_cast<std::size_t>(maxPendingParseBytes),
                static_cast<std::size_t>(maxPendingParseTasks));
        }
    }

    [[nodiscard]] ServerCore::Session::SessionId Id() const noexcept override { return mId; }

    [[nodiscard]] ServerCore::Session::SessionState State() const noexcept override
    {
        return mSessionState.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::stop_token GetCancellationToken() const noexcept override
    {
        return mCancellation.get_token();
    }

    void RequestCancellationIfClosing() noexcept
    {
        const auto state = State();
        if (state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
            mCancellation.request_stop();
    }

    [[nodiscard]] Core::IpEndpoint LocalEndpoint() const noexcept override
    {
        return mConnection->LocalEndpoint();
    }
    [[nodiscard]] Core::IpEndpoint RemoteEndpoint() const noexcept override
    {
        return mConnection->RemoteEndpoint();
    }

    Core::Status SendBinary(
        const std::uint32_t type, const std::span<const std::byte> payload) override
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard guard(mOutboundMutex);
        const auto host = mHost.lock();
        if (!CanSendLocked() || !host)
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        if (host->mOptions.payloadMode != Protocol::PayloadMode::Binary)
            return RecordAndReturn(
                Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));

        // 오류 집계와 할당 실패 처리는 JSON Send와 같은 규칙을 따른다.
        try
        {
            Core::Result<std::vector<std::byte>> framed = EncodeBinaryFrame(type, payload);
            if (!framed.IsOk())
            {
                return RecordAndReturn(std::move(framed).TakeStatus());
            }

            Net::ConnectionSendOutcome sendOutcome = mConnection->SendWithOutcome(framed.Value());
            Core::Status queued = std::move(sendOutcome.status);
            if (!queued.IsOk())
            {
                if (sendOutcome.connectionClosedByFailure)
                {
                    return queued;
                }
                return RecordAndReturn(std::move(queued));
            }

            host->RecordQueuedSendFrame();
            return queued;
        }
        catch (const std::bad_alloc&)
        {
            return RecordAndReturn(Core::Status::AllocationFailure());
        }
        catch (const std::system_error& error)
        {
            return RecordAndReturn(PlatformFailureFrom(error));
        }
    }

    Core::Status SendBinaryAndDisconnect(const std::uint32_t type,
        const std::span<const std::byte> payload, Core::Status reason) override
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard guard(mOutboundMutex);
        if (const auto host = mHost.lock();
            host && host->mOptions.payloadMode != Protocol::PayloadMode::Binary)
        {
            return RecordAndReturn(
                Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
        }
        if (!BeginGracefulCloseLocked(std::move(reason)))
        {
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }

        return SendFinalFrameLocked([&]() { return EncodeBinaryFrame(type, payload); });
    }

    Core::Status MarkAuthenticated() override
    {
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr)
        {
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        }
        if (!host->mJobRunner.IsCurrentThread())
        {
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        }
        const auto authTimeout = host->mOptions.authenticationTimeout.count();
        if (State() == ServerCore::Session::SessionState::Connected && authTimeout > 0 &&
            Core::MillisecondsSinceProcessStart() - mCreatedAt >=
                static_cast<std::uint64_t>(authTimeout))
        {
            Disconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout));
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout);
        }

        const std::shared_ptr<NetworkSession> self = shared_from_this();
        {
            const std::lock_guard<std::mutex> guard(mFinalizationMutex);
            if (mFinalized.load(std::memory_order_relaxed))
            {
                return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
            }

            ServerCore::Session::SessionState current =
                mSessionState.load(std::memory_order_acquire);
            for (;;)
            {
                if (current == ServerCore::Session::SessionState::Authenticated)
                {
                    return Core::Status::FailWithoutMessage(Core::ErrorCode::AlreadyExists);
                }
                if (current == ServerCore::Session::SessionState::Closing ||
                    current == ServerCore::Session::SessionState::Closed)
                {
                    return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
                }

                if (mSessionState.compare_exchange_weak(current,
                        ServerCore::Session::SessionState::Authenticated, std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    ++mLifecycleNotificationDepth;
                    break;
                }
            }
        }

        // OnSessionOpened 관찰자 안에서 인증이 중첩될 수 있고, 이 callback 안에서도 즉시
        // Disconnect할 수 있다. notification depth가 마지막 callback 반환까지 종료를 미룬다.
        host->NotifyAuthenticated(self);
        CompleteLifecycleNotification();
        return Core::Status::Ok();
    }

    Core::Status Send(const Protocol::MessageFields& fields) override
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard guard(mOutboundMutex);
        if (!CanSendLocked())
        {
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }

        try
        {
            Core::Result<std::vector<std::byte>> framed = PrepareOutboundFrame(fields);
            if (!framed.IsOk())
            {
                return RecordAndReturn(std::move(framed).TakeStatus());
            }

            Net::ConnectionSendOutcome sendOutcome = mConnection->SendWithOutcome(framed.Value());
            Core::Status queued = std::move(sendOutcome.status);
            if (!queued.IsOk())
            {
                // 즉시 소켓 실패는 Connection이 같은 사유로 OnDisconnected를 보낸다. 그 경로가
                // error counter를 한 번 올리게 하고, 연결이 열린 채인 큐 할당 실패만 여기서 센다.
                if (sendOutcome.connectionClosedByFailure)
                {
                    return queued;
                }
                return RecordAndReturn(std::move(queued));
            }

            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->RecordQueuedSendFrame();
            }
            return queued;
        }
        catch (const std::bad_alloc&)
        {
            return RecordAndReturn(Core::Status::AllocationFailure());
        }
        catch (const std::system_error& error)
        {
            return RecordAndReturn(PlatformFailureFrom(error));
        }
    }

    Core::Status SendPrepared(const Protocol::PreparedMessage& prepared) override
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard guard(mOutboundMutex);
        if (!CanSendLocked())
        {
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }
        if (const auto host = mHost.lock();
            host && host->mOptions.payloadMode != Protocol::PayloadMode::Json)
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);

        // 준비 객체를 이동한 뒤의 빈 저장소는 정상 봉투가 아니다. 프레이머는 범용이라
        // 0길이 body도 인코딩하므로 메시지 계약을 이 경계에서 지킨다.
        if (prepared.Size() == 0)
            return RecordAndReturn(
                Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));

        try
        {
            Core::Result<std::vector<std::byte>> framed =
                Protocol::EncodeFrame(prepared.Bytes(), mMaxBodySize);
            if (!framed.IsOk())
            {
                return RecordAndReturn(std::move(framed).TakeStatus());
            }

            Net::ConnectionSendOutcome sendOutcome = mConnection->SendWithOutcome(framed.Value());
            Core::Status queued = std::move(sendOutcome.status);
            if (!queued.IsOk())
            {
                // 즉시 소켓 실패는 Connection이 같은 사유로 OnDisconnected를 보낸다. 그 경로가
                // error counter를 한 번 올리게 하고, 연결이 열린 채인 큐 할당 실패만 여기서 센다.
                if (sendOutcome.connectionClosedByFailure)
                {
                    return queued;
                }
                return RecordAndReturn(std::move(queued));
            }

            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->RecordQueuedSendFrame();
            }
            return queued;
        }
        catch (const std::bad_alloc&)
        {
            return RecordAndReturn(Core::Status::AllocationFailure());
        }
        catch (const std::system_error& error)
        {
            return RecordAndReturn(PlatformFailureFrom(error));
        }
    }

    Core::Status SendAndDisconnect(
        const Protocol::MessageFields& fields, Core::Status reason) override
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard guard(mOutboundMutex);
        // 봉투 종류가 Host와 다르면 Send처럼 세션을 건드리지 않고 거절한다. Binary 세션의 종료 경로는
        // SendBinaryAndDisconnect다.
        if (const auto host = mHost.lock();
            host && host->mOptions.payloadMode != Protocol::PayloadMode::Json)
        {
            return RecordAndReturn(
                Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
        }
        if (!BeginGracefulCloseLocked(std::move(reason)))
        {
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }

        return SendFinalFrameLocked([&]() { return PrepareOutboundFrame(fields); });
    }

    /// <summary>Closing으로 옮긴 뒤 마지막 frame을 큐에 넣고 drain 종료를 요청한다.</summary>
    /// <remarks>인코딩·큐 실패는 남은 송신을 버리고 즉시 닫는다. mOutboundMutex를 쥔 채 부른다.</remarks>
    template <class Encode> [[nodiscard]] Core::Status SendFinalFrameLocked(Encode encode)
    {
        try
        {
            Core::Result<std::vector<std::byte>> framed = encode();
            if (!framed.IsOk())
            {
                mConnection->Close();
                return RecordAndReturn(std::move(framed).TakeStatus());
            }

            Net::ConnectionSendOutcome sendOutcome = mConnection->SendWithOutcome(framed.Value());
            Core::Status queued = std::move(sendOutcome.status);
            if (!queued.IsOk())
            {
                mConnection->Close();
                if (sendOutcome.connectionClosedByFailure)
                {
                    return queued;
                }
                return RecordAndReturn(std::move(queued));
            }

            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->RecordQueuedSendFrame();
            }
            mConnection->CloseAfterSend();
            return queued;
        }
        catch (const std::bad_alloc&)
        {
            mConnection->Close();
            return RecordAndReturn(Core::Status::AllocationFailure());
        }
        catch (const std::system_error& error)
        {
            mConnection->Close();
            return RecordAndReturn(PlatformFailureFrom(error));
        }
    }

    void Disconnect(Core::Status reason) override
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard guard(mOutboundMutex);
        DisconnectLocked(std::move(reason));
    }

    void OnBytesReceived(std::span<const std::byte> bytes) override
    {
        if (bytes.empty())
        {
            return;
        }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        WaitBeforeSessionReceiveForTest();
#endif

        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
        {
            return;
        }

        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr)
        {
            mConnection->Close();
            return;
        }
        if (!host->IsAccepting() || host->mDraining.load(std::memory_order_acquire))
        {
            return;
        }

        bool rateExceeded = false;
        const auto receivedAt = Core::MillisecondsSinceProcessStart();
        {
            const std::lock_guard guard(mIdleMutex);
            mLastReceivedMilliseconds = receivedAt;
            if (receivedAt - mByteWindowStarted >= 1000)
            {
                mByteWindowStarted = receivedAt;
                mByteWindowBytes = 0;
            }
            const auto limit = host->mOptions.maxInputBytesPerSecond;
            if (limit != 0)
            {
                rateExceeded = bytes.size() > limit || mByteWindowBytes > limit - bytes.size();
                if (!rateExceeded)
                    mByteWindowBytes += bytes.size();
            }
        }
        if (rateExceeded)
        {
            FailAndDisconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::TooLarge));
            return;
        }

        bool queueReceiveJob = false;
        bool reservedHostReceiveBytes = false;
        Core::Status rejected = Core::Status::Ok();
        try
        {
            {
                const std::lock_guard<std::mutex> guard(mReceiveMutex);
                if (mInputClosed)
                    return;
                const std::size_t limit = static_cast<std::size_t>(mMaxPendingReceiveBytes);
                if (bytes.size() > limit || mBufferedReceiveBytes > limit - bytes.size() ||
                    mBufferedReceiveChunks >= host->mOptions.maxPendingReceiveChunks)
                {
                    rejected = Core::Status::Fail(Core::ErrorCode::TooLarge,
                        "the session exceeded its pending receive byte limit");
                }
                else if (!host->TryReservePendingReceiveBytes(bytes.size()))
                {
                    rejected = Core::Status::Fail(Core::ErrorCode::TooLarge,
                        "the ServerHost exceeded its total pending receive byte limit");
                }
                else
                {
                    reservedHostReceiveBytes = true;
                    mPendingReceiveChunks.push_back({ bytes.size(), receivedAt });
                    try
                    {
                        mPendingReceiveBytes.insert(
                            mPendingReceiveBytes.end(), bytes.begin(), bytes.end());
                    }
                    catch (...)
                    {
                        mPendingReceiveChunks.pop_back();
                        throw;
                    }
                    mBufferedReceiveBytes += bytes.size();
                    ++mBufferedReceiveChunks;
                    if (!mReceiveJobQueued)
                    {
                        mReceiveJobQueued = true;
                        queueReceiveJob = true;
                    }
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            if (reservedHostReceiveBytes)
            {
                host->ReleasePendingReceiveBytes(bytes.size());
            }
            rejected = Core::Status::AllocationFailure();
        }
        catch (...)
        {
            if (reservedHostReceiveBytes)
            {
                host->ReleasePendingReceiveBytes(bytes.size());
            }
            rejected = Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }

        if (!rejected.IsOk())
        {
            FailAndDisconnect(std::move(rejected));
            return;
        }
        if (!queueReceiveJob)
        {
            return;
        }

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
            FailAndDisconnect(std::move(posted));
        }
    }

    void OnDisconnected(Core::Status reason) noexcept override
    {
        if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
        {
            host->RecordError(reason);
        }
        try
        {
            const std::lock_guard<std::mutex> guard(mCloseReasonMutex);
            RememberCloseReasonLocked(std::move(reason));
            // 사유와 Closed 공개를 같은 임계구역에서 정한다. 로컬 종료가 Closing을 먼저 차지한
            // 뒤 transport callback이 그 사유보다 앞서 들어가는 경쟁을 막는다.
            mSessionState.store(
                ServerCore::Session::SessionState::Closed, std::memory_order_release);
        }
        catch (...)
        {
            // mutex 구현 실패 같은 최후 경계에서도 끊김 정리 자체는 반드시 계속한다.
            mSessionState.store(
                ServerCore::Session::SessionState::Closed, std::memory_order_release);
        }
        DiscardQueuedReceiveBytes();

        if (mFinalized.load(std::memory_order_acquire))
        {
            return;
        }

        try
        {
            const std::shared_ptr<ServerHost::State> host = mHost.lock();
            if (host == nullptr)
            {
                return;
            }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
            if (ConsumeFinalizePostFailureForTest())
            {
                FinalizeAfterRunnerFailure();
                return;
            }
#endif

            const std::shared_ptr<NetworkSession> self = shared_from_this();
            const Core::Status posted =
                host->mJobRunner.PostControl([self]() { self->FinalizeOnRunner(); });
            if (!posted.IsOk())
            {
                // 정상 Host 종료 중에는 JobRunner가 이 작업을 drain하므로 여기에 오지 않는다.
                // 외부 실행자 실패/OOM처럼 더는 queue에 넣을 수 없는 경우에도 Stop이 세션 수에서
                // 영구 대기하지 않도록 최소 종료 정리를 보장한다.
                FinalizeAfterRunnerFailure();
            }
        }
        catch (...)
        {
            FinalizeAfterRunnerFailure();
        }
    }

    void OpenOnRunner()
    {
        try
        {
            const std::shared_ptr<ServerHost::State> host = mHost.lock();
            if (host == nullptr)
            {
                return;
            }

            SERVERCORE_ASSERT(host->mJobRunner.IsCurrentThread(),
                "NetworkSession::OpenOnRunner() must run in the ServerHost JobRunner context");

            const std::shared_ptr<NetworkSession> self = shared_from_this();
            Core::Status registered = Core::Status::Ok();
            bool finalizeClosedSession = false;
            bool rejectClosingSession = false;
            {
                // Register와 open 통지 시작을 하나의 전이로 만든다. I/O 스레드의 최후 정리가
                // 이 사이에 들어오면 등록된 닫힌 세션이 Registry에 남거나 OnClosed가 OnOpened보다
                // 먼저 나갈 수 있다.
                const std::lock_guard<std::mutex> guard(mFinalizationMutex);
                if (mFinalized.load(std::memory_order_relaxed))
                {
                    return;
                }

                const ServerCore::Session::SessionState state =
                    mSessionState.load(std::memory_order_acquire);
                if (state == ServerCore::Session::SessionState::Closed)
                {
                    finalizeClosedSession = true;
                }
                else if (!host->IsAccepting() ||
                         state == ServerCore::Session::SessionState::Closing)
                {
                    rejectClosingSession = true;
                }
                else
                {
                    if (host->mDatagrams)
                    {
                        auto token = host->mDatagrams->RegisterSession(mId);
                        if (!token.IsOk())
                            registered = std::move(token).TakeStatus();
                    }
                    if (registered.IsOk())
                        registered = host->mRegistry.Register(self);
                    if (registered.IsOk())
                    {
                        mRegistered.store(true, std::memory_order_release);
                        mOpened.store(true, std::memory_order_release);
                        ++mLifecycleNotificationDepth;
                    }
                }
            }

            if (finalizeClosedSession)
            {
                FinalizeOnRunner();
                return;
            }
            if (rejectClosingSession)
            {
                Disconnect(Core::Status::Fail(
                    Core::ErrorCode::Closed, "the ServerHost stopped accepting this session"));
                return;
            }
            if (!registered.IsOk())
            {
                FailAndDisconnect(std::move(registered));
                return;
            }

            // 관찰자는 이 callback 안에서 곧바로 Disconnect할 수 있다. callback 동안에는
            // finalization 잠금을 잡지 않고, 실행자 Post까지 실패한 종료만 아래로 미뤄
            // OnOpened 뒤에 Registry 제거와 OnClosed가 이어지게 한다.
            host->NotifyOpened(self);

            CompleteLifecycleNotification();
        }
        catch (const std::bad_alloc&)
        {
            FailAndDisconnect(Core::Status::AllocationFailure());
        }
    }

    void AbortBeforeOpened(Core::Status reason) noexcept
    {
        RecordFailure(reason);
        try
        {
            RememberCloseReason(std::move(reason));
        }
        catch (...)
        {
            // 사유 보관 실패는 수락 수명 정리를 막지 않는다.
        }

        mSessionState.store(ServerCore::Session::SessionState::Closed, std::memory_order_release);
        DiscardQueuedReceiveBytes();

        // Open 작업을 queue에 넣지 못한 경로도 I/O thread의 finalizer fallback과 같은 claim을
        // 쓴다. 둘이 겹쳐도 예약·Registry·Host slot은 정확히 한 번만 정리된다.
        FinalizeAfterRunnerFailure();

        try
        {
            mConnection->Close();
        }
        catch (...)
        {
            Core::ReportAssertFailure("Connection::Close() did not throw", __FILE__, __LINE__,
                "a Connection threw while aborting a session before it opened");
        }
    }

private:
    void ConsumePendingBytesOnRunner();
    void CompleteReceiveBatchOnRunner(std::size_t batchSize, std::size_t chunks);
    void ReleaseDeferredReceiveBatchOnRunner();
    [[nodiscard]] bool HasUndispatchedFramesOnRunner();
    void DiscardQueuedReceiveBytes();
    void DiscardUnregisteredId() noexcept;
    void FinalizeAfterRunnerFailure() noexcept;

public:
    [[nodiscard]] std::size_t QueuedSendBytes() const noexcept override
    {
        return mConnection->QueuedSendBytes();
    }

    /// <summary>Host가 이 세션에 준 종료 슬롯 번호를 기록한다. Host의 mLifecycleMutex 아래에서만 부른다.</summary>
    void AssignSlot(const std::size_t index) noexcept { mSlotIndex = index; }

    /// <summary>AssignSlot이 기록한 번호다. Host의 mLifecycleMutex 아래에서만 읽는다.</summary>
    [[nodiscard]] std::size_t SlotIndex() const noexcept { return mSlotIndex; }
    [[nodiscard]] std::size_t RetainedSendBytes() const noexcept
    {
        const auto flow = Net::GetConnectionFlowControl(mConnection);
        return flow ? flow->RetainedSendBytes() : 0;
    }

    Core::Result<Core::CompletionSubscription> WaitForSendCapacity(std::size_t requiredBytes,
        std::function<void(Core::Status)> callback, std::stop_token cancellation) override
    {
        using Result = Core::Result<Core::CompletionSubscription>;
        if (!callback || requiredBytes == 0)
            return Result::FromStatus(
                Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
        const auto state = State();
        if (state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
            return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        const auto flow = Net::GetConnectionFlowControl(mConnection);
        if (!flow)
            return Result::FromStatus(
                Core::Status::FailWithoutMessage(Core::ErrorCode::Unimplemented));
        try
        {
            // 구독자는 Host가 만든 바깥 구독을 받는다. 전송 계층의 안쪽 구독은 바깥 callback이
            // 소유하므로, 바깥을 Reset하거나 완료하면 안쪽 등록도 함께 풀려 다음 대기를 막지 않는다.
            auto inner = std::make_shared<std::optional<Core::CompletionSubscription>>();
            auto outer = Core::CompletionSubscription::Create(
                [inner, callback = std::move(callback)](Core::Status status)
                { callback(std::move(status)); });
            if (!outer.IsOk())
                return outer;
            auto relay = std::make_shared<SendCapacityRelay>(outer.Value().GetSource());
            // 등록 중 즉시 완료되어도 relay가 Arm 전까지 보관한다. 등록이 실패하면 callback 없이 오류만 돌려준다.
            auto registered = flow->WaitForSendCapacity(requiredBytes,
                [relay](Core::Status status) { SendCapacityRelay::Deliver(relay, status.Code()); });
            if (!registered.IsOk())
                return Result::FromStatus(std::move(registered).TakeStatus());
            *inner = std::move(registered.Value());
            // 이미 멈춘 토큰은 여기서 Cancelled를 먼저 넘기고, 뒤의 Arm이 넘기는 준비 완료는 무시된다.
            outer.Value().BindCancellation(GetCancellationToken(), cancellation);
            SendCapacityRelay::Arm(relay);
            return outer;
        }
        catch (...)
        {
            return Result::FromStatus(Core::Status::AllocationFailure());
        }
    }

    void DrainSends()
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard guard(mOutboundMutex);
        if (BeginGracefulCloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed)))
            mConnection->CloseAfterSend();
    }

    void StopReceiving() noexcept
    {
        const std::lock_guard guard(mReceiveMutex);
        mInputClosed = true;
    }

    /// <summary>graceful 절대 기한이나 열린 세션의 유휴 제한을 넘겼으면 즉시 닫는다.</summary>
    /// <remarks>
    /// Closing은 수신 시각 대신 Closing 시작 시각을 본다. 따라서 유휴 검사가 정상 drain을
    /// 앞질러 취소하거나, 종료 중 들어온 바이트가 drain 기한을 늘릴 수 없다. 잠금 순서는
    /// idle 뒤 outbound이며 수신 경로는 두 잠금을 함께 잡지 않으므로 역순 교착이 없다.
    /// </remarks>
    [[nodiscard]] bool DisconnectIfExpired(const std::uint64_t nowMilliseconds,
        const std::uint64_t idleTimeoutMilliseconds,
        const std::uint64_t gracefulTimeoutMilliseconds)
    {
        FinalizationDeferral finalization(*this);
        const SessionLockGuard idleGuard(mIdleMutex);
        const SessionLockGuard outboundGuard(mOutboundMutex);
        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (state == ServerCore::Session::SessionState::Closed)
        {
            return false;
        }
        if (state == ServerCore::Session::SessionState::Closing)
        {
            if (!mGracefulCloseStartedMilliseconds.has_value() ||
                *mGracefulCloseStartedMilliseconds > nowMilliseconds ||
                nowMilliseconds - *mGracefulCloseStartedMilliseconds < gracefulTimeoutMilliseconds)
            {
                return false;
            }
            // 첫 종료 사유는 유지한다. 남은 WSASend 버퍼는 취소 완료 뒤 회수되며, 그 뒤에만
            // OnDisconnected가 세션 슬롯을 반환한다. 새 할당도 필요하지 않다.
            mConnection->Close();
            return true;
        }
        const auto host = mHost.lock();
        if (host)
        {
            const auto elapsed = [nowMilliseconds](
                                     std::uint64_t start, std::chrono::milliseconds timeout)
            {
                return timeout.count() > 0 && nowMilliseconds >= start &&
                       nowMilliseconds - start >= static_cast<std::uint64_t>(timeout.count());
            };
            const auto incomplete = mIncompleteStarted.load(std::memory_order_acquire);
            if ((state == ServerCore::Session::SessionState::Connected &&
                    elapsed(mCreatedAt, host->mOptions.authenticationTimeout)) ||
                (incomplete != 0 && elapsed(incomplete - 1, host->mOptions.frameCompletionTimeout)))
            {
                DisconnectLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout));
                return true;
            }
        }
        if (idleTimeoutMilliseconds == 0 || mLastReceivedMilliseconds > nowMilliseconds ||
            nowMilliseconds - mLastReceivedMilliseconds < idleTimeoutMilliseconds)
        {
            return false;
        }

        DisconnectLocked(
            Core::Status::Fail(Core::ErrorCode::Timeout, "the session exceeded its idle timeout"));
        return true;
    }

private:
    void CompleteLifecycleNotification() noexcept
    {
        bool finalizeAfterNotification = false;
        {
            const std::lock_guard<std::mutex> guard(mFinalizationMutex);
            SERVERCORE_ASSERT(mLifecycleNotificationDepth != 0,
                "a NetworkSession lifecycle notification completed without beginning");
            --mLifecycleNotificationDepth;
            if (mLifecycleNotificationDepth == 0)
            {
                finalizeAfterNotification = std::exchange(mFinalizeAfterNotification, false);
            }
        }

        if (finalizeAfterNotification)
        {
            FinalizeAfterRunnerFailure();
        }
    }

    void RecordFailure(const Core::Status& status) const noexcept
    {
        if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
        {
            host->RecordError(status);
        }
    }

    [[nodiscard]] Core::Status RecordAndReturn(Core::Status status) const
    {
        RecordFailure(status);
        return status;
    }

    [[nodiscard]] bool CanSendLocked() const noexcept
    {
        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        return state != ServerCore::Session::SessionState::Closing &&
               state != ServerCore::Session::SessionState::Closed;
    }

    [[nodiscard]] bool BeginGracefulCloseLocked(Core::Status reason)
    {
        // local 종료의 선형화 지점은 상태 전이와 사유 설치를 함께 보호하는 이 잠금 안이다.
        // mOutboundMutex 다음에 이 잠금을 얻으며, 역순으로 두 잠금을 잡는 경로는 없다.
        const std::lock_guard<std::mutex> reasonGuard(mCloseReasonMutex);
        ServerCore::Session::SessionState current = mSessionState.load(std::memory_order_acquire);
        for (;;)
        {
            if (current == ServerCore::Session::SessionState::Closing ||
                current == ServerCore::Session::SessionState::Closed)
            {
                return false;
            }

            if (mSessionState.compare_exchange_weak(current,
                    ServerCore::Session::SessionState::Closing, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                RememberCloseReasonLocked(std::move(reason));
                mGracefulCloseStartedMilliseconds = Core::MillisecondsSinceProcessStart();
                return true;
            }
        }
    }

    void DisconnectLocked(Core::Status reason)
    {
        std::unique_lock<std::mutex> reasonGuard(mCloseReasonMutex);
        ServerCore::Session::SessionState current = mSessionState.load(std::memory_order_acquire);
        for (;;)
        {
            if (current == ServerCore::Session::SessionState::Closed)
            {
                return;
            }
            if (current == ServerCore::Session::SessionState::Closing)
            {
                // SendAndDisconnect()가 시작한 drain도 Host 종료 같은 명시적 즉시 종료는 취소할 수
                // 있어야 한다. 첫 종료 사유는 이미 보관되어 있으므로 여기서 덮어쓰지 않는다.
                reasonGuard.unlock();
                mConnection->Close();
                return;
            }

            if (mSessionState.compare_exchange_weak(current,
                    ServerCore::Session::SessionState::Closing, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                RememberCloseReasonLocked(std::move(reason));
                // Close()는 진행 중인 I/O가 없으면 OnDisconnected를 동기 호출할 수 있다. 그
                // callback이 같은 사유 잠금을 얻도록 transport 경계 전에 놓는다.
                reasonGuard.unlock();
                mConnection->Close();
                return;
            }
        }
    }

    [[nodiscard]] Core::Result<std::vector<std::byte>> EncodeBinaryFrame(
        const std::uint32_t type, const std::span<const std::byte> payload) const
    {
        Core::Result<std::vector<std::byte>> encoded =
            Protocol::EncodeBinaryMessage(type, payload, mMaxBodySize);
        if (!encoded.IsOk())
        {
            return encoded;
        }
        return Protocol::EncodeFrame(encoded.Value(), mMaxBodySize);
    }

    [[nodiscard]] Core::Result<std::vector<std::byte>> PrepareOutboundFrame(
        const Protocol::MessageFields& fields) const
    {
        if (const auto host = mHost.lock();
            host && host->mOptions.payloadMode != Protocol::PayloadMode::Json)
            return Core::Result<std::vector<std::byte>>::FromStatus(
                Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
        Core::Result<std::vector<std::byte>> serialized = Protocol::SerializeMessage(fields);
        if (!serialized.IsOk())
        {
            return Core::Result<std::vector<std::byte>>::FromStatus(
                std::move(serialized).TakeStatus());
        }

        return Protocol::EncodeFrame(serialized.Value(), mMaxBodySize);
    }

    void FailAndDisconnect(Core::Status failure)
    {
        RecordFailure(failure);
        Disconnect(std::move(failure));
    }

    void ConsumeBytesOnRunner(const std::span<const std::byte> bytes)
    {
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr || !host->mJobRunner.IsCurrentThread())
        {
            return;
        }

        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
        {
            return;
        }

        const auto incomplete = mIncompleteStarted.load(std::memory_order_acquire);
        const auto timeout = host->mOptions.frameCompletionTimeout.count();
        if (incomplete != 0 && timeout > 0 && mCurrentBatchReceivedAt >= incomplete - 1 &&
            mCurrentBatchReceivedAt - (incomplete - 1) >= static_cast<std::uint64_t>(timeout))
        {
            FailAndDisconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout));
            return;
        }

        if (mParseAccounting == nullptr)
        {
            ConsumeSynchronouslyOnRunner(host, bytes);
            return;
        }

        ConsumeWithParseWorkersOnRunner(bytes);
    }

    static bool AdmitFrame(void* context, std::size_t size) noexcept
    {
        auto& self = *static_cast<NetworkSession*>(context);
        const auto host = self.mHost.lock();
        if (!host)
            return false;
        if (self.mCurrentBatchReceivedAt - self.mFrameWindowStarted >= 1000)
        {
            self.mFrameWindowStarted = self.mCurrentBatchReceivedAt;
            self.mFrameWindowCount = 0;
        }
        const auto maximum = host->mOptions.maxInputFramesPerSecond;
        if (maximum != 0 && self.mFrameWindowCount >= maximum)
            return false;
        if (maximum != 0)
            ++self.mFrameWindowCount;
        return self.mAppendParseAdmission == nullptr ||
               ParseAdmission::Admit(self.mAppendParseAdmission, size);
    }

    void UpdateFrameDeadline(std::size_t completedBefore) noexcept
    {
        if (!mFrameReader.HasIncompleteFrame())
            mIncompleteStarted.store(0, std::memory_order_release);
        else if (mIncompleteStarted.load(std::memory_order_relaxed) == 0 ||
                 mFrameReader.CompletedFrameCount() > completedBefore)
            mIncompleteStarted.store(mCurrentBatchReceivedAt + 1, std::memory_order_release);
    }

    void ConsumeSynchronouslyOnRunner(
        const std::shared_ptr<ServerHost::State>& host, const std::span<const std::byte> bytes)
    {
        Core::Status appended = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            const ServerCore::Session::SessionState state =
                mSessionState.load(std::memory_order_acquire);
            if (state == ServerCore::Session::SessionState::Closing ||
                state == ServerCore::Session::SessionState::Closed ||
                mFinalized.load(std::memory_order_acquire))
            {
                return;
            }
            const auto completeBefore = mFrameReader.CompletedFrameCount();
            appended = mFrameReader.Append(bytes, this, &NetworkSession::AdmitFrame);
            UpdateFrameDeadline(completeBefore);
        }
        if (!appended.IsOk())
        {
            FailAndDisconnect(std::move(appended));
            return;
        }

        DispatchCompletedFramesOnRunner(host);
    }

    /// <summary>완결 frame을 이번 수신 작업의 남은 dispatch 한도만큼 처리기로 넘긴다.</summary>
    /// <remarks>한도가 바닥나면 남은 frame은 FrameReader에 두고 다음 수신 작업이 이어 받는다.</remarks>
    void DispatchCompletedFramesOnRunner(const std::shared_ptr<ServerHost::State>& host)
    {
        while (mDispatchBudget != 0)
        {
            Core::Result<std::vector<std::byte>> frame = [&]()
            {
                const std::lock_guard<std::mutex> guard(mParseStateMutex);
                return mFrameReader.TakeNextFrame();
            }();
            if (!frame.IsOk())
            {
                if (frame.GetStatus().Code() == Core::ErrorCode::WouldBlock)
                {
                    return;
                }

                FailAndDisconnect(std::move(frame).TakeStatus());
                return;
            }

            --mDispatchBudget;
            host->RecordReceivedFrame();

            if (host->mOptions.payloadMode == Protocol::PayloadMode::Binary)
            {
                auto message = Protocol::DecodeBinaryMessage(frame.Value());
                if (!message.IsOk())
                {
                    FailAndDisconnect(std::move(message).TakeStatus());
                    return;
                }
                if (!BeginMessageDispatch())
                    return;
                Core::Status dispatched = Core::Status::Ok();
                try
                {
                    dispatched = host->mBinaryHandler(shared_from_this(), message.Value());
                }
                catch (...)
                {
                    CompleteLifecycleNotification();
                    throw;
                }
                CompleteLifecycleNotification();
                if (!dispatched.IsOk())
                {
                    host->RecordError(dispatched);
                    host->LogMessageFailure(dispatched, mId);
                }
                const auto state = State();
                if (state == ServerCore::Session::SessionState::Closing ||
                    state == ServerCore::Session::SessionState::Closed)
                    return;
                continue;
            }

            Core::Result<Protocol::Message> message =
                Protocol::ParseMessage(frame.Value(), HostJsonParseLimits(host->mOptions));
            if (!message.IsOk())
            {
                FailAndDisconnect(std::move(message).TakeStatus());
                return;
            }

            if (!BeginMessageDispatch())
            {
                return;
            }

            Core::Status dispatched = Core::Status::Ok();
            try
            {
                dispatched = host->mDispatcher.Dispatch(shared_from_this(), message.Value());
            }
            catch (...)
            {
                CompleteLifecycleNotification();
                throw;
            }
            CompleteLifecycleNotification();
            if (!dispatched.IsOk())
            {
                host->RecordError(dispatched);
                // L4는 handler 거부, type별 body 상한, body 없는 error 봉투를 연결 종료로
                // 번역하지 않는다. UnknownType의 Disconnect 정책도 Dispatcher가 이미 처리한다.
                host->LogMessageFailure(dispatched, mId);
            }

            const ServerCore::Session::SessionState afterDispatch =
                mSessionState.load(std::memory_order_acquire);
            if (afterDispatch == ServerCore::Session::SessionState::Closing ||
                afterDispatch == ServerCore::Session::SessionState::Closed)
            {
                return;
            }
        }
    }

    void ConsumeWithParseWorkersOnRunner(const std::span<const std::byte> bytes)
    {
        SERVERCORE_ASSERT(mParseAccounting != nullptr,
            "parse worker consumption requires a NetworkSession parse accounting ledger");
        Core::Status appended = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            const ServerCore::Session::SessionState state =
                mSessionState.load(std::memory_order_acquire);
            if (state == ServerCore::Session::SessionState::Closing ||
                state == ServerCore::Session::SessionState::Closed ||
                mFinalized.load(std::memory_order_acquire))
            {
                return;
            }

            const std::size_t completedBytesBefore = mFrameReader.CompletedBodyBytes();
            const std::size_t completedFramesBefore = mFrameReader.CompletedFrameCount();
            ParseAdmission admission(*mParseAccounting);
            mAppendParseAdmission = &admission;
            appended = mFrameReader.Append(bytes, this, &NetworkSession::AdmitFrame);
            mAppendParseAdmission = nullptr;
            UpdateFrameDeadline(completedFramesBefore);

            const std::size_t completedBytesAfter = mFrameReader.CompletedBodyBytes();
            const std::size_t completedFramesAfter = mFrameReader.CompletedFrameCount();
            SERVERCORE_ASSERT(completedBytesAfter >= completedBytesBefore,
                "FrameReader completed bytes decreased while appending");
            SERVERCORE_ASSERT(completedFramesAfter >= completedFramesBefore,
                "FrameReader completed frame count decreased while appending");
            const std::size_t newCompletedBytes = completedBytesAfter - completedBytesBefore;
            const std::size_t newCompletedFrames = completedFramesAfter - completedFramesBefore;
            admission.ReleaseNotStored(newCompletedBytes, newCompletedFrames);
            mUnscheduledParseBytes += newCompletedBytes;
            mUnscheduledParseTasks += newCompletedFrames;

            if (!appended.IsOk())
            {
                // admission 거부 또는 FrameReader 자체 실패 전에 같은 batch의 앞 frame이 완료될
                // 수 있다. fallback finalizer와 같은 잠금 아래 모두 비워 reservation을 한 번만
                // 반납한다.
                DiscardQueuedParseFramesLocked();
            }
        }

        if (!appended.IsOk())
        {
            FailAndDisconnect(std::move(appended));
            return;
        }

        StartNextParseOnRunner();
    }

    // 서로 다른 세션의 JSON만 병렬화한다. 한 세션은 현재 결과의 JobRunner 처리까지 끝낸 뒤
    // 다음 본문을 넘겨, Join 뒤 Profile/Chat이 worker 완료 순서 때문에 앞서 실행되지 않게 한다.
    void StartNextParseOnRunner()
    {
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr || !host->mJobRunner.IsCurrentThread() || !host->IsAccepting() ||
            mParseAccounting == nullptr)
        {
            return;
        }

        std::shared_ptr<ParseWorkItem> work;
        std::vector<std::byte> body;
        Core::Status preparation = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            const ServerCore::Session::SessionState state =
                mSessionState.load(std::memory_order_acquire);
            if (state == ServerCore::Session::SessionState::Closing ||
                state == ServerCore::Session::SessionState::Closed ||
                mFinalized.load(std::memory_order_acquire) || mParseInFlight)
            {
                return;
            }

            Core::Result<std::vector<std::byte>> nextBody = mFrameReader.TakeNextFrame();
            if (!nextBody.IsOk())
            {
                if (nextBody.GetStatus().Code() == Core::ErrorCode::WouldBlock)
                {
                    return;
                }
                preparation = std::move(nextBody).TakeStatus();
            }
            else
            {
                body = std::move(nextBody.Value());
                SERVERCORE_ASSERT(mUnscheduledParseTasks != 0,
                    "a parse frame was removed without a pending task reservation");
                SERVERCORE_ASSERT(mUnscheduledParseBytes >= body.size(),
                    "a parse frame was removed without enough pending byte reservation");
                --mUnscheduledParseTasks;
                mUnscheduledParseBytes -= body.size();

                try
                {
                    work = std::make_shared<ParseWorkItem>(
                        mParseAccounting, std::move(body), HostJsonParseLimits(host->mOptions));
                    mParseInFlight = true;
                }
                catch (const std::bad_alloc&)
                {
                    // body는 completed queue에서 이미 빠졌으므로 이 reservation은 여기서 직접
                    // 반납한다. 실제 payload를 먼저 놓아 aggregate 상한에 빈틈을 만들지 않는다.
                    const std::size_t bodyBytes = body.size();
                    std::vector<std::byte>().swap(body);
                    mParseAccounting->Release(bodyBytes, 1);
                    preparation = Core::Status::AllocationFailure();
                }
            }
        }

        if (!preparation.IsOk())
        {
            FailAndDisconnect(std::move(preparation));
            return;
        }

        host->RecordReceivedFrame();

        if (host->mParsePool == nullptr)
        {
            {
                const std::lock_guard<std::mutex> guard(mParseStateMutex);
                mParseInFlight = false;
            }
            FailAndDisconnect(Core::Status::Fail(
                Core::ErrorCode::Closed, "the ServerHost parse worker pool is not available"));
            return;
        }

        const std::weak_ptr<NetworkSession> self = weak_from_this();
        const JobRunner::Lease runner = host->mJobRunner.AcquireControlLease();
        Core::Status posted = Core::Status::Ok();
        try
        {
            posted = host->mParsePool->Post(
                [self, runner, work]() mutable
                {
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
                    WaitBeforeParseForTest();
#endif
                    work->Parse();
                    const auto failCompletionPost = [&self]() noexcept
                    {
                        try
                        {
                            if (const std::shared_ptr<NetworkSession> session = self.lock())
                            {
                                // 이 실패 경로에서는 추가 할당도 실패할 수 있다. 빈 설명을 써서
                                // Status 자체가 두 번째 OOM 원인이 되지 않게 한다.
                                session->FailAndDisconnect(Core::Status::AllocationFailure());
                            }
                        }
                        catch (...)
                        {
                            // worker 밖으로 예외를 새면 ParseWorkerPool이 프로세스를 중단한다. 연결
                            // Close의 최선 노력까지 실패한 경우에는 reservation 소멸만 보장한다.
                        }
                    };

                    try
                    {
                        Core::Status completed = runner.Post(
                            [self, work]() mutable
                            {
                                if (const std::shared_ptr<NetworkSession> session = self.lock())
                                {
                                    session->CompleteParsedMessageOnRunner(std::move(work));
                                }
                            });
                        if (!completed.IsOk())
                        {
                            // JobRunner가 살아 있는 동안의 allocation 실패는 이 세션이 다음 frame을
                            // 영구히 기다리게 해서는 안 된다. worker는 Dispatcher/Registry를 만지지
                            // 않고 thread-safe한 Disconnect 경계만 통과시킨다. 정상 Stop의 Closed도
                            // 같은 경로를 타지만 CloseAllSessions가 뒤이어 정리하므로 무해하다.
                            if (const std::shared_ptr<NetworkSession> session = self.lock())
                            {
                                session->FailAndDisconnect(std::move(completed));
                            }
                        }
                    }
                    catch (...)
                    {
                        // lambda를 std::function으로 바꾸는 과정도 JobRunner::Post() 호출 전에
                        // 할당 실패할 수 있다. 그 경우에도 연결을 닫아 reservation을 끝낸다.
                        failCompletionPost();
                    }
                });
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
            {
                const std::lock_guard<std::mutex> guard(mParseStateMutex);
                mParseInFlight = false;
            }
            FailAndDisconnect(std::move(posted));
        }
    }

    [[nodiscard]] bool BeginMessageDispatch() noexcept
    {
        const std::lock_guard<std::mutex> guard(mFinalizationMutex);
        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (mFinalized.load(std::memory_order_relaxed) ||
            state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
        {
            return false;
        }

        // off-runner finalizer fallback은 이 깊이가 0이 될 때까지 OnSessionClosed를 미룬다.
        // 그러므로 닫힘 통지가 이미 시작된 뒤 새 handler가 호출되거나, 실행 중인 handler보다
        // 닫힘 통지가 앞서는 순서가 생기지 않는다.
        ++mLifecycleNotificationDepth;
        return true;
    }

    [[nodiscard]] bool BeginFinalizationDeferral() noexcept
    {
        const std::lock_guard<std::mutex> guard(mFinalizationMutex);
        if (mFinalized.load(std::memory_order_relaxed))
        {
            return false;
        }

        ++mLifecycleNotificationDepth;
        return true;
    }

    void CompleteParsedMessageOnRunner(std::shared_ptr<ParseWorkItem> work)
    {
        SERVERCORE_ASSERT(work != nullptr, "a parse completion arrived without owned work");

        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr || !host->mJobRunner.IsCurrentThread())
        {
            return;
        }

        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            SERVERCORE_ASSERT(mParseInFlight,
                "a parse completion arrived without an active NetworkSession parse task");
            mParseInFlight = false;
        }

        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (!host->IsAccepting() || state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed ||
            mFinalized.load(std::memory_order_acquire))
        {
            return;
        }

        Core::Result<Protocol::Message>& message = work->Message();
        if (!message.IsOk())
        {
            FailAndDisconnect(std::move(message).TakeStatus());
            return;
        }

        if (!BeginMessageDispatch())
        {
            return;
        }

        Core::Status dispatched = Core::Status::Ok();
        try
        {
            dispatched = host->mDispatcher.Dispatch(shared_from_this(), message.Value());
        }
        catch (...)
        {
            CompleteLifecycleNotification();
            throw;
        }
        CompleteLifecycleNotification();
        if (!dispatched.IsOk())
        {
            host->RecordError(dispatched);
            host->LogMessageFailure(dispatched, mId);
        }

        const ServerCore::Session::SessionState afterDispatch =
            mSessionState.load(std::memory_order_acquire);
        if (afterDispatch != ServerCore::Session::SessionState::Closing &&
            afterDispatch != ServerCore::Session::SessionState::Closed)
        {
            StartNextParseOnRunner();
        }
    }

    void DiscardQueuedParseFrames() noexcept
    {
        const std::lock_guard<std::mutex> guard(mParseStateMutex);
        DiscardQueuedParseFramesLocked();
    }

    void DiscardQueuedParseFramesLocked() noexcept
    {
        // 동기 parse도 handler가 현재 frame에서 Disconnect하면 같은 Append에서 완성된 뒤 frame을
        // 남길 수 있다. 외부가 닫힌 Session을 계속 소유해도 completed/current payload뿐 아니라
        // maxBodySize 크기의 codec 저장소까지 즉시 놓는다.
        mFrameReader.ReleaseStorage();
        if (mParseAccounting != nullptr)
        {
            mParseAccounting->Release(mUnscheduledParseBytes, mUnscheduledParseTasks);
        }
        mUnscheduledParseBytes = 0;
        mUnscheduledParseTasks = 0;
    }

    void FinalizeOnRunner()
    {
        RequestCancellationIfClosing();
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr)
        {
            return;
        }

        SERVERCORE_ASSERT(host->mJobRunner.IsCurrentThread(),
            "NetworkSession::FinalizeOnRunner() must run in the ServerHost JobRunner context");

        bool registered = false;
        bool opened = false;
        {
            const std::lock_guard<std::mutex> guard(mFinalizationMutex);
            if (mFinalized.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }

            registered = mRegistered.exchange(false, std::memory_order_acq_rel);
            opened = mOpened.exchange(false, std::memory_order_acq_rel);
        }

        // 아직 worker에 넘기지 않은 완료 frame은 이 시점부터 처리하지 않는다. 실행 중인 한 건은
        // ParseReservation이 worker/완료 lambda 수명 끝까지 따로 붙들므로 여기서 건드리지 않는다.
        DiscardQueuedParseFrames();

        if (registered)
        {
            const Core::Status unregistered = host->mRegistry.Unregister(mId);
            if (!unregistered.IsOk() && unregistered.Code() != Core::ErrorCode::NotFound)
            {
                host->LogStatus(Core::LogLevel::Warn, unregistered, mId);
            }
        }
        else
        {
            // OpenOnRunner 전에 끊겼거나 Register 자체가 실패한 경우에도 IssueId 예약은 남기지
            // 않는다. 이 호출은 I/O 완료와 경합해도 SessionRegistry의 예약 잠금이 한 번만
            // 소비하게 한다.
            DiscardUnregisteredId();
        }

        // slot을 통지 전에 비워야 외부 fallback 통지 안의 Stop()이 자기 세션을 기다리지 않는다.
        // 대신 별도의 통지 계수를 같은 잠금에서 먼저 올려, 다른 Stop/Run 호출은 callback 반환까지
        // 계속 기다리게 한다. slot이 마지막 소유자여도 아래 통지 동안 this가 살아 있어야 한다.
        const std::shared_ptr<NetworkSession> keepAlive = weak_from_this().lock();
        SERVERCORE_ASSERT(
            keepAlive != nullptr, "a finalizing NetworkSession lost all shared ownership");
        host->FinishNetworkSession(this, opened);

        if (opened)
        {
            host->NotifyClosed(mId, TakeCloseReason());
        }
    }

    void RememberCloseReason(Core::Status reason)
    {
        const std::lock_guard<std::mutex> guard(mCloseReasonMutex);
        RememberCloseReasonLocked(std::move(reason));
    }

    void RememberCloseReasonLocked(Core::Status reason)
    {
        if (mHasCloseReason)
        {
            return;
        }

        mCloseReason = std::move(reason);
        mHasCloseReason = true;
    }

    [[nodiscard]] Core::Status TakeCloseReason()
    {
        const std::lock_guard<std::mutex> guard(mCloseReasonMutex);
        if (mHasCloseReason)
        {
            mHasCloseReason = false;
            return std::move(mCloseReason);
        }

        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    }

    std::weak_ptr<ServerHost::State> mHost;
    ServerCore::Session::SessionId mId;
    std::shared_ptr<Net::Connection> mConnection;
    std::size_t mSlotIndex = (std::numeric_limits<std::size_t>::max)();
    std::stop_source mCancellation;
    const std::uint64_t mCreatedAt = Core::MillisecondsSinceProcessStart();
    std::atomic<std::uint64_t> mIncompleteStarted{ 0 }; // timestamp + 1, zero means none
    std::uint64_t mByteWindowStarted = 0;
    std::size_t mByteWindowBytes = 0;
    std::uint64_t mFrameWindowStarted = 0;
    std::size_t mFrameWindowCount = 0;
    std::uint64_t mCurrentBatchReceivedAt = 0;
    std::uint64_t mLastReceivedMilliseconds = 0;
    // FrameReader는 원래 JobRunner 단일 스레드 전용이다. finalizer Post 실패만 I/O 스레드에서
    // completed payload를 직접 버려야 하므로, 그 fallback과 모든 parser 상태 변경을 직렬화한다.
    std::mutex mParseStateMutex;
    Protocol::FrameReader mFrameReader;
    std::uint32_t mMaxBodySize;
    std::uint32_t mMaxPendingReceiveBytes;
    std::shared_ptr<ParseAccounting> mParseAccounting;
    ParseAdmission* mAppendParseAdmission = nullptr;
    std::size_t mUnscheduledParseBytes = 0;
    std::size_t mUnscheduledParseTasks = 0;
    bool mParseInFlight = false;
    std::mutex mIdleMutex;
    std::mutex mReceiveMutex;
    std::vector<std::byte> mPendingReceiveBytes;
    struct ReceiveChunk
    {
        std::size_t bytes;
        std::uint64_t receivedAt;
    };
    std::vector<ReceiveChunk> mPendingReceiveChunks;
    std::size_t mBufferedReceiveChunks = 0;
    // mPendingReceiveBytes에서 꺼내 처리 중인 batch도 포함한다. vector를 swap했다는 이유로
    // 수신 예산을 먼저 돌려주면 처리 중인 payload와 새 수신이 상한 밖에서 공존할 수 있다.
    std::size_t mBufferedReceiveBytes = 0;
    // 아래 셋은 runner만 만진다. 동기 파싱에서 dispatch 한도 때문에 FrameReader에 남은 frame이 있으면
    // 그 batch의 수신 예산을 바로 돌려주지 않고 여기에 미뤄, 남은 frame까지 예산 안에 둔다.
    std::size_t mDispatchBudget = 0;
    std::size_t mDeferredReceiveBytes = 0;
    std::size_t mDeferredReceiveChunks = 0;
    bool mReceiveJobQueued = false;
    bool mInputClosed = false;
    std::mutex mOutboundMutex;
    // mOutboundMutex가 보호하며, 시계가 0인 첫 밀리초에도 유효한 시작 시각이므로 optional이다.
    std::optional<std::uint64_t> mGracefulCloseStartedMilliseconds;
    std::atomic<ServerCore::Session::SessionState> mSessionState{
        ServerCore::Session::SessionState::Connected
    };
    std::mutex mFinalizationMutex;
    std::atomic<bool> mRegistered{ false };
    std::atomic<bool> mOpened{ false };
    std::atomic<bool> mFinalized{ false };
    std::size_t mLifecycleNotificationDepth = 0;
    bool mFinalizeAfterNotification = false;
    std::mutex mCloseReasonMutex;
    Core::Status mCloseReason = Core::Status::Ok();
    bool mHasCloseReason = false;
};
}
