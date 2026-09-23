#include "TestHarness.h"
#include "ServerCore/Runtime/TaskExecutor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCore::Runtime::TaskExecutor;
using ServerCore::Runtime::TaskExecutorOptions;
using ServerCore::Runtime::TaskHandle;
using ServerCore::Runtime::TaskOptions;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using Clock = std::chrono::steady_clock;

// Even a failed assertion must not leave the suite waiting on a blocked worker.
class Gate
{
public:
    void Enter(std::stop_token token = {})
    {
        std::unique_lock guard(mMutex);
        ++mEntered;
        mWake.notify_all();
        (void)mWake.wait_for(guard, token, 5s, [this] { return mOpen; });
    }

    bool WaitForEntered(std::size_t count = 1)
    {
        std::unique_lock guard(mMutex);
        return mWake.wait_for(guard, 2s, [this, count] { return mEntered >= count; });
    }

    bool WaitForOpen()
    {
        std::unique_lock guard(mMutex);
        return mWake.wait_for(guard, 2s, [this] { return mOpen; });
    }

    void Open()
    {
        { const std::lock_guard guard(mMutex); mOpen = true; }
        mWake.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable_any mWake;
    std::size_t mEntered = 0;
    bool mOpen = false;
};

bool WaitForStop(std::stop_token token)
{
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock guard(mutex);
    (void)wake.wait_for(guard, token, 2s, [] { return false; });
    return token.stop_requested();
}

TaskHandle Accepted(TaskExecutor& executor, TaskExecutor::Task task, const TaskOptions& options = {})
{
    auto result = executor.Submit(std::move(task), options);
    ExpectTrue(result.IsOk(), "task is admitted");
    return result.IsOk() ? std::move(result.Value()) : TaskHandle{};
}

void ExpectCode(ErrorCode expected, const Status& actual, const char* description)
{
    ExpectTrue(expected == actual.Code(), description);
}

void TaskExecutorBoundedAdmission()
{
    TaskExecutor executor;
    const auto beforeStart = executor.Submit([](std::stop_token) { return Status::Ok(); });
    ExpectCode(ErrorCode::Closed, beforeStart.GetStatus(), "inactive executor rejects submissions");
    ExpectCode(ErrorCode::InvalidArgument, executor.Start({0, 2, 10}), "zero workers are invalid");
    ExpectCode(ErrorCode::InvalidArgument, executor.Start({2, 0, 10}), "zero pending capacity is invalid");
    ExpectCode(ErrorCode::InvalidArgument, executor.Start({2, 2, 0}), "zero byte budget is invalid");
    ExpectTrue(executor.Start({2, 2, 10}).IsOk(), "bounded executor starts after invalid configuration");
    ExpectCode(ErrorCode::Closed, executor.Start(), "executor cannot start twice");
    ExpectCode(ErrorCode::InvalidArgument, executor.Submit({}).GetStatus(), "empty function is invalid");
    ExpectCode(ErrorCode::TooLarge,
        executor.Submit([](std::stop_token) { return Status::Ok(); }, TaskOptions{11}).GetStatus(),
        "a task exceeding total budget is TooLarge");
    Gate gate;
    const auto block = [&gate](std::stop_token) { gate.Enter(); return Status::Ok(); };
    const auto first = Accepted(executor, block, TaskOptions{2});
    const auto second = Accepted(executor, block, TaskOptions{2});
    ExpectTrue(gate.WaitForEntered(2), "exactly two workers execute independently");
    ExpectEqual(std::size_t{2}, executor.RunningCount(), "running count tracks occupied workers");
    const auto queued = Accepted(executor, [](std::stop_token) { return Status::Ok(); }, TaskOptions{6});
    auto rejectedCapture = std::make_shared<int>(1);
    const std::weak_ptr<int> rejectedLifetime = rejectedCapture;
    const auto bytesFull = executor.Submit(
        [capture = std::move(rejectedCapture)](std::stop_token) { (void)capture; return Status::Ok(); },
        TaskOptions{1});
    ExpectCode(ErrorCode::WouldBlock, bytesFull.GetStatus(), "queued plus running bytes bound admission");
    ExpectTrue(rejectedLifetime.expired(), "rejected submission releases captured state");
    const auto zeroBytes = Accepted(executor, [](std::stop_token) { return Status::Ok(); });
    ExpectCode(ErrorCode::WouldBlock, executor.Submit([](std::stop_token) { return Status::Ok(); }).GetStatus(),
        "pending count also bounds zero-byte tasks");
    ExpectEqual(std::size_t{2}, executor.PendingCount(), "only waiting tasks consume pending slots");
    ExpectEqual(std::size_t{10}, executor.RetainedBytes(), "rejection preserves exact retained-byte total");
    ExpectCode(ErrorCode::Timeout, first.WaitUntil(Clock::now() + 20ms), "wait timeout bounds only observation");
    ExpectCode(ErrorCode::WouldBlock, first.GetStatus(), "wait timeout leaves task unfinished");
    ExpectTrue(!first.GetToken().stop_requested(), "wait timeout does not request task cancellation");
    gate.Open();
    for (const auto& handle : {first, second, queued, zeroBytes})
        ExpectTrue(handle.WaitUntil(Clock::now() + 2s).IsOk(), "admitted tasks complete successfully");
    ExpectEqual(std::size_t{0}, executor.RetainedBytes(), "completion returns retained-byte capacity");
    ExpectTrue(executor.Stop().IsOk(), "bounded executor joins");
    TaskHandle invalid;
    ExpectTrue(!invalid.IsValid() && !invalid.IsFinished() && !invalid.RequestCancel(),
        "default handle has no task");
    ExpectCode(ErrorCode::InvalidArgument, invalid.GetStatus(), "invalid handle status is explicit");
    ExpectCode(ErrorCode::InvalidArgument, invalid.Wait(), "invalid handle cannot wait");
}

void TaskExecutorQueuedCancellation()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({1, 1, 8}).IsOk(), "cancellation executor starts");
    Gate gate;
    const auto running = Accepted(executor, [&gate](std::stop_token) { gate.Enter(); return Status::Ok(); }, TaskOptions{4});
    ExpectTrue(gate.WaitForEntered(), "worker is occupied");
    auto captured = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = captured;
    std::atomic<unsigned int> executions{0};
    const auto queued = Accepted(executor,
        [value = std::move(captured), &executions](std::stop_token) {
            (void)value;
            ++executions;
            return Status::Ok();
        }, TaskOptions{4});
    ExpectTrue(queued.RequestCancel(), "first queued cancellation is accepted");
    ExpectCode(ErrorCode::Cancelled, queued.WaitUntil(Clock::now() + 2s), "queued task cancels while worker stays busy");
    ExpectTrue(lifetime.expired(), "queued cancellation promptly releases captured resources");
    ExpectEqual(std::size_t{0}, executor.PendingCount(), "queued cancellation returns pending slot");
    ExpectEqual(std::size_t{4}, executor.RetainedBytes(), "only the running task remains charged");
    ExpectTrue(!queued.RequestCancel(), "late repeated cancellation is a no-op");
    std::stop_source parent;
    TaskOptions options{4};
    options.parentToken = parent.get_token();
    const auto replacement = Accepted(executor, [&executions](std::stop_token) { ++executions; return Status::Ok(); }, options);
    (void)parent.request_stop();
    ExpectCode(ErrorCode::Cancelled, replacement.WaitUntil(Clock::now() + 2s),
        "parent cancellation removes queued replacement without a free worker");
    ExpectTrue(replacement.GetToken().stop_requested(), "parent cancellation reaches the task token");
    const auto preCancelled = Accepted(executor, [&executions](std::stop_token) { ++executions; return Status::Ok(); }, options);
    ExpectCode(ErrorCode::Cancelled, preCancelled.WaitUntil(Clock::now() + 2s), "already cancelled parent skips execution");
    ExpectEqual(0u, executions.load(), "none of the cancelled queued functions execute");
    gate.Open();
    ExpectTrue(running.WaitUntil(Clock::now() + 2s).IsOk(), "unrelated running task completes");
    ExpectTrue(executor.Stop().IsOk(), "cancellation executor joins");
}

