#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Core/Error.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace ServerCore::Runtime
{
namespace Detail
{
struct RequestLimitState;
struct RequestLimitEntry;
}
struct RequestLimiterOptions
{
    std::size_t maxKeys = 1024;
    std::size_t maxKeyBytes = 128;
    std::size_t maxRetainedKeyBytes = 128 * 1024;
    std::uint64_t refillTokens = 100;
    std::uint64_t burstTokens = 200;
    std::chrono::milliseconds refillInterval{ 1000 };
    std::size_t maxConcurrentPerKey = 8;
    std::size_t maxConcurrentTotal = 1024;
    std::chrono::milliseconds idleExpiry{ 300000 };
};
// A move-only concurrency lease. Destruction returns concurrency capacity, never
// rate tokens. It may outlive the limiter and must cover the actual work lifetime.
class RequestPermit
{
public:
    RequestPermit() noexcept = default;
    SERVERCORE_API ~RequestPermit();
    SERVERCORE_API RequestPermit(RequestPermit&&) noexcept;
    SERVERCORE_API RequestPermit& operator=(RequestPermit&&) noexcept;
    RequestPermit(const RequestPermit&) = delete;
    RequestPermit& operator=(const RequestPermit&) = delete;
    SERVERCORE_API void Reset() noexcept;
    [[nodiscard]] bool IsValid() const noexcept { return mEntry != nullptr; }

private:
    friend class RequestLimiter;
    SERVERCORE_API RequestPermit(std::shared_ptr<Detail::RequestLimitState>, Detail::RequestLimitEntry*) noexcept;
    std::shared_ptr<Detail::RequestLimitState> mState;
    Detail::RequestLimitEntry* mEntry = nullptr;
};
enum class RequestLimitReason
{
    None,
    Rate,
    KeyConcurrency,
    TotalConcurrency,
    KeyCapacity
};
struct RequestAdmission
{
    std::optional<RequestPermit> permit;
    RequestLimitReason reason = RequestLimitReason::None;
    // Advisory rate wait only; zero for concurrency/table pressure. No reservation.
    std::chrono::milliseconds retryAfter{};
    [[nodiscard]] bool Allowed() const noexcept { return permit.has_value(); }
};
struct RequestLimiterSnapshot
{
    std::size_t keys = 0, retainedKeyBytes = 0, active = 0;
};
// Protocol-neutral, thread-safe token bucket and concurrency admission. Keys are
// opaque bytes supplied by the application (no automatic IP/account selection).
// Rejections consume no rate/concurrency budget. There is no waiting queue.
class RequestLimiter
{
public:
    SERVERCORE_API static Core::Result<RequestLimiter> Create(const RequestLimiterOptions& options = {});
    SERVERCORE_API Core::Result<RequestAdmission> TryAcquire(std::string_view key, std::uint64_t cost = 1) const;
    // Idle entries expire only when no permit is active AND their bucket is full,
    // preventing a short idleExpiry from resetting a depleted rate allowance.
    SERVERCORE_API std::size_t PruneExpired() const noexcept;
    SERVERCORE_API void Close() const noexcept;
    [[nodiscard]] SERVERCORE_API RequestLimiterSnapshot Snapshot() const noexcept;

private:
    explicit RequestLimiter(std::shared_ptr<Detail::RequestLimitState> state) noexcept
        : mState(std::move(state))
    {
    }
    std::shared_ptr<Detail::RequestLimitState> mState;
};
}
