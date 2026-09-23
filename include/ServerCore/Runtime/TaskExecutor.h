#pragma once
#include "ServerCore/Export.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/CompletionSubscription.h"
#include "ServerCore/Observability/Metrics.h"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace ServerCore::Runtime
{
namespace Detail
{
class TaskExecutorState;
class TaskState;
class TaskGroupState;
class TimerSchedulerState;
}

struct TaskExecutorOptions
{
    std::size_t workerCount = 2;
    std::size_t maxPendingTasks = 128;
    // Caller-declared retained bytes of queued and running tasks together.
    // This bounds admission, not allocations performed inside task functions.
    std::size_t maxRetainedBytes = 4 * 1024 * 1024;
};

struct TaskOptions
{
    std::size_t retainedBytes = 0;
    std::stop_token parentToken{};
    std::optional<std::chrono::steady_clock::time_point> deadline{};
};

// Copyable observation/cancellation handle. It does not keep an executor alive.
// Default handles are invalid: status/waits return InvalidArgument.
class TaskHandle
{
public:
    TaskHandle() noexcept = default;
    [[nodiscard]] SERVERCORE_API bool IsValid() const noexcept;
    [[nodiscard]] SERVERCORE_API std::stop_token GetToken() const noexcept;
    // First cancellation wins; late cancellation does not change the token or
    // result. Standard stop callbacks may execute synchronously on this caller.
    [[nodiscard]] SERVERCORE_API bool RequestCancel() const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsFinished() const noexcept;
    // WouldBlock until terminal. A running cancelled task remains nonterminal
    // until its function returns and its captured state has been released.
    [[nodiscard]] SERVERCORE_API Core::Status GetStatus() const;
    // A wait deadline only bounds the wait; it does not cancel the task.
    // Unfinished waits from this executor's threads or cancellation callbacks
    // return InvalidArgument.
    [[nodiscard]] SERVERCORE_API Core::Status Wait() const;
    [[nodiscard]] SERVERCORE_API Core::Status WaitUntil(std::chrono::steady_clock::time_point deadline) const;
    // At most 16 pending observers per task; terminal calls may complete inline.
    // Fires after work/completion captures retire and the handle is terminal.
    SERVERCORE_API Core::Result<Core::CompletionSubscription> WaitForCompletion(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) const;

private:
    friend class TaskExecutor;
    friend class Detail::TaskGroupState;
    friend class Detail::TimerSchedulerState;
    SERVERCORE_API Core::Status ObserveCompletion(Core::CompletionSource source) const noexcept;
    SERVERCORE_API explicit TaskHandle(std::shared_ptr<Detail::TaskState> state) noexcept;
    std::shared_ptr<Detail::TaskState> mState;
};

// Fixed worker pool with a bounded pending queue and shared retained-byte budget.
// Start is single-use. Jobs may run concurrently; use JobRunner for serial work.
// Cancellation is cooperative for running jobs. A deadline requests cancellation
// and selects Timeout; the first cancellation reason wins over the job's result.
// Exceptions from a job become PlatformError and do not terminate a worker.
//
// A coordinator removes cancelled queued jobs and processes deadlines even when
// all workers are occupied. Parent cancellation and RequestStop wake it without
// executing user stop callbacks inline. Such callbacks and capture destructors
// must finish promptly and must not wait on work requiring this executor.
//
// Stop/destruction join the threads. A noncooperative job can delay them without
// bound. Destroy outside executor threads/cancellation callbacks; Stop from one
// returns InvalidArgument.
class TaskExecutor
{
public:
    // Single-owner callbacks: pass named tasks/completions with std::move.
    using Task = std::move_only_function<Core::Status(std::stop_token)>;
    using Completion = std::move_only_function<void(const Core::Status&)>;

    SERVERCORE_API TaskExecutor();
    SERVERCORE_API ~TaskExecutor();
    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    SERVERCORE_API Core::Status Start(const TaskExecutorOptions& options = {});
    // Empty functions are InvalidArgument; a task larger than the byte budget
    // is TooLarge. Used capacity is WouldBlock; inactive/stopped is Closed.
    // Rejection never retains the function or consumes count/byte capacity.
    SERVERCORE_API Core::Result<TaskHandle> Submit(Task task, const TaskOptions& options = {});
    // Keep legacy std::function inputs, including empty-input validation.
    template<class Callback>
        requires std::same_as<std::remove_cvref_t<Callback>,
            std::function<Core::Status(std::stop_token)>>
    Core::Result<TaskHandle> Submit(Callback&& task, const TaskOptions& options = {})
    {
        return Submit(NormalizeCallback<Core::Status(std::stop_token)>(
            std::forward<Callback>(task)), options);
    }
    // Runs once for every accepted task, including queued cancellation, on an
    // executor worker/coordinator after the task releases its captures. It must
    // return promptly; exceptions are contained. Include its captures in the
    // declared retainedBytes. Wait observes completion only after it returns.
    // A rejected submission never invokes completion.
    SERVERCORE_API Core::Result<TaskHandle> SubmitWithCompletion(
        Task task, Completion completion, const TaskOptions& options = {});
    template<class Work = Task, class Done = Completion>
        requires (std::same_as<std::remove_cvref_t<Work>,
                      std::function<Core::Status(std::stop_token)>> ||
                     std::same_as<std::remove_cvref_t<Done>,
                         std::function<void(const Core::Status&)>>) &&
                 std::constructible_from<Task, Work> &&
                 std::constructible_from<Completion, Done>
    Core::Result<TaskHandle> SubmitWithCompletion(
        Work&& task, Done&& completion, const TaskOptions& options = {})
    {
        return SubmitWithCompletion(
            NormalizeCallback<Core::Status(std::stop_token)>(std::forward<Work>(task)),
            NormalizeCallback<void(const Core::Status&)>(std::forward<Done>(completion)), options);
    }
    SERVERCORE_API void RequestStop() noexcept;
    SERVERCORE_API Core::Status Stop();
    [[nodiscard]] SERVERCORE_API bool IsStopRequested() const noexcept;
    // Includes this executor's worker, completion and cancellation scopes.
    [[nodiscard]] SERVERCORE_API bool IsCurrentThreadWorker() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t PendingCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RunningCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RetainedBytes() const noexcept;
    [[nodiscard]] SERVERCORE_API Observability::TaskExecutorMetricsSnapshot GetMetrics() const noexcept;

private:
    template<class Signature, class Callback>
    static std::move_only_function<Signature> NormalizeCallback(Callback&& callback)
    {
        if constexpr (std::same_as<std::remove_cvref_t<Callback>, std::function<Signature>>)
        {
            if (!callback)
                return {};
        }
        return std::move_only_function<Signature>(std::forward<Callback>(callback));
    }

    std::shared_ptr<Detail::TaskExecutorState> mState;
};
}