void TaskExecutorDeadlinesAndExceptions()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({1, 2, 10}).IsOk(), "deadline executor starts");
    Gate gate;
    TaskOptions options{5};
    options.deadline = Clock::now() + 1s;
    const auto running = Accepted(executor, [&gate](std::stop_token) { gate.Enter(); return Status::Ok(); }, options);
    ExpectTrue(gate.WaitForEntered(), "deadline task starts before expiration");
    TaskOptions queuedOptions{3};
    queuedOptions.deadline = Clock::now() + 50ms;
    std::atomic<bool> queuedRan{false};
    const auto queued = Accepted(executor, [&queuedRan](std::stop_token) { queuedRan = true; return Status::Ok(); }, queuedOptions);
    ExpectCode(ErrorCode::Timeout, queued.WaitUntil(Clock::now() + 2s), "queued deadline does not need a free worker");
    ExpectTrue(!queuedRan.load() && queued.GetToken().stop_requested(), "expired queued job is skipped and token stops");
    ExpectTrue(WaitForStop(running.GetToken()), "running deadline triggers cooperative stop token");
    ExpectCode(ErrorCode::WouldBlock, running.GetStatus(), "deadline does not claim a noncooperative job finished");
    ExpectEqual(std::size_t{5}, executor.RetainedBytes(), "running deadline retains its memory budget");
    ExpectTrue(!running.RequestCancel(), "manual cancellation cannot replace deadline outcome");
    gate.Open();
    ExpectCode(ErrorCode::Timeout, running.WaitUntil(Clock::now() + 2s), "running deadline publishes Timeout after function returns");
    std::stop_source parent;
    TaskOptions parentOptions{2};
    parentOptions.parentToken = parent.get_token();
    Gate cooperative;
    const auto parentTask = Accepted(executor, [&cooperative](std::stop_token token) {
        cooperative.Enter(token);
        return Status::FailWithoutMessage(ErrorCode::InvalidFormat);
    }, parentOptions);
    ExpectTrue(cooperative.WaitForEntered(), "parent cancellation task starts");
    (void)parent.request_stop();
    ExpectCode(ErrorCode::Cancelled, parentTask.WaitUntil(Clock::now() + 2s),
        "observed cancellation wins over running task return status");
    const auto throwing = Accepted(executor, [](std::stop_token) -> Status { throw std::runtime_error("task failure"); });
    ExpectCode(ErrorCode::PlatformError, throwing.WaitUntil(Clock::now() + 2s), "job exceptions become PlatformError");
    const auto preserved = Accepted(executor, [](std::stop_token) {
        return Status::Fail(ErrorCode::InvalidFormat, "application status");
    });
    const auto result = preserved.WaitUntil(Clock::now() + 2s);
    ExpectCode(ErrorCode::InvalidFormat, result, "worker survives an exception and retains application failure code");
    ExpectEqual(std::string("application status"), result.Message(), "application status message is preserved");
    ExpectTrue(executor.Stop().IsOk(), "deadline executor joins");
}

