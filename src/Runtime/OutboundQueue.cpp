#include "ServerCore/Runtime/OutboundQueue.h"
#include "Runtime/CompletionSignalInternal.h"
#include "ServerCore/Net/ConnectionFlowControl.h"
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
#include "Runtime/TransportTestAccess.h"
#endif
#include <algorithm>
#include <limits>
#include <list>
#include <mutex>

namespace ServerCore::Runtime
{
namespace
{
using Core::ErrorCode;
using Core::Status;
using Clock = std::chrono::steady_clock;
Status Fail(ErrorCode code) noexcept
{
    return Status::FailWithoutMessage(code);
}
template <class T> Core::Result<T> Error(ErrorCode code)
{
    return Core::Result<T>::FromStatus(Fail(code));
}
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
struct OutboundArmGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IOutboundArmGate> gate;
};
[[nodiscard]] OutboundArmGateSlot& GetOutboundArmGateSlot()
{
    static OutboundArmGateSlot slot;
    return slot;
}
void BeforeOutboundArmForTest() noexcept
{
    std::shared_ptr<TestAccess::IOutboundArmGate> gate;
    {
        const std::lock_guard guard(GetOutboundArmGateSlot().mutex);
        gate = GetOutboundArmGateSlot().gate.lock();
    }
    if (gate)
        gate->BeforeArm();
}
#endif
}
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
void TestAccess::InstallOutboundArmGate(std::shared_ptr<IOutboundArmGate> gate)
{
    const std::lock_guard guard(GetOutboundArmGateSlot().mutex);
    GetOutboundArmGateSlot().gate = std::move(gate);
}
void TestAccess::ClearOutboundArmGate(const std::shared_ptr<IOutboundArmGate>& expected)
{
    const std::lock_guard guard(GetOutboundArmGateSlot().mutex);
    if (GetOutboundArmGateSlot().gate.lock() == expected)
        GetOutboundArmGateSlot().gate.reset();
}
#endif
std::span<const std::byte> OutboundPayload::Bytes() const noexcept
{
    return mPrepared ? mPrepared->Bytes() : std::span(mBytes);
}
std::size_t OutboundPayload::WireBytes() const noexcept
{
    return Bytes().size() + (mKind == Kind::Raw ? 0 : mKind == Kind::Prepared ? 4 : 8);
}
Core::Result<std::shared_ptr<const OutboundPayload>> OutboundPayload::CopyBytes(
    std::span<const std::byte> bytes, std::size_t max)
{
    if (!max || bytes.empty())
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::InvalidArgument);
    if (bytes.size() > max)
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::TooLarge);
    try
    {
        auto value = std::make_shared<OutboundPayload>();
        value->mBytes.assign(bytes.begin(), bytes.end());
        return Core::Result<std::shared_ptr<const OutboundPayload>>::FromValue(std::move(value));
    }
    catch (...)
    {
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::PlatformError);
    }
}
Core::Result<std::shared_ptr<const OutboundPayload>> OutboundPayload::CopyBinary(
    std::uint32_t type, std::span<const std::byte> bytes, std::size_t max)
{
    if (!type || !max)
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::InvalidArgument);
    if (max < 8 || bytes.size() > max - 8)
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::TooLarge);
    try
    {
        auto value = std::make_shared<OutboundPayload>();
        value->mKind = Kind::Binary;
        value->mBinaryType = type;
        value->mBytes.assign(bytes.begin(), bytes.end());
        return Core::Result<std::shared_ptr<const OutboundPayload>>::FromValue(std::move(value));
    }
    catch (...)
    {
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::PlatformError);
    }
}
Core::Result<std::shared_ptr<const OutboundPayload>> OutboundPayload::CopyPrepared(
    const Protocol::PreparedMessage& message, std::size_t max)
{
    if (!max || !message.Size())
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::InvalidArgument);
    if (max < 4 || message.Size() > max - 4)
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::TooLarge);
    try
    {
        auto value = std::make_shared<OutboundPayload>();
        value->mKind = Kind::Prepared;
        value->mPrepared = std::make_unique<Protocol::PreparedMessage>(message);
        return Core::Result<std::shared_ptr<const OutboundPayload>>::FromValue(std::move(value));
    }
    catch (...)
    {
        return Error<std::shared_ptr<const OutboundPayload>>(ErrorCode::PlatformError);
    }
}

