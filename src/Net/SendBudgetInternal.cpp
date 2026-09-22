#include "Net/SendBudgetInternal.h"
#include "Core/AtomicBudgetInternal.h"
#include "ServerCore/Core/Assert.h"

namespace ServerCore::Net
{
SendBudget::SendBudget(const std::size_t limitBytes) noexcept
    : mLimitBytes(limitBytes)
{
    SERVERCORE_ASSERT(limitBytes != 0, "SendBudget requires a positive byte limit");
}

bool SendBudget::TryReserve(const std::size_t byteCount) noexcept
{
    return Core::Detail::TryReserveBudget(mUsedBytes, mLimitBytes, byteCount);
}

void SendBudget::Release(const std::size_t byteCount) noexcept
{
    Core::Detail::ReleaseBudget(mUsedBytes, byteCount,
        "released send payload exceeded the shared Connection budget");
}

std::size_t SendBudget::LimitBytes() const noexcept
{
    return mLimitBytes;
}

std::size_t SendBudget::UsedBytes() const noexcept
{
    return mUsedBytes.load(std::memory_order_acquire);
}

}