void TaskExecutorNestedCancellation()
{
    Gate targetWorker;
    Gate targetCoordinator;
    Gate sourceWorker;
    TaskExecutor source;
    TaskExecutor target;
    ExpectTrue(source.Start({1, 1, 1}).IsOk(), "nested cancellation source starts");
    ExpectTrue(target.Start({1, 2, 1}).IsOk(), "nested cancellation target starts");
    const auto running = Accepted(target, [&targetWorker](std::stop_token) {
        targetWorker.Enter();
        return Status::Ok();
    });
    ExpectTrue(targetWorker.WaitForEntered(), "target worker remains occupied during nested cancellation");

    // Hold the target coordinator in a separate, bounded callback so the source
    // worker deterministically executes the next cancellation callback inline.
    std::stop_source parent;
    TaskOptions options;
    options.parentToken = parent.get_token();
    const auto coordinatorTask = Accepted(target, [](std::stop_token) { return Status::Ok(); }, options);
    std::stop_callback holdCoordinator(coordinatorTask.GetToken(), [&targetCoordinator] { targetCoordinator.Enter(); });
    (void)parent.request_stop();
    ExpectTrue(targetCoordinator.WaitForEntered(), "target coordinator enters controlled cancellation callback");
    const auto nested = Accepted(target, [](std::stop_token) { return Status::Ok(); });
    TaskHandle own;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> sourceWaitRejected{false};
    std::atomic<bool> targetWaitRejected{false};
    std::atomic<bool> sourceContextRestored{false};
    std::stop_callback onNestedCancel(nested.GetToken(), [&] {
        sourceWaitRejected = own.WaitUntil(Clock::now() + 50ms).Code() == ErrorCode::InvalidArgument;
        targetWaitRejected = nested.WaitUntil(Clock::now() + 50ms).Code() == ErrorCode::InvalidArgument;
    });
    own = Accepted(source, [&](std::stop_token) {
        sourceWorker.Enter();
        cancelled = nested.RequestCancel();
        sourceContextRestored = own.WaitUntil(Clock::now() + 50ms).Code() == ErrorCode::InvalidArgument;
        return Status::Ok();
    });
    ExpectTrue(sourceWorker.WaitForEntered(), "source worker starts before its handle is used");
    sourceWorker.Open();
    ExpectTrue(own.WaitUntil(Clock::now() + 2s).IsOk(), "nested cancellation returns without blocking its worker");
    ExpectTrue(cancelled.load() && sourceWaitRejected.load() && targetWaitRejected.load(),
        "nested callback rejects waits on both its source worker and target executor");
    ExpectTrue(sourceContextRestored.load(), "leaving nested cancellation restores the source worker context");
    targetCoordinator.Open();
    targetWorker.Open();
    ExpectCode(ErrorCode::Cancelled, coordinatorTask.WaitUntil(Clock::now() + 2s), "coordinator task completes cancellation");
    ExpectCode(ErrorCode::Cancelled, nested.WaitUntil(Clock::now() + 2s), "nested task completes cancellation");
    ExpectTrue(running.WaitUntil(Clock::now() + 2s).IsOk(), "target worker is released");
    ExpectTrue(source.Stop().IsOk() && target.Stop().IsOk(), "both nested cancellation executors join");
}

