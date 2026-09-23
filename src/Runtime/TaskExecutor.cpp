#include "ServerCore/Runtime/TaskExecutor.h"
#include "Observability/MetricsInternal.h"
#include "Runtime/CompletionSignalInternal.h"

#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ServerCore::Runtime
{
namespace Detail
{
using Core::ErrorCode;
using Core::Status;
using Clock = std::chrono::steady_clock;

class ExecutorScope;
thread_local const ExecutorScope* currentExecutorScope = nullptr;

// Cancellation can nest across executors on one worker. Keep every enclosing
// executor visible so an inline callback cannot hide a worker's self-join guard.
class ExecutorScope
{
public:
    explicit ExecutorScope(TaskExecutorState* executor) noexcept
        : mExecutor(executor)
        , mPrevious(currentExecutorScope)
    {
        currentExecutorScope = this;
    }
    ~ExecutorScope() { currentExecutorScope = mPrevious; }
    ExecutorScope(const ExecutorScope&) = delete;
    ExecutorScope& operator=(const ExecutorScope&) = delete;

    static bool Contains(const TaskExecutorState* executor) noexcept
    {
        for (auto scope = currentExecutorScope; scope; scope = scope->mPrevious)
            if (scope->mExecutor == executor)
                return true;
        return false;
    }

private:
    const TaskExecutorState* mExecutor;
    const ExecutorScope* mPrevious;
};

enum class TaskPhase
{
    Queued,
    Running,
    Completing,
    Terminal
};

struct ParentCancellation
{
    std::weak_ptr<TaskState> task;
    void operator()() const noexcept;
};

class TaskState
{
public:
    TaskState(
        TaskExecutor::Task value, TaskExecutor::Completion completed, TaskOptions configuration)
        : job(std::move(value))
        , completion(std::move(completed))
        , options(std::move(configuration))
    {
    }

    TaskExecutor::Task job;
    TaskExecutor::Completion completion;
    const TaskOptions options;
    std::stop_source cancellation;
    std::weak_ptr<TaskExecutorState> owner;
    std::unique_ptr<std::stop_callback<ParentCancellation>> parentRegistration;
    // Executor mutex owns scheduling fields.
    TaskPhase phase = TaskPhase::Queued;
    ErrorCode cancelReason = ErrorCode::Ok;
    bool signalSent = false;
    // Handles need completion state after their executor has been destroyed.
    mutable std::mutex resultMutex;
    std::condition_variable finished;
    bool terminal = false;
    Status result = Status::FailWithoutMessage(ErrorCode::WouldBlock);
    Clock::time_point admitted{};
    CompletionSignal completionSignal;
};

class TaskExecutorState : public std::enable_shared_from_this<TaskExecutorState>
{
public:
    Status Start(const TaskExecutorOptions& configuration)
    {
        if (configuration.workerCount == 0 || configuration.maxPendingTasks == 0 ||
            configuration.maxRetainedBytes == 0)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        if (ExecutorScope::Contains(this))
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard joinGuard(joinMutex);
        {
            const std::lock_guard guard(mutex);
            if (started || stopping)
                return Status::FailWithoutMessage(ErrorCode::Closed);
            options = configuration;
            started = true;
        }
        try
        {
            workers.reserve(options.workerCount);
            const auto self = shared_from_this();
            coordinator = std::thread([self] { self->Coordinate(); });
            for (std::size_t index = 0; index < options.workerCount; ++index)
                workers.emplace_back([self] { self->Run(); });
            {
                const std::lock_guard guard(mutex);
                ready = true;
            }
            wake.notify_all();
            return Status::Ok();
        }
        catch (...)
        {
            RequestStop();
            Join();
            return Status::AllocationFailure();
        }
    }

    Core::Result<std::shared_ptr<TaskState>> Submit(TaskExecutor::Task job,
        TaskExecutor::Completion completion, const TaskOptions& configuration)
    {
        using Result = Core::Result<std::shared_ptr<TaskState>>;
        if (!job)
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        {
            const std::lock_guard guard(mutex);
            if (configuration.retainedBytes > options.maxRetainedBytes)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
            if (!ready || stopping)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
            if (pending >= options.maxPendingTasks ||
                configuration.retainedBytes > options.maxRetainedBytes - retained)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::WouldBlock));
        }
        std::shared_ptr<TaskState> task;
        try
        {
            task =
                std::make_shared<TaskState>(std::move(job), std::move(completion), configuration);
            task->owner = shared_from_this();
            // Registration may invoke immediately. This task is not admitted yet,
            // so the callback records only its cancellation reason.
            if (configuration.parentToken.stop_possible())
                task->parentRegistration = std::make_unique<std::stop_callback<ParentCancellation>>(
                    configuration.parentToken, ParentCancellation{ task });
            {
                const std::lock_guard guard(mutex);
                if (!ready || stopping)
                    return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
                if (pending >= options.maxPendingTasks ||
                    configuration.retainedBytes > options.maxRetainedBytes - retained)
                    return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::WouldBlock));
                ObserveCancellation(*task, Clock::now());
                tasks.push_back(task);
                task->admitted = Clock::now();
                Observability::Detail::Add(metrics.acceptedTasks);
                ++pending;
                retained += configuration.retainedBytes;
            }
        }
        catch (...)
        {
            return Result::FromStatus(Status::AllocationFailure());
        }
        wake.notify_all();
        return Result::FromValue(std::move(task));
    }

    bool Cancel(const std::shared_ptr<TaskState>& task) noexcept
    {
        {
            const std::lock_guard guard(mutex);
            if (task->phase == TaskPhase::Completing || task->phase == TaskPhase::Terminal ||
                task->cancelReason != ErrorCode::Ok)
                return false;
            task->cancelReason = task->options.deadline && Clock::now() >= *task->options.deadline
                                     ? ErrorCode::Timeout
                                     : ErrorCode::Cancelled;
        }
        wake.notify_all();
        return true;
    }

    void RequestStop() noexcept
    {
        {
            const std::lock_guard guard(mutex);
            stopping = true;
            const auto now = Clock::now();
            for (const auto& task : tasks)
            {
                ObserveCancellation(*task, now);
                if (task->cancelReason == ErrorCode::Ok)
                    task->cancelReason = ErrorCode::Cancelled;
            }
        }
        wake.notify_all();
    }

    Status Stop()
    {
        if (ExecutorScope::Contains(this))
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard joinGuard(joinMutex);
        RequestStop();
        Join();
        return Status::Ok();
    }

    bool IsStopRequested() const noexcept
    {
        const std::lock_guard guard(mutex);
        return stopping;
    }
    std::size_t PendingCount() const noexcept
    {
        const std::lock_guard guard(mutex);
        return pending;
    }
    std::size_t RunningCount() const noexcept
    {
        const std::lock_guard guard(mutex);
        return running;
    }
    std::size_t RetainedBytes() const noexcept
    {
        const std::lock_guard guard(mutex);
        return retained;
    }
    void Rejected() noexcept
    {
        const std::lock_guard guard(mutex);
        Observability::Detail::Add(metrics.rejectedTasks);
    }
    Observability::TaskExecutorMetricsSnapshot GetMetrics() const noexcept
    {
        const std::lock_guard guard(mutex);
        auto result = metrics;
        result.pendingTasks = pending;
        result.runningTasks = running;
        result.retainedBytes = retained;
        result.lifecycle = joined ? Observability::Lifecycle::Stopped : stopping ? Observability::Lifecycle::Draining :
            ready ? Observability::Lifecycle::Running : Observability::Lifecycle::Created;
        return result;
    }

