#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#include <cstdint>
#include <functional>

namespace ServerCore::Runtime
{
enum class TickLagPolicy
{
    Skip,
    CatchUp
};
struct TickOptions
{
    std::chrono::milliseconds interval{ 16 };
    TickLagPolicy lagPolicy = TickLagPolicy::Skip;
    std::size_t maxCatchUp = 4;
    std::size_t retainedBytes = 0; // Caller-owned callback captures, excluding fixed bookkeeping.
    std::stop_token parentToken{};
};
struct TickInfo
{
    std::uint64_t index = 0; // 1-based logical interval, including skipped intervals.
    std::chrono::steady_clock::time_point scheduled{}, actual{};
    std::chrono::steady_clock::duration lateness{};
    std::uint64_t skipped = 0; // Skipped intervals preceding this callback.
};
struct TickMetrics
{
    std::uint64_t executed = 0, skipped = 0;
    std::chrono::steady_clock::duration lastLateness{}, maxLateness{};
};
namespace Detail
{
struct TickState;
}
// Observation handle; copying/dropping does not cancel. Explicit RequestCancel
// and Wait follow TimerHandle semantics, including self-wait rejection. One
// scheduler timer owns the callback until cancellation/completion. The supplied
// scheduler/executor must outlive their normal stop/cleanup boundaries.
class TickHandle
{
public:
    bool RequestCancel() const noexcept { return mTimer.RequestCancel(); }
    bool IsFinished() const noexcept { return mTimer.IsFinished(); }
    Core::Status GetStatus() const noexcept { return mTimer.GetStatus(); }
    Core::Status Wait() const { return mTimer.Wait(); }
    Core::Status WaitUntil(std::chrono::steady_clock::time_point deadline) const
    {
        return mTimer.WaitUntil(deadline);
    }
    Core::Result<Core::CompletionSubscription> WaitForCompletion(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) const
    {
        return mTimer.WaitForCompletion(std::move(callback), cancellation);
    }
    SERVERCORE_API TickMetrics Metrics() const noexcept;

private:
    friend SERVERCORE_API Core::Result<TickHandle> ScheduleTicks(TimerScheduler&,
        std::move_only_function<Core::Status(const TickInfo&, std::stop_token)>,
        const TickOptions&);
    TickHandle(TimerHandle timer, std::shared_ptr<Detail::TickState> state)
        : mTimer(std::move(timer))
        , mState(std::move(state))
    {
    }
    TimerHandle mTimer;
    std::shared_ptr<Detail::TickState> mState;
};
// Uses one existing fixed-delay timer; logical time is fixed-rate. No simulation
// state or additional worker. Skip invokes the newest due tick. CatchUp invokes
// at most maxCatchUp most-recent due ticks in order, discarding older lag. A long
// callback never causes overlap. Each batch snapshots time once; later delays
// reconcile on the next wake. Callback failure terminates the timer. Cancellation
// is checked between catch-up calls; running work remains cooperative.
SERVERCORE_API Core::Result<TickHandle> ScheduleTicks(TimerScheduler& scheduler,
    std::move_only_function<Core::Status(const TickInfo&, std::stop_token)> callback,
    const TickOptions& options = {});
}
