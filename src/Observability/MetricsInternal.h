#pragma once
#include "ServerCore/Observability/Metrics.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace ServerCore::Observability::Detail
{
inline void Add(std::uint64_t& value, std::uint64_t amount = 1) noexcept
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    value = amount > maximum - value ? maximum : value + amount;
}
inline void Add(std::atomic<std::uint64_t>& value, std::uint64_t amount = 1) noexcept
{
    auto old = value.load(std::memory_order_relaxed);
    for (;;)
    {
        auto next = old;
        Add(next, amount);
        if (value.compare_exchange_weak(old, next, std::memory_order_relaxed))
            return;
    }
}
inline void Maximum(std::atomic<std::uint64_t>& value, std::uint64_t candidate) noexcept
{
    auto old = value.load(std::memory_order_relaxed);
    while (
        old < candidate && !value.compare_exchange_weak(old, candidate, std::memory_order_relaxed))
    {
    }
}
inline std::uint64_t Elapsed(std::chrono::steady_clock::time_point start) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start)
                             .count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}
inline std::size_t LatencyBucket(std::uint64_t nanoseconds) noexcept
{
    std::size_t index = 0;
    while (index < LatencyBucketUpperBoundsNanoseconds.size() &&
           nanoseconds > LatencyBucketUpperBoundsNanoseconds[index])
        ++index;
    return index;
}
inline void ObserveLatency(LatencyHistogramSnapshot& histogram, std::uint64_t nanoseconds) noexcept
{
    Add(histogram.buckets[LatencyBucket(nanoseconds)]);
}
}