private:
    void ObserveCancellation(TaskState& task, Clock::time_point now) const noexcept
    {
        if (task.cancelReason != ErrorCode::Ok)
            return;
        if (task.options.deadline && now >= *task.options.deadline)
            task.cancelReason = ErrorCode::Timeout;
        else if (task.options.parentToken.stop_requested())
            task.cancelReason = ErrorCode::Cancelled;
    }

    void Join()
    {
        for (auto& worker : workers)
            if (worker.joinable())
                worker.join();
        if (coordinator.joinable())
            coordinator.join();
        workers.clear();
        const std::lock_guard guard(mutex);
        joined = true;
    }

    void Publish(const std::shared_ptr<TaskState>& task, Status result)
    {
        // Arbitrary capture destructors and stop callbacks never run under mutex.
        task->job = {};
        task->parentRegistration.reset();
        if (task->completion)
        {
            try
            {
                task->completion(result);
            }
            catch (...)
            { /* Completion observers cannot change the task result. */
            }
            task->completion = {};
        }
        {
            const std::lock_guard guard(mutex);
            retained -= task->options.retainedBytes;
            task->phase = TaskPhase::Terminal;
            Observability::Detail::Add(metrics.completedTasks);
            if (result.Code() == ErrorCode::Cancelled)
                Observability::Detail::Add(metrics.cancelledTasks);
            else if (result.Code() == ErrorCode::Timeout)
                Observability::Detail::Add(metrics.timedOutTasks);
            else if (!result.IsOk())
                Observability::Detail::Add(metrics.failedTasks);
            const auto elapsed = Observability::Detail::Elapsed(task->admitted);
            Observability::Detail::Add(metrics.totalLatencyNanoseconds, elapsed);
            Observability::Detail::ObserveLatency(metrics.latencyHistogram, elapsed);
            if (elapsed > metrics.maxLatencyNanoseconds)
                metrics.maxLatencyNanoseconds = elapsed;
        }
        const auto terminalCode = result.Code();
        {
            const std::lock_guard resultGuard(task->resultMutex);
            task->result = std::move(result);
            task->terminal = true;
        }
        task->finished.notify_all();
        task->completionSignal.Complete(terminalCode);
        wake.notify_all();
    }

    void Run()
    {
        const ExecutorScope executorScope(this);
        for (;;)
        {
            std::shared_ptr<TaskState> task;
            TaskExecutor::Task job;
            {
                std::unique_lock guard(mutex);
                for (;;)
                {
                    if (stopping)
                        return;
                    if (ready)
                    {
                        const auto now = Clock::now();
                        for (const auto& candidate : tasks)
                        {
                            if (candidate->phase != TaskPhase::Queued)
                                continue;
                            ObserveCancellation(*candidate, now);
                            if (candidate->cancelReason != ErrorCode::Ok)
                            {
                                wake.notify_all();
                                continue;
                            }
                            task = candidate;
                            task->phase = TaskPhase::Running;
                            --pending;
                            ++running;
                            break;
                        }
                        if (task)
                            break;
                    }
                    wake.wait(guard);
                }
            }
            // Running grants this worker exclusive callback ownership. Moving
            // an inline callable can invoke user move/destructor code, so do it
            // after releasing the scheduler lock. Cancellation only signals a
            // running task; the coordinator cannot clear this callback.
            job = std::move(task->job);
            Status result = Status::Ok();
            try
            {
                result = job(task->cancellation.get_token());
            }
            catch (...)
            {
                result = Status::FailWithoutMessage(ErrorCode::PlatformError);
            }
            job = {};
            ErrorCode reason;
            {
                const std::lock_guard guard(mutex);
                ObserveCancellation(*task, Clock::now());
                reason = task->cancelReason;
                task->phase = TaskPhase::Completing;
                --running;
                tasks.remove(task);
            }
            if (reason != ErrorCode::Ok)
            {
                (void)task->cancellation.request_stop();
                result = Status::FailWithoutMessage(reason);
            }
            Publish(task, std::move(result));
        }
    }

    void Coordinate()
    {
        const ExecutorScope executorScope(this);
        std::unique_lock guard(mutex);
        for (;;)
        {
            std::shared_ptr<TaskState> selected;
            bool queued = false;
            std::optional<Clock::time_point> nextDeadline;
            const auto now = Clock::now();
            for (const auto& task : tasks)
            {
                ObserveCancellation(*task, now);
                if (task->cancelReason != ErrorCode::Ok && !task->signalSent)
                {
                    selected = task;
                    task->signalSent = true;
                    queued = task->phase == TaskPhase::Queued;
                    if (queued)
                    {
                        task->phase = TaskPhase::Completing;
                        --pending;
                    }
                    break;
                }
                if (task->cancelReason == ErrorCode::Ok && task->options.deadline &&
                    (!nextDeadline || *task->options.deadline < *nextDeadline))
                    nextDeadline = task->options.deadline;
            }
            if (selected)
            {
                const auto reason = selected->cancelReason;
                if (queued)
                    tasks.remove(selected);
                guard.unlock();
                (void)selected->cancellation.request_stop();
                if (queued)
                    Publish(selected, Status::FailWithoutMessage(reason));
                guard.lock();
                continue;
            }
            if (stopping && tasks.empty())
                return;
            if (nextDeadline)
                wake.wait_until(guard, *nextDeadline);
            else
                wake.wait(guard);
        }
    }

    mutable std::mutex mutex;
    std::mutex joinMutex;
    std::condition_variable wake;
    TaskExecutorOptions options;
    std::list<std::shared_ptr<TaskState>> tasks;
    std::vector<std::thread> workers;
    std::thread coordinator;
    bool started = false;
    bool ready = false;
    bool stopping = false;
    bool joined = false;
    std::size_t pending = 0;
    std::size_t running = 0;
    std::size_t retained = 0;
    Observability::TaskExecutorMetricsSnapshot metrics;
};

