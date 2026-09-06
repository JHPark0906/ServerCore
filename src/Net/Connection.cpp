#include "Net/ConnectionInternal.h"

#include "Net/WinsockInternal.h"
#include "ServerCore/Core/Assert.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <utility>

namespace ServerCore::Net
{
SendBudget::SendBudget(const std::size_t limitBytes) noexcept
    : mLimitBytes(limitBytes)
{
    SERVERCORE_ASSERT(limitBytes != 0, "SendBudget requires a positive byte limit");
}

bool SendBudget::TryReserve(const std::size_t byteCount) noexcept
{
    if (byteCount == 0)
    {
        return true;
    }

    std::size_t current = mUsedBytes.load(std::memory_order_acquire);
    for (;;)
    {
        if (current > mLimitBytes || byteCount > mLimitBytes - current)
        {
            return false;
        }
        if (mUsedBytes.compare_exchange_weak(
                current, current + byteCount, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true;
        }
    }
}

void SendBudget::Release(const std::size_t byteCount) noexcept
{
    if (byteCount == 0)
    {
        return;
    }

    std::size_t current = mUsedBytes.load(std::memory_order_acquire);
    for (;;)
    {
        SERVERCORE_ASSERT(
            current >= byteCount, "released send payload exceeded the shared Connection budget");
        if (mUsedBytes.compare_exchange_weak(
                current, current - byteCount, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return;
        }
    }
}

std::size_t SendBudget::LimitBytes() const noexcept
{
    return mLimitBytes;
}

std::size_t SendBudget::UsedBytes() const noexcept
{
    return mUsedBytes.load(std::memory_order_acquire);
}

std::shared_ptr<TcpConnection> TcpConnection::Create(
    SOCKET socket, std::shared_ptr<WinsockScope> winsock, std::shared_ptr<SendBudget> sendBudget)
{
    SERVERCORE_ASSERT(socket != INVALID_SOCKET, "Create() was given INVALID_SOCKET");
    SERVERCORE_ASSERT(winsock != nullptr, "Create() was given a null Winsock reference");

    return std::make_shared<TcpConnection>(
        CreationKey{}, socket, std::move(winsock), std::move(sendBudget));
}

TcpConnection::TcpConnection(CreationKey, SOCKET socket, std::shared_ptr<WinsockScope> winsock,
    std::shared_ptr<SendBudget> sendBudget)
    : mWinsock(std::move(winsock))
    , mSendBudget(std::move(sendBudget))
    , mSocket(socket)
{
    mReceiveOperation.kind = IoOperationKind::Receive;
    mReceiveOperation.target = this;

    mSendOperation.kind = IoOperationKind::Send;
    mSendOperation.target = this;
}

TcpConnection::~TcpConnection()
{
    SERVERCORE_ASSERT(mPendingOperations == 0,
        "a connection was destroyed while an overlapped request was still pending");
    SERVERCORE_ASSERT(!mReceiveCallbackActive,
        "a connection was destroyed while an observer receive callback was still active");
    SERVERCORE_ASSERT(
        !mSendInFlight, "a connection was destroyed while a send request was still pending");

    // 정상 경로에서는 마지막 완료나 Close가 이미 큐를 비운다. 그래도 관찰자 없는 미시작 연결
    // 같은 마지막 소유 경계에서 공유 예산이 남지 않도록 소멸자가 최종 회수한다.
    DiscardQueuedSendsLocked();

    // 여기까지 왔다는 것은 이 객체를 가리키는 참조가 하나도 없다는 뜻이므로 다른 스레드가
    // 상태를 건드리지 않는다. 그래도 NotifyDisconnectedOnce가 잠금을 잡으므로 여기서는
    // 잡지 않는다.
    if (mSocket != INVALID_SOCKET)
    {
        ::shutdown(mSocket, SD_BOTH);
        ::closesocket(mSocket);
        mSocket = INVALID_SOCKET;
    }

    if (!mObserverAssigned)
    {
        return;
    }

    Core::Status reason = mClosed.load(std::memory_order_acquire)
                              ? std::move(mCloseReason)
                              : Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);

    NotifyDisconnectedOnce(std::move(reason));
}

Core::Status TcpConnection::Start()
{
    Core::Status result = Core::Status::Ok();
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        SERVERCORE_ASSERT(!mStarted, "Start() was called more than once on one connection");
        mStarted = true;
        result = StartReceiveLocked();
    }

    if (!result.IsOk())
    {
        // 수신을 걸지 못했으면 이 연결은 아무것도 받지 못한다. 걸린 요청이 없으므로 통지
        // 조건이 이미 맞는다.
        MaybeNotifyDisconnected();
    }

    return result;
}

Core::Status TcpConnection::Send(std::span<const std::byte> bytes)
{
    ConnectionSendOutcome outcome = SendWithOutcome(bytes);
    return std::move(outcome.status);
}

ConnectionSendOutcome TcpConnection::SendWithOutcome(std::span<const std::byte> bytes)
{
    bool connectionClosedByFailure = false;
    Core::Status status = SendInternal(bytes, connectionClosedByFailure);
    return ConnectionSendOutcome{ std::move(status), connectionClosedByFailure };
}

Core::Status TcpConnection::SendInternal(
    std::span<const std::byte> bytes, bool& connectionClosedByFailure)
{
    connectionClosedByFailure = false;
    try
    {
        Core::Status result = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mMutex);

            // 닫힘을 빈 요청보다 먼저 본다. 반대로 두면 닫힌 연결에 빈 것을 보내는 것이 성공으로
            // 보이고, 같은 연결이 요청 크기에 따라 다른 답을 준다.
            if (mClosed.load(std::memory_order_relaxed))
            {
                return Core::Status::Fail(
                    Core::ErrorCode::Closed, "Send() was called on a connection that is closed");
            }

            if (mCloseAfterSendRequested)
            {
                return Core::Status::Fail(Core::ErrorCode::Closed,
                    "Send() was called after CloseAfterSend() began draining the connection");
            }

            if (bytes.empty())
            {
                return Core::Status::Ok();
            }

            // 덧셈으로 검사하면 아주 큰 span에서 size_t가 감겨 상한을 우회할 수 있다. 먼저
            // 남은 자리를 빼서 비교하면 그 경로도 요청 전체 거절이라는 Send 계약을 지킨다.
            if (mRetainedSendBytes > SendQueueLimitBytes ||
                bytes.size() > SendQueueLimitBytes - mRetainedSendBytes)
            {
                // 절반만 담으면 상대가 받는 바이트 흐름이 조용히 깨진다. 그래서 통째로 거절한다.
                std::string message = "the send queue retains ";
                message.append(std::to_string(mRetainedSendBytes));
                message.append(" payload bytes and the request adds ");
                message.append(std::to_string(bytes.size()));
                message.append(", which exceeds SendQueueLimitBytes=");
                message.append(std::to_string(SendQueueLimitBytes));
                message.append("; nothing was queued");
                return Core::Status::Fail(Core::ErrorCode::WouldBlock, std::move(message));
            }

            const bool sharedBudgetReserved =
                mSendBudget != nullptr && mSendBudget->TryReserve(bytes.size());
            if (mSendBudget != nullptr && !sharedBudgetReserved)
            {
                return Core::Status::Fail(Core::ErrorCode::WouldBlock,
                    "the shared Connection send queue budget is full; nothing was queued");
            }

            try
            {
                mSendQueue.emplace_back(bytes.begin(), bytes.end());
            }
            catch (...)
            {
                if (sharedBudgetReserved)
                {
                    mSendBudget->Release(bytes.size());
                }
                throw;
            }
            mQueuedSendBytes += bytes.size();
            mRetainedSendBytes += bytes.size();

            if (mSendInFlight)
            {
                return Core::Status::Ok();
            }

            result = StartSendLocked();
            connectionClosedByFailure = !result.IsOk() && mClosed.load(std::memory_order_relaxed);
        }

