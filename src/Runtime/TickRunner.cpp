#include "ServerCore/Runtime/TickRunner.h"
#include <algorithm>
#include <limits>
#include <mutex>

namespace ServerCore::Runtime
{
namespace Detail
{
struct TickState
{
    mutable std::mutex mutex;
    TickMetrics metrics;
};
}
TickMetrics TickHandle::Metrics() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->metrics;
}
Core::Result<TickHandle> ScheduleTicks(TimerScheduler& scheduler,
    std::move_only_function<Core::Status(const TickInfo&, std::stop_token)> callback,
    const TickOptions& options)
{
    using Result = Core::Result<TickHandle>;
    using Clock = std::chrono::steady_clock;
    using Core::ErrorCode;
    if (!callback || options.interval.count() <= 0 || options.interval > std::chrono::hours(24) ||
        !options.maxCatchUp || options.maxCatchUp > 1024 ||
        (options.lagPolicy != TickLagPolicy::Skip && options.lagPolicy != TickLagPolicy::CatchUp))
        return Result::FromStatus(Core::Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    if (options.retainedBytes >
        (std::numeric_limits<std::size_t>::max)() - sizeof(Detail::TickState))
        return Result::FromStatus(Core::Status::FailWithoutMessage(ErrorCode::TooLarge));
    try
    {
        auto state = std::make_shared<Detail::TickState>();
        const auto interval = std::chrono::duration_cast<Clock::duration>(options.interval);
        const auto now = Clock::now();
        if (interval > Clock::time_point::max() - now)
            return Result::FromStatus(Core::Status::FailWithoutMessage(ErrorCode::TooLarge));
        const auto epoch = now + interval;
        TimerOptions timer;
        timer.due = epoch;
        timer.repeatInterval = interval;
        // 논리 틱이 epoch + n×interval이므로 타이머도 같은 격자를 따라야 콜백 시간만큼 위상이 밀려
        // 틱을 건너뛰지 않는다(EXEC-1).
        timer.repeatMode = TimerRepeatMode::FixedRate;
        timer.parentToken = options.parentToken;
        timer.retainedBytes = options.retainedBytes + sizeof(Detail::TickState);
        const auto maxCalls =
            options.lagPolicy == TickLagPolicy::Skip ? std::size_t{ 1 } : options.maxCatchUp;
        auto scheduled = scheduler.Schedule(
            [state, epoch, interval, maxCalls, next = std::uint64_t{ 0 },
                work = std::move(callback)](std::stop_token token) mutable
            {
                const auto actual = Clock::now();
                if (actual < epoch)
                    return Core::Status::Ok();
                const auto due = static_cast<std::uint64_t>((actual - epoch) / interval) + 1;
                if (due <= next)
                    return Core::Status::Ok();
                const auto count = (std::min)(due - next, static_cast<std::uint64_t>(maxCalls));
                const auto skipped = due - next - count;
                next += skipped;
                {
                    const std::lock_guard guard(state->mutex);
                    state->metrics.skipped += skipped;
                }
                for (std::uint64_t call = 0; call < count; ++call)
                {
                    if (token.stop_requested())
                        return Core::Status::FailWithoutMessage(ErrorCode::Cancelled);
                    // next*interval is bounded by actual-epoch, avoiding future-clock overflow.
                    const auto logical = epoch + interval * static_cast<Clock::duration::rep>(next);
                    const auto started = Clock::now();
                    TickInfo tick{ next + 1, logical, started, started - logical,
                        call == 0 ? skipped : 0 };
                    ++next;
                    {
                        const std::lock_guard guard(state->mutex);
                        ++state->metrics.executed;
                        state->metrics.lastLateness = tick.lateness;
                        state->metrics.maxLateness =
                            (std::max)(state->metrics.maxLateness, tick.lateness);
                    }
                    auto result = work(tick, token);
                    if (!result.IsOk())
                        return result;
                }
                return Core::Status::Ok();
            },
            timer);
        if (!scheduled.IsOk())
            return Result::FromStatus(std::move(scheduled).TakeStatus());
        return Result::FromValue(TickHandle(std::move(scheduled.Value()), std::move(state)));
    }
    catch (...)
    {
        return Result::FromStatus(Core::Status::AllocationFailure());
    }
}
}
