#include "ServerCore/Runtime/SessionTask.h"
#include "TestHarness.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace ServerCore;
using Core::ErrorCode;
using Core::Status;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
class Gate
{
public:
    void Wait(std::stop_token token = {})
    {
        std::unique_lock guard(mMutex);
        mEntered = true;
        mWake.notify_all();
        (void)mWake.wait_for(guard, token, 3s, [this] { return mOpen; });
    }
    bool Entered()
    {
        std::unique_lock guard(mMutex);
        return mWake.wait_for(guard, 2s, [this] { return mEntered; });
    }
    void Open()
    {
        const std::lock_guard guard(mMutex);
        mOpen = true;
        mWake.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable_any mWake;
    bool mOpen = false, mEntered = false;
};
class TestSession final : public ServerCore::Session::Session
{
public:
    ServerCore::Session::SessionId Id() const noexcept override { return static_cast<ServerCore::Session::SessionId>(1); }
    ServerCore::Session::SessionState State() const noexcept override { return state.load(); }
    std::stop_token GetCancellationToken() const noexcept override { return stop.get_token(); }
    Status MarkAuthenticated() override
    {
        state = ServerCore::Session::SessionState::Authenticated;
        return Status::Ok();
    }
    Status Send(const Protocol::MessageFields&) override { return Status::Ok(); }
    Status SendAndDisconnect(const Protocol::MessageFields&, Status reason) override
    {
        Disconnect(std::move(reason));
        return Status::Ok();
    }
    void Disconnect(Status) override
    {
        state = ServerCore::Session::SessionState::Closed;
        (void)stop.request_stop();
    }

private:
    std::atomic<ServerCore::Session::SessionState> state{ ServerCore::Session::SessionState::Connected };
    std::stop_source stop;
};
void BoundedRunnerReservations()
{
    Runtime::JobRunner runner;
    ExpectTrue(runner.Configure({ 2, 10, 1, 4 }).IsOk(), "small runner budget configures");
    auto reserved = runner.Reserve(6);
    ExpectTrue(reserved.IsOk(), "completion reserves count and bytes");
    if (!reserved.IsOk())
        return;
    std::vector<int> order;
    ExpectTrue(runner.Post([&] { order.push_back(1); }, 4).IsOk(),
        "ordinary work uses remaining capacity");
    ExpectTrue(runner.Post([] {}, 1).Code() == ErrorCode::WouldBlock,
        "reservation prevents over-admission");
    ExpectTrue(runner.Post([] {}, 11).Code() == ErrorCode::TooLarge, "one oversized job rejected");
    ExpectTrue(runner.PostControl([&] { order.push_back(2); }, 4).IsOk(),
        "control capacity survives user exhaustion");
    ExpectTrue(
        runner.PostControl([] {}).Code() == ErrorCode::WouldBlock, "control lane is bounded too");
    runner.CloseAdmission();
    ExpectTrue(runner.Post([] {}).Code() == ErrorCode::Closed, "drain closes ordinary admission");
    ExpectTrue(!runner.AcquireControlLease().IsStopRequested(),
        "maintenance lease survives drain admission close");
    ExpectTrue(reserved.Value().Post([&] { order.push_back(3); }).IsOk(),
        "accepted reserved completion survives drain");
    ExpectEqual(std::size_t{ 3 }, runner.OutstandingCount(), "both lanes counted together");
    runner.RequestStop();
    runner.RunUntilStopped();
    ExpectTrue(order == std::vector<int>({ 1, 2, 3 }),
        "completion and control preserve actual posting FIFO");
    ExpectEqual(std::size_t{ 0 }, runner.OutstandingCount(), "all accepted work released");
    ExpectEqual(
        std::size_t{ 0 }, runner.RetainedBytes(), "all bytes released after capture destruction");

    Runtime::JobRunner cancelled;
    auto abandoned = cancelled.Reserve(8);
    ExpectTrue(abandoned.IsOk(), "reserve before forced stop");
    cancelled.RequestStop();
    cancelled.RunUntilStopped();
    if (abandoned.IsOk())
        ExpectTrue(abandoned.Value().Post([] {}).Code() == ErrorCode::Closed,
            "forced stop rejects unused reservations without waiting");
    ExpectEqual(
        std::size_t{ 0 }, cancelled.OutstandingCount(), "closed reservation releases capacity");
}
void RunnerChargesExecutingCaptures()
{
    Runtime::JobRunner runner;
    ExpectTrue(runner.Configure({ 1, 8, 1, 8 }).IsOk(), "one-slot runner configures");
    Gate gate;
    std::atomic<bool> chargedAtDestruction{ false };
    struct Capture
    {
        Runtime::JobRunner& runner;
        std::atomic<bool>& observed;
        ~Capture() { observed = runner.RetainedBytes() == 8; }
    };
    auto capture = std::make_shared<Capture>(runner, chargedAtDestruction);
    ExpectTrue(runner.Post([&gate, capture] { gate.Wait(); }, 8).IsOk(), "capturing work accepted");
    capture.reset();
    std::thread thread([&] { runner.RunUntilStopped(); });
    ExpectTrue(gate.Entered(), "work executing");
    ExpectEqual(std::size_t{ 0 }, runner.PendingCount(), "executing work not pending");
    ExpectTrue(
        runner.Post([] {}).Code() == ErrorCode::WouldBlock, "executing work still consumes count");
    runner.RequestStop();
    gate.Open();
    thread.join();
    ExpectTrue(chargedAtDestruction.load(),
        "capture cleanup executes outside mutex before releasing budget");
    ExpectEqual(std::size_t{ 0 }, runner.RetainedBytes(), "cleanup releases bytes");
}
void SessionTaskReturnsThroughReservation()
{
    Runtime::JobRunner runner;
    ExpectTrue(runner.Configure({ 1, 8, 1, 8 }).IsOk(), "single continuation runner");
    Runtime::TaskExecutor executor;
    ExpectTrue(executor.Start({ 1, 2, 16 }).IsOk(), "executor starts");
    auto session = std::make_shared<TestSession>();
    Gate gate;
    bool applied = false;
    Runtime::SessionTaskOptions options;
    options.task.retainedBytes = 8;
    options.completionRetainedBytes = 8;
    auto submitted = Runtime::SubmitSessionTask(
        executor, runner.AcquireLease(), session,
        [&](std::stop_token token)
        {
            gate.Wait(token);
            return Status::Ok();
        },
        [&](ServerCore::Session::Session& target, const Status& status)
        { applied = runner.IsCurrentThread() && &target == session.get() && status.IsOk(); },
        options);
    ExpectTrue(submitted.IsOk(), "bridge admitted");
    if (!submitted.IsOk())
        return;
    ExpectTrue(gate.Entered(), "background work executes");
    ExpectTrue(runner.Post([] {}).Code() == ErrorCode::WouldBlock,
        "completion reserved before work starts");
    runner.CloseAdmission();
    gate.Open();
    ExpectTrue(submitted.Value().WaitUntil(std::chrono::steady_clock::now() + 2s).IsOk(),
        "background completion posts despite admission close");
    runner.RequestStop();
    runner.RunUntilStopped();
    ExpectTrue(applied, "apply runs on session's serial context");
    ExpectEqual(std::size_t{ 0 }, runner.OutstandingCount(), "reservation consumed once");
    ExpectTrue(executor.Stop().IsOk(), "executor joins");
}
void SessionTaskCancelsQueuedAndRunning()
{
    Runtime::TaskExecutor executor;
    ExpectTrue(executor.Start({ 1, 3, 32 }).IsOk(), "executor starts");
    Gate blocker;
    auto blocked = executor.Submit(
        [&](std::stop_token token)
        {
            blocker.Wait(token);
            return Status::Ok();
        });
    ExpectTrue(blocked.IsOk() && blocker.Entered(), "worker occupied");
    Runtime::JobRunner runner;
    auto session = std::make_shared<TestSession>();
    std::atomic<unsigned> workCalls{ 0 };
    bool applied = false;
    auto queued = Runtime::SubmitSessionTask(
        executor, runner.AcquireLease(), session,
        [&](std::stop_token)
        {
            ++workCalls;
            return Status::Ok();
        },
        [&](ServerCore::Session::Session&, const Status&) { applied = true; });
    ExpectTrue(queued.IsOk(), "session work queued");
    session->Disconnect(Status::FailWithoutMessage(ErrorCode::Cancelled));
    if (queued.IsOk())
        ExpectTrue(queued.Value().WaitUntil(std::chrono::steady_clock::now() + 2s).Code() ==
                       ErrorCode::Cancelled,
            "session close cancels queued work while worker occupied");
    runner.RequestStop();
    runner.RunUntilStopped();
    ExpectTrue(workCalls == 0 && !applied, "closed session receives neither work nor stale apply");
    blocker.Open();
    if (blocked.IsOk())
        (void)blocked.Value().Wait();

    Runtime::JobRunner second;
    auto live = std::make_shared<TestSession>();
    Gate runningGate;
    auto running = Runtime::SubmitSessionTask(
        executor, second.AcquireLease(), live,
        [&](std::stop_token token)
        {
            runningGate.Wait(token);
            return Status::Ok();
        },
        [&](ServerCore::Session::Session&, const Status&) { applied = true; });
    ExpectTrue(running.IsOk() && runningGate.Entered(), "session task running");
    live->Disconnect(Status::FailWithoutMessage(ErrorCode::Cancelled));
    if (running.IsOk())
        ExpectTrue(running.Value().WaitUntil(std::chrono::steady_clock::now() + 2s).Code() ==
                       ErrorCode::Cancelled,
            "running task receives session cancellation token");
    second.RequestStop();
    second.RunUntilStopped();
    ExpectTrue(!applied, "cancelled running task cannot mutate closed session");
    ExpectTrue(executor.Stop().IsOk(), "executor joins");
}
void TaskCompletionCleanupAndRejection()
{
    Runtime::TaskExecutor executor;
    ExpectTrue(executor.Start({ 1, 2, 8 }).IsOk(), "completion executor starts");
    unsigned calls = 0;
    auto rejected = executor.SubmitWithCompletion([](std::stop_token) { return Status::Ok(); },
        [&](const Status&) { ++calls; }, Runtime::TaskOptions{ 9 });
    ExpectTrue(!rejected.IsOk() && calls == 0, "rejected work never invokes completion");
    auto accepted = executor.SubmitWithCompletion([](std::stop_token) { return Status::Ok(); },
        [&](const Status&)
        {
            ++calls;
            throw std::runtime_error("observer failure");
        },
        Runtime::TaskOptions{ 8 });
    ExpectTrue(accepted.IsOk(), "accepted completion");
    if (accepted.IsOk())
        ExpectTrue(accepted.Value().WaitUntil(std::chrono::steady_clock::now() + 2s).IsOk(),
            "completion exception contained without changing task result");
    ExpectEqual(1u, calls, "accepted completion exactly once");
    ExpectEqual(std::size_t{ 0 }, executor.RetainedBytes(),
        "completion storage budget released before Wait completes");
    ExpectTrue(executor.Stop().IsOk(), "completion executor joins");
}
void SessionTaskRejectsEmptyLegacyWork()
{
    Runtime::TaskExecutor executor;
    Runtime::JobRunner runner;
    const auto session = std::make_shared<TestSession>();
    std::function<Status(std::stop_token)> work;
    const auto apply = [](Session::Session&, const Status&) {};
    auto result = Runtime::SubmitSessionTask(executor, runner.AcquireLease(), session, work, apply);
    ExpectTrue(!result.IsOk() && result.GetStatus().Code() == ErrorCode::InvalidArgument,
        "empty legacy session work is rejected before execution or reservation");
    result = Runtime::SubmitSessionTask(executor, runner.AcquireLease(), session, std::move(work), apply);
    ExpectTrue(!result.IsOk() && result.GetStatus().Code() == ErrorCode::InvalidArgument,
        "moved empty legacy session work keeps the same rejection");
    ExpectEqual(std::size_t{ 0 }, runner.OutstandingCount(), "rejection retains no completion slot");
}
const ServerCoreTest::CheckRegistration emptyLegacyWork(
    "Runtime.SessionTaskRejectsEmptyLegacyWork", SessionTaskRejectsEmptyLegacyWork);
const ServerCoreTest::CheckRegistration bounds(
    "Runtime.BoundedRunnerReservations", BoundedRunnerReservations);
const ServerCoreTest::CheckRegistration captures(
    "Runtime.RunnerChargesExecutingCaptures", RunnerChargesExecutingCaptures);
const ServerCoreTest::CheckRegistration returning(
    "Runtime.SessionTaskReturnsThroughReservation", SessionTaskReturnsThroughReservation);
const ServerCoreTest::CheckRegistration cancelling(
    "Runtime.SessionTaskCancelsQueuedAndRunning", SessionTaskCancelsQueuedAndRunning);
const ServerCoreTest::CheckRegistration completion(
    "Runtime.TaskCompletionCleanupAndRejection", TaskCompletionCleanupAndRejection);
}
