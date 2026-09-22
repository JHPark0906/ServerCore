#include "Net/Linux/ConnectionInternal.h"

#include "Net/Linux/EpollInternal.h"
#include "Net/Linux/PosixInternal.h"
#include "ServerCore/Core/Assert.h"

#include <array>
#include <utility>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ServerCore::Net
{
std::shared_ptr<TcpConnection> TcpConnection::Create(const int descriptor,
    IoContext& context, std::shared_ptr<SendBudget> budget)
{
    SERVERCORE_ASSERT(descriptor >= 0, "a connection requires a valid descriptor");
    return std::make_shared<TcpConnection>(CreationKey{}, descriptor, context, std::move(budget));
}

TcpConnection::TcpConnection(CreationKey, const int descriptor,
    IoContext& context, std::shared_ptr<SendBudget> budget)
    : mContext(context), mDescriptor(descriptor), mSends(std::move(budget)) {}

TcpConnection::~TcpConnection()
{
    SERVERCORE_ASSERT(!mProcessing, "a connection callback outlived its ownership");
    // Registered callbacks own this connection. Therefore destruction cannot race
    // a worker; only unstarted or already unregistered connections reach here.
    if (mDescriptor >= 0)
    {
        ::shutdown(mDescriptor, SHUT_RDWR);
        ::close(mDescriptor);
        mDescriptor = -1;
    }
    DiscardSendsLocked();
    if (!mClosed.exchange(true))
        mCloseReason = Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    NotifyDisconnected();
}

Core::Status TcpConnection::Start()
{
    Core::Status status = Core::Status::Ok();
    {
        const std::lock_guard guard(mMutex);
        SERVERCORE_ASSERT(!mStarted, "a connection can only start once");
        mStarted = true;
        if (mClosed.load()) return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        try
        {
            auto registration = IoContextAccess::Register(mContext, mDescriptor,
                EPOLLIN | EPOLLRDHUP | (mSends.Empty() ? 0U : static_cast<std::uint32_t>(EPOLLOUT)),
                [self = shared_from_this()](const std::uint32_t events) { self->OnReady(events); });
            if (registration.IsOk()) mRegistration = registration.Value();
            else status = std::move(registration).TakeStatus();
        }
        catch (...) { status = Core::Status::AllocationFailure(); }
        if (!status.IsOk()) CloseWithFailureLocked(status);
    }
    if (!status.IsOk()) NotifyDisconnected();
    return status;
}

Core::Status TcpConnection::Send(const std::span<const std::byte> bytes)
{
    auto outcome = SendWithOutcome(bytes);
    return std::move(outcome.status);
}

ConnectionSendOutcome TcpConnection::SendWithOutcome(const std::span<const std::byte> bytes)
{
    Core::Status status = Core::Status::Ok();
    bool closedByFailure = false;
    try
    {
        const std::lock_guard guard(mMutex);
        if (mClosed.load() || mCloseAfterSend)
            return {Core::Status::FailWithoutMessage(Core::ErrorCode::Closed), false};
        if (bytes.empty()) return {Core::Status::Ok(), false};
        status = mSends.Enqueue(bytes);
        if (!status.IsOk()) return {std::move(status), false};
        // Queue acceptance is synchronous; actual writes always run on an I/O
        // worker. This preserves callback affinity and bounded queue accounting.
        if (mStarted && !mProcessing)
        {
            status = RearmLocked();
            if (!status.IsOk())
            {
                CloseWithFailureLocked(status);
                closedByFailure = true;
            }
        }
    }
    catch (...) { status = Core::Status::AllocationFailure(); }
    if (closedByFailure) NotifyDisconnected();
    return {std::move(status), closedByFailure};
}

Core::Status TcpConnection::RearmLocked() noexcept
{
    return IoContextAccess::Rearm(mContext, mDescriptor, mRegistration,
        EPOLLIN | EPOLLRDHUP | (mSends.Empty() ? 0U : static_cast<std::uint32_t>(EPOLLOUT)));
}

void TcpConnection::DiscardSendsLocked() noexcept
{
    mSends.Clear();
}

void TcpConnection::CloseWithFailureLocked(Core::Status& failure) noexcept
{
    // Both the initiating call and OnDisconnected own the same diagnostic.
    // As in the IOCP backend, failure to copy that diagnostic degrades both
    // results to the allocation-free PlatformError instead of throwing.
    try { CloseLocked(failure); }
    catch (...)
    {
        failure = Core::Status::AllocationFailure();
        CloseLocked(Core::Status::AllocationFailure());
    }
}

void TcpConnection::CloseLocked(Core::Status reason, const bool graceful) noexcept
{
    if (mClosed.exchange(true, std::memory_order_acq_rel)) return;
    mCloseReason = std::move(reason);
    if (mDescriptor >= 0)
    {
        if (mRegistration != 0)
        {
            IoContextAccess::Remove(mContext, mDescriptor, mRegistration);
            mRegistration = 0;
        }
        ::shutdown(mDescriptor, graceful ? SHUT_WR : SHUT_RDWR);
        ::close(mDescriptor);
        mDescriptor = -1;
    }
    // A callback may be paused while Close runs. Keep its accepted work alive
    // until the worker exits that callback, matching the disconnect barrier.
    if (!mProcessing) DiscardSendsLocked();
}

void TcpConnection::Close()
{
    {
        const std::lock_guard guard(mMutex);
        CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
    }
    NotifyDisconnected();
}

void TcpConnection::CloseAfterSend()
{
    {
        const std::lock_guard guard(mMutex);
        if (!mClosed.load())
        {
            mCloseAfterSend = true;
            if (mSends.Empty()) CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed), true);
        }
    }
    NotifyDisconnected();
}