        if (!result.IsOk())
        {
            MaybeNotifyDisconnected();
        }

        return result;
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::exception&)
    {
        return Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
    }
}

void TcpConnection::Close()
{
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mCloseAfterSendRequested = false;
        MarkClosedLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        CloseSocketLocked();
    }

    MaybeNotifyDisconnected();
}

// 전송층 자체에는 drain 기한이 없다. ServerHost가 세션의 절대 종료 기한을 검사하고
// 필요하면 Close로 중단하므로, 여기서는 송신 순서와 겹침 버퍼 수명만 관리한다.
void TcpConnection::CloseAfterSend()
{
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        if (!mClosed.load(std::memory_order_relaxed))
        {
            mCloseAfterSendRequested = true;
            FinishCloseAfterSendLocked();
        }
    }

    MaybeNotifyDisconnected();
}

void TcpConnection::SetObserver(std::weak_ptr<IConnectionObserver> observer)
{
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        SERVERCORE_ASSERT(
            !mObserverAssigned, "SetObserver() was called more than once on one connection");
        mObserverAssigned = true;
        mObserver = std::move(observer);
    }

    // 관찰자를 걸기 전에 이미 끊긴 연결이면 통지가 여기서 간다.
    MaybeNotifyDisconnected();
}

bool TcpConnection::IsOpen() const noexcept
{
    return !mClosed.load(std::memory_order_acquire);
}

