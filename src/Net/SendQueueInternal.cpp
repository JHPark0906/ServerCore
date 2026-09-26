#include "Net/SendQueueInternal.h"
#include "Net/SendCapacityInternal.h"

#include "ServerCore/Core/Assert.h"

#include <utility>

namespace ServerCore::Net
{
SendQueue::SendQueue(std::shared_ptr<SendBudget> budget)
    : mBudget(budget ? std::move(budget) : std::make_shared<SendBudget>(SendQueueLimitBytes))
    , mCapacity(std::make_shared<SendCapacityState>(mBudget))
{
}
SendQueue::~SendQueue()
{
    CloseCapacityWaits();
    Clear();
    // The shared dispatcher may already be invoking another connection's
    // callback. Complete our terminal wait before unregistering its state;
    // a deferred shared pass could otherwise lose this destroyed queue.
    mCapacity->Notify();
    mBudget->Notify();
}

Core::Status SendQueue::Enqueue(const std::span<const std::byte> bytes) noexcept
{
    if (bytes.empty())
        return Core::Status::Ok();
    if (mRetainedBytes > mBudget->ConnectionLimitBytes() ||
        bytes.size() > mBudget->ConnectionLimitBytes() - mRetainedBytes)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock);
    if (mBudget && !mBudget->TryReserve(bytes.size()))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock);
    try
    {
        mPayloads.emplace_back(bytes.begin(), bytes.end());
    }
    catch (...)
    {
        if (mBudget)
            mBudget->Release(bytes.size());
        return Core::Status::AllocationFailure();
    }
    mQueuedBytes += bytes.size();
    mRetainedBytes += bytes.size();
    mCapacity->SetRetainedBytes(mRetainedBytes);
    return Core::Status::Ok();
}

bool SendQueue::Empty() const noexcept
{
    return mPayloads.empty();
}

std::span<std::byte> SendQueue::Front() noexcept
{
    SERVERCORE_ASSERT(
        !mPayloads.empty(), "a send queue must have a front payload before borrowing it");
    return std::span(mPayloads.front()).subspan(mOffset);
}

void SendQueue::Consume(const std::size_t bytes) noexcept
{
    SERVERCORE_ASSERT(!mPayloads.empty() && bytes <= mPayloads.front().size() - mOffset,
        "a send completion exceeded the remaining front payload");
    mOffset += bytes;
    mQueuedBytes -= bytes;
    if (mOffset == mPayloads.front().size())
    {
        const std::size_t released = mPayloads.front().size();
        mPayloads.pop_front();
        mOffset = 0;
        mRetainedBytes -= released;
        mCapacity->SetRetainedBytes(mRetainedBytes);
        if (mBudget)
            mBudget->Release(released);
    }
}

void SendQueue::Clear() noexcept
{
    const std::size_t released = mRetainedBytes;
    mPayloads.clear();
    mOffset = mQueuedBytes = mRetainedBytes = 0;
    mCapacity->SetRetainedBytes(0);
    if (mBudget)
        mBudget->Release(released);
}

std::size_t SendQueue::QueuedBytes() const noexcept
{
    return mQueuedBytes;
}
std::size_t SendQueue::RetainedBytes() const noexcept
{
    return mRetainedBytes;
}
std::shared_ptr<SendBudget> SendQueue::Budget() const noexcept
{
    return mBudget;
}
void SendQueue::CloseCapacityWaits() noexcept
{
    mCapacity->Close();
}
Core::Result<SendCapacitySubscription> SendQueue::WaitForCapacity(
    std::size_t bytes, std::function<void(Core::Status)> callback, std::stop_token cancellation)
{
    const auto capacity = mCapacity;
    return capacity->Wait(bytes, std::move(callback), cancellation);
}
}