void TaskExecutorShutdownAndReentrancy()
{
    TaskHandle retainedHandle;
    {
        TaskExecutor executor;
        ExpectTrue(executor.Start({1, 1, 8}).IsOk(), "shutdown executor starts");
        Gate gate;
        TaskHandle own;
        std::atomic<bool> rejectedSelfWait{false};
        std::atomic<bool> rejectedSelfStop{false};
        std::atomic<bool> requestReturned{false};
        own = Accepted(executor, [&](std::stop_token) {
            gate.Enter();
            rejectedSelfWait = own.Wait().Code() == ErrorCode::InvalidArgument;
            rejectedSelfStop = executor.Stop().Code() == ErrorCode::InvalidArgument;
            executor.RequestStop();
            requestReturned = true;
            return Status::Ok();
        }, TaskOptions{4});
        ExpectTrue(gate.WaitForEntered(), "shutdown job awaits queued task");
        std::atomic<bool> queuedRan{false};
        const auto queued = Accepted(executor, [&queuedRan](std::stop_token) { queuedRan = true; return Status::Ok(); },
            TaskOptions{4});
        gate.Open();
        ExpectCode(ErrorCode::Cancelled, own.WaitUntil(Clock::now() + 2s), "callback may request executor shutdown");
        ExpectCode(ErrorCode::Cancelled, queued.WaitUntil(Clock::now() + 2s), "shutdown cancels pending job");
        ExpectTrue(rejectedSelfWait.load() && rejectedSelfStop.load() && requestReturned.load(),
            "self-wait and self-join reject while nonblocking stop request returns");
        ExpectTrue(!queuedRan.load() && executor.IsStopRequested(), "shutdown closes admission before queued job executes");
        ExpectCode(ErrorCode::Closed, executor.Submit([](std::stop_token) { return Status::Ok(); }).GetStatus(),
            "stopped executor rejects new tasks");
        ExpectCode(ErrorCode::InvalidArgument, executor.Submit({}).GetStatus(), "invalid function precedes stopped state");
        ExpectCode(ErrorCode::TooLarge,
            executor.Submit([](std::stop_token) { return Status::Ok(); }, TaskOptions{9}).GetStatus(),
            "oversize task precedes stopped state");
        ExpectTrue(executor.Stop().IsOk() && executor.Stop().IsOk(), "external stop joins idempotently");
        ExpectEqual(std::size_t{0}, executor.RetainedBytes(), "joined executor releases all byte capacity");
        retainedHandle = own;
    }
    ExpectCode(ErrorCode::Cancelled, retainedHandle.Wait(), "terminal handle survives executor destruction");
    ExpectTrue(!retainedHandle.RequestCancel(), "handle cannot cancel destroyed executor");
    TaskExecutor noncooperative;
    ExpectTrue(noncooperative.Start({1, 1, 1}).IsOk(), "noncooperative executor starts");
    Gate gate;
    const auto running = Accepted(noncooperative, [&gate](std::stop_token) { gate.Enter(); return Status::Ok(); }, TaskOptions{1});
    ExpectTrue(gate.WaitForEntered(), "noncooperative task is running");
    std::atomic<bool> cancellationJoinRejected{false};
    Gate cancellationFinished;
    std::stop_callback onCancel(running.GetToken(), [&] {
        cancellationJoinRejected = noncooperative.Stop().Code() == ErrorCode::InvalidArgument;
        cancellationFinished.Open();
    });
    ExpectTrue(running.RequestCancel(), "manual running cancellation is accepted");
    ExpectTrue(cancellationFinished.WaitForOpen(), "cancellation callback completes");
    ExpectTrue(cancellationJoinRejected.load(), "external stop callback cannot join its own executor");
    noncooperative.RequestStop();
    ExpectCode(ErrorCode::WouldBlock, running.GetStatus(), "RequestStop returns before noncooperative task completes");
    gate.Open();
    ExpectTrue(noncooperative.Stop().IsOk(), "external Stop waits for cooperative release");
    ExpectCode(ErrorCode::Cancelled, running.GetStatus(), "shutdown outcome wins over late success");
    TaskExecutorNestedCancellation();
}