std::size_t TcpConnection::QueuedSendBytes() const noexcept
{
    const std::lock_guard<std::mutex> guard(mMutex);
    return mQueuedSendBytes;
}

void TcpConnection::OnIoCompleted(
    IoOperation& operation, DWORD bytesTransferred, unsigned long errorCode)
{
    SERVERCORE_ASSERT(
        operation.kind == IoOperationKind::Receive || operation.kind == IoOperationKind::Send,
        "a connection received a completion whose kind is neither Receive nor Send");

    if (operation.kind == IoOperationKind::Receive)
    {
        OnReceiveCompleted(static_cast<ReceiveOperation&>(operation), bytesTransferred, errorCode);
        return;
    }

    OnSendCompleted(static_cast<SendOperation&>(operation), bytesTransferred, errorCode);
}

void TcpConnection::OnReceiveCompleted(
    ReceiveOperation& operation, DWORD bytesTransferred, unsigned long errorCode)
{
    // 자기 몫의 참조를 지역으로 옮긴다. 이 지역이 사라지는 자리가 이 객체의 마지막 참조가
    // 놓이는 자리일 수 있으므로, 잠금이 풀린 뒤여야 한다. 선언 순서가 그것을 정한다.
    std::shared_ptr<TcpConnection> keepAlive;
    std::weak_ptr<IConnectionObserver> observer;
    bool hasBytesToDeliver = false;

    {
        const std::lock_guard<std::mutex> guard(mMutex);

        keepAlive = std::move(operation.owner);
        --mPendingOperations;

        // Close()가 먼저 소켓을 닫았다면 이 완료는 취소 정리일 뿐이다. 그 사유를
        // WSA_OPERATION_ABORTED로 덮지 않고, 종료 중 새 오류 문자열도 만들지 않는다.
        if (!mClosed.load(std::memory_order_relaxed))
        {
            if (errorCode != 0)
            {
                MarkClosedLocked(MakeSocketFailure("WSARecv", static_cast<int>(errorCode)));
                CloseSocketLocked();
            }
            else if (bytesTransferred == 0)
            {
                // TCP에서 0바이트 수신은 상대가 자기 쪽 보내기를 닫았다는 뜻뿐이다.
                MarkClosedLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
                CloseSocketLocked();
            }
            else
            {
                observer = mObserver;
                SERVERCORE_ASSERT(!mReceiveCallbackActive,
                    "a second receive observer callback began before the first one ended");
                mReceiveCallbackActive = true;
                hasBytesToDeliver = true;
            }
        }
    }

    if (hasBytesToDeliver)
    {
        // 관찰자 호출은 잠금 밖에서 한다. 관찰자가 그 안에서 Send나 Close를 부를 수 있다.
        // 이 시점에 수신 요청은 걸려 있지 않으므로 버퍼를 읽는 것이 안전하다. 대신 이 호출이
        // 끝날 때까지 mReceiveCallbackActive를 유지해, 다른 스레드의 Close가 OnDisconnected를
        // 동시에 보내지 못하게 한다.
        if (const std::shared_ptr<IConnectionObserver> target = observer.lock())
        {
            target->OnBytesReceived(
                std::span<const std::byte>(operation.buffer.data(), bytesTransferred));
        }

        const std::lock_guard<std::mutex> guard(mMutex);

        SERVERCORE_ASSERT(mReceiveCallbackActive,
            "a receive completion lost its active observer callback marker");
        mReceiveCallbackActive = false;

        // 관찰자가 수신 예산 고갈 같은 이유로 Close()했으면 새 WSARecv를 만들지 않는다. 닫힌
        // 경로의 Status조차 만들 필요가 없고, 아래 통지가 기존 종료 사유를 전달한다.
        if (!mClosed.load(std::memory_order_relaxed))
        {
            // 실패는 이미 닫힘 사유로 기록되고 소켓도 닫힌다. 그래서 여기서 값을 다시 다루지
            // 않고 일부러 버린다. 아래 MaybeNotifyDisconnected가 그 결과를 관찰자에게 전한다.
            (void)StartReceiveLocked();
        }
    }

    MaybeNotifyDisconnected();
}