class OutboundQueue::State : public std::enable_shared_from_this<State>
{
public:
    struct Entry
    {
        std::shared_ptr<const OutboundPayload> payload;
        OutboundEnqueueOptions options;
    };
    State(TimerScheduler& target, OutboundSink callbacks, OutboundQueueOptions limits)
        : scheduler(target)
        , sink(std::move(callbacks))
        , options(limits)
    {
    }
    TimerScheduler& scheduler;
    const OutboundSink sink;
    const OutboundQueueOptions options;
    enum class Target
    {
        Any,
        Raw,
        Session
    };
    Target target = Target::Any;
    mutable std::mutex mutex;
    std::list<std::shared_ptr<Entry>> queue;
    std::shared_ptr<Entry> active;
    OutboundMetrics metrics;
    bool pumping = false, changed = false, draining = false, closed = false, terminal = false,
         halting = false;
    ErrorCode result = ErrorCode::WouldBlock;
    Core::CompletionSubscription capacity;
    TimerHandle timer;
    // 스케줄러가 가득 찼을 때 실행 중인 자기 타이머의 완료를 기다리는 재무장 구독.
    Core::CompletionSubscription wakeRetry;
    std::uint64_t timerGeneration = 0;
    Detail::CompletionSignal completion;

    void Release(std::list<std::shared_ptr<Entry>>& retired) noexcept
    {
        std::size_t count = 0, bytes = 0;
        for (const auto& item : retired)
        {
            ++count;
            bytes += item->payload->WireBytes();
        }
        retired.clear(); // User-provided shared-owner destructors run outside mutex.
        if (count)
        {
            const std::lock_guard guard(mutex);
            metrics.pending -= count;
            metrics.retainedBytes -= bytes;
        }
    }
    void FinishIfReady() noexcept
    {
        ErrorCode code = ErrorCode::WouldBlock;
        bool complete = false;
        {
            const std::lock_guard guard(mutex);
            if (!terminal && !pumping && !active && !halting && metrics.pending == 0 &&
                (closed || draining))
            {
                terminal = true;
                if (!closed)
                    result = ErrorCode::Ok;
                code = result;
                complete = true;
            }
        }
        if (complete)
            completion.Complete(code);
    }
    void Halt(ErrorCode code) noexcept
    {
        Core::CompletionSubscription wait, retry;
        TimerHandle wake;
        {
            const std::lock_guard guard(mutex);
            if (terminal || halting)
                return;
            halting = true;
            if (!closed)
            {
                closed = true;
                result = code;
            }
            changed = true;
            wait = std::move(capacity);
            retry = std::move(wakeRetry);
            wake = timer;
            ++timerGeneration;
        }
        wait.Reset();
        retry.Reset();
        (void)wake.RequestCancel();
        // Close/destruction must not allocate a temporary list sentinel (MSVC).
        // Detach one owning pointer at a time; reentrant Close observes halting.
        for (;;)
        {
            std::shared_ptr<Entry> retired;
            std::size_t bytes = 0;
            {
                const std::lock_guard guard(mutex);
                const auto found = std::find_if(
                    queue.begin(), queue.end(), [&](const auto& item) { return item != active; });
                if (found == queue.end())
                {
                    halting = false;
                    break;
                }
                retired = std::move(*found);
                queue.erase(found);
                ++metrics.discarded;
                bytes = retired->payload->WireBytes();
            }
            retired.reset();
            {
                const std::lock_guard guard(mutex);
                --metrics.pending;
                metrics.retainedBytes -= bytes;
            }
        }
        FinishIfReady();
    }
    void Wake(ErrorCode code) noexcept
    {
        if (code != ErrorCode::Ok)
        {
            Halt(code);
            return;
        }
        Drive();
    }
    // 세대가 같을 때만 펌프를 도는 깨움 타이머를 건다. Schedule은 콜백을 인라인으로 부르지 않는다.
    Core::Result<TimerHandle> ScheduleWake(std::uint64_t generation, Clock::time_point due)
    {
        TimerOptions configuration;
        configuration.due = due;
        configuration.retainedBytes = sizeof(std::weak_ptr<State>);
        return scheduler.Schedule(
            [weak = weak_from_this(), generation](std::stop_token token)
            {
                if (token.stop_requested())
                    return Fail(ErrorCode::Cancelled);
                if (auto self = weak.lock())
                {
                    {
                        const std::lock_guard guard(self->mutex);
                        if (self->timerGeneration != generation)
                            return Status::Ok();
                    }
                    self->Drive();
                }
                return Status::Ok();
            },
            configuration);
    }
    bool Arm(Clock::time_point due)
    {
        std::unique_lock guard(mutex);
        if (closed)
        {
            pumping = false;
            return false;
        }
        // Drive가 changed를 마지막으로 본 잠금 뒤에 온 Enqueue·용량 알림은 pumping 때문에 changed만
        // 남기고 돌아갔다. 여기서 pumping을 내리므로, 그 변화가 있으면 만료 시각까지 미루지 않고 곧바로
        // 다시 돌게 한다(EXEC-3, Runtime.OutboundCapacityWakeBeforeArmIsKept).
        if (changed)
            due = (std::min)(due, Clock::now());
        // 대기 중인 깨움 타이머는 새 슬롯을 잡지 않고 옮긴다. 콜백의 세대가 그대로라 아래와 같은 게시
        // 규칙을 따른다. 매번 취소하고 새로 만들면 취소된 타이머가 정리될 때까지 공유 스케줄러의 슬롯을
        // 차지했다(EXEC-5, Runtime.OutboundQueueFitsOneSchedulerSlot).
        if (timer.Reschedule(due).IsOk())
        {
            pumping = false;
            return true;
        }
        // Schedule never invokes its callback inline. Publish the timer and
        // release pump ownership atomically, so an immediate wake cannot vanish
        // between scheduling and the active driver's handoff.
        const auto generation = ++timerGeneration;
        auto scheduled = ScheduleWake(generation, due);
        pumping = false;
        if (scheduled.IsOk())
        {
            timer = scheduled.Value();
            return true;
        }
        const auto code = scheduled.GetStatus().Code();
        // 스케줄러가 가득 찼어도 이 큐의 타이머가 아직 실행 중이면(대개 이 Drive를 부른 타이머다) 그것이
        // 끝나며 슬롯을 돌려준다. 그때 다시 건다. 완료 구독은 인라인으로 불릴 수 있으므로 잠금 밖에서 건다.
        const auto running = timer;
        if (code == ErrorCode::WouldBlock && running.IsValid() && !running.IsFinished())
        {
            ++metrics.deferredWakes;
            guard.unlock();
            auto retry = running.WaitForCompletion(
                [weak = weak_from_this(), generation](Status)
                {
                    if (auto self = weak.lock())
                        self->RetryWake(generation);
                });
            if (retry.IsOk())
            {
                Core::CompletionSubscription replaced;
                {
                    const std::lock_guard relock(mutex);
                    replaced = std::move(wakeRetry);
                    wakeRetry = std::move(retry.Value());
                }
                replaced.Reset();
                return false;
            }
            Halt(retry.GetStatus().Code());
            return false;
        }
        guard.unlock();
        Halt(code);
        return false;
    }
    // 이 큐의 이전 타이머가 끝나 슬롯을 돌려준 뒤 스케줄러의 타이머 스레드에서 불린다. 사용자 코드를
    // 부르지 않고 즉시 깨움 하나만 건다. 펌프가 깨어나 다음 시각을 다시 정한다.
    void RetryWake(std::uint64_t generation) noexcept
    {
        auto code = ErrorCode::PlatformError;
        try
        {
            std::unique_lock guard(mutex);
            if (closed || timerGeneration != generation)
                return;
            auto scheduled = ScheduleWake(generation, Clock::now());
            if (scheduled.IsOk())
            {
                timer = scheduled.Value();
                return;
            }
            code = scheduled.GetStatus().Code();
        }
        catch (...)
        {
        }
        Halt(code);
    }
    void Drive() noexcept
    {
        {
            const std::lock_guard guard(mutex);
            changed = true;
            if (pumping || closed)
                return;
            pumping = true;
        }
        try
        {
            Core::CompletionSubscription previous;
            {
                const std::lock_guard guard(mutex);
                previous = std::move(capacity);
            }
            previous.Reset();
            std::size_t attempts = 0;
            bool continuation = false;
            for (;;)
            {
                std::list<std::shared_ptr<Entry>> retired;
                std::shared_ptr<Entry> item;
                {
                    const std::lock_guard guard(mutex);
                    changed = false;
                    const auto now = Clock::now();
                    for (auto it = queue.begin(); it != queue.end();)
                    {
                        if ((*it)->options.expires && *(*it)->options.expires <= now)
                        {
                            auto current = it++;
                            retired.splice(retired.end(), queue, current);
                            ++metrics.expired;
                        }
                        else
                            ++it;
                    }
                    if (!closed && !queue.empty() && attempts < options.maxPumpMessages)
                        active = item = queue.front();
                    else
                        continuation = !closed && !queue.empty();
                }
                Release(retired);
                if (!item)
                    break;
                ++attempts;
                auto sent = sink.send(*item->payload);
                {
                    const std::lock_guard guard(mutex);
                    active.reset();
                    if (sent.IsOk() || closed)
                    {
                        auto found = std::find(queue.begin(), queue.end(), item);
                        if (found != queue.end())
                            retired.splice(retired.end(), queue, found);
                        if (sent.IsOk())
                            ++metrics.sent;
                        else
                            ++metrics.discarded;
                    }
                }
                const auto required = item->payload->WireBytes();
                item.reset();
                Release(retired);
                if (sent.IsOk())
                    continue;
                if (sent.Code() != ErrorCode::WouldBlock)
                {
                    Halt(sent.Code());
                    break;
                }
                {
                    const std::lock_guard guard(mutex);
                    if (closed)
                        break;
                }
                auto waiting = sink.waitCapacity(required,
                    [weak = weak_from_this()](Status status)
                    {
                        if (auto self = weak.lock())
                            self->Wake(status.Code());
                    });
                if (!waiting.IsOk())
                {
                    Halt(waiting.GetStatus().Code());
                    break;
                }
                bool retry;
                {
                    const std::lock_guard guard(mutex);
                    retry = changed;
                    if (!closed)
                        capacity = std::move(waiting.Value());
                }
                if (retry)
                {
                    Core::CompletionSubscription old;
                    {
                        const std::lock_guard guard(mutex);
                        old = std::move(capacity);
                    }
                    old.Reset();
                    continue;
                }
                break;
            }
            std::optional<Clock::time_point> next;
            {
                const std::lock_guard guard(mutex);
                if (!closed)
                {
                    if (continuation || changed)
                        next = Clock::now();
                    for (const auto& item : queue)
                        if (item->options.expires && (!next || *item->options.expires < *next))
                            next = item->options.expires;
                }
                if (!next)
                    pumping = false;
            }
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
            if (next)
                BeforeOutboundArmForTest();
#endif
            if (next)
                (void)Arm(*next);
            FinishIfReady();
        }
        catch (...)
        {
            {
                const std::lock_guard guard(mutex);
                active.reset();
                pumping = false;
            }
            Halt(ErrorCode::PlatformError);
        }
    }
};