void TaskExecutorCompletionSubscriptions()
{
    auto gate = std::make_shared<Gate>();
    auto capture = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = capture;
    std::atomic<unsigned> completed{0}, cancelled{0}, dropped{0};
    std::atomic<bool> observedCleanup{true};
    TaskExecutor executor;
    ExpectTrue(executor.Start({1, 1, 8}).IsOk(), "completion subscription executor starts");
    const auto task = Accepted(executor, [gate, capture](std::stop_token token) {
        (void)capture;
        gate->Enter(token);
        return Status::Ok();
    });
    capture.reset();
    ExpectTrue(gate->WaitForEntered(), "completion subscriptions register before terminal publication");
    std::vector<ServerCore::Core::CompletionSubscription> subscriptions;
    const auto callback = [&, task](Status status) {
        if (status.Code() == ErrorCode::Cancelled) { ++cancelled; return; }
        if (!status.IsOk() || !lifetime.expired() || !task.IsFinished() || !task.GetStatus().IsOk())
            observedCleanup = false;
        ++completed;
    };
    for (unsigned index = 0; index < 16; ++index)
    {
        auto registered = task.WaitForCompletion(callback);
        ExpectTrue(registered.IsOk(), "bounded terminal observer is accepted");
        if (registered.IsOk()) subscriptions.push_back(std::move(registered.Value()));
    }
    ExpectCode(ErrorCode::WouldBlock, task.WaitForCompletion(callback).GetStatus(),
        "task bounds simultaneous terminal observers");
    if (!subscriptions.empty())
        ExpectTrue(subscriptions.front().Cancel(), "cancelling an observer completes it independently of its task");
    if (subscriptions.size() > 1) subscriptions[1].Reset();
    auto discarded = task.WaitForCompletion([&](Status) { ++dropped; });
    ExpectTrue(discarded.IsOk(), "cancelled and reset observers return registration capacity");
    if (discarded.IsOk()) discarded.Value().Reset();
    auto replacement = task.WaitForCompletion(callback);
    ExpectTrue(replacement.IsOk(), "a reset registration can be replaced before completion");
    ExpectTrue(completed == 0 && cancelled == 1 && dropped == 0,
        "observer cancellation does not finish the running task or invoke a dropped callback");
    gate->Open();
    ExpectTrue(task.WaitUntil(Clock::now() + 2s).IsOk(), "observed task completes successfully");
    ExpectTrue(executor.Stop().IsOk(), "worker joins after terminal notification delivery");
    ExpectTrue(completed == 15 && cancelled == 1 && dropped == 0 && observedCleanup,
        "terminal observers run once after capture cleanup and may query terminal status");
    auto immediate = task.WaitForCompletion(callback);
    ExpectTrue(immediate.IsOk() && completed == 16 && observedCleanup,
        "an observer registered after completion runs immediately with the stable outcome");
    if (immediate.IsOk()) ExpectTrue(!immediate.Value().Cancel(), "completed observer ignores late cancellation");
}