void TcpConnection::OnSendCompleted(
    SendOperation& operation, DWORD bytesTransferred, unsigned long errorCode)
{
    std::shared_ptr<TcpConnection> keepAlive;

    {
        const std::lock_guard<std::mutex> guard(mMutex);

        keepAlive = std::move(operation.owner);
        --mPendingOperations;
        mSendInFlight = false;

        // Close()가 먼저 소켓을 닫아도, WSASend가 빌린 맨 앞 vector는 이 완료가 돌아올 때까지
        // 살아 있어야 한다. 이제 요청이 끝났으므로 성공·실패와 관계없이 남은 큐를 버릴 수 있다.
        if (mClosed.load(std::memory_order_relaxed))
        {
            DiscardQueuedSendsLocked();
        }
        else if (errorCode != 0)
        {
            MarkClosedLocked(MakeSocketFailure("WSASend", static_cast<int>(errorCode)));
            CloseSocketLocked();
        }
        else if (bytesTransferred == 0)
        {
            // Send는 비어 있지 않은 버퍼만 건다. 여기서 다시 같은 요청을 걸면 큐가 전혀 줄지
            // 않아 0바이트 완료를 끝없이 반복할 수 있으므로, 진행할 수 없는 연결로 닫는다.
            MarkClosedLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
            CloseSocketLocked();
        }
        else
        {
            SERVERCORE_ASSERT(
                !mSendQueue.empty(), "a send completed while the send queue was empty");

            mSendOffset += bytesTransferred;
            mQueuedSendBytes -= bytesTransferred;

            if (mSendOffset >= mSendQueue.front().size())
            {
                SERVERCORE_ASSERT(mRetainedSendBytes >= mSendQueue.front().size(),
                    "completed send payload exceeded the retained send-byte budget");
                const std::size_t completedPayloadBytes = mSendQueue.front().size();
                mRetainedSendBytes -= completedPayloadBytes;
                mSendOffset = 0;
                mSendQueue.pop_front();
                if (mSendBudget != nullptr)
                {
                    // 실제 payload 저장소를 먼저 놓은 뒤 공유 예산을 반환한다. 반대로 하면 다른
                    // Connection이 그 예산으로 새 vector를 만드는 짧은 동안 Host 상한을 넘는다.
                    mSendBudget->Release(completedPayloadBytes);
                }
            }

            if (!mSendQueue.empty())
            {
                // 실패는 이미 닫힘 사유로 기록된다. 위 수신 쪽과 같은 이유로 버린다.
                (void)StartSendLocked();
            }

            FinishCloseAfterSendLocked();
        }
    }

    MaybeNotifyDisconnected();
}

