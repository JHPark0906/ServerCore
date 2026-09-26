// ServerHost의 연결 수락, 세션 슬롯, 수신·parse 예산, 세션 만료 검사다.
#include "Runtime/ServerHostNetworkSession.h"

namespace ServerCore::Runtime
{
bool ServerHost::State::IsAccepting() const noexcept
{
    return mAccepting.load(std::memory_order_acquire);
}

bool ServerHost::State::TryReservePendingReceiveBytes(const std::size_t byteCount) noexcept
{
    return Core::Detail::TryReserveBudget(
        mPendingReceiveBytes, mOptions.maxTotalPendingReceiveBytes, byteCount);
}

void ServerHost::State::ReleasePendingReceiveBytes(const std::size_t byteCount) noexcept
{
    Core::Detail::ReleaseBudget(mPendingReceiveBytes, byteCount,
        "released receive bytes exceeded the ServerHost aggregate budget");
}

bool ServerHost::State::TryReservePendingParseWork(
    const std::size_t byteCount, const std::size_t taskCount) noexcept
{
    if (!Core::Detail::TryReserveBudget(
            mPendingParseBytes, mOptions.maxTotalPendingParseBytes, byteCount))
        return false;
    if (Core::Detail::TryReserveBudget(
            mPendingParseTasks, mOptions.maxTotalPendingParseTasks, taskCount))
        return true;

    Core::Detail::ReleaseBudget(mPendingParseBytes, byteCount,
        "released parse work exceeded the ServerHost aggregate budget");
    return false;
}

void ServerHost::State::ReleasePendingParseWork(
    const std::size_t byteCount, const std::size_t taskCount) noexcept
{
    Core::Detail::ReleaseBudget(mPendingParseBytes, byteCount,
        "released parse work exceeded the ServerHost aggregate budget");
    Core::Detail::ReleaseBudget(mPendingParseTasks, taskCount,
        "released parse work exceeded the ServerHost aggregate budget");
}

bool ServerHost::State::BeginNetworkSession()
{
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    if (!mAccepting.load(std::memory_order_acquire) || mDraining.load(std::memory_order_acquire))
    {
        return false;
    }
    if (mOutstandingSessions >= static_cast<std::size_t>(mOptions.maxConcurrentSessions))
    {
        return false;
    }

    ++mOutstandingSessions;
    return true;
}

void ServerHost::State::TrackNetworkSession(const std::shared_ptr<NetworkSession>& session) noexcept
{
    SERVERCORE_ASSERT(session != nullptr, "ServerHost cannot track a null NetworkSession");

    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    SERVERCORE_ASSERT(mOutstandingSessions != 0,
        "ServerHost tracked a NetworkSession before reserving an outstanding slot");

    SERVERCORE_ASSERT(!mFreeSessionSlots.empty(),
        "ServerHost exhausted its preallocated NetworkSession shutdown slots");
    const std::size_t index = mFreeSessionSlots.back();
    mFreeSessionSlots.pop_back();
    mSessionSlots[index] = session;
    session->AssignSlot(index);
}

void ServerHost::State::FinishNetworkSession(
    const NetworkSession* const session, const bool beginCloseNotification) noexcept
{
    if (session != nullptr && mDatagrams)
        mDatagrams->UnregisterSession(session->Id());
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    SERVERCORE_ASSERT(
        mOutstandingSessions != 0, "ServerHost finalized a network session that it did not count");

    // slot/outstanding이 0으로 보이는 순간과 OnSessionClosed 시작 사이에 Stop이 빠져나가지 않게
    // 같은 잠금 안에서 통지 계수를 먼저 올린다. callback 안의 Stop만 TLS 깊이만큼 이 계수를
    // 제외할 수 있고, 다른 Stop과 Run은 통지가 실제로 끝날 때까지 기다린다.
    if (beginCloseNotification)
    {
        ++mInFlightCloseNotifications;
    }

    if (session != nullptr)
    {
        const std::size_t index = session->SlotIndex();
        SERVERCORE_ASSERT(index < mSessionSlots.size() && mSessionSlots[index].get() == session,
            "ServerHost finalized a NetworkSession absent from its shutdown slots");
        mSessionSlots[index].reset();
        // 사용 중 슬롯과 빈 번호의 합은 Start가 잡은 용량과 같으므로 이 push_back은 할당하지 않는다.
        SERVERCORE_ASSERT(mFreeSessionSlots.size() < mFreeSessionSlots.capacity(),
            "ServerHost returned more session slots than it preallocated");
        mFreeSessionSlots.push_back(index);
    }

    --mOutstandingSessions;
    if (mOutstandingSessions == 0)
    {
        mLifecycleChanged.notify_all();
    }
}

void ServerHost::State::CloseAllSessions() noexcept
{
    for (std::size_t index = 0; index < mSessionSlots.size(); ++index)
    {
        std::shared_ptr<NetworkSession> session;
        {
            const std::lock_guard<std::mutex> guard(mLifecycleMutex);
            session = mSessionSlots[index];
        }

        if (session == nullptr)
        {
            continue;
        }

        try
        {
            session->Disconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }
        catch (...)
        {
            // 예외를 삼키고 기다리면 이 세션의 OnDisconnected가 오지 않아 Stop()이 영구히
            // 멈춘다. Disconnect의 공개 no-throw 계약 위반을 이 자리에서 명확히 드러낸다.
            Core::ReportAssertFailure("Session::Disconnect() did not throw", __FILE__, __LINE__,
                "a Session implementation threw while ServerHost was stopping");
        }
    }
}

void ServerHost::State::CloseExpiredSessionsOnRunner()
{
    SERVERCORE_ASSERT(mJobRunner.IsCurrentThread(),
        "ServerHost::CloseExpiredSessionsOnRunner() must run in the JobRunner context");

    if (!mAccepting.load(std::memory_order_acquire))
    {
        return;
    }
    AdvanceDrain();
    if (mLogger && mMessageLogLimiter)
    {
        mMessageLogLimiter->Flush(*mLogger, false);
    }

    const std::uint64_t nowMilliseconds = Core::MillisecondsSinceProcessStart();
    const std::uint64_t idleTimeoutMilliseconds =
        static_cast<std::uint64_t>(mOptions.idleSessionTimeout.count());
    const std::uint64_t gracefulTimeoutMilliseconds =
        static_cast<std::uint64_t>(mOptions.gracefulCloseTimeout.count());
    try
    {
        const Core::Status visited = mRegistry.ForEach(
            [nowMilliseconds, idleTimeoutMilliseconds, gracefulTimeoutMilliseconds](
                const std::shared_ptr<Session::Session>& session)
            {
                // ServerHost의 Registry에는 NetworkSession만 들어간다. 이 조립 지점에서만 구체 구현의
                // 유휴 시각을 읽어 Session 공개 API에 Host 내부 상태가 새지 않게 한다.
                const std::shared_ptr<NetworkSession> networkSession =
                    std::dynamic_pointer_cast<NetworkSession>(session);
                SERVERCORE_ASSERT(networkSession != nullptr,
                    "ServerHost registry contained a session it did not create");
                if (networkSession == nullptr)
                {
                    return;
                }
                (void)networkSession->DisconnectIfExpired(
                    nowMilliseconds, idleTimeoutMilliseconds, gracefulTimeoutMilliseconds);
            });
        // 유휴 검사는 다음 주기에 다시 온다. 메모리가 부족한 순간의 snapshot은 건너뛰고 오류로만 센다.
        RecordError(visited);
    }
    catch (const std::bad_alloc&)
    {
        // 유휴 검사는 다음 주기에 다시 온다. 메모리가 부족한 순간의 snapshot을 건너뛰되,
        // PeriodicRunner 콜백 밖으로 예외를 보내 서버 전체를 끝내지는 않는다.
        RecordError(Core::Status::AllocationFailure());
    }
}

void ServerHost::State::OnConnectionAccepted(std::shared_ptr<Net::Connection> connection) noexcept
{
    if (connection == nullptr)
    {
        return;
    }

    if (!IsAccepting())
    {
        connection->Close();
        return;
    }

    bool counted = false;
    bool issued = false;
    Session::SessionId issuedId = Session::SessionId::Invalid;
    std::shared_ptr<NetworkSession> session;
    try
    {
        // 상한이 찬 연결에는 번호를 발급하지 않는다. 그렇지 않으면 거절 연결이 IssueId 예약을
        // 계속 남겨 세션 상한과 무관하게 레지스트리 메모리를 키울 수 있다.
        if (!BeginNetworkSession())
        {
            connection->Close();
            return;
        }
        counted = true;

        Core::Result<Session::SessionId> id = mRegistry.IssueId();
        if (!id.IsOk())
        {
            RecordError(id.GetStatus());
            LogStatus(Core::LogLevel::Error, id.GetStatus());
            FinishNetworkSession(nullptr);
            counted = false;
            connection->Close();
            return;
        }
        issuedId = id.Value();
        issued = true;

        session = std::make_shared<NetworkSession>(weak_from_this(), issuedId, connection,
            mOptions.maxBodySize, mOptions.maxPendingReceiveBytes, mOptions.maxPendingParseBytes,
            mOptions.parseWorkerThreadCount == 0 ? 0 : mOptions.maxPendingParseTasks);
        TrackNetworkSession(session);
        connection->SetObserver(session);

        Core::Status posted = mJobRunner.PostControl([session]() { session->OpenOnRunner(); });
        if (!posted.IsOk())
        {
            session->AbortBeforeOpened(std::move(posted));
        }
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        else
        {
            // Acceptor는 이 callback이 돌아온 뒤 Connection::Start()를 부른다. pending I/O가 없는
            // Connection::Close()의 동기 종료 경계를 시험할 수 있도록 그 직전만 멈춘다.
            WaitBeforeConnectionStartForTest();
        }
#endif
    }
    catch (const std::bad_alloc&)
    {
        if (session != nullptr)
        {
            session->AbortBeforeOpened(Core::Status::AllocationFailure());
        }
        else if (counted)
        {
            RecordError(Core::Status::AllocationFailure());
            if (issued)
            {
                try
                {
                    (void)mRegistry.DiscardIssuedId(issuedId);
                }
                catch (...)
                {
                    // 지속 OOM에서도 I/O 완료 경계 밖으로 예외를 내보내지 않는다.
                }
            }
            FinishNetworkSession(nullptr);
            connection->Close();
        }
        else
        {
            connection->Close();
        }
    }
    catch (...)
    {
        if (session != nullptr)
        {
            session->AbortBeforeOpened(
                Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError));
        }
        else if (counted)
        {
            RecordError(Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError));
            if (issued)
            {
                try
                {
                    (void)mRegistry.DiscardIssuedId(issuedId);
                }
                catch (...)
                {
                    // 이 경로는 I/O 완료 처리다. 예약 정리 실패가 worker 밖으로 새면 안 된다.
                }
            }
            FinishNetworkSession(nullptr);
            connection->Close();
        }
        else
        {
            connection->Close();
        }
    }
}
}