void TaskExecutorCompletionRace()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({1, 1, 1}).IsOk(), "race executor starts");
    for (unsigned int iteration = 0; iteration < 32; ++iteration)
    {
        Gate gate;
        const auto handle = Accepted(executor, [&gate](std::stop_token) { gate.Enter(); return Status::Ok(); }, TaskOptions{1});
        ExpectTrue(gate.WaitForEntered(), "racing task starts");
        gate.Open();
        const bool cancelled = handle.RequestCancel();
        const auto result = handle.WaitUntil(Clock::now() + 2s);
        ExpectTrue(result.Code() == ErrorCode::Ok || result.Code() == ErrorCode::Cancelled,
            "completion and cancellation choose one terminal outcome");
        ExpectTrue(cancelled == (result.Code() == ErrorCode::Cancelled), "accepted cancellation determines terminal status");
        ExpectTrue(handle.GetToken().stop_requested() == cancelled, "successful completion cannot acquire late cancellation");
        ExpectTrue(!handle.RequestCancel(), "terminal cancellation is a no-op");
        ExpectCode(result.Code(), handle.GetStatus(), "terminal status remains stable");
        ExpectEqual(std::size_t{0}, executor.RetainedBytes(), "every race returns byte capacity exactly once");
    }
    ExpectTrue(executor.Stop().IsOk(), "race executor joins");
    TaskExecutorCompletionSubscriptions();
}