Core::Status TcpConnection::StartReceiveLocked()
{
    if (mClosed.load(std::memory_order_relaxed) || mSocket == INVALID_SOCKET)
    {
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    }

    WSABUF buffer{};
    buffer.len = static_cast<ULONG>(mReceiveOperation.buffer.size());
    buffer.buf = reinterpret_cast<CHAR*>(mReceiveOperation.buffer.data());

    mReceiveOperation.overlapped = OVERLAPPED{};
    mReceiveOperation.owner = shared_from_this();

    DWORD receivedBytes = 0;
    DWORD flags = 0;
    const int result = ::WSARecv(
        mSocket, &buffer, 1, &receivedBytes, &flags, &mReceiveOperation.overlapped, nullptr);

    if (result == SOCKET_ERROR)
    {
        const int socketError = ::WSAGetLastError();
        if (socketError != WSA_IO_PENDING)
        {
            mReceiveOperation.owner.reset();

            Core::Status failure = MakeSocketFailure("WSARecv", socketError);
            try
            {
                MarkClosedLocked(failure);
            }
            catch (const std::bad_alloc&)
            {
                failure = Core::Status::AllocationFailure();
                MarkClosedLocked(Core::Status::AllocationFailure());
            }
            CloseSocketLocked();
            return failure;
        }
    }

    // 요청이 즉시 끝났더라도 완료 통지는 완료 포트로 온다. 그렇게 두는 설정을 켜지 않았다.
    // 그래서 두 경우 모두 "걸려 있는 요청 하나"로 센다.
    ++mPendingOperations;
    return Core::Status::Ok();
}

Core::Status TcpConnection::StartSendLocked()
{
    if (mClosed.load(std::memory_order_relaxed) || mSocket == INVALID_SOCKET)
    {
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    }

    SERVERCORE_ASSERT(!mSendQueue.empty(), "StartSendLocked() was called with an empty send queue");
    SERVERCORE_ASSERT(!mSendInFlight, "StartSendLocked() was called while a send was in flight");

    std::vector<std::byte>& front = mSendQueue.front();
    SERVERCORE_ASSERT(
        mSendOffset < front.size(), "the send offset is not inside the front of the send queue");

    WSABUF buffer{};
    buffer.len = static_cast<ULONG>(front.size() - mSendOffset);
    buffer.buf = reinterpret_cast<CHAR*>(front.data() + mSendOffset);

    mSendOperation.overlapped = OVERLAPPED{};
    mSendOperation.owner = shared_from_this();

    DWORD sentBytes = 0;
    const int result =
        ::WSASend(mSocket, &buffer, 1, &sentBytes, 0, &mSendOperation.overlapped, nullptr);

    if (result == SOCKET_ERROR)
    {
        const int socketError = ::WSAGetLastError();
        if (socketError != WSA_IO_PENDING)
        {
            mSendOperation.owner.reset();

            Core::Status failure = MakeSocketFailure("WSASend", socketError);
            try
            {
                MarkClosedLocked(failure);
            }
            catch (const std::bad_alloc&)
            {
                failure = Core::Status::AllocationFailure();
                MarkClosedLocked(Core::Status::AllocationFailure());
            }
            CloseSocketLocked();
            return failure;
        }
    }

    mSendInFlight = true;
    ++mPendingOperations;
    return Core::Status::Ok();
}