void ParentCancellation::operator()() const noexcept
{
    const auto state = task.lock();
    if (state)
        if (const auto owner = state->owner.lock())
            (void)owner->Cancel(state);
}

Status ReadResult(const std::shared_ptr<TaskState>& task)
{
    try
    {
        return task->result;
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}
}

TaskHandle::TaskHandle(std::shared_ptr<Detail::TaskState> state) noexcept
    : mState(std::move(state))
{
}
bool TaskHandle::IsValid() const noexcept
{
    return mState != nullptr;
}
std::stop_token TaskHandle::GetToken() const noexcept
{
    return mState ? mState->cancellation.get_token() : std::stop_token{};
}
bool TaskHandle::RequestCancel() const noexcept
{
    const auto state = mState;
    if (!state)
        return false;
    const auto owner = state->owner.lock();
    if (!owner || !owner->Cancel(state))
        return false;
    // A callback may run on this external caller while a worker waits for its
    // stop_callback destructor. Treat it as executor work to reject self-join.
    const Detail::ExecutorScope executorScope(owner.get());
    (void)state->cancellation.request_stop();
    return true;
}
bool TaskHandle::IsFinished() const noexcept
{
    if (!mState)
        return false;
    const std::lock_guard guard(mState->resultMutex);
    return mState->terminal;
}
Core::Status TaskHandle::GetStatus() const
{
    if (!mState)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    const std::lock_guard guard(mState->resultMutex);
    return Detail::ReadResult(mState);
}
Core::Status TaskHandle::Wait() const
{
    return WaitUntil(std::chrono::steady_clock::time_point::max());
}
Core::Status TaskHandle::WaitUntil(std::chrono::steady_clock::time_point deadline) const
{
    if (!mState)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    std::unique_lock guard(mState->resultMutex);
    if (!mState->terminal)
    {
        const auto owner = mState->owner.lock();
        if (owner && Detail::ExecutorScope::Contains(owner.get()))
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        if (!mState->finished.wait_until(guard, deadline, [this] { return mState->terminal; }))
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout);
    }
    return Detail::ReadResult(mState);
}