void TcpConnection::SetObserver(std::weak_ptr<IConnectionObserver> observer)
{
    {
        const std::lock_guard guard(mMutex);
        SERVERCORE_ASSERT(!mObserverAssigned, "a connection observer may only be assigned once");
        mObserverAssigned = true;
        mObserver = std::move(observer);
    }
    NotifyDisconnected();
}

bool TcpConnection::IsOpen() const noexcept { return !mClosed.load(std::memory_order_acquire); }
std::size_t TcpConnection::QueuedSendBytes() const noexcept
{
    const std::lock_guard guard(mMutex);
    return mSends.QueuedBytes();
}

void TcpConnection::NotifyDisconnected()
{
    std::weak_ptr<IConnectionObserver> observer;
    Core::Status reason = Core::Status::Ok();
    {
        const std::lock_guard guard(mMutex);
        if (!mClosed.load() || mProcessing || !mObserverAssigned || mDisconnectNotified) return;
        mDisconnectNotified = true;
        observer = mObserver;
        reason = std::move(mCloseReason);
    }
    if (auto target = observer.lock()) target->OnDisconnected(std::move(reason));
}

void TcpConnection::OnReady(const std::uint32_t events)
{
    std::array<std::byte, ReceiveBufferSize> buffer{};
    {
        const std::lock_guard guard(mMutex);
        if (mClosed.load() || mProcessing) return;
        mProcessing = true;
    }
    // Cap each readiness dispatch so a continuously readable peer cannot starve
    // others. Level-triggered one-shot rearming reports remaining readiness.
    if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0)
    {
        for (unsigned int attempt = 0; attempt < 16; ++attempt)
        {
            std::shared_ptr<IConnectionObserver> observer;
            ssize_t count = -1;
            {
                const std::lock_guard guard(mMutex);
                if (mClosed.load()) break;
                count = ::recv(mDescriptor, buffer.data(), buffer.size(), 0);
                if (count < 0)
                {
                    const int error = errno;
                    if (error == EINTR) continue;
                    if (!IsWouldBlock(error)) CloseLocked(MakePosixFailure("recv", error));
                    break;
                }
                if (count == 0)
                {
                    CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
                    break;
                }
                observer = mObserver.lock();
            }
            if (observer) observer->OnBytesReceived(std::span(buffer).first(static_cast<std::size_t>(count)));
        }
    }
    {
        const std::lock_guard guard(mMutex);
        // A receive callback may have just queued a reply even without EPOLLOUT.
        for (unsigned int attempt = 0; attempt < 64 && !mClosed.load() && !mSends.Empty(); ++attempt)
        {
            const auto front = mSends.Front();
            const ssize_t count = ::send(mDescriptor, front.data(), front.size(), MSG_NOSIGNAL);
            if (count < 0)
            {
                const int error = errno;
                if (error == EINTR) continue;
                if (!IsWouldBlock(error)) CloseLocked(MakePosixFailure("send", error));
                break;
            }
            if (count == 0)
            {
                CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
                break;
            }
            mSends.Consume(static_cast<std::size_t>(count));
        }
        if (!mClosed.load() && mCloseAfterSend && mSends.Empty())
            CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed), true);
        mProcessing = false;
        if (mClosed.load()) DiscardSendsLocked();
        else
        {
            auto status = RearmLocked();
            if (!status.IsOk()) CloseLocked(std::move(status));
        }
    }
    NotifyDisconnected();
}
}
