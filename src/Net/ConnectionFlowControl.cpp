#include "ServerCore/Net/ConnectionFlowControl.h"

#include "Net/SendBudgetInternal.h"
#include "Net/SendCapacityInternal.h"
#include "ServerCore/Net/Connection.h"

#include <condition_variable>
#include <optional>
#include <thread>
#include <utility>

namespace ServerCore::Net
{
using Core::ErrorCode;
using Core::Status;

std::shared_ptr<ConnectionFlowControl> GetConnectionFlowControl(
    const std::shared_ptr<Connection>& connection) noexcept
{
    return std::dynamic_pointer_cast<ConnectionFlowControl>(connection);
}

SendCapacityState::SendCapacityState(std::shared_ptr<SendBudget> budget)
    : mBudget(std::move(budget))
{
}
SendCapacityState::~SendCapacityState()
{
    if (mRegistration)
        mBudget->Unregister(mRegistration);
}
void SendCapacityState::SetRetainedBytes(std::size_t bytes) noexcept
{
    mRetainedBytes.store(bytes, std::memory_order_release);
}
void SendCapacityState::Close() noexcept
{
    mClosed.store(true, std::memory_order_release);
    mBudget->MarkChanged();
}

void SendCapacityState::Notify() noexcept
{
    Core::CompletionSource pending;
    std::size_t requiredBytes = 0;
    {
        const std::lock_guard guard(mMutex);
        pending = mPending;
        requiredBytes = mRequiredBytes;
    }
    if (!pending.IsPending())
        return;
    if (mClosed.load(std::memory_order_acquire))
    {
        (void)pending.Complete(ErrorCode::Closed);
        return;
    }
    // Release publishes local retained storage before shared accounting. Read
    // shared accounting first so observing released bytes also sees local release.
    const auto shared = mBudget->UsedBytes();
    const auto local = mRetainedBytes.load(std::memory_order_acquire);
    if (local <= mBudget->ConnectionLimitBytes() &&
        requiredBytes <= mBudget->ConnectionLimitBytes() - local &&
        shared <= mBudget->LimitBytes() && requiredBytes <= mBudget->LimitBytes() - shared)
        (void)pending.Complete(ErrorCode::Ok);
}

Core::Result<SendCapacitySubscription> SendCapacityState::Wait(
    std::size_t bytes, std::function<void(Status)> callback, std::stop_token cancellation)
{
    using Result = Core::Result<SendCapacitySubscription>;
    if (!callback || bytes == 0)
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    if (bytes > mBudget->ConnectionLimitBytes() || bytes > mBudget->LimitBytes())
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
    try
    {
        auto subscription = SendCapacitySubscription::Create(std::move(callback));
        if (!subscription.IsOk())
            return subscription;
        auto source = subscription.Value().GetSource();
        {
            const std::lock_guard guard(mMutex);
            if (mClosed.load(std::memory_order_acquire))
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
            if (mPending.IsPending())
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::AlreadyExists));
            if (!mRegistration)
                mRegistration = mBudget->Register(shared_from_this());
            mPending = source;
            mRequiredBytes = bytes;
        }
        // Publish before attaching: an already-cancelled token may invoke the
        // callback inline, and that callback may register a successor wait.
        subscription.Value().BindCancellation(cancellation);
        Notify();
        return subscription;
    }
    catch (...)
    {
        return Result::FromStatus(Status::AllocationFailure());
    }
}
}
