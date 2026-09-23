#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include <chrono>
#include <cstddef>
#include <memory>
#include <stop_token>

namespace ServerCore::Runtime
{
namespace Detail { class TimerSchedulerState; class TimerState; }
struct TimerSchedulerOptions
{
    std::size_t maxTimers = 1024;
    std::size_t maxRetainedBytes = 4 * 1024 * 1024;
};
struct TimerOptions
{
    std::chrono::steady_clock::time_point due = std::chrono::steady_clock::now();
    // Zero is one-shot. Positive intervals use fixed delay after completion,
    // never overlap or catch up missed ticks. A failed callback ends the timer.
    std::chrono::steady_clock::duration repeatInterval{};
    std::size_t retainedBytes = 0;
    std::stop_token parentToken{};
};
class TimerHandle
{
public:
    TimerHandle() noexcept = default;
    [[nodiscard]] SERVERCORE_API bool IsValid() const noexcept;
    [[nodiscard]] SERVERCORE_API bool RequestCancel() const noexcept;
    // Pending timers only; WouldBlock while dispatching/executing, Closed after
    // cancellation/terminal. Reuses the original admission and capture budget.
    SERVERCORE_API Core::Status Reschedule(std::chrono::steady_clock::time_point due) const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsFinished() const noexcept;
    [[nodiscard]] SERVERCORE_API Core::Status GetStatus() const noexcept;
    SERVERCORE_API Core::Status Wait() const;
    SERVERCORE_API Core::Status WaitUntil(std::chrono::steady_clock::time_point deadline) const;
    // Up to 16 pending observers; fires after terminal state and capture cleanup.
    SERVERCORE_API Core::Result<Core::CompletionSubscription> WaitForCompletion(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) const;
private:
    friend class TimerScheduler;
    SERVERCORE_API explicit TimerHandle(std::shared_ptr<Detail::TimerState> state) noexcept;
    std::shared_ptr<Detail::TimerState> mState;
};
// One timing thread per scheduler; callbacks run on the supplied TaskExecutor.
// The executor must outlive Stop/destruction. Start once; stop cancels pending
// timers and joins after active callbacks/captures retire. Noncooperative user
// work can delay Stop without bound. Never destroy from either owned context.
// Callback memory remains charged while waiting, dispatching and executing;
// declare its retained bytes in TimerOptions (also charged to the executor).
// Executor WouldBlock delays dispatch with 1 ms retries; other errors end
// the timer. Scheduling deadlines are not hard real-time guarantees.
class TimerScheduler
{
public:
    using Callback = TaskExecutor::Task;
    SERVERCORE_API TimerScheduler();
    SERVERCORE_API ~TimerScheduler();
    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler&) = delete;
    SERVERCORE_API Core::Status Start(TaskExecutor& executor, const TimerSchedulerOptions& options = {});
    SERVERCORE_API Core::Result<TimerHandle> Schedule(Callback callback, const TimerOptions& options = {});
    SERVERCORE_API void RequestStop() noexcept;
    SERVERCORE_API Core::Status Stop();
    [[nodiscard]] SERVERCORE_API std::size_t TimerCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RetainedBytes() const noexcept;
private:
    std::shared_ptr<Detail::TimerSchedulerState> mState;
};
}
