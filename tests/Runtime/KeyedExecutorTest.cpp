#include "ServerCore/Runtime/KeyedExecutor.h"
#include "TestHarness.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace
{
using namespace ServerCore;
using namespace std::chrono_literals;
using Core::ErrorCode;
using Core::Status;
using Runtime::KeyedExecutor;
using Runtime::KeyedTaskHandle;
using Runtime::TaskOptions;
using ServerCoreTest::ExpectTrue;
using Clock = std::chrono::steady_clock;

class Gate
{
public:
    void Enter(std::stop_token token)
    {
        std::unique_lock guard(mutex);
        ++entered;
        changed.notify_all();
        (void)changed.wait_for(guard, token, 5s, [&] { return open; });
    }
    bool WaitEntered()
    {
        std::unique_lock guard(mutex);
        return changed.wait_for(guard, 3s, [&] { return entered != 0; });
    }
    void Open()
    {
        { const std::lock_guard guard(mutex); open = true; }
        changed.notify_all();
    }
private:
    std::mutex mutex;
    std::condition_variable_any changed;
    unsigned entered = 0;
    bool open = false;
};

KeyedTaskHandle Accepted(KeyedExecutor& executor, std::uint64_t key,
    KeyedExecutor::Task task, TaskOptions options = {})
{
    auto result = executor.Submit(key, std::move(task), options);
    ExpectTrue(result.IsOk(), "keyed task is admitted");
    return result.IsOk() ? std::move(result.Value()) : KeyedTaskHandle{};
}
void Code(const KeyedTaskHandle& task, ErrorCode code)
{
    ExpectTrue(task.WaitUntil(Clock::now() + 3s).Code() == code,
        "keyed task reaches the expected terminal outcome");
}

void KeyedExecutorFifoAndParallelism()
{
    auto gate = std::make_shared<Gate>();
    std::atomic<int> active{0};
    std::atomic<bool> overlap{false}, independent{false};
    std::mutex orderMutex;
    std::vector<int> order;
    KeyedExecutor executor;
    ExpectTrue(executor.Start({2, 3, 8, 64, 4, 32}).IsOk(), "parallel keyed executor starts");
    auto first = Accepted(executor, 7, [&, gate](std::stop_token token) {
        if (active.fetch_add(1) != 0) overlap = true;
        { const std::lock_guard guard(orderMutex); order.push_back(1); }
        gate->Enter(token);
        --active;
        return Status::Ok();
    });
    ExpectTrue(gate->WaitEntered(), "first key remains busy while more work arrives");
    std::vector<KeyedTaskHandle> tail;
    for (int index = 2; index <= 4; ++index)
        tail.push_back(Accepted(executor, 7, [&, index](std::stop_token) {
            if (active.fetch_add(1) != 0) overlap = true;
            { const std::lock_guard guard(orderMutex); order.push_back(index); }
            --active;
            return Status::Ok();
        }));
    auto other = Accepted(executor, 8, [&](std::stop_token) {
        independent = active.load() == 1;
        return Status::Ok();
    });
    Code(other, ErrorCode::Ok);
    ExpectTrue(independent && !tail.back().IsFinished(),
        "an independent key finishes while same-key work remains blocked");
    gate->Open();
    Code(first, ErrorCode::Ok);
    for (const auto& task : tail) Code(task, ErrorCode::Ok);
    ExpectTrue(executor.Stop().IsOk(), "parallel executor joins");
    ExpectTrue(!overlap && order == std::vector<int>({1, 2, 3, 4}),
        "one key executes FIFO without overlap across multiple workers");
    ExpectTrue(executor.KeyCount() == 0 && executor.OutstandingCount() == 0,
        "finished keys retire even while observation handles remain alive");
}

void KeyedExecutorRoundRobin()
{
    auto gate = std::make_shared<Gate>();
    std::vector<int> order;
    KeyedExecutor executor;
    ExpectTrue(executor.Start({1, 3, 16, 64, 8, 32}).IsOk(), "single-worker fair executor starts");
    auto blocker = Accepted(executor, 1, [gate](std::stop_token token) {
        gate->Enter(token); return Status::Ok();
    });
    ExpectTrue(gate->WaitEntered(), "fairness setup occupies the worker");
    std::vector<KeyedTaskHandle> tasks;
    for (int index = 1; index <= 3; ++index)
        tasks.push_back(Accepted(executor, 1, [&, index](std::stop_token) {
            order.push_back(10 + index); return Status::Ok();
        }));
    for (std::uint64_t key = 2; key <= 3; ++key)
        for (int index = 1; index <= 2; ++index)
            tasks.push_back(Accepted(executor, key, [&, key, index](std::stop_token) {
                order.push_back(static_cast<int>(key) * 10 + index); return Status::Ok();
            }));
    gate->Open();
    Code(blocker, ErrorCode::Ok);
    for (const auto& task : tasks) Code(task, ErrorCode::Ok);
    ExpectTrue(executor.Stop().IsOk(), "fair executor joins");
    ExpectTrue(order == std::vector<int>({21, 31, 11, 22, 32, 12, 13}),
        "ready keys get one turn before a busy key is requeued at the tail");
}

struct ChargedCapture
{
    KeyedExecutor* executor;
    std::atomic<bool>* released;
    ~ChargedCapture()
    {
        *released = executor->RetainedBytes(11) == 6 && executor->OutstandingCount(11) >= 2;
    }
};

void KeyedExecutorLimitsAndCancellation()
{
    auto gate = std::make_shared<Gate>();
    std::atomic<bool> releasedWhileCharged{false}, completionAfterRelease{false};
    std::atomic<int> invoked{0};
    KeyedExecutor executor;
    ExpectTrue(executor.Start({1, 2, 5, 10, 3, 6}).IsOk(), "bounded keyed executor starts");
    auto active = Accepted(executor, 11, [gate](std::stop_token token) {
        gate->Enter(token); return Status::Ok();
    }, TaskOptions{2});
    ExpectTrue(gate->WaitEntered(), "running work occupies both key and global capacity");
    auto pending = executor.SubmitWithCompletion(11,
        [owned = std::make_unique<ChargedCapture>(&executor, &releasedWhileCharged), &invoked]
        (std::stop_token) { (void)owned; ++invoked; return Status::Ok(); },
        [&](const Status& status) {
            completionAfterRelease = releasedWhileCharged && status.Code() == ErrorCode::Cancelled &&
                executor.RetainedBytes(11) == 6;
        }, TaskOptions{4});
    ExpectTrue(pending.IsOk(), "same-key queued work fills the per-key byte budget");
    auto other = Accepted(executor, 22, [&](std::stop_token) { ++invoked; return Status::Ok(); }, TaskOptions{4});
    ExpectTrue(executor.Submit(11, [](std::stop_token) { return Status::Ok(); }, TaskOptions{1})
        .GetStatus().Code() == ErrorCode::WouldBlock, "per-key/global used byte capacity rejects work");
    ExpectTrue(executor.Submit(33, [](std::stop_token) { return Status::Ok(); })
        .GetStatus().Code() == ErrorCode::WouldBlock, "active key registry has a hard cap");
    ExpectTrue(executor.Submit(11, [](std::stop_token) { return Status::Ok(); }, TaskOptions{7})
        .GetStatus().Code() == ErrorCode::TooLarge, "one task exceeding a per-key byte cap is TooLarge");
    auto fourth = Accepted(executor, 11, [&](std::stop_token) { ++invoked; return Status::Ok(); });
    ExpectTrue(executor.Submit(11, [](std::stop_token) { return Status::Ok(); })
        .GetStatus().Code() == ErrorCode::WouldBlock, "per-key outstanding count is capped before the global count is full");
    auto fifth = Accepted(executor, 22, [&](std::stop_token) { ++invoked; return Status::Ok(); });
    ExpectTrue(executor.Submit(22, [](std::stop_token) { return Status::Ok(); })
        .GetStatus().Code() == ErrorCode::WouldBlock, "global outstanding count includes running work");
    if (pending.IsOk())
    {
        ExpectTrue(pending.Value().RequestCancel(), "queued cancellation is accepted");
        Code(pending.Value(), ErrorCode::Cancelled);
    }
    ExpectTrue(releasedWhileCharged && completionAfterRelease && invoked == 0,
        "coordinator releases cancelled captures outside locks before returning their budgets");
    ExpectTrue(executor.RetainedBytes(11) == 2 && executor.RetainedBytes() == 6,
        "queued cancellation returns bytes while the worker remains occupied");
    ExpectTrue(fourth.RequestCancel() && other.RequestCancel() && fifth.RequestCancel(), "remaining queued work cancels");
    Code(fourth, ErrorCode::Cancelled);
    Code(other, ErrorCode::Cancelled);
    Code(fifth, ErrorCode::Cancelled);
    ExpectTrue(executor.KeyCount() == 1, "cancellation returns idle key registry capacity");
    std::stop_source parent;
    TaskOptions options; options.parentToken = parent.get_token(); options.retainedBytes = 1;
    auto reused = Accepted(executor, 33, [&](std::stop_token) { ++invoked; return Status::Ok(); }, options);
    (void)parent.request_stop();
    Code(reused, ErrorCode::Cancelled);
    ExpectTrue(reused.GetToken().stop_requested(), "parent cancellation reaches the keyed task token");
    ExpectTrue(executor.Stop().IsOk(), "stop cancels and joins the occupied worker");
    Code(active, ErrorCode::Cancelled);
    ExpectTrue(executor.OutstandingCount() == 0 && executor.RetainedBytes() == 0 && executor.KeyCount() == 0,
        "shutdown releases every task, byte and registry charge");
    const auto metrics = executor.GetMetrics();
    ExpectTrue(metrics.acceptedTasks == metrics.completedTasks && metrics.cancelledTasks == metrics.completedTasks,
        "accepted cancellation outcomes are terminal exactly once");
}

void KeyedExecutorCompletionSubscriptions()
{
    auto gate = std::make_shared<Gate>();
    auto capture = std::make_shared<int>(1);
    const std::weak_ptr<int> lifetime = capture;
    std::atomic<unsigned> completed{0}, cancelled{0}, dropped{0};
    std::atomic<bool> observedCleanup{false};
    KeyedExecutor executor;
    ExpectTrue(executor.Start({1, 1, 2, 8, 2, 8}).IsOk(), "keyed observer fixture starts");
    const auto task = Accepted(executor, 42, [gate, capture](std::stop_token token) {
        (void)capture;
        gate->Enter(token);
        return Status::Ok();
    });
    capture.reset();
    ExpectTrue(gate->WaitEntered(), "keyed task remains active while observers register");
    auto pending = task.WaitForCompletion([&, task](Status status) {
        observedCleanup = status.IsOk() && lifetime.expired() && task.IsFinished() &&
            task.GetStatus().IsOk() && executor.OutstandingCount(42) == 0;
        ++completed;
    });
    std::stop_source cancellation;
    auto cancelledWait = task.WaitForCompletion([&](Status status) {
        if (status.Code() == ErrorCode::Cancelled) ++cancelled;
    }, cancellation.get_token());
    auto discarded = task.WaitForCompletion([&](Status) { ++dropped; });
    ExpectTrue(pending.IsOk() && cancelledWait.IsOk() && discarded.IsOk(), "keyed task accepts completion observers");
    if (discarded.IsOk()) discarded.Value().Reset();
    (void)cancellation.request_stop();
    ExpectTrue(cancelled == 1 && !task.IsFinished() && !task.GetToken().stop_requested(),
        "cancelling a keyed observation leaves the task active");
    gate->Open();
    Code(task, ErrorCode::Ok);
    ExpectTrue(executor.Stop().IsOk(), "keyed worker joins after terminal callbacks");
    ExpectTrue(completed == 1 && cancelled == 1 && dropped == 0 && observedCleanup,
        "keyed completion publishes cleanup and accounting before one-shot notifications");
    auto immediate = task.WaitForCompletion([&, task](Status status) {
        observedCleanup = status.IsOk() && task.GetStatus().IsOk();
        ++completed;
    });
    ExpectTrue(immediate.IsOk() && completed == 2 && observedCleanup,
        "completed keyed handles support immediate reentrant observation");
}

void KeyedExecutorDeadlineShutdownAndReentrancy()
{
    auto gate = std::make_shared<Gate>();
    auto self = std::make_shared<KeyedTaskHandle>();
    std::atomic<bool> selfWaitRejected{false}, selfStopRejected{false};
    std::atomic<int> expiredInvoked{0};
    KeyedExecutor executor;
    ExpectTrue(!self->IsValid() && self->GetStatus().Code() == ErrorCode::InvalidArgument && !self->RequestCancel(),
        "default keyed handle is invalid");
    ExpectTrue(executor.Submit(0, {}).GetStatus().Code() == ErrorCode::InvalidArgument &&
        executor.Submit(0, [](std::stop_token) { return Status::Ok(); }).GetStatus().Code() == ErrorCode::Closed,
        "invalid callbacks are distinguished from inactive executor admission");
    ExpectTrue(executor.Start({0, 2, 4, 16, 2, 8}).Code() == ErrorCode::InvalidArgument,
        "zero worker configuration is invalid");
    ExpectTrue(executor.Start({1, 2, 4, 16, 2, 8}).IsOk(), "executor starts after invalid options");
    *self = Accepted(executor, 0, [&, gate, self](std::stop_token token) {
        gate->Enter(token);
        selfWaitRejected = self->WaitUntil(Clock::now() + 20ms).Code() == ErrorCode::InvalidArgument;
        selfStopRejected = executor.Stop().Code() == ErrorCode::InvalidArgument;
        return Status::Ok();
    });
    ExpectTrue(gate->WaitEntered(), "self-wait fixture waits for its observation handle");
    TaskOptions deadline; deadline.deadline = Clock::now() + 30ms;
    auto expired = Accepted(executor, 0, [&](std::stop_token) {
        ++expiredInvoked; return Status::Ok();
    }, deadline);
    Code(expired, ErrorCode::Timeout);
    ExpectTrue(expired.GetToken().stop_requested() && expiredInvoked == 0,
        "queued deadline expires behind a busy key without invoking its callback");
    gate->Open();
    Code(*self, ErrorCode::Ok);
    ExpectTrue(selfWaitRejected && selfStopRejected, "executor callbacks cannot self-wait or self-join");
    ExpectTrue(!self->RequestCancel() && !self->GetToken().stop_requested(),
        "late cancellation does not change a successful task token");
    auto throws = executor.SubmitWithCompletion(0, [](std::stop_token) -> Status {
        throw std::runtime_error("task failure");
    }, [](const Status&) { throw std::runtime_error("observer failure"); });
    ExpectTrue(throws.IsOk(), "throwing task is admitted");
    if (throws.IsOk()) Code(throws.Value(), ErrorCode::PlatformError);
    std::function<Status(std::stop_token)> legacy;
    ExpectTrue(executor.Submit(0, legacy).GetStatus().Code() == ErrorCode::InvalidArgument,
        "empty legacy callbacks preserve invalid-input semantics");
    auto recovered = Accepted(executor, 0, [](std::stop_token) { return Status::Ok(); });
    Code(recovered, ErrorCode::Ok);
    ExpectTrue(executor.Stop().IsOk() && executor.Stop().IsOk(), "shutdown is idempotent");
    ExpectTrue(executor.Submit(0, [](std::stop_token) { return Status::Ok(); }).GetStatus().Code() == ErrorCode::Closed &&
        executor.Start().Code() == ErrorCode::Closed, "stopped executor cannot admit work or restart");
    const auto metrics = executor.GetMetrics();
    ExpectTrue(metrics.acceptedTasks == metrics.completedTasks && metrics.timedOutTasks == 1 && metrics.failedTasks == 1,
        "timeouts and exceptions retain distinct completed metrics");
    KeyedExecutorCompletionSubscriptions();
}

const ServerCoreTest::CheckRegistration fifo("Runtime.KeyedExecutorFifoAndParallelism", KeyedExecutorFifoAndParallelism);
const ServerCoreTest::CheckRegistration fairness("Runtime.KeyedExecutorRoundRobin", KeyedExecutorRoundRobin);
const ServerCoreTest::CheckRegistration limits("Runtime.KeyedExecutorLimitsAndCancellation", KeyedExecutorLimitsAndCancellation);
const ServerCoreTest::CheckRegistration lifecycle("Runtime.KeyedExecutorDeadlineShutdownAndReentrancy", KeyedExecutorDeadlineShutdownAndReentrancy);
}
