#include "Net/Linux/ConnectionInternal.h"

#include "Net/Linux/EpollInternal.h"
#include "Net/Linux/PosixInternal.h"
#include "Net/EndpointInternal.h"
#include "ServerCore/Core/Assert.h"

#include <array>
#include <utility>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ServerCore::Net
{
std::shared_ptr<TcpConnection> TcpConnection::Create(const int descriptor,
    IoContext& context, std::shared_ptr<SendBudget> budget,Core::IpEndpoint local,Core::IpEndpoint remote)
{
    SERVERCORE_ASSERT(descriptor >= 0, "a connection requires a valid descriptor");
    return std::make_shared<TcpConnection>(CreationKey{}, descriptor, context, std::move(budget),local,remote);
}

TcpConnection::TcpConnection(CreationKey, const int descriptor,
    IoContext& context, std::shared_ptr<SendBudget> budget,Core::IpEndpoint local,Core::IpEndpoint remote)
    : mContext(context), mDescriptor(descriptor), mLocalEndpoint(local.IsValid()?local:ReadSocketEndpoint(descriptor,false)),
      mRemoteEndpoint(remote.IsValid()?remote:ReadSocketEndpoint(descriptor,true)), mSends(std::move(budget)) {}

TcpConnection::~TcpConnection()
{
    const SendNotificationScope notifications(mSends.Budget());
    SERVERCORE_ASSERT(!mProcessing, "a connection callback outlived its ownership");
    // Registered callbacks own this connection. Therefore destruction cannot race
    // a worker; only unstarted or already unregistered connections reach here.
    if (mDescriptor >= 0)
    {
        ::shutdown(mDescriptor, SHUT_RDWR);
        ::close(mDescriptor);
        mDescriptor = -1;
    }
    mSends.CloseCapacityWaits();
    DiscardSendsLocked();
    if (!mClosed.exchange(true))
        mCloseReason = Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    NotifyDisconnected();
}

Core::Status TcpConnection::Start()
{
    const SendNotificationScope notifications(mSends.Budget());
    Core::Status status = Core::Status::Ok();
    {
        const std::lock_guard guard(mMutex);
        SERVERCORE_ASSERT(!mStarted, "a connection can only start once");
        mStarted = true;
        if (mClosed.load()) return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        try
        {
            auto registration = IoContextAccess::Register(mContext, mDescriptor, InterestLocked(),
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
    const SendNotificationScope notifications(mSends.Budget());
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

std::uint32_t TcpConnection::InterestLocked() const noexcept
{
    return (mReceivePaused ? 0U : static_cast<std::uint32_t>(EPOLLIN | EPOLLRDHUP)) |
        (mSends.Empty() ? 0U : static_cast<std::uint32_t>(EPOLLOUT));
}

Core::Status TcpConnection::RearmLocked() noexcept
{
    const auto interests = InterestLocked();
    // A paused idle connection keeps its registry owner but parks its one-shot
    // event. EPOLLHUP is reported even with an empty mask; rearming it repeatedly
    // would busy-loop until the application resumes or closes the connection.
    if (interests == 0) return Core::Status::Ok();
    return IoContextAccess::Rearm(mContext, mDescriptor, mRegistration, interests);
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
    mSends.CloseCapacityWaits();
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
    const SendNotificationScope notifications(mSends.Budget());
    {
        const std::lock_guard guard(mMutex);
        CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
    }
    NotifyDisconnected();
}

void TcpConnection::CloseAfterSend()
{
    const SendNotificationScope notifications(mSends.Budget());
    {
        const std::lock_guard guard(mMutex);
        if (!mClosed.load())
        {
            mCloseAfterSend = true;
            mSends.CloseCapacityWaits();
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

Core::Status TcpConnection::PauseReceive()
{
    const SendNotificationScope notifications(mSends.Budget());
    Core::Status status = Core::Status::Ok();
    {
        const std::lock_guard guard(mMutex);
        if (mClosed.load()) return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        if (mReceivePaused) return status;
        mReceivePaused = true;
        // A processing worker will apply the new interests before returning.
        // Otherwise remove the previously armed read interest now, including
        // when the resulting mask is empty. A queued event still checks pause.
        if (mStarted && !mProcessing)
        {
            status = IoContextAccess::Rearm(mContext, mDescriptor, mRegistration, InterestLocked());
            if (!status.IsOk()) CloseWithFailureLocked(status);
        }
    }
    if (!status.IsOk()) NotifyDisconnected();
    return status;
}

Core::Status TcpConnection::ResumeReceive()
{
    const SendNotificationScope notifications(mSends.Budget());
    Core::Status status = Core::Status::Ok();
    {
        const std::lock_guard guard(mMutex);
        if (mClosed.load()) return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        if (!mReceivePaused) return status;
        mReceivePaused = false;
        // Acceptor invokes its handler before Start. Resuming there must not
        // register the descriptor or let received bytes precede the handler.
        if (mStarted && !mProcessing)
        {
            status = RearmLocked();
            if (!status.IsOk()) CloseWithFailureLocked(status);
        }
    }
    if (!status.IsOk()) NotifyDisconnected();
    return status;
}

bool TcpConnection::IsReceivePaused() const noexcept
{
    const std::lock_guard guard(mMutex);
    return mReceivePaused;
}

std::size_t TcpConnection::RetainedSendBytes() const noexcept
{
    const std::lock_guard guard(mMutex);
    return mSends.RetainedBytes();
}

Core::Result<SendCapacitySubscription> TcpConnection::WaitForSendCapacity(
    const std::size_t requiredBytes, std::function<void(Core::Status)> callback,
    const std::stop_token cancellation)
{
    // An already available/closed/cancelled wait can invoke its callback inline.
    // SendQueue's capacity state synchronizes this operation independently.
    return mSends.WaitForCapacity(requiredBytes, std::move(callback), cancellation);
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
    const SendNotificationScope notifications(mSends.Budget());
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
                if (mClosed.load() || mReceivePaused) break;
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
        // Pause suppresses reads, including EOF detection, but a real socket
        // error is terminal independently of read admission. Querying SO_ERROR
        // also clears a stale error indication before a possible rearm.
        if (!mClosed.load() && mReceivePaused && (events & EPOLLERR) != 0)
        {
            int error = 0;
            socklen_t length = sizeof(error);
            if (::getsockopt(mDescriptor, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                CloseLocked(MakePosixFailure("getsockopt(SO_ERROR)", errno));
            else if (error != 0) CloseLocked(MakePosixFailure("socket", error));
        }
        // A receive callback may have just queued a reply even without EPOLLOUT.
        bool sendBlocked = false;
        for (unsigned int attempt = 0; attempt < 64 && !mClosed.load() && !mSends.Empty(); ++attempt)
        {
            const auto front = mSends.Front();
            const ssize_t count = ::send(mDescriptor, front.data(), front.size(), MSG_NOSIGNAL);
            if (count < 0)
            {
                const int error = errno;
                if (error == EINTR) continue;
                if (IsWouldBlock(error)) sendBlocked = true;
                else CloseLocked(MakePosixFailure("send", error));
                break;
            }
            if (count == 0)
            {
                CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
                break;
            }
            mSends.Consume(static_cast<std::size_t>(count));
        }
        // A hung-up socket that could not drain its writes cannot make progress
        // by rearming EPOLLOUT: HUP would immediately wake every retry. With no
        // writes, leave it parked so Resume can still consume buffered input.
        if (!mClosed.load() && mReceivePaused && (events & EPOLLHUP) != 0 && sendBlocked)
            CloseLocked(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
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
