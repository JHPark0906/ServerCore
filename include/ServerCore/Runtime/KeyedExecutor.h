#pragma once
#include "ServerCore/Export.h"

#include "ServerCore/Runtime/TaskExecutor.h"

#include <cstdint>

namespace ServerCore::Runtime
{
namespace Detail
{
class KeyedExecutorState;
class KeyedTaskState;
}

struct KeyedExecutorOptions
{
    std::size_t workerCount = 2;
    std::size_t maxKeys = 1024;
    std::size_t maxOutstandingTasks = 4096;
    std::size_t maxRetainedBytes = 16 * 1024 * 1024;
    std::size_t maxOutstandingTasksPerKey = 128;
    std::size_t maxRetainedBytesPerKey = 4 * 1024 * 1024;
};

// A copyable observation handle; it does not extend the executor's lifetime.
// Cancellation is cooperative for running work. Wait observes capture/completion
// cleanup, not merely the end of the task function. Late cancellation is a no-op.
class KeyedTaskHandle
{
public:
    KeyedTaskHandle() noexcept = default;
    [[nodiscard]] SERVERCORE_API bool IsValid() const noexcept;
    [[nodiscard]] SERVERCORE_API std::stop_token GetToken() const noexcept;
    [[nodiscard]] SERVERCORE_API bool RequestCancel() const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsFinished() const noexcept;
    [[nodiscard]] SERVERCORE_API Core::Status GetStatus() const;
    [[nodiscard]] SERVERCORE_API Core::Status Wait() const;
    [[nodiscard]] SERVERCORE_API Core::Status WaitUntil(std::chrono::steady_clock::time_point deadline) const;
    // Up to 16 pending one-shot observers; callbacks run outside scheduler
    // locks after task/completion captures retire. Terminal calls may run inline.
    SERVERCORE_API Core::Result<Core::CompletionSubscription> WaitForCompletion(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) const;

private:
    friend class KeyedExecutor;
    SERVERCORE_API explicit KeyedTaskHandle(std::shared_ptr<Detail::KeyedTaskState> state) noexcept;
    std::shared_ptr<Detail::KeyedTaskState> mState;
};

// Same-key tasks run in accepted FIFO order without overlap; different keys may
// run concurrently. Ready keys rotate after each task. A running task is never
// preempted, so fairness cannot bound latency of noncooperative application work.
//
// Count/byte limits include queued, running and completing work. Bytes are the
// caller's retainedBytes declaration, not allocations performed by a callback.
// Idle keys are removed; retained handles do not consume registry capacity.
//
// A coordinator promptly removes cancelled queued tasks even if every worker is
// busy. Their capture destructors/completions can therefore run concurrently with
// a running task of that key. These observers must synchronize their own state.
// Running tasks retain their key through completion/capture cleanup.
//
// Start is single-use. RequestStop is callback-safe and nonblocking; Stop joins
// outside executor threads/cancellation callbacks (otherwise InvalidArgument).
// Callbacks must return promptly; noncooperative work can delay Stop indefinitely.
// Destroy only outside this executor's threads/cancellation callbacks.
class KeyedExecutor
{
public:
    using Task = TaskExecutor::Task;
    using Completion = TaskExecutor::Completion;

    SERVERCORE_API KeyedExecutor();
    SERVERCORE_API ~KeyedExecutor();
    KeyedExecutor(const KeyedExecutor&) = delete;
    KeyedExecutor& operator=(const KeyedExecutor&) = delete;

    SERVERCORE_API Core::Status Start(const KeyedExecutorOptions& options = {});
    // Empty task: InvalidArgument. A task exceeding either byte cap: TooLarge.
    // Used count/bytes/key capacity: WouldBlock. Inactive/stopped: Closed.
    // Rejection retains no callback, invokes no completion and consumes no budget.
    SERVERCORE_API Core::Result<KeyedTaskHandle> Submit(std::uint64_t key, Task task,
        const TaskOptions& options = {});
    SERVERCORE_API Core::Result<KeyedTaskHandle> SubmitWithCompletion(std::uint64_t key, Task task,
        Completion completion, const TaskOptions& options = {});

    template<class Callback>
        requires std::same_as<std::remove_cvref_t<Callback>,
            std::function<Core::Status(std::stop_token)>>
    Core::Result<KeyedTaskHandle> Submit(std::uint64_t key, Callback&& task,
        const TaskOptions& options = {})
    {
        return Submit(key, Normalize<Core::Status(std::stop_token)>(
            std::forward<Callback>(task)), options);
    }
    template<class Work = Task, class Done = Completion>
        requires (std::same_as<std::remove_cvref_t<Work>,
                      std::function<Core::Status(std::stop_token)>> ||
                  std::same_as<std::remove_cvref_t<Done>,
                      std::function<void(const Core::Status&)>>) &&
                 std::constructible_from<Task, Work> && std::constructible_from<Completion, Done>
    Core::Result<KeyedTaskHandle> SubmitWithCompletion(std::uint64_t key, Work&& task,
        Done&& completion, const TaskOptions& options = {})
    {
        return SubmitWithCompletion(key,
            Normalize<Core::Status(std::stop_token)>(std::forward<Work>(task)),
            Normalize<void(const Core::Status&)>(std::forward<Done>(completion)), options);
    }

    SERVERCORE_API void RequestStop() noexcept;
    SERVERCORE_API Core::Status Stop();
    [[nodiscard]] SERVERCORE_API bool IsStopRequested() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t KeyCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t PendingCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RunningCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t OutstandingCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t OutstandingCount(std::uint64_t key) const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RetainedBytes() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RetainedBytes(std::uint64_t key) const noexcept;
    [[nodiscard]] SERVERCORE_API Observability::TaskExecutorMetricsSnapshot GetMetrics() const noexcept;

private:
    template<class Signature, class Callback>
    static std::move_only_function<Signature> Normalize(Callback&& callback)
    {
        if constexpr (std::same_as<std::remove_cvref_t<Callback>, std::function<Signature>>)
            if (!callback) return {};
        return std::move_only_function<Signature>(std::forward<Callback>(callback));
    }
    std::shared_ptr<Detail::KeyedExecutorState> mState;
};
}