void TaskExecutorMoveOnlyOwnership()
{
    struct ReentrantState
    {
        TaskExecutor* executor = nullptr;
        std::atomic<int> moves{0};
        std::atomic<int> destructions{0};
        std::atomic<int> calls{0};
    } reentrantState;
    // A pointer-sized noexcept-movable target exercises implementations that
    // keep move_only_function targets inline. Even moved-from destructors can
    // run application code and must be able to query the executor safely.
    struct ReentrantTask
    {
        explicit ReentrantTask(ReentrantState* value) : state(value) {}
        ReentrantTask(ReentrantTask&& other) noexcept : state(other.state)
        {
            (void)state->executor->RetainedBytes();
            ++state->moves;
        }
        ~ReentrantTask()
        {
            (void)state->executor->PendingCount();
            ++state->destructions;
        }
        Status operator()(std::stop_token)
        {
            ++state->calls;
            return Status::Ok();
        }
        ReentrantState* state;
    };
    struct Capture
    {
        explicit Capture(std::atomic<int>& count) : released(count) {}
        ~Capture() { ++released; }
        std::atomic<int>& released;
    };
    std::atomic<int> tasksReleased{0};
    std::atomic<int> completionsReleased{0};
    std::atomic<int> tasksCalled{0};
    std::atomic<int> completionsCalled{0};
    std::atomic<bool> completionAfterTaskRelease{false};
    Gate gate;
    TaskExecutor executor;
    reentrantState.executor = &executor;
    ExpectTrue(executor.Start({1, 2, 16}).IsOk(), "move-only executor starts");

    auto completed = executor.SubmitWithCompletion(
        [capture = std::make_unique<Capture>(tasksReleased), &tasksCalled](std::stop_token) {
            (void)capture;
            ++tasksCalled;
            return Status::Ok();
        },
        [capture = std::make_unique<Capture>(completionsReleased), &tasksReleased,
            &completionsCalled, &completionAfterTaskRelease](const Status& status) {
            (void)capture;
            completionAfterTaskRelease = status.IsOk() && tasksReleased.load() == 1;
            ++completionsCalled;
        }, TaskOptions{4});
    ExpectTrue(completed.IsOk(), "unique-owned task and completion are admitted");
    if (completed.IsOk())
        ExpectTrue(completed.Value().WaitUntil(Clock::now() + 2s).IsOk(), "unique-owned task completes");
    ExpectTrue(completionAfterTaskRelease.load(), "completion observes released task captures");
    ExpectEqual(1, completionsReleased.load(), "terminal wait observes released completion captures");

    const auto blocked = Accepted(executor, [&gate](std::stop_token token) {
        gate.Enter(token);
        return Status::Ok();
    });
    ExpectTrue(gate.WaitForEntered(), "worker is occupied before move-only queued cancellation");
    auto cancelled = executor.SubmitWithCompletion(
        [capture = std::make_unique<Capture>(tasksReleased), &tasksCalled](std::stop_token) {
            (void)capture;
            ++tasksCalled;
            return Status::Ok();
        },
        [capture = std::make_unique<Capture>(completionsReleased), &tasksReleased,
            &completionsCalled, &completionAfterTaskRelease](const Status& status) {
            (void)capture;
            completionAfterTaskRelease = status.Code() == ErrorCode::Cancelled && tasksReleased.load() == 2;
            ++completionsCalled;
        }, TaskOptions{4});
    ExpectTrue(cancelled.IsOk(), "unique-owned queued task is admitted");
    if (cancelled.IsOk())
    {
        ExpectTrue(cancelled.Value().RequestCancel(), "queued unique-owned task can be cancelled");
        ExpectCode(ErrorCode::Cancelled, cancelled.Value().WaitUntil(Clock::now() + 2s),
            "queued cancellation completes while worker is occupied");
    }
    ExpectTrue(completionAfterTaskRelease.load(), "cancel completion observes released queued task captures");
    ExpectEqual(1, tasksCalled.load(), "cancelled queued task is never invoked");
    ExpectEqual(2, completionsCalled.load(), "accepted task completions each run once");
    ExpectEqual(2, completionsReleased.load(), "cancelled completion releases its unique capture");
    ExpectEqual(std::size_t{0}, executor.RetainedBytes(), "queued cancellation returns retained capacity");

    auto rejected = executor.SubmitWithCompletion(
        [capture = std::make_unique<Capture>(tasksReleased)](std::stop_token) {
            (void)capture;
            return Status::Ok();
        },
        [capture = std::make_unique<Capture>(completionsReleased), &completionsCalled](const Status&) {
            (void)capture;
            ++completionsCalled;
        }, TaskOptions{17});
    ExpectCode(ErrorCode::TooLarge, rejected.GetStatus(), "oversized move-only submission is rejected");
    ExpectEqual(3, tasksReleased.load(), "rejection releases unique task capture synchronously");
    ExpectEqual(3, completionsReleased.load(), "rejection releases unique completion capture synchronously");
    ExpectEqual(2, completionsCalled.load(), "rejection does not invoke completion");
    gate.Open();
    ExpectTrue(blocked.WaitUntil(Clock::now() + 2s).IsOk(), "occupied worker completes");

    std::function<Status(std::stop_token)> legacyTask;
    std::function<void(const Status&)> legacyCompletion;
    ExpectCode(ErrorCode::InvalidArgument, executor.Submit(legacyTask).GetStatus(),
        "empty legacy task retains InvalidArgument behavior");
    ExpectCode(ErrorCode::InvalidArgument, executor.SubmitWithCompletion(legacyTask, {}).GetStatus(),
        "empty legacy task and default completion retain InvalidArgument behavior");
    ExpectCode(ErrorCode::InvalidArgument, executor.SubmitWithCompletion({}, legacyCompletion).GetStatus(),
        "default task and legacy completion retain InvalidArgument behavior");
    legacyTask = [&tasksCalled](std::stop_token) { ++tasksCalled; return Status::Ok(); };
    auto legacy = executor.SubmitWithCompletion(legacyTask,
        [capture = std::make_unique<Capture>(completionsReleased), &completionsCalled](const Status&) {
            (void)capture;
            ++completionsCalled;
        });
    ExpectTrue(legacy.IsOk(), "legacy lvalue task accepts a move-only completion");
    if (legacy.IsOk())
        ExpectTrue(legacy.Value().WaitUntil(Clock::now() + 2s).IsOk(), "mixed legacy task completes");
    auto optionalCompletion = executor.SubmitWithCompletion(
        [capture = std::make_unique<Capture>(tasksReleased)](std::stop_token) {
            (void)capture;
            return Status::Ok();
        }, legacyCompletion);
    ExpectTrue(optionalCompletion.IsOk(), "move-only task accepts an empty legacy optional completion");
    if (optionalCompletion.IsOk())
        ExpectTrue(optionalCompletion.Value().WaitUntil(Clock::now() + 2s).IsOk(),
            "empty legacy completion does not prevent task completion");
    auto reentrant = executor.Submit(ReentrantTask{&reentrantState});
    ExpectTrue(reentrant.IsOk(), "small reentrant move-only task is accepted");
    if (reentrant.IsOk())
        ExpectTrue(reentrant.Value().WaitUntil(Clock::now() + 2s).IsOk(),
            "callback moves and destruction can query the executor without deadlocking");
    ExpectTrue(executor.Stop().IsOk(), "move-only executor stops");
    ExpectEqual(1, reentrantState.calls.load(), "reentrant task runs exactly once");
    ExpectTrue(reentrantState.moves.load() > 0 && reentrantState.destructions.load() > 0,
        "move constructor and destructor both complete their executor queries");
    ExpectEqual(4, tasksReleased.load(), "all owned task captures are released");
    ExpectEqual(4, completionsReleased.load(), "all owned completion captures are released");
}

const ServerCoreTest::CheckRegistration moveOnly("Runtime.TaskExecutorMoveOnlyOwnership", TaskExecutorMoveOnlyOwnership);
const ServerCoreTest::CheckRegistration bounded("Runtime.TaskExecutorBoundedAdmission", TaskExecutorBoundedAdmission);
const ServerCoreTest::CheckRegistration cancellation("Runtime.TaskExecutorQueuedCancellation", TaskExecutorQueuedCancellation);
const ServerCoreTest::CheckRegistration deadlines("Runtime.TaskExecutorDeadlinesAndExceptions", TaskExecutorDeadlinesAndExceptions);
const ServerCoreTest::CheckRegistration shutdown("Runtime.TaskExecutorShutdownAndReentrancy", TaskExecutorShutdownAndReentrancy);
const ServerCoreTest::CheckRegistration race("Runtime.TaskExecutorCompletionRace", TaskExecutorCompletionRace);
}
