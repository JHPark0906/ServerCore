#include "ServerCore/Runtime/RequestLimiter.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace ServerCore::Runtime
{
namespace Detail
{
using Clock = std::chrono::steady_clock;
struct RequestLimitEntry
{
    long double tokens = 0;
    std::size_t active = 0;
    Clock::time_point refill{}, seen{};
};
struct RequestLimitState
{
    explicit RequestLimitState(RequestLimiterOptions value)
        : options(value)
    {
    }
    void Refill(RequestLimitEntry& entry, Clock::time_point now) noexcept
    {
        if (now <= entry.refill)
            return;
        const auto elapsed =
            std::chrono::duration<long double, std::milli>(now - entry.refill).count();
        entry.tokens = (std::min)(static_cast<long double>(options.burstTokens),
            entry.tokens + elapsed * static_cast<long double>(options.refillTokens) /
                               static_cast<long double>(options.refillInterval.count()));
        entry.refill = now;
    }
    // 비활성 항목의 버킷이 가득 차는 가장 이른 시각의 하한. 반올림은 이르게, 먼 값은 24시간으로
    // 잘라 오버플로 없이 보수적으로 둔다.
    Clock::time_point FullAt(const RequestLimitEntry& entry) const noexcept
    {
        const auto missing = static_cast<long double>(options.burstTokens) - entry.tokens;
        const auto milliseconds = missing *
                                  static_cast<long double>(options.refillInterval.count()) /
                                  static_cast<long double>(options.refillTokens);
        constexpr long double cap = 24.0L * 60 * 60 * 1000;
        return entry.refill + std::chrono::duration_cast<Clock::duration>(
                                  std::chrono::duration<long double, std::milli>(
                                      (std::min)(cap, (std::max)(0.0L, milliseconds))));
    }
    // pressure가 참이면 키 표가 가득 찬 상태다. 활성 permit이 없고 버킷이 가득 찬 항목은 새로 만든
    // 항목과 구별되지 않으므로 idleExpiry와 무관하게 비운다(EXEC-4).
    std::size_t Prune(Clock::time_point now, bool pressure = false) noexcept
    {
        std::size_t removed = 0;
        auto retry = Clock::time_point::max();
        for (auto it = entries.begin(); it != entries.end();)
        {
            auto& entry = it->second;
            Refill(entry, now);
            const bool full = entry.tokens >= static_cast<long double>(options.burstTokens);
            if (!entry.active &&
                (closed || (full && (pressure || now - entry.seen >= options.idleExpiry))))
            {
                keyBytes -= it->first.size();
                it = entries.erase(it);
                ++removed;
            }
            else
            {
                if (!entry.active)
                    retry = (std::min)(retry, FullAt(entry));
                ++it;
            }
        }
        if (pressure)
            pressureRetry = retry;
        return removed;
    }
    RequestLimiterOptions options;
    std::mutex mutex;
    std::map<std::string, RequestLimitEntry, std::less<>> entries;
    std::size_t keyBytes = 0, active = 0;
    // 이 시각 전에는 압박 스캔이 비울 항목이 없다. 비활성 항목은 토큰이 줄 뿐 더 일찍 차지 않고,
    // 활성 항목이 비활성이 되는 permit 반환은 이 값을 지운다. 그래서 가득 찬 동안 새 키마다 전체를
    // 다시 훑지 않는다.
    Clock::time_point pressureRetry{};
    bool closed = false;
};
}
using Core::ErrorCode;
using Core::Result;
using Core::Status;
RequestPermit::RequestPermit(
    std::shared_ptr<Detail::RequestLimitState> state, Detail::RequestLimitEntry* entry) noexcept
    : mState(std::move(state))
    , mEntry(entry)
{
}
RequestPermit::~RequestPermit()
{
    Reset();
}
RequestPermit::RequestPermit(RequestPermit&& other) noexcept
    : mState(std::move(other.mState))
    , mEntry(std::exchange(other.mEntry, nullptr))
{
}
RequestPermit& RequestPermit::operator=(RequestPermit&& other) noexcept
{
    if (this != &other)
    {
        Reset();
        mState = std::move(other.mState);
        mEntry = std::exchange(other.mEntry, nullptr);
    }
    return *this;
}
void RequestPermit::Reset() noexcept
{
    auto state = std::move(mState);
    auto* entry = std::exchange(mEntry, nullptr);
    if (!state || !entry)
        return;
    std::lock_guard lock(state->mutex);
    --entry->active;
    --state->active;
    entry->seen = Detail::Clock::now();
    state->pressureRetry = {};
    if (state->closed)
        (void)state->Prune(entry->seen);
}
Result<RequestLimiter> RequestLimiter::Create(const RequestLimiterOptions& options)
{
    if (!options.maxKeys || options.maxKeys > 65536 || !options.maxKeyBytes ||
        options.maxKeyBytes > 4096 || options.maxRetainedKeyBytes < options.maxKeyBytes ||
        options.maxRetainedKeyBytes > 256 * 1024 * 1024 || !options.refillTokens ||
        options.refillTokens > 1000000000 || !options.burstTokens ||
        options.burstTokens > 1000000000 || options.refillInterval.count() <= 0 ||
        options.refillInterval > std::chrono::hours(24) || options.idleExpiry.count() <= 0 ||
        options.idleExpiry > std::chrono::hours(24 * 7) || !options.maxConcurrentPerKey ||
        !options.maxConcurrentTotal || options.maxConcurrentPerKey > options.maxConcurrentTotal ||
        options.maxConcurrentTotal > 1000000)
        return Result<RequestLimiter>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    try
    {
        return Result<RequestLimiter>::FromValue(
            RequestLimiter(std::make_shared<Detail::RequestLimitState>(options)));
    }
    catch (...)
    {
        return Result<RequestLimiter>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::PlatformError));
    }
}
Result<RequestAdmission> RequestLimiter::TryAcquire(std::string_view key, std::uint64_t cost) const
{
    if (!mState)
        return Result<RequestAdmission>::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
    const auto& options = mState->options;
    if (key.empty() || !cost)
        return Result<RequestAdmission>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    if (key.size() > options.maxKeyBytes || cost > options.burstTokens)
        return Result<RequestAdmission>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::TooLarge));
    try
    {
        std::lock_guard lock(mState->mutex);
        const auto now = Detail::Clock::now();
        if (mState->closed)
            return Result<RequestAdmission>::FromStatus(
                Status::FailWithoutMessage(ErrorCode::Closed));
        RequestAdmission result;
        if (mState->active >= options.maxConcurrentTotal)
        {
            result.reason = RequestLimitReason::TotalConcurrency;
            return Result<RequestAdmission>::FromValue(std::move(result));
        }
        auto found = mState->entries.find(key);
        if (found == mState->entries.end())
        {
            if ((mState->entries.size() >= options.maxKeys ||
                    key.size() > options.maxRetainedKeyBytes - mState->keyBytes) &&
                now >= mState->pressureRetry)
                (void)mState->Prune(now, true);
            if (mState->entries.size() >= options.maxKeys ||
                key.size() > options.maxRetainedKeyBytes - mState->keyBytes)
            {
                result.reason = RequestLimitReason::KeyCapacity;
                return Result<RequestAdmission>::FromValue(std::move(result));
            }
            found = mState->entries
                        .emplace(std::string(key),
                            Detail::RequestLimitEntry{
                                static_cast<long double>(options.burstTokens), 0, now, now })
                        .first;
            mState->keyBytes += key.size();
        }
        auto& entry = found->second;
        mState->Refill(entry, now);
        entry.seen = now;
        if (entry.active >= options.maxConcurrentPerKey)
            result.reason = RequestLimitReason::KeyConcurrency;
        else if (entry.tokens < static_cast<long double>(cost))
        {
            result.reason = RequestLimitReason::Rate;
            const auto delay = std::ceil((static_cast<long double>(cost) - entry.tokens) *
                                         static_cast<long double>(options.refillInterval.count()) /
                                         static_cast<long double>(options.refillTokens));
            result.retryAfter =
                std::chrono::milliseconds(static_cast<std::int64_t>((std::max)(1.0L, delay)));
        }
        else
        {
            entry.tokens -= static_cast<long double>(cost);
            ++entry.active;
            ++mState->active;
            result.permit = RequestPermit(mState, &entry);
        }
        return Result<RequestAdmission>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Result<RequestAdmission>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::PlatformError));
    }
}
std::size_t RequestLimiter::PruneExpired() const noexcept
{
    if (!mState)
        return 0;
    std::lock_guard lock(mState->mutex);
    return mState->Prune(Detail::Clock::now());
}
void RequestLimiter::Close() const noexcept
{
    if (!mState)
        return;
    std::lock_guard lock(mState->mutex);
    mState->closed = true;
    (void)mState->Prune(Detail::Clock::now());
}
RequestLimiterSnapshot RequestLimiter::Snapshot() const noexcept
{
    if (!mState)
        return {};
    std::lock_guard lock(mState->mutex);
    return { mState->entries.size(), mState->keyBytes, mState->active };
}
}