void TcpConnection::DiscardQueuedSendsLocked() noexcept
{
    SERVERCORE_ASSERT(!mSendInFlight,
        "DiscardQueuedSendsLocked() was called while WSASend still borrowed the queue front");

    const std::size_t discardedPayloadBytes = mRetainedSendBytes;
    mSendQueue.clear();
    mSendOffset = 0;
    mQueuedSendBytes = 0;
    mRetainedSendBytes = 0;
    if (mSendBudget != nullptr)
    {
        // 완료 경로와 마찬가지로 payload를 먼저 해제해야 반환된 예산을 다른 연결이 써도
        // 실제 동시 보관 메모리가 Host 공유 상한 안에 남는다.
        mSendBudget->Release(discardedPayloadBytes);
    }
}

void TcpConnection::MarkClosedLocked(Core::Status reason)
{
    if (mClosed.load(std::memory_order_relaxed))
    {
        // 첫 사유가 이긴다. 나중 사유는 대개 첫 사유의 결과이므로 덮으면 원인이 지워진다.
        return;
    }

    mCloseReason = std::move(reason);
    mClosed.store(true, std::memory_order_release);

    if (!mSendInFlight)
    {
        DiscardQueuedSendsLocked();
    }
}

void TcpConnection::CloseSocketLocked()
{
    if (mSocket == INVALID_SOCKET)
    {
        return;
    }

    // 양방향을 먼저 내린다. 그러면 진행 중인 요청이 곧 실패로 돌아온다.
    ::shutdown(mSocket, SD_BOTH);
    ::closesocket(mSocket);
    mSocket = INVALID_SOCKET;
}

void TcpConnection::CloseSocketAfterSendLocked()
{
    if (mSocket == INVALID_SOCKET)
    {
        return;
    }

    // SD_RECEIVE까지 함께 내리면 아직 읽지 않은 inbound data가 있을 때 TCP reset을 낼 수 있다.
    // 이 경로는 마지막 송신을 먼저 보내려는 것이므로, 보낼 쪽만 내린다. 기본 linger 설정의
    // closesocket()은 남은 TCP 송신을 background graceful close로 처리한다.
    ::shutdown(mSocket, SD_SEND);
    ::closesocket(mSocket);
    mSocket = INVALID_SOCKET;
}

void TcpConnection::FinishCloseAfterSendLocked()
{
    if (!mCloseAfterSendRequested || mClosed.load(std::memory_order_relaxed) || mSendInFlight ||
        !mSendQueue.empty())
    {
        return;
    }

    MarkClosedLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
    CloseSocketAfterSendLocked();
}

void TcpConnection::MaybeNotifyDisconnected()
{
    Core::Status reason = Core::Status::Ok();
    std::weak_ptr<IConnectionObserver> observer;

    {
        const std::lock_guard<std::mutex> guard(mMutex);

        if (!mObserverAssigned || !mClosed.load(std::memory_order_relaxed) ||
            mPendingOperations != 0 || mReceiveCallbackActive)
        {
            return;
        }

        // 닫힘 통지를 차지한 호출만 사유를 꺼낸다. 복사하면 종료 중 메모리 고갈에서 다시
        // 할당할 수 있으므로, 더는 쓰지 않을 상태와 관찰자 참조를 잠금 안에서 이동·복사한다.
        if (mDisconnectNotified.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        reason = std::move(mCloseReason);
        observer = mObserver;
    }

    if (const std::shared_ptr<IConnectionObserver> target = observer.lock())
    {
        target->OnDisconnected(std::move(reason));
    }
}

void TcpConnection::NotifyDisconnectedOnce(Core::Status reason)
{
    // 이 한 줄이 "정확히 한 번"을 지킨다. 여러 스레드가 동시에 끊김을 발견해도 뒤집기에
    // 성공하는 것은 하나뿐이다.
    if (mDisconnectNotified.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    std::weak_ptr<IConnectionObserver> observer;
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        observer = mObserver;
    }

    if (const std::shared_ptr<IConnectionObserver> target = observer.lock())
    {
        target->OnDisconnected(std::move(reason));
    }
}
}
