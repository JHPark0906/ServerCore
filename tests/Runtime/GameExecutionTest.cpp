#include "Runtime/ExecutionTestAccess.h"
#include "Runtime/TransportTestAccess.h"
#include "ServerCore/Runtime/OutboundQueue.h"
#include "ServerCore/Runtime/TickRunner.h"
#include "TestHarness.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
namespace R = ServerCore::Runtime;
namespace C = ServerCore::Core;
namespace TA = ServerCore::Runtime::TestAccess;
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
/// <summary>반복 타이머 재무장 값을 기록한다. 스케줄러 mutex 아래에서 불리므로 할당하지 않는다.</summary>
struct RepeatRecorder final : TA::ITimerRepeatObserver
{
    struct Record
    {
        std::chrono::steady_clock::time_point served, next, now;
    };
    std::mutex mutex;
    std::vector<Record> records;
    RepeatRecorder() { records.reserve(64); }
    void OnRepeatArmed(std::chrono::steady_clock::time_point served,
        std::chrono::steady_clock::time_point next,
        std::chrono::steady_clock::time_point now) noexcept override
    {
        const std::lock_guard guard(mutex);
        if (records.size() < records.capacity())
            records.push_back({ served, next, now });
    }
    std::vector<Record> Take()
    {
        const std::lock_guard guard(mutex);
        auto taken = records;
        records.clear(); // 예약한 용량을 남겨 두어야 다음 기록이 버려지지 않는다.
        return taken;
    }
};
/// <summary>steady_clock으로 바쁘게 기다려 완료 시각이 맡은 due보다 확실히 늦게 한다.</summary>
void BusyUntil(std::chrono::steady_clock::time_point until)
{
    while (std::chrono::steady_clock::now() < until)
    {
    }
}
// EXEC-1: 논리 틱은 고정 속도인데 하부 타이머가 완료 시점부터 다시 재면 위상이 밀려 틱을 건너뛴다.
// 벽시계 지연을 재는 대신 재무장 규칙 자체를 본다. 고정 지연이면 다음 due가 완료 시각 + interval이라
// 격자를 벗어나고 served + interval보다 늦으므로, 아래 두 조건이 결정적으로 붉어진다.
void TickRunnerRearmsOnFixedRateGrid()
{
    constexpr auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(10ms);
    constexpr int tickCount = 8, overrunTick = 4;
    auto recorder = std::make_shared<RepeatRecorder>();
    TA::InstallTimerRepeatObserver(recorder);
    R::TaskExecutor executor;
    ExpectTrue(executor.Start({ 1, 16, 8192 }).IsOk(), "grid executor starts");
    R::TimerScheduler scheduler;
    ExpectTrue(scheduler.Start(executor, { 8, 8192 }).IsOk(), "grid scheduler starts");
    std::atomic<int> calls{ 0 };
    R::TickOptions options;
    options.interval = 10ms;
    auto tick = R::ScheduleTicks(
        scheduler,
        [&](const R::TickInfo& info, std::stop_token)
        {
            // 한 번은 두 주기 반을 넘겨, 지나간 격자점이 즉시 실행 하나로 합쳐지는지도 본다.
            BusyUntil(
                info.actual + (calls.load() + 1 == overrunTick ? interval * 5 / 2 : interval / 5));
            ++calls;
            return C::Status::Ok();
        },
        options);
    ExpectTrue(tick.IsOk(), "grid tick admitted");
    if (tick.IsOk())
    {
        ExpectTrue(Await([&] { return calls.load() >= tickCount; }), "grid ticks keep running");
        (void)tick.Value().RequestCancel();
        (void)tick.Value().WaitUntil(std::chrono::steady_clock::now() + 3s);
    }
    auto records = recorder->Take();
    ExpectTrue(records.size() >= static_cast<std::size_t>(tickCount - 1),
        "tick timer re-armed after each completed tick");
    const auto origin =
        records.empty() ? std::chrono::steady_clock::time_point{} : records.front().served;
    ExpectTrue(std::any_of(records.begin(), records.end(),
                   [](const RepeatRecorder::Record& record) { return record.next <= record.now; }),
        "overrun tick coalesced its missed grid points into one immediate dispatch");
    for (const auto& record : records)
    {
        ExpectTrue((record.next - origin) % interval == std::chrono::steady_clock::duration::zero(),
            "tick timer next due lies on the logical grid");
        ExpectTrue(record.next <= (std::max)(record.served + interval, record.now),
            "tick timer next due is not delayed past the next grid point");
    }

    // 기본 반복 규칙은 고정 지연 그대로다: 다음 due는 완료 처리 시각 + interval이다.
    auto repeating = scheduler.Schedule(
        [](std::stop_token)
        {
            BusyUntil(std::chrono::steady_clock::now() + 2ms);
            return C::Status::Ok();
        },
        { std::chrono::steady_clock::now(), interval, 0, {} });
    ExpectTrue(repeating.IsOk(), "fixed-delay timer admitted");
    if (repeating.IsOk())
    {
        ExpectTrue(Await(
                       [&]
                       {
                           const std::lock_guard guard(recorder->mutex);
                           return recorder->records.size() >= 3;
                       }),
            "fixed-delay timer re-armed");
        (void)repeating.Value().RequestCancel();
        (void)repeating.Value().WaitUntil(std::chrono::steady_clock::now() + 3s);
    }
    records = recorder->Take();
    ExpectTrue(records.size() >= 3, "fixed-delay re-arms observed");
    for (const auto& record : records)
        ExpectTrue(record.next == record.now + interval,
            "fixed-delay next due is measured from completion");
    TA::ClearTimerRepeatObserver(recorder);
    (void)scheduler.Stop();
    (void)executor.Stop();
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
/// <summary>펌프가 다음 시각을 정한 뒤 타이머를 걸기 전의 창에서 용량을 연다.</summary>
struct OpenCapacityBeforeArm final : TA::IOutboundArmGate
{
    std::shared_ptr<Sink> sink;
    std::atomic<bool> armed{ false }, fired{ false };
    void BeforeArm() noexcept override
    {
        if (!armed.exchange(false))
            return;
        sink->Open();
        fired = true;
    }
};
// EXEC-3: 펌프가 changed를 마지막으로 본 잠금과 타이머를 게시하며 pumping을 내리는 잠금 사이에 온
// 용량 알림은 changed만 남기고 사라져, 만료 타이머가 울릴 때까지 아무것도 보내지 않았다.
void OutboundCapacityWakeBeforeArmIsKept()
{
    R::TaskExecutor executor;
    (void)executor.Start({ 2, 32, 16384 });
    R::TimerScheduler scheduler;
    (void)scheduler.Start(executor, { 32, 16384 });
    auto sink = std::make_shared<Sink>();
    auto gate = std::make_shared<OpenCapacityBeforeArm>();
    gate->sink = sink;
    TA::InstallOutboundArmGate(gate);
    auto created = R::OutboundQueue::Create(scheduler, sink->Callbacks(), { 4, 16, 2 });
    ExpectTrue(created.IsOk(), "handoff queue created");
    if (created.IsOk())
    {
        auto queue = created.Value();
        R::OutboundEnqueueOptions expires;
        expires.expires = std::chrono::steady_clock::now() + 30s;
        gate->armed = true;
        ExpectTrue(queue->Enqueue(Payload("a"), expires).IsOk(), "expiring item admitted");
        ExpectTrue(
            gate->fired.load(), "capacity opened between the pump's last check and its handoff");
        ExpectTrue(Await([&] { return sink->Values().size() == 1; }),
            "capacity signal in the handoff window is not lost");
        ExpectEqual(std::uint64_t{ 0 }, queue->Metrics().expired, "no item waits for its expiry");
        queue->Close();
    }
    TA::ClearOutboundArmGate(gate);
    (void)scheduler.Stop();
    (void)executor.Stop();
}
// EXEC-5: 큐가 깨움 타이머를 걸 때마다 이전 타이머를 취소하고 새 타이머를 만들면, 취소된 타이머가
// 정리될 때까지 슬롯을 차지해 공유 스케줄러가 순간 가득 차고, Schedule의 WouldBlock 한 번에 큐가
// 영구 종료됐다. 슬롯이 하나뿐인 스케줄러로 두 경로를 본다.
void OutboundQueueFitsOneSchedulerSlot()
{
    R::TaskExecutor executor;
    (void)executor.Start({ 2, 32, 16384 });
    R::TimerScheduler scheduler;
    (void)scheduler.Start(executor, { 1, 4096 });
    // 막힌 큐에 만료가 있는 항목을 거듭 넣어도 대기 중인 타이머 하나를 옮겨 쓴다.
    auto blockedSink = std::make_shared<Sink>();
    auto blocked = R::OutboundQueue::Create(scheduler, blockedSink->Callbacks(), { 16, 1024, 4 });
    ExpectTrue(blocked.IsOk(), "blocked queue created");
    if (blocked.IsOk())
    {
        R::OutboundEnqueueOptions expires;
        expires.expires = std::chrono::steady_clock::now() + 30s;
        for (int index = 0; index < 10; ++index)
            ExpectTrue(
                blocked.Value()->Enqueue(Payload("e"), expires).IsOk(), "expiring item admitted");
        ExpectTrue(
            !blocked.Value()->IsFinished(), "rearming a pending wake needs no second timer slot");
        ExpectEqual(std::size_t{ 10 }, blocked.Value()->Metrics().pending, "no item is discarded");
        ExpectTrue(scheduler.TimerCount() <= 1, "blocked queue holds at most one timer");
        blocked.Value()->Close();
    }
    ExpectTrue(
        Await([&] { return scheduler.TimerCount() == 0; }), "closed queue releases its timer");

    // 펌프가 자기 타이머 안에서 연속 배치를 걸면 그 타이머가 슬롯을 쥐고 있다. 슬롯이 풀리면 다시 건다.
    auto sink = std::make_shared<Sink>();
    auto created = R::OutboundQueue::Create(scheduler, sink->Callbacks(), { 8, 1024, 1 });
    ExpectTrue(created.IsOk(), "continuation queue created");
    if (created.IsOk())
    {
        auto queue = created.Value();
        for (const auto text : { "1", "2", "3" })
            ExpectTrue(queue->Enqueue(Payload(text)).IsOk(), "continuation item admitted");
        sink->Open();
        ExpectTrue(Await([&] { return sink->Values().size() == 3; }),
            "continuation waits for its own timer slot instead of closing the queue");
        ExpectTrue(sink->Values() == std::vector<std::string>{ "1", "2", "3" }, "FIFO order kept");
        queue->BeginDrain();
        ExpectTrue(Await([&] { return queue->IsFinished(); }) && queue->GetStatus().IsOk(),
            "queue drains successfully");
        ExpectTrue(queue->Metrics().deferredWakes >= 1, "the full-scheduler rearm path ran");
        queue->Close();
    }
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
const ServerCoreTest::CheckRegistration tickGrid(
    "Runtime.TickRunnerRearmsOnFixedRateGrid", TickRunnerRearmsOnFixedRateGrid);
const ServerCoreTest::CheckRegistration queue(
    "Runtime.OutboundFifoLatestAndBatch", OutboundFifoLatestAndBatch);
const ServerCoreTest::CheckRegistration expiry(
    "Runtime.OutboundExpiryAndReentrancy", OutboundExpiryAndReentrancy);
const ServerCoreTest::CheckRegistration handoff(
    "Runtime.OutboundCapacityWakeBeforeArmIsKept", OutboundCapacityWakeBeforeArmIsKept);
const ServerCoreTest::CheckRegistration oneSlot(
    "Runtime.OutboundQueueFitsOneSchedulerSlot", OutboundQueueFitsOneSchedulerSlot);
}
