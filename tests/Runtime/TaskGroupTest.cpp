#include "ServerCore/Runtime/TaskGroup.h"
#include "TestHarness.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace ServerCore::Runtime;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using Clock = std::chrono::steady_clock;
template <class Predicate> bool Await(Predicate predicate)
{
    const auto deadline = Clock::now() + 2s;
    while (!predicate())
    {
        if (Clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
void TaskGroupBoundsAndCollection()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({ 2, 8, 64 }).IsOk(), "group executor starts");
    TaskGroup group;
    ExpectTrue(group.Start(executor, { 1, 8 }).IsOk(), "bounded group starts");
    auto capture = std::make_unique<int>(42);
    auto first = group.Submit(
        [owned = std::move(capture)](std::stop_token)
        {
            return *owned == 42 ? Status::Ok()
                                : Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        },
        TaskOptions{ 8 });
    ExpectTrue(first.IsOk(), "group accepts move-only child");
    if (!first.IsOk())
        return;
    ExpectTrue(first.Value().task.WaitUntil(Clock::now() + 2s).IsOk(), "first child terminal");
    ExpectTrue(
        Await([&] { return group.RetainedBytes() == 0; }), "group observes true child completion");
    ExpectEqual(std::size_t{ 0 }, group.RetainedBytes(), "completed task capture bytes returned");
    ExpectTrue(group.Submit([](std::stop_token) { return Status::Ok(); }).GetStatus().Code() ==
                   ErrorCode::WouldBlock,
        "uncollected completion occupies bounded child slot");
    auto completed = group.TakeCompletions();
    ExpectTrue(completed.IsOk() && completed.Value().size() == 1 &&
                   completed.Value()[0].id == first.Value().id &&
                   completed.Value()[0].code == ErrorCode::Ok,
        "owned result collection identifies accepted child");
    auto second = group.Submit(
        [](std::stop_token) { return Status::FailWithoutMessage(ErrorCode::NotFound); });
    ExpectTrue(second.IsOk(), "collection frees admission slot");
    std::atomic<bool> observed{ false }, valid{ false };
    auto subscription = group.WaitForCompletion(
        [&](Status status)
        {
            valid = status.IsOk() && group.GetStatus().IsOk() && second.Value().task.IsFinished() &&
                    group.RetainedBytes() == 0;
            observed = true;
        });
    ExpectTrue(subscription.IsOk(), "group completion observer closes admission");
    ExpectTrue(group.WaitUntil(Clock::now() + 2s).IsOk(),
        "group waits for completion regardless of child outcome");
    ExpectTrue(Await([&] { return observed.load(); }) && valid,
        "group observer sees true child terminal state");
    completed = group.TakeCompletions();
    ExpectTrue(completed.IsOk() && completed.Value().size() == 1 &&
                   completed.Value()[0].code == ErrorCode::NotFound,
        "group preserves failed child code");
    ExpectTrue(group.Submit([](std::stop_token) { return Status::Ok(); }).GetStatus().Code() ==
                   ErrorCode::Closed,
        "wait closes admission");
    ExpectTrue(group.Stop().IsOk() && executor.Stop().IsOk(), "group and executor join");
}
void TaskGroupCancellationAndDeadlines()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({ 2, 8, 128 }).IsOk(), "cancellation executor starts");
    const auto waitForCancellation = [](std::stop_token token)
    {
        const auto limit = Clock::now() + 2s;
        while (!token.stop_requested() && Clock::now() < limit)
            std::this_thread::sleep_for(1ms);
        return token.stop_requested() ? Status::Ok()
                                      : Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    };
    std::stop_source parent;
    TaskGroup group;
    TaskGroupOptions options;
    options.parentToken = parent.get_token();
    ExpectTrue(group.Start(executor, options).IsOk(), "parent-linked group starts");
    std::stop_source childCancellation;
    TaskOptions childOptions;
    childOptions.parentToken = childCancellation.get_token();
    auto first = group.Submit(waitForCancellation, childOptions);
    auto second = group.Submit(waitForCancellation);
    ExpectTrue(first.IsOk() && second.IsOk(), "group admits independently cancellable children");
    childCancellation.request_stop();
    if (first.IsOk())
        ExpectTrue(first.Value().task.WaitUntil(Clock::now() + 2s).Code() == ErrorCode::Cancelled,
            "child-specific token remains linked");
    if (second.IsOk())
        ExpectTrue(!second.Value().task.GetToken().stop_requested(),
            "child cancellation does not cancel siblings");
    parent.request_stop();
    ExpectTrue(
        group.WaitUntil(Clock::now() + 2s).IsOk(), "parent cancellation terminates group children");
    if (second.IsOk())
        ExpectTrue(second.Value().task.GetStatus().Code() == ErrorCode::Cancelled,
            "parent token reaches remaining child");
    ExpectTrue(group.Stop().IsOk(), "cancelled group stops");

    TaskGroup timed;
    options = {};
    options.deadline = Clock::now() + 40ms;
    ExpectTrue(timed.Start(executor, options).IsOk(), "deadline group starts");
    childOptions = {};
    childOptions.deadline = Clock::now() + 1h;
    auto child = timed.Submit(waitForCancellation, childOptions);
    ExpectTrue(child.IsOk(), "deadline child admitted");
    ExpectTrue(timed.WaitUntil(Clock::now() + 2s).IsOk(), "earlier group deadline completes child");
    if (child.IsOk())
        ExpectTrue(child.Value().task.GetStatus().Code() == ErrorCode::Timeout,
            "group deadline retains Timeout reason");
    ExpectTrue(timed.Stop().IsOk() && executor.Stop().IsOk(), "deadline group and executor stop");
}
void TaskGroupWaitAndShutdownRace()
{
    TaskExecutor executor;
    ExpectTrue(executor.Start({ 4, 256, 1024 }).IsOk(), "race executor starts");
    TaskGroup group;
    ExpectTrue(group.Start(executor, { 256, 1024 }).IsOk(), "race group starts");
    std::atomic<unsigned> collected{ 0 };
    std::atomic<bool> duplicate{ false }, collectorDone{ false };
    std::thread collector(
        [&]
        {
            std::set<std::uint64_t> ids;
            while (!collectorDone)
            {
                auto batch = group.TakeCompletions();
                if (batch.IsOk())
                    for (const auto& completion : batch.Value())
                    {
                        if (!ids.insert(completion.id).second)
                            duplicate = true;
                        ++collected;
                    }
                std::this_thread::yield();
            }
        });
    unsigned accepted = 0;
    for (unsigned index = 0; index < 100; ++index)
    {
        auto child = group.Submit([](std::stop_token) { return Status::Ok(); });
        if (child.IsOk())
            ++accepted;
    }
    ExpectTrue(
        group.WaitUntil(Clock::now() + 2s).IsOk(), "wait races safely with completion collection");
    collectorDone = true;
    collector.join();
    auto tail = group.TakeCompletions();
    if (tail.IsOk())
        collected += static_cast<unsigned>(tail.Value().size());
    ExpectEqual(accepted, collected.load(), "every accepted child produces exactly one completion");
    ExpectTrue(!duplicate, "completion cannot be collected twice");
    ExpectTrue(group.Stop().IsOk(), "race group stops");

    TaskGroup self;
    ExpectTrue(self.Start(executor).IsOk(), "self-wait group starts");
    std::atomic<bool> rejected{ false };
    auto child = self.Submit(
        [&](std::stop_token)
        {
            rejected = self.Wait().Code() == ErrorCode::InvalidArgument &&
                       self.Stop().Code() == ErrorCode::InvalidArgument;
            return Status::Ok();
        });
    if (child.IsOk())
        (void)child.Value().task.WaitUntil(Clock::now() + 2s);
    ExpectTrue(rejected, "group cannot starve its own executor with wait or stop");
    ExpectTrue(
        self.Stop().IsOk() && executor.Stop().IsOk(), "self-wait guard permits external shutdown");
}
const ServerCoreTest::CheckRegistration groupBounds(
    "Runtime.TaskGroupBoundsAndCollection", TaskGroupBoundsAndCollection);
const ServerCoreTest::CheckRegistration groupCancel(
    "Runtime.TaskGroupCancellationAndDeadlines", TaskGroupCancellationAndDeadlines);
const ServerCoreTest::CheckRegistration groupRace(
    "Runtime.TaskGroupWaitAndShutdownRace", TaskGroupWaitAndShutdownRace);
}
