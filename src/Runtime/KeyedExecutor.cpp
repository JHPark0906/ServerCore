#include "ServerCore/Runtime/KeyedExecutor.h"
#include "Observability/MetricsInternal.h"
#include "Runtime/CompletionSignalInternal.h"

#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ServerCore::Runtime
{
namespace Detail
{
using Core::ErrorCode;
using Core::Status;
using KeyedClock = std::chrono::steady_clock;

class KeyedScope;
thread_local const KeyedScope* currentKeyedScope = nullptr;
class KeyedScope
{
public:
    explicit KeyedScope(const KeyedExecutorState* value) noexcept
        : executor(value), previous(currentKeyedScope) { currentKeyedScope = this; }
    ~KeyedScope() { currentKeyedScope = previous; }
    KeyedScope(const KeyedScope&) = delete;
    KeyedScope& operator=(const KeyedScope&) = delete;
    static bool Contains(const KeyedExecutorState* value) noexcept
    {
        for (auto scope = currentKeyedScope; scope; scope = scope->previous)
            if (scope->executor == value) return true;
        return false;
    }
private:
    const KeyedExecutorState* executor;
    const KeyedScope* previous;
};

enum class KeyedPhase { Queued, Running, Completing, Terminal };
struct KeyedParentCancellation
{
    std::weak_ptr<KeyedTaskState> task;
    void operator()() const noexcept;
};
class KeyedTaskState
{
public:
    KeyedTaskState(std::uint64_t value, KeyedExecutor::Task function,
        KeyedExecutor::Completion observer, TaskOptions configuration)
        : key(value), job(std::move(function)), completion(std::move(observer)), options(configuration) {}
    const std::uint64_t key;
    KeyedExecutor::Task job;
    KeyedExecutor::Completion completion;
    const TaskOptions options;
    std::stop_source cancellation;
    std::weak_ptr<KeyedExecutorState> owner;
    std::unique_ptr<std::stop_callback<KeyedParentCancellation>> parentRegistration;
    KeyedPhase phase = KeyedPhase::Queued;
    ErrorCode cancelReason = ErrorCode::Ok;
    bool signalSent = false;
    KeyedClock::time_point admitted{};
    mutable std::mutex resultMutex;
    std::condition_variable finished;
    bool terminal = false;
    Status result = Status::FailWithoutMessage(ErrorCode::WouldBlock);
    CompletionSignal completionSignal;
};

class KeyedExecutorState : public std::enable_shared_from_this<KeyedExecutorState>
{
    using TaskPtr = std::shared_ptr<KeyedTaskState>;
    struct Key
    {
        std::list<TaskPtr> queued;
        std::size_t outstanding = 0, bytes = 0;
        bool busy = false, ready = false;
        Key* previous = nullptr;
        Key* next = nullptr;
    };
public:
    Status Start(const KeyedExecutorOptions& configuration)
    {
        if (!configuration.workerCount || !configuration.maxKeys ||
            !configuration.maxOutstandingTasks || !configuration.maxRetainedBytes ||
            !configuration.maxOutstandingTasksPerKey || !configuration.maxRetainedBytesPerKey ||
            KeyedScope::Contains(this))
            return Fail(ErrorCode::InvalidArgument);
        const std::lock_guard joinGuard(joinMutex);
        {
            const std::lock_guard guard(mutex);
            if (started || stopping) return Fail(ErrorCode::Closed);
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
            { const std::lock_guard guard(mutex); ready = true; }
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

    Core::Result<TaskPtr> Submit(std::uint64_t key, KeyedExecutor::Task job,
        KeyedExecutor::Completion completion, const TaskOptions& configuration)
    {
        using Result = Core::Result<TaskPtr>;
        if (!job) return Result::FromStatus(Fail(ErrorCode::InvalidArgument));
        {
            const std::lock_guard guard(mutex);
            auto admitted = Admission(key, configuration.retainedBytes);
            if (!admitted.IsOk()) return Result::FromStatus(std::move(admitted));
        }
        // All callback moves and temporary owner destruction occur outside the
        // scheduler lock. Prebuild list nodes so admission commits cannot fail.
        TaskPtr task;
        try
        {
            task = std::make_shared<KeyedTaskState>(key, std::move(job),
                std::move(completion), configuration);
            task->owner = shared_from_this();
            if (configuration.parentToken.stop_possible())
                task->parentRegistration = std::make_unique<std::stop_callback<KeyedParentCancellation>>(
                    configuration.parentToken, KeyedParentCancellation{task});
            auto preparedKey = std::make_unique<Key>();
            std::list<TaskPtr> globalNode{task}, keyNode{task};
            {
                const std::lock_guard guard(mutex);
                auto admitted = Admission(key, configuration.retainedBytes);
                if (!admitted.IsOk()) return Result::FromStatus(std::move(admitted));
                auto found = keys.find(key);
                if (found == keys.end())
                    found = keys.emplace(key, std::move(preparedKey)).first;
                auto& slot = *found->second;
                ObserveCancellation(*task, KeyedClock::now());
                task->admitted = KeyedClock::now();
                tasks.splice(tasks.end(), globalNode);
                slot.queued.splice(slot.queued.end(), keyNode);
                ++slot.outstanding;
                slot.bytes += configuration.retainedBytes;
                ++outstanding;
                ++pending;
                retained += configuration.retainedBytes;
                Observability::Detail::Add(metrics.acceptedTasks);
                Enqueue(slot);
            }
        }
        catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
        wake.notify_all();
        return Result::FromValue(std::move(task));
    }

    bool Cancel(const TaskPtr& task) noexcept
    {
        {
            const std::lock_guard guard(mutex);
            if (task->phase == KeyedPhase::Completing || task->phase == KeyedPhase::Terminal ||
                task->cancelReason != ErrorCode::Ok) return false;
            task->cancelReason = task->options.deadline && KeyedClock::now() >= *task->options.deadline
                ? ErrorCode::Timeout : ErrorCode::Cancelled;
        }
        wake.notify_all();
        return true;
    }
    void RequestStop() noexcept
    {
        {
            const std::lock_guard guard(mutex);
            stopping = true;
            for (const auto& task : tasks)
            {
                if (task->phase == KeyedPhase::Completing) continue;
                ObserveCancellation(*task, KeyedClock::now());
                if (task->cancelReason == ErrorCode::Ok) task->cancelReason = ErrorCode::Cancelled;
            }
        }
        wake.notify_all();
    }
    Status Stop()
    {
        if (KeyedScope::Contains(this)) return Fail(ErrorCode::InvalidArgument);
        const std::lock_guard joinGuard(joinMutex);
        RequestStop();
        Join();
        return Status::Ok();
    }
    bool IsStopRequested() const noexcept { const std::lock_guard guard(mutex); return stopping; }
    std::size_t KeyCount() const noexcept { const std::lock_guard guard(mutex); return keys.size(); }
    std::size_t OutstandingCount() const noexcept { const std::lock_guard guard(mutex); return outstanding; }
    std::size_t OutstandingCount(std::uint64_t key) const noexcept
    {
        const std::lock_guard guard(mutex);
        const auto found = keys.find(key);
        return found == keys.end() ? 0 : found->second->outstanding;
    }
    std::size_t RetainedBytes(std::uint64_t key) const noexcept
    {
        const std::lock_guard guard(mutex);
        const auto found = keys.find(key);
        return found == keys.end() ? 0 : found->second->bytes;
    }
    void Rejected() noexcept { const std::lock_guard guard(mutex); Observability::Detail::Add(metrics.rejectedTasks); }
    Observability::TaskExecutorMetricsSnapshot GetMetrics() const noexcept
    {
        const std::lock_guard guard(mutex);
        auto result = metrics;
        result.pendingTasks = pending;
        result.runningTasks = running;
        result.retainedBytes = retained;
        return result;
    }

private:
    static Status Fail(ErrorCode code) noexcept { return Status::FailWithoutMessage(code); }
    Status Admission(std::uint64_t key, std::size_t bytes) const noexcept
    {
        if (bytes > options.maxRetainedBytes || bytes > options.maxRetainedBytesPerKey)
            return Fail(ErrorCode::TooLarge);
        if (!ready || stopping) return Fail(ErrorCode::Closed);
        if (outstanding >= options.maxOutstandingTasks || bytes > options.maxRetainedBytes - retained)
            return Fail(ErrorCode::WouldBlock);
        const auto found = keys.find(key);
        if (found == keys.end())
            return keys.size() >= options.maxKeys ? Fail(ErrorCode::WouldBlock) : Status::Ok();
        const auto& slot = *found->second;
        if (slot.outstanding >= options.maxOutstandingTasksPerKey ||
            bytes > options.maxRetainedBytesPerKey - slot.bytes) return Fail(ErrorCode::WouldBlock);
        return Status::Ok();
    }
    static void ObserveCancellation(KeyedTaskState& task, KeyedClock::time_point now) noexcept
    {
        if (task.cancelReason != ErrorCode::Ok) return;
        if (task.options.deadline && now >= *task.options.deadline) task.cancelReason = ErrorCode::Timeout;
        else if (task.options.parentToken.stop_requested()) task.cancelReason = ErrorCode::Cancelled;
    }
    void Enqueue(Key& key) noexcept
    {
        if (key.ready || key.busy || key.queued.empty()) return;
        key.ready = true;
        key.previous = readyTail;
        key.next = nullptr;
        if (readyTail) readyTail->next = &key;
        else readyHead = &key;
        readyTail = &key;
    }
    void RemoveReady(Key& key) noexcept
    {
        if (!key.ready) return;
        if (key.previous) key.previous->next = key.next;
        else readyHead = key.next;
        if (key.next) key.next->previous = key.previous;
        else readyTail = key.previous;
        key.previous = key.next = nullptr;
        key.ready = false;
    }
    void Join()
    {
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        if (coordinator.joinable()) coordinator.join();
        workers.clear();
    }
    void Publish(const TaskPtr& task, Status result, bool occupiedKey)
    {
        const auto terminalCode = result.Code();
        task->job = {};
        task->parentRegistration.reset();
        if (task->completion)
        {
            try { task->completion(result); } catch (...) {}
            task->completion = {};
        }
        {
            const std::lock_guard guard(mutex);
            auto found = keys.find(task->key);
            auto& key = *found->second;
            --key.outstanding;
            key.bytes -= task->options.retainedBytes;
            --outstanding;
            retained -= task->options.retainedBytes;
            if (occupiedKey) { --running; key.busy = false; }
            task->phase = KeyedPhase::Terminal;
            tasks.remove(task);
            Observability::Detail::Add(metrics.completedTasks);
            if (result.Code() == ErrorCode::Cancelled) Observability::Detail::Add(metrics.cancelledTasks);
            else if (result.Code() == ErrorCode::Timeout) Observability::Detail::Add(metrics.timedOutTasks);
            else if (!result.IsOk()) Observability::Detail::Add(metrics.failedTasks);
            const auto elapsed = Observability::Detail::Elapsed(task->admitted);
            Observability::Detail::Add(metrics.totalLatencyNanoseconds, elapsed);
            Observability::Detail::ObserveLatency(metrics.latencyHistogram, elapsed);
            if (elapsed > metrics.maxLatencyNanoseconds) metrics.maxLatencyNanoseconds = elapsed;
            if (!key.outstanding) { RemoveReady(key); keys.erase(found); }
            else Enqueue(key);
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
        const KeyedScope scope(this);
        for (;;)
        {
            TaskPtr task;
            {
                std::unique_lock guard(mutex);
                for (;;)
                {
                    if (stopping) return;
                    if (ready && readyHead)
                    {
                        auto& key = *readyHead;
                        RemoveReady(key);
                        task = key.queued.front();
                        ObserveCancellation(*task, KeyedClock::now());
                        if (task->cancelReason != ErrorCode::Ok)
                        {
                            task.reset();
                            wake.notify_all();
                            continue;
                        }
                        key.queued.pop_front();
                        key.busy = true;
                        task->phase = KeyedPhase::Running;
                        --pending;
                        ++running;
                        break;
                    }
                    wake.wait(guard);
                }
            }
            Status result = Status::Ok();
            try { result = task->job(task->cancellation.get_token()); }
            catch (...) { result = Fail(ErrorCode::PlatformError); }
            ErrorCode reason;
            {
                const std::lock_guard guard(mutex);
                ObserveCancellation(*task, KeyedClock::now());
                reason = task->cancelReason;
                task->phase = KeyedPhase::Completing;
            }
            if (reason != ErrorCode::Ok)
            {
                (void)task->cancellation.request_stop();
                result = Fail(reason);
            }
            Publish(task, std::move(result), true);
        }
    }
    void Coordinate()
    {
        const KeyedScope scope(this);
        std::unique_lock guard(mutex);
        for (;;)
        {
            TaskPtr selected;
            bool queued = false;
            std::optional<KeyedClock::time_point> nextDeadline;
            const auto now = KeyedClock::now();
            for (const auto& task : tasks)
            {
                if (task->phase == KeyedPhase::Completing) continue;
                ObserveCancellation(*task, now);
                if (task->cancelReason != ErrorCode::Ok && !task->signalSent)
                {
                    selected = task;
                    task->signalSent = true;
                    queued = task->phase == KeyedPhase::Queued;
                    if (queued)
                    {
                        task->phase = KeyedPhase::Completing;
                        --pending;
                        auto& key = *keys.find(task->key)->second;
                        key.queued.remove(task);
                        if (key.queued.empty()) RemoveReady(key);
                        else Enqueue(key);
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
                guard.unlock();
                (void)selected->cancellation.request_stop();
                if (queued) Publish(selected, Fail(reason), false);
                guard.lock();
                continue;
            }
            if (stopping && tasks.empty()) return;
            if (nextDeadline && *nextDeadline != KeyedClock::time_point::max()) wake.wait_until(guard, *nextDeadline);
            else wake.wait(guard);
        }
    }
    mutable std::mutex mutex;
    std::mutex joinMutex;
    std::condition_variable wake;
    KeyedExecutorOptions options;
    std::unordered_map<std::uint64_t, std::unique_ptr<Key>> keys;
    std::list<TaskPtr> tasks;
    Key* readyHead = nullptr;
    Key* readyTail = nullptr;
    std::vector<std::thread> workers;
    std::thread coordinator;
    bool started = false, ready = false, stopping = false;
    std::size_t pending = 0, running = 0, outstanding = 0, retained = 0;
    Observability::TaskExecutorMetricsSnapshot metrics;
};

void KeyedParentCancellation::operator()() const noexcept
{
    if (const auto value = task.lock())
        if (const auto owner = value->owner.lock()) (void)owner->Cancel(value);
}
Status ReadKeyedResult(const std::shared_ptr<KeyedTaskState>& task)
{
    try { return task->result; }
    catch (...) { return Status::AllocationFailure(); }
}
}

KeyedTaskHandle::KeyedTaskHandle(std::shared_ptr<Detail::KeyedTaskState> state) noexcept : mState(std::move(state)) {}
bool KeyedTaskHandle::IsValid() const noexcept { return mState != nullptr; }
std::stop_token KeyedTaskHandle::GetToken() const noexcept
{ return mState ? mState->cancellation.get_token() : std::stop_token{}; }
bool KeyedTaskHandle::RequestCancel() const noexcept
{
    const auto state = mState;
    if (!state) return false;
    const auto owner = state->owner.lock();
    if (!owner || !owner->Cancel(state)) return false;
    const Detail::KeyedScope scope(owner.get());
    (void)state->cancellation.request_stop();
    return true;
}
bool KeyedTaskHandle::IsFinished() const noexcept
{
    if (!mState) return false;
    const std::lock_guard guard(mState->resultMutex);
    return mState->terminal;
}
Core::Status KeyedTaskHandle::GetStatus() const
{
    if (!mState) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    const std::lock_guard guard(mState->resultMutex);
    return Detail::ReadKeyedResult(mState);
}
Core::Status KeyedTaskHandle::Wait() const
{ return WaitUntil(std::chrono::steady_clock::time_point::max()); }
Core::Result<Core::CompletionSubscription> KeyedTaskHandle::WaitForCompletion(
    std::function<void(Core::Status)> callback, std::stop_token cancellation) const
{
    if (!mState) return Core::Result<Core::CompletionSubscription>::FromStatus(
        Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
    return mState->completionSignal.Subscribe(std::move(callback), cancellation);
}
Core::Status KeyedTaskHandle::WaitUntil(std::chrono::steady_clock::time_point deadline) const
{
    const auto state = mState;
    if (!state) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    std::unique_lock guard(state->resultMutex);
    if (!state->terminal)
    {
        const auto owner = state->owner.lock();
        if (owner && Detail::KeyedScope::Contains(owner.get()))
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        if (deadline == std::chrono::steady_clock::time_point::max())
            state->finished.wait(guard, [&] { return state->terminal; });
        else if (!state->finished.wait_until(guard, deadline, [&] { return state->terminal; }))
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout);
    }
    return Detail::ReadKeyedResult(state);
}
KeyedExecutor::KeyedExecutor() : mState(std::make_shared<Detail::KeyedExecutorState>()) {}
KeyedExecutor::~KeyedExecutor()
{
    SERVERCORE_ASSERT(!Detail::KeyedScope::Contains(mState.get()),
        "KeyedExecutor must be destroyed outside its threads/cancellation callbacks");
    (void)mState->Stop();
}
Core::Status KeyedExecutor::Start(const KeyedExecutorOptions& options) { return mState->Start(options); }
Core::Result<KeyedTaskHandle> KeyedExecutor::Submit(std::uint64_t key, Task task, const TaskOptions& options)
{ return SubmitWithCompletion(key, std::move(task), {}, options); }
Core::Result<KeyedTaskHandle> KeyedExecutor::SubmitWithCompletion(std::uint64_t key, Task task,
    Completion completion, const TaskOptions& options)
{
    auto submitted = mState->Submit(key, std::move(task), std::move(completion), options);
    if (!submitted.IsOk())
    {
        mState->Rejected();
        return Core::Result<KeyedTaskHandle>::FromStatus(std::move(submitted).TakeStatus());
    }
    return Core::Result<KeyedTaskHandle>::FromValue(KeyedTaskHandle(std::move(submitted.Value())));
}
void KeyedExecutor::RequestStop() noexcept { mState->RequestStop(); }
Core::Status KeyedExecutor::Stop() { return mState->Stop(); }
bool KeyedExecutor::IsStopRequested() const noexcept { return mState->IsStopRequested(); }
std::size_t KeyedExecutor::KeyCount() const noexcept { return mState->KeyCount(); }
std::size_t KeyedExecutor::PendingCount() const noexcept { return mState->GetMetrics().pendingTasks; }
std::size_t KeyedExecutor::RunningCount() const noexcept { return mState->GetMetrics().runningTasks; }
std::size_t KeyedExecutor::OutstandingCount() const noexcept { return mState->OutstandingCount(); }
std::size_t KeyedExecutor::OutstandingCount(std::uint64_t key) const noexcept { return mState->OutstandingCount(key); }
std::size_t KeyedExecutor::RetainedBytes() const noexcept { return mState->GetMetrics().retainedBytes; }
std::size_t KeyedExecutor::RetainedBytes(std::uint64_t key) const noexcept { return mState->RetainedBytes(key); }
Observability::TaskExecutorMetricsSnapshot KeyedExecutor::GetMetrics() const noexcept { return mState->GetMetrics(); }
}
