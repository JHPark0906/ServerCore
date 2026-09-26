#include "Net/SendBudgetInternal.h"
#include "Core/AtomicBudgetInternal.h"
#include "Net/SendCapacityInternal.h"
#include "ServerCore/Core/Assert.h"

namespace ServerCore::Net
{
SendBudget::SendBudget(
    const std::size_t limitBytes, const std::size_t connectionLimitBytes) noexcept
    : mLimitBytes(limitBytes)
    , mConnectionLimitBytes(connectionLimitBytes)
{
    SERVERCORE_ASSERT(limitBytes != 0, "SendBudget requires a positive byte limit");
    SERVERCORE_ASSERT(connectionLimitBytes != 0, "SendBudget requires a positive connection limit");
}

bool SendBudget::TryReserve(const std::size_t byteCount) noexcept
{
    return Core::Detail::TryReserveBudget(mUsedBytes, mLimitBytes, byteCount);
}

void SendBudget::Release(const std::size_t byteCount) noexcept
{
    Core::Detail::ReleaseBudget(
        mUsedBytes, byteCount, "released send payload exceeded the shared Connection budget");
    if (byteCount)
        MarkChanged();
}

std::size_t SendBudget::LimitBytes() const noexcept
{
    return mLimitBytes;
}

std::size_t SendBudget::UsedBytes() const noexcept
{
    return mUsedBytes.load(std::memory_order_acquire);
}

std::size_t SendBudget::ConnectionLimitBytes() const noexcept
{
    return mConnectionLimitBytes;
}
void SendBudget::MarkChanged() noexcept
{
    mChanged.store(true, std::memory_order_release);
}

std::uint64_t SendBudget::Register(const std::shared_ptr<SendCapacityState>& state)
{
    const std::lock_guard guard(mWaitersMutex);
    SERVERCORE_ASSERT(mNextId != 0, "send capacity registration ids exhausted");
    const auto id = mNextId++;
    mWaiters.emplace(id, state);
    return id;
}

void SendBudget::Unregister(std::uint64_t id) noexcept
{
    const std::lock_guard guard(mWaitersMutex);
    mWaiters.erase(id);
}

void SendBudget::Notify() noexcept
{
    if (!mChanged.load(std::memory_order_acquire) ||
        mNotifying.test_and_set(std::memory_order_acquire))
        return;
    for (;;)
    {
        mChanged.store(false, std::memory_order_release);
        std::uint64_t last = 0;
        std::uint64_t upper = 0;
        {
            const std::lock_guard guard(mWaitersMutex);
            upper = mNextId - 1;
        }
        for (;;)
        {
            std::shared_ptr<SendCapacityState> state;
            {
                const std::lock_guard guard(mWaitersMutex);
                const auto next = mWaiters.upper_bound(last);
                if (next == mWaiters.end() || next->first > upper)
                    break;
                last = next->first;
                state = next->second.lock();
            }
            if (state)
                state->Notify();
        }
        if (mChanged.load(std::memory_order_acquire))
            continue;
        mNotifying.clear(std::memory_order_release);
        // A release may race clearing the dispatcher flag. Either this caller
        // reacquires it, or the releasing caller performs the next pass.
        if (!mChanged.load(std::memory_order_acquire) ||
            mNotifying.test_and_set(std::memory_order_acquire))
            return;
    }
}
}
