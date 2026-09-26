#pragma once

#include "ServerCore/Core/Assert.h"

#include <atomic>
#include <cstddef>

namespace ServerCore::Core::Detail
{
// Reserve the entire amount or leave the counter unchanged. Subtraction before
// addition makes this safe even when amount is near the size_t limit.
[[nodiscard]] inline bool TryReserveBudget(
    std::atomic<std::size_t>& used, const std::size_t limit, const std::size_t amount) noexcept
{
    if (amount == 0)
        return true;
    std::size_t current = used.load(std::memory_order_acquire);
    for (;;)
    {
        if (current > limit || amount > limit - current)
            return false;
        if (used.compare_exchange_weak(
                current, current + amount, std::memory_order_acq_rel, std::memory_order_acquire))
            return true;
    }
}

inline void ReleaseBudget(std::atomic<std::size_t>& used, const std::size_t amount,
    const char* const failureContext) noexcept
{
    if (amount == 0)
        return;
    std::size_t current = used.load(std::memory_order_acquire);
    for (;;)
    {
        SERVERCORE_ASSERT(current >= amount, failureContext);
        if (used.compare_exchange_weak(
                current, current - amount, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
}
}