TaskExecutor::TaskExecutor()
    : mState(std::make_shared<Detail::TaskExecutorState>())
{
}
Core::Status TaskHandle::ObserveCompletion(Core::CompletionSource source) const noexcept
{
    if (!mState) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    return mState->completionSignal.Observe(std::move(source));
}
Core::Result<Core::CompletionSubscription> TaskHandle::WaitForCompletion(
    std::function<void(Core::Status)> callback, std::stop_token cancellation) const
{
    if (!mState) return Core::Result<Core::CompletionSubscription>::FromStatus(
        Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
    return mState->completionSignal.Subscribe(std::move(callback), cancellation);
}
TaskExecutor::~TaskExecutor()
{
    SERVERCORE_ASSERT(!Detail::ExecutorScope::Contains(mState.get()),
        "TaskExecutor must be destroyed outside its threads");
    (void)mState->Stop();
}
Core::Status TaskExecutor::Start(const TaskExecutorOptions& options)
{
    return mState->Start(options);
}
Core::Result<TaskHandle> TaskExecutor::Submit(Task task, const TaskOptions& options)
{
    return SubmitWithCompletion(std::move(task), {}, options);
}
Core::Result<TaskHandle> TaskExecutor::SubmitWithCompletion(
    Task task, Completion completion, const TaskOptions& options)
{
    auto submitted = mState->Submit(std::move(task), std::move(completion), options);
    if (!submitted.IsOk())
    {
        mState->Rejected();
        return Core::Result<TaskHandle>::FromStatus(std::move(submitted).TakeStatus());
    }
    return Core::Result<TaskHandle>::FromValue(TaskHandle(std::move(submitted.Value())));
}
void TaskExecutor::RequestStop() noexcept
{
    mState->RequestStop();
}
Core::Status TaskExecutor::Stop()
{
    return mState->Stop();
}
bool TaskExecutor::IsStopRequested() const noexcept
{
    return mState->IsStopRequested();
}
bool TaskExecutor::IsCurrentThreadWorker() const noexcept
{
    return Detail::ExecutorScope::Contains(mState.get());
}
std::size_t TaskExecutor::PendingCount() const noexcept
{
    return mState->PendingCount();
}
std::size_t TaskExecutor::RunningCount() const noexcept
{
    return mState->RunningCount();
}
std::size_t TaskExecutor::RetainedBytes() const noexcept
{
    return mState->RetainedBytes();
}
Observability::TaskExecutorMetricsSnapshot TaskExecutor::GetMetrics() const noexcept
{
    return mState->GetMetrics();
}
}
