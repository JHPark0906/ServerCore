#include "ServerCore/Runtime/OutboundQueue.h"
#include "ServerCore/Runtime/TickRunner.h"
#include "TestHarness.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
using namespace std::chrono_literals;
namespace R = ServerCore::Runtime;
namespace C = ServerCore::Core;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
template <class Predicate> bool Await(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
struct Gate
{
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false, released = false;
    void Release()
    {
        {
            const std::lock_guard guard(mutex);
            released = true;
        }
        wake.notify_all();
    }
    C::Status Block(std::stop_token token)
    {
        std::stop_callback cancel(token, [this] { wake.notify_all(); });
        std::unique_lock guard(mutex);
        entered = true;
        wake.notify_all();
        wake.wait(guard, [&] { return released || token.stop_requested(); });
        return C::Status::Ok();
    }
};
void LogicalTickPoliciesAndCancellation()
{
    for (const auto policy : { R::TickLagPolicy::Skip, R::TickLagPolicy::CatchUp })
    {
        R::TaskExecutor executor;
        ExpectTrue(executor.Start({ 1, 16, 8192 }).IsOk(), "tick executor starts");
        R::TimerScheduler scheduler;
        ExpectTrue(scheduler.Start(executor, { 8, 8192 }).IsOk(), "common scheduler starts");
        auto gate = std::make_shared<Gate>();
        auto blocked =
            executor.Submit([gate](std::stop_token token) { return gate->Block(token); });
        ExpectTrue(blocked.IsOk(), "worker deliberately occupied before tick admission");
        {
            std::unique_lock guard(gate->mutex);
            ExpectTrue(gate->wake.wait_for(guard, 3s, [&] { return gate->entered; }),
                "blocking job started");
        }
        std::stop_source cancel;
        std::vector<R::TickInfo> ticks;
        R::TickOptions options;
        options.interval = 5ms;
        options.lagPolicy = policy;
        options.maxCatchUp = 3;
        options.parentToken = cancel.get_token();
        auto tick = R::ScheduleTicks(
            scheduler,
            [&](const R::TickInfo& info, std::stop_token)
            {
                ticks.push_back(info);
                if (ticks.size() == (policy == R::TickLagPolicy::Skip ? 1u : 3u))
                    (void)cancel.request_stop();
                return C::Status::Ok();
            },
            options);
        ExpectTrue(tick.IsOk(), "logical tick admitted on existing scheduler");
        std::this_thread::sleep_for(50ms);
        gate->Release();
        if (tick.IsOk())
        {
            auto result = tick.Value().WaitUntil(std::chrono::steady_clock::now() + 3s);
            ExpectTrue(result.Code() == C::ErrorCode::Cancelled,
                "cancel inside callback retires repeating timer");
            (void)tick.Value().RequestCancel();
            (void)scheduler.Stop();
            ExpectEqual(policy == R::TickLagPolicy::Skip ? std::size_t{ 1 } : std::size_t{ 3 },
                ticks.size(), "late batch obeys skip/catch-up cap");
            if (!ticks.empty())
            {
                ExpectTrue(ticks.front().skipped > 0 && ticks.front().index > 1,
                    "late logical intervals are explicitly skipped");
                ExpectTrue(ticks.front().actual >= ticks.front().scheduled,
                    "lateness measured against logical due time");
            }
            for (std::size_t i = 1; i < ticks.size(); ++i)
                ExpectEqual(ticks[i - 1].index + 1, ticks[i].index, "catch-up indices are serial");
            const auto metrics = tick.Value().Metrics();
            ExpectEqual(static_cast<std::uint64_t>(ticks.size()), metrics.executed,
                "terminal metrics count callback invocations");
            ExpectTrue(metrics.skipped > 0 && metrics.maxLateness >= metrics.lastLateness,
                "lag metrics preserve bounds");
        }
        gate->Release();
        (void)scheduler.Stop();
        (void)executor.Stop();
    }
}
struct Sink : std::enable_shared_from_this<Sink>
{
    std::mutex mutex;
    bool available = false;
    std::vector<std::string> sent;
    C::CompletionSource pending;
    std::function<void()> duringSend;
    R::OutboundSink Callbacks()
    {
        auto self = shared_from_this();
        return { [self](const R::OutboundPayload& payload)
            {
                std::function<void()> callback;
                {
                    const std::lock_guard guard(self->mutex);
                    callback = self->duringSend;
                }
                if (callback)
                    callback();
                const std::lock_guard guard(self->mutex);
                if (!self->available)
                    return C::Status::FailWithoutMessage(C::ErrorCode::WouldBlock);
                self->sent.emplace_back(
                    reinterpret_cast<const char*>(payload.Bytes().data()), payload.Bytes().size());
                return C::Status::Ok();
            },
            [self](std::size_t, std::function<void(C::Status)> callback)
            {
                auto result = C::CompletionSubscription::Create(std::move(callback));
                if (!result.IsOk())
                    return result;
                bool ready;
                {
                    const std::lock_guard guard(self->mutex);
                    self->pending = result.Value().GetSource();
                    ready = self->available;
                }
                if (ready)
                    (void)result.Value().GetSource().Complete();
                return result;
            } };
    }
    void Open()
    {
        C::CompletionSource source;
        {
            const std::lock_guard guard(mutex);
            available = true;
            source = pending;
        }
        (void)source.Complete();
    }
    std::vector<std::string> Values()
    {
        const std::lock_guard guard(mutex);
        return sent;
    }
};
std::shared_ptr<const R::OutboundPayload> Payload(std::string_view text)
{
    auto value =
        R::OutboundPayload::CopyBytes(std::as_bytes(std::span(text.data(), text.size())), 1024);
    return value.IsOk() ? std::move(value.Value()) : nullptr;
}
void OutboundFifoLatestAndBatch()
{
    R::TaskExecutor executor;
    (void)executor.Start({ 2, 32, 16384 });
    R::TimerScheduler scheduler;
    (void)scheduler.Start(executor, { 32, 16384 });
    auto sink = std::make_shared<Sink>();
    auto created = R::OutboundQueue::Create(scheduler, sink->Callbacks(), { 3, 3, 2 });
    ExpectTrue(created.IsOk(), "bounded staging queue created");
    if (!created.IsOk())
        return;
    auto queue = created.Value();
    ExpectTrue(queue->Enqueue(Payload("a")).IsOk(), "FIFO head retained on WouldBlock");
    R::OutboundEnqueueOptions latest;
    latest.latestKey = 7;
    ExpectTrue(queue->Enqueue(Payload("b"), latest).IsOk(), "latest key initially admitted");
    ExpectTrue(queue->Enqueue(Payload("z"), latest).IsOk(),
        "queued latest value replaces without moving FIFO position");
    ExpectTrue(queue->Enqueue(Payload("c")).IsOk(), "third item admitted");
    ExpectTrue(queue->Enqueue(Payload("d")).Code() == C::ErrorCode::WouldBlock,
        "count/byte bound includes blocked head");
    sink->Open();
    queue->BeginDrain();
    ExpectTrue(Await([&] { return queue->GetStatus().IsOk(); }),
        "capacity event and bounded timer continuation drain queue");
    ExpectTrue(sink->Values() == std::vector<std::string>{ "a", "z", "c" },
        "FIFO order preserved and superseded bytes never sent");
    auto metrics = queue->Metrics();
    ExpectTrue(metrics.sent == 3 && metrics.replaced == 1 && metrics.pending == 0 &&
                   metrics.retainedBytes == 0,
        "staging metrics release all accepted storage");
    auto second =
        R::OutboundQueue::Create(scheduler, std::make_shared<Sink>()->Callbacks(), { 1, 1, 1 });
    if (second.IsOk())
    {
        const std::vector<std::shared_ptr<R::OutboundQueue>> recipients{ queue, second.Value(),
            nullptr, second.Value() };
        auto batch = R::BatchEnqueue(recipients, Payload("x"));
        ExpectTrue(
            batch.IsOk() &&
                batch.Value() == std::vector<C::ErrorCode>{ C::ErrorCode::Closed, C::ErrorCode::Ok,
                                     C::ErrorCode::InvalidArgument, C::ErrorCode::WouldBlock },
            "batch preserves recipient-specific partial admission and duplicate recipients");
        second.Value()->Close();
    }
    queue->Close();
    (void)scheduler.Stop();
    (void)executor.Stop();
}
void OutboundExpiryAndReentrancy()
{
    R::TaskExecutor executor;
    (void)executor.Start({ 2, 32, 16384 });
    R::TimerScheduler scheduler;
    (void)scheduler.Start(executor, { 32, 16384 });
    auto sink = std::make_shared<Sink>();
    auto created = R::OutboundQueue::Create(scheduler, sink->Callbacks(), { 4, 16, 2 });
    ExpectTrue(created.IsOk(), "expiry fixture created");
    if (!created.IsOk())
        return;
    auto queue = created.Value();
    R::OutboundEnqueueOptions expires;
    expires.expires = std::chrono::steady_clock::now() + 25ms;
    ExpectTrue(
        queue->Enqueue(Payload("old"), expires).IsOk(), "expiring item waits on blocked transport");
    ExpectTrue(Await(
                   [&]
                   {
                       // Expiry is counted when detached; storage and its charge retire
                       // afterward, outside the queue lock.
                       const auto metrics = queue->Metrics();
                       return metrics.expired == 1 && metrics.pending == 0 &&
                              metrics.retainedBytes == 0;
                   }),
        "timer retires expired item and storage without a transport capacity event");
    ExpectEqual(std::size_t{ 0 }, queue->Metrics().retainedBytes,
        "expiry returns queue byte budget promptly");
    std::atomic<int> nested{ 0 };
    {
        const std::lock_guard guard(sink->mutex);
        sink->available = true;
        sink->duringSend = [weak = std::weak_ptr(queue), &nested]
        {
            if (auto owner = weak.lock())
            {
                R::OutboundEnqueueOptions key;
                key.latestKey = 1;
                if (owner->Enqueue(Payload("new"), key).Code() == C::ErrorCode::WouldBlock)
                    ++nested;
                owner->Close();
            }
        };
    }
    R::OutboundEnqueueOptions key;
    key.latestKey = 1;
    ExpectTrue(queue->Enqueue(Payload("sent"), key).IsOk(),
        "reentrant close cannot undo an accepted transport send");
    // The expiry pump may still be handing off when Enqueue accepts this item.
    // Terminal completion, rather than staging admission, ends that work.
    ExpectTrue(Await([&] { return queue->IsFinished(); }),
        "reentrant close completes in-flight admission cleanup");
    ExpectEqual(1, nested.load(), "in-flight latest replacement is atomically rejected");
    ExpectTrue(
        queue->GetStatus().Code() == C::ErrorCode::Cancelled && queue->Metrics().pending == 0,
        "close terminal waits for in-flight admission cleanup");
    ExpectTrue(sink->Values() == std::vector<std::string>{ "sent" },
        "only original in-flight bytes were accepted");
    {
        const std::lock_guard guard(sink->mutex);
        sink->duringSend = {};
    }
    auto failing = std::make_shared<Sink>();
    auto failed = R::OutboundQueue::Create(scheduler, failing->Callbacks(), { 1, 16, 1 });
    if (failed.IsOk())
    {
        failing->duringSend = [weak = std::weak_ptr(failed.Value())]
        {
            if (auto owner = weak.lock())
                owner->Close();
            throw 1;
        };
        ExpectTrue(failed.Value()->Enqueue(Payload("x")).IsOk(),
            "staging accepts before reentrant sink failure");
        ExpectTrue(failed.Value()->IsFinished() && failed.Value()->Metrics().pending == 0,
            "throw after reentrant close still retires active item exactly once");
        failing->duringSend = {};
    }
    auto blocked = R::OutboundQueue::Create(scheduler,
        { [](const R::OutboundPayload&)
            { return C::Status::FailWithoutMessage(C::ErrorCode::WouldBlock); },
            [](std::size_t, std::function<void(C::Status)>)
            {
                return C::Result<C::CompletionSubscription>::FromStatus(
                    C::Status::FailWithoutMessage(C::ErrorCode::WouldBlock));
            } },
        { 1, 16, 1 });
    if (blocked.IsOk())
    {
        std::atomic<int> completions{ 0 };
        auto waiting = blocked.Value()->WaitForCompletion(
            [&](C::Status status)
            {
                if (status.Code() == C::ErrorCode::WouldBlock && blocked.Value()->IsFinished())
                    ++completions;
            });
        ExpectTrue(blocked.Value()->Enqueue(Payload("x")).IsOk(),
            "queue admission precedes capacity registration failure");
        ExpectEqual(1, completions.load(),
            "terminal WouldBlock is distinct from unfinished and notifies exactly once");
        ExpectEqual(std::size_t{ 0 }, blocked.Value()->Metrics().retainedBytes,
            "failed capacity registration returns retained bytes");
    }
    (void)scheduler.Stop();
    (void)executor.Stop();
}
const ServerCoreTest::CheckRegistration ticks(
    "Runtime.LogicalTickPoliciesAndCancellation", LogicalTickPoliciesAndCancellation);
const ServerCoreTest::CheckRegistration queue(
    "Runtime.OutboundFifoLatestAndBatch", OutboundFifoLatestAndBatch);
const ServerCoreTest::CheckRegistration expiry(
    "Runtime.OutboundExpiryAndReentrancy", OutboundExpiryAndReentrancy);
}