Core::Result<std::shared_ptr<OutboundQueue>> OutboundQueue::Create(
    TimerScheduler& scheduler, OutboundSink sink, const OutboundQueueOptions& options)
{
    if (!sink.send || !sink.waitCapacity || !options.maxMessages || options.maxMessages > 65536 ||
        !options.maxRetainedBytes || !options.maxPumpMessages || options.maxPumpMessages > 1024)
        return Error<std::shared_ptr<OutboundQueue>>(ErrorCode::InvalidArgument);
    try
    {
        return Core::Result<std::shared_ptr<OutboundQueue>>::FromValue(
            std::shared_ptr<OutboundQueue>(
                new OutboundQueue(std::make_shared<State>(scheduler, std::move(sink), options))));
    }
    catch (...)
    {
        return Error<std::shared_ptr<OutboundQueue>>(ErrorCode::PlatformError);
    }
}
Core::Result<std::shared_ptr<OutboundQueue>> OutboundQueue::ForConnection(TimerScheduler& scheduler,
    std::shared_ptr<Net::Connection> connection, const OutboundQueueOptions& options)
{
    auto flow = Net::GetConnectionFlowControl(connection);
    if (!connection || !flow)
        return Error<std::shared_ptr<OutboundQueue>>(ErrorCode::InvalidArgument);
    auto result = Create(scheduler,
        { [connection](const OutboundPayload& value)
            {
                return value.Type() == OutboundPayload::Kind::Raw
                           ? connection->Send(value.Bytes())
                           : Fail(ErrorCode::InvalidArgument);
            },
            [flow](std::size_t bytes, std::function<void(Status)> callback)
            { return flow->WaitForSendCapacity(bytes, std::move(callback)); } },
        options);
    if (result.IsOk())
        result.Value()->mState->target = State::Target::Raw;
    return result;
}
Core::Result<std::shared_ptr<OutboundQueue>> OutboundQueue::ForSession(TimerScheduler& scheduler,
    std::shared_ptr<Session::Session> session, const OutboundQueueOptions& options)
{
    if (!session)
        return Error<std::shared_ptr<OutboundQueue>>(ErrorCode::InvalidArgument);
    auto result = Create(scheduler,
        { [session](const OutboundPayload& value)
            {
                if (value.Type() == OutboundPayload::Kind::Prepared)
                    return session->SendPrepared(*value.Prepared());
                if (value.Type() == OutboundPayload::Kind::Binary)
                    return session->SendBinary(value.BinaryType(), value.Bytes());
                return Fail(ErrorCode::InvalidArgument);
            },
            [session](std::size_t bytes, std::function<void(Status)> callback)
            { return session->WaitForSendCapacity(bytes, std::move(callback)); } },
        options);
    if (result.IsOk())
        result.Value()->mState->target = State::Target::Session;
    return result;
}
OutboundQueue::~OutboundQueue()
{
    Close();
}
Status OutboundQueue::Enqueue(
    std::shared_ptr<const OutboundPayload> payload, const OutboundEnqueueOptions& options)
{
    const auto state = mState;
    if (!payload || !payload->WireBytes())
        return Fail(ErrorCode::InvalidArgument);
    if ((state->target == State::Target::Raw && payload->Type() != OutboundPayload::Kind::Raw) ||
        (state->target == State::Target::Session && payload->Type() == OutboundPayload::Kind::Raw))
        return Fail(ErrorCode::InvalidArgument);
    const auto bytes = payload->WireBytes();
    if (bytes > state->options.maxRetainedBytes)
        return Fail(ErrorCode::TooLarge);
    if (options.expires && *options.expires <= Clock::now())
        return Fail(ErrorCode::Timeout);
    try
    {
        std::list<std::shared_ptr<State::Entry>> prepared;
        prepared.push_back(
            std::make_shared<State::Entry>(State::Entry{ std::move(payload), options }));
        std::shared_ptr<State::Entry> replaced;
        {
            const std::lock_guard guard(state->mutex);
            if (state->closed || state->draining)
                return Fail(ErrorCode::Closed);
            auto found = state->queue.end();
            if (options.latestKey)
                found =
                    std::find_if(state->queue.begin(), state->queue.end(), [&](const auto& entry)
                        { return entry->options.latestKey == options.latestKey; });
            if (found != state->queue.end())
            {
                if (*found == state->active)
                    return Fail(ErrorCode::WouldBlock);
                const auto previous = (*found)->payload->WireBytes();
                if (bytes >
                    state->options.maxRetainedBytes - (state->metrics.retainedBytes - previous))
                    return Fail(ErrorCode::WouldBlock);
                replaced = std::move(*found);
                *found = std::move(prepared.front());
                prepared.clear();
                state->metrics.retainedBytes = state->metrics.retainedBytes - previous + bytes;
                ++state->metrics.replaced;
            }
            else
            {
                if (state->metrics.pending == state->options.maxMessages ||
                    bytes > state->options.maxRetainedBytes - state->metrics.retainedBytes)
                    return Fail(ErrorCode::WouldBlock);
                state->queue.splice(state->queue.end(), prepared);
                ++state->metrics.pending;
                state->metrics.retainedBytes += bytes;
            }
            ++state->metrics.enqueued;
            state->changed = true;
        }
        replaced.reset();
        state->Drive();
        return Status::Ok();
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}
void OutboundQueue::BeginDrain() noexcept
{
    const auto state = mState;
    {
        const std::lock_guard guard(state->mutex);
        state->draining = true;
    }
    state->Drive();
    state->FinishIfReady();
}
void OutboundQueue::Close() noexcept
{
    const auto state = mState;
    state->Halt(ErrorCode::Cancelled);
}
bool OutboundQueue::IsFinished() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->terminal;
}
Status OutboundQueue::GetStatus() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->terminal
               ? (mState->result == ErrorCode::Ok ? Status::Ok() : Fail(mState->result))
               : Fail(ErrorCode::WouldBlock);
}
OutboundMetrics OutboundQueue::Metrics() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->metrics;
}
Core::Result<Core::CompletionSubscription> OutboundQueue::WaitForCompletion(
    std::function<void(Status)> callback, std::stop_token token) const
{
    const auto state = mState;
    return state->completion.Subscribe(std::move(callback), token);
}
Core::Result<std::vector<ErrorCode>> BatchEnqueue(
    std::span<const std::shared_ptr<OutboundQueue>> recipients,
    std::shared_ptr<const OutboundPayload> payload, const OutboundEnqueueOptions& options)
{
    if (!payload)
        return Error<std::vector<ErrorCode>>(ErrorCode::InvalidArgument);
    if (recipients.size() > 4096)
        return Error<std::vector<ErrorCode>>(ErrorCode::TooLarge);
    try
    {
        std::vector<ErrorCode> result;
        result.reserve(recipients.size());
        for (const auto& recipient : recipients)
            result.push_back(recipient ? recipient->Enqueue(payload, options).Code()
                                       : ErrorCode::InvalidArgument);
        return Core::Result<std::vector<ErrorCode>>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Error<std::vector<ErrorCode>>(ErrorCode::PlatformError);
    }
}
}
