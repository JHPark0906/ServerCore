#include "TestHarness.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace ServerCore::Runtime;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCoreTest::ExpectTrue;
using ServerCoreTest::ExpectEqual;
using Clock = std::chrono::steady_clock;
template<class Predicate> bool Await(Predicate predicate)
{
    const auto deadline = Clock::now() + 2s;
    while (!predicate())
    {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
void TimerSchedulerAdmissionAndReschedule()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({2, 8, 64}).IsOk(), "timer executor starts");
    TimerScheduler scheduler;
    ExpectTrue(scheduler.Start(executor, {2, 8}).IsOk(), "shared scheduler starts");
    ExpectTrue(scheduler.Schedule({}).GetStatus().Code() == ErrorCode::InvalidArgument, "empty timer rejected");
    TimerOptions options;
    options.due = Clock::now() + 1h;
    options.retainedBytes = 4;
    std::atomic<unsigned> calls{0};
    auto capture = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = capture;
    auto result = scheduler.Schedule([capture = std::move(capture), &calls, &executor](std::stop_token)
    {
        if (*capture == 7 && executor.IsCurrentThreadWorker()) ++calls;
        return Status::Ok();
    }, options);
    ExpectTrue(result.IsOk(), "future timer admitted");
    if (!result.IsOk()) return;
    auto timer = result.Value();
    std::atomic<bool> observed{false}, observationValid{false};
    auto subscription = timer.WaitForCompletion([&](Status status)
    {
        observationValid = status.IsOk() && timer.IsFinished() && lifetime.expired();
        observed = true;
    });
    ExpectTrue(subscription.IsOk(), "timer completion observation admitted");
    auto second = scheduler.Schedule([](std::stop_token) { return Status::Ok(); }, options);
    ExpectTrue(second.IsOk(), "second timer fills bounded capacity");
    ExpectTrue(scheduler.Schedule([](std::stop_token) { return Status::Ok(); }, options).GetStatus().Code()
        == ErrorCode::WouldBlock, "timer count is bounded");
    options.retainedBytes = 9;
    ExpectTrue(scheduler.Schedule([](std::stop_token) { return Status::Ok(); }, options).GetStatus().Code()
        == ErrorCode::TooLarge, "impossible timer payload rejected");
    ExpectEqual(std::size_t{8}, scheduler.RetainedBytes(), "waiting timer captures remain charged");
    ExpectTrue(timer.Reschedule(Clock::now()).IsOk(), "pending timer can move earlier");
    ExpectTrue(timer.WaitUntil(Clock::now() + 2s).IsOk(), "rescheduled one-shot finishes");
    ExpectEqual(1u, calls.load(), "one-shot runs once on executor");
    ExpectTrue(lifetime.expired(), "terminal timer has released user capture");
    ExpectTrue(Await([&] { return observed.load(); }) && observationValid,
        "timer observer sees terminal state after capture retirement");
    ExpectTrue(timer.Reschedule(Clock::now()).Code() == ErrorCode::Closed, "terminal timer cannot restart");
    ExpectTrue(scheduler.Stop().IsOk(), "scheduler cancels remaining pending timer");
    if (second.IsOk()) ExpectTrue(second.Value().GetStatus().Code() == ErrorCode::Cancelled, "pending timer cancelled");
    ExpectEqual(std::size_t{0}, scheduler.RetainedBytes(), "stop returns all timer bytes");
    ExpectTrue(executor.Stop().IsOk(), "executor stops after scheduler");
}
void TimerSchedulerRepeatsAndCancellation()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({3, 8, 128}).IsOk(), "repeat executor starts");
    TimerScheduler scheduler;
    ExpectTrue(scheduler.Start(executor).IsOk(), "repeat scheduler starts");
    std::stop_source parent;
    std::atomic<unsigned> active{0}, calls{0};
    std::atomic<bool> overlap{false}, selfStopRejected{true};
    TimerOptions options;
    options.repeatInterval = 1ms;
    options.parentToken = parent.get_token();
    auto timer = scheduler.Schedule([&](std::stop_token)
    {
        if (active.fetch_add(1) != 0) overlap = true;
        if (scheduler.Stop().Code() != ErrorCode::InvalidArgument) selfStopRejected = false;
        std::this_thread::sleep_for(10ms);
        ++calls;
        --active;
        return Status::Ok();
    }, options);
    ExpectTrue(timer.IsOk(), "repeating timer admitted");
    ExpectTrue(Await([&] { return calls.load() >= 3; }), "timer repeats after completion");
    parent.request_stop();
    if (timer.IsOk())
        ExpectTrue(timer.Value().WaitUntil(Clock::now() + 2s).Code() == ErrorCode::Cancelled, "parent cancels repeats");
    const auto finalCalls = calls.load();
    std::this_thread::sleep_for(20ms);
    ExpectEqual(finalCalls, calls.load(), "cancelled repeating timer never fires again");
    ExpectTrue(!overlap && selfStopRejected, "repeat callbacks never overlap or join their own executor");
    ExpectTrue(scheduler.Stop().IsOk(), "repeat scheduler joins");
    ExpectTrue(executor.Stop().IsOk(), "repeat executor joins");
}
void TimerSchedulerShutdownAndExecutorPressure()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({1, 1, 64}).IsOk(), "pressure executor starts");
    std::atomic<bool> entered{false}, release{false};
    auto blocker = executor.Submit([&](std::stop_token token)
    {
        entered = true;
        while (!release && !token.stop_requested()) std::this_thread::sleep_for(1ms);
        return Status::Ok();
    });
    ExpectTrue(Await([&] { return entered.load(); }), "executor worker occupied");
    auto queued = executor.Submit([](std::stop_token) { return Status::Ok(); });
    TimerScheduler scheduler;
    ExpectTrue(scheduler.Start(executor, {4, 64}).IsOk(), "pressure scheduler starts");
    std::atomic<unsigned> calls{0};
    auto capture = std::make_shared<int>(1);
    const std::weak_ptr<int> lifetime = capture;
    TimerOptions options;
    options.retainedBytes = 8;
    auto timer = scheduler.Schedule([capture = std::move(capture), &calls](std::stop_token)
    { (void)capture; ++calls; return Status::Ok(); }, options);
    ExpectTrue(timer.IsOk(), "timer admission independent from temporary executor pressure");
    std::this_thread::sleep_for(20ms);
    ExpectEqual(0u, calls.load(), "timer cannot bypass executor capacity");
    ExpectEqual(std::size_t{8}, scheduler.RetainedBytes(), "retrying timer remains charged");
    ExpectTrue(scheduler.Stop().IsOk(), "pending timer stop does not wait for unrelated blocked worker");
    ExpectTrue(lifetime.expired(), "pressure cancellation releases capture");
    if (timer.IsOk()) ExpectTrue(timer.Value().GetStatus().Code() == ErrorCode::Cancelled, "pressure timer cancelled");
    release = true;
    if (blocker.IsOk()) (void)blocker.Value().WaitUntil(Clock::now() + 2s);
    if (queued.IsOk()) (void)queued.Value().WaitUntil(Clock::now() + 2s);
    ExpectTrue(executor.Stop().IsOk(), "pressure executor joins");

    TaskExecutor reentrantExecutor;
    ExpectTrue(reentrantExecutor.Start().IsOk(), "reentrant-move executor starts");
    TimerScheduler reentrantScheduler;
    ExpectTrue(reentrantScheduler.Start(reentrantExecutor).IsOk(), "reentrant-move scheduler starts");
    struct Callback
    {
        TimerScheduler* scheduler;
        std::atomic<bool>* armed;
        Callback(TimerScheduler* owner, std::atomic<bool>* flag) : scheduler(owner), armed(flag) {}
        Callback(Callback&& other) noexcept : scheduler(other.scheduler), armed(other.armed)
        { if (*armed) (void)scheduler->TimerCount(); }
        ~Callback() { if (*armed) (void)scheduler->RetainedBytes(); }
        Status operator()(std::stop_token) { return Status::Ok(); }
    };
    std::atomic<bool> armed{false};
    TimerOptions future;
    future.due = Clock::now() + 1h;
    auto reentrant = reentrantScheduler.Schedule(Callback{&reentrantScheduler, &armed}, future);
    armed = true;
    ExpectTrue(reentrant.IsOk(), "small callback admitted before retirement move probe");
    ExpectTrue(reentrantScheduler.Stop().IsOk(), "retirement moves and destroys user target outside scheduler mutex");
    ExpectTrue(reentrantExecutor.Stop().IsOk(), "reentrant-move executor joins");
}
const ServerCoreTest::CheckRegistration timerAdmission("Runtime.TimerSchedulerAdmissionAndReschedule", TimerSchedulerAdmissionAndReschedule);
const ServerCoreTest::CheckRegistration timerRepeats("Runtime.TimerSchedulerRepeatsAndCancellation", TimerSchedulerRepeatsAndCancellation);
const ServerCoreTest::CheckRegistration timerPressure("Runtime.TimerSchedulerShutdownAndExecutorPressure", TimerSchedulerShutdownAndExecutorPressure);
}
