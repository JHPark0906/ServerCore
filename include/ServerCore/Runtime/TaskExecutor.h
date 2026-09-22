#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Observability/Metrics.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace ServerCore::Runtime
{
namespace Detail
{
class TaskExecutorState;
class TaskState;
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
    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] std::stop_token GetToken() const noexcept;
    // First cancellation wins; late cancellation does not change the token or
    // result. Standard stop callbacks may execute synchronously on this caller.
    [[nodiscard]] bool RequestCancel() const noexcept;
    [[nodiscard]] bool IsFinished() const noexcept;
    // WouldBlock until terminal. A running cancelled task remains nonterminal
    // until its function returns and its captured state has been released.
    [[nodiscard]] Core::Status GetStatus() const;
    // A wait deadline only bounds the wait; it does not cancel the task.
    // Unfinished waits from this executor's threads or cancellation callbacks
    // return InvalidArgument.
    [[nodiscard]] Core::Status Wait() const;
    [[nodiscard]] Core::Status WaitUntil(std::chrono::steady_clock::time_point deadline) const;

private:
    friend class TaskExecutor;
    explicit TaskHandle(std::shared_ptr<Detail::TaskState> state) noexcept;
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
    using Task = std::function<Core::Status(std::stop_token)>;
    using Completion = std::function<void(const Core::Status&)>;

    TaskExecutor();
    ~TaskExecutor();
    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    Core::Status Start(const TaskExecutorOptions& options = {});
    // Empty functions are InvalidArgument; a task larger than the byte budget
    // is TooLarge. Used capacity is WouldBlock; inactive/stopped is Closed.
    // Rejection never retains the function or consumes count/byte capacity.
    Core::Result<TaskHandle> Submit(Task task, const TaskOptions& options = {});
    // Runs once for every accepted task, including queued cancellation, on an
    // executor worker/coordinator after the task releases its captures. It must
    // return promptly; exceptions are contained. Include its captures in the
    // declared retainedBytes. Wait observes completion only after it returns.
    // A rejected submission never invokes completion.
    Core::Result<TaskHandle> SubmitWithCompletion(
        Task task, Completion completion, const TaskOptions& options = {});
    void RequestStop() noexcept;
    Core::Status Stop();
    [[nodiscard]] bool IsStopRequested() const noexcept;
    [[nodiscard]] std::size_t PendingCount() const noexcept;
    [[nodiscard]] std::size_t RunningCount() const noexcept;
    [[nodiscard]] std::size_t RetainedBytes() const noexcept;
    [[nodiscard]] Observability::TaskExecutorMetricsSnapshot GetMetrics() const noexcept;

private:
    std::shared_ptr<Detail::TaskExecutorState> mState;
};
}
