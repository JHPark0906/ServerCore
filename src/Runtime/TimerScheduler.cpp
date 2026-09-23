#include "ServerCore/Runtime/TimerScheduler.h"
#include "Runtime/CompletionSignalInternal.h"
#include <algorithm>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <utility>

namespace ServerCore::Runtime
{
namespace Detail
{
using Core::ErrorCode;
using Core::Status;
using Clock = std::chrono::steady_clock;
class TimerScope;
thread_local const TimerScope* currentTimerScope = nullptr;
class TimerScope
{
public:
    explicit TimerScope(const TimerSchedulerState* state) noexcept
        : mState(state), mPrevious(currentTimerScope) { currentTimerScope = this; }
    ~TimerScope() { currentTimerScope = mPrevious; }
    static bool Contains(const TimerSchedulerState* state) noexcept
    {
        for (auto scope = currentTimerScope; scope; scope = scope->mPrevious)
            if (scope->mState == state) return true;
        return false;
    }
private:
    const TimerSchedulerState* mState;
    const TimerScope* mPrevious;
};
struct CancelTimer
{
    std::weak_ptr<TimerState> timer;
    void operator()() const noexcept;
};
class TimerState
{
public:
    TimerState(TimerScheduler::Callback value, TimerOptions configuration)
        : callback(std::move(value)), options(std::move(configuration)), due(options.due) {}
    TimerScheduler::Callback callback;
    const TimerOptions options;
    std::weak_ptr<TimerSchedulerState> owner;
    std::stop_source cancellation;
    std::unique_ptr<std::stop_callback<CancelTimer>> parent;
    // Scheduling fields are protected by owner->mutex.
    Clock::time_point due;
    TaskHandle task;
    Core::CompletionSubscription taskCompletion;
    CompletionSignal completionSignal;
    bool cancelled = false, signalSent = false, inFlight = false;
    bool published = false, completionReady = false, retiring = false;
    ErrorCode completionCode = ErrorCode::Ok;
    mutable std::mutex resultMutex;
    std::condition_variable finished;
    bool terminal = false;
    ErrorCode result = ErrorCode::WouldBlock;
};
class TimerSchedulerState : public std::enable_shared_from_this<TimerSchedulerState>
{
public:
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable wake;
    std::thread worker;
    TaskExecutor* executor = nullptr;
    TimerSchedulerOptions options;
    std::list<std::shared_ptr<TimerState>> timers;
    std::size_t retained = 0;
    bool started = false, ready = false, stopping = false;

    bool Forbidden() const noexcept
    {
        const std::lock_guard guard(mutex);
        return TimerScope::Contains(this) || (executor && executor->IsCurrentThreadWorker());
    }
    Status Start(TaskExecutor& target, const TimerSchedulerOptions& configuration)
    {
        if (configuration.maxTimers == 0 || configuration.maxRetainedBytes == 0)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard lifecycleGuard(lifecycle);
        {
            const std::lock_guard guard(mutex);
            if (started || stopping) return Status::FailWithoutMessage(ErrorCode::Closed);
            started = true;
            executor = &target;
            options = configuration;
        }
        try { worker = std::thread([self = shared_from_this()] { self->Run(); }); }
        catch (...)
        {
            const std::lock_guard guard(mutex);
            stopping = true;
            executor = nullptr;
            return Status::AllocationFailure();
        }
        { const std::lock_guard guard(mutex); ready = true; }
        wake.notify_all();
        return Status::Ok();
    }
    Core::Result<std::shared_ptr<TimerState>> Schedule(TimerScheduler::Callback callback,
        const TimerOptions& configuration)
    {
        using Result = Core::Result<std::shared_ptr<TimerState>>;
        if (!callback || configuration.repeatInterval < Clock::duration::zero())
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        std::shared_ptr<TimerState> timer;
        try
        {
            timer = std::make_shared<TimerState>(std::move(callback), configuration);
            timer->owner = shared_from_this();
            if (configuration.parentToken.stop_possible())
                timer->parent = std::make_unique<std::stop_callback<CancelTimer>>(
                    configuration.parentToken, CancelTimer{timer});
            const std::lock_guard guard(mutex);
            if (!ready || stopping)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
            if (configuration.retainedBytes > options.maxRetainedBytes)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
            if (timers.size() >= options.maxTimers ||
                configuration.retainedBytes > options.maxRetainedBytes - retained)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::WouldBlock));
            timers.push_back(timer);
            retained += configuration.retainedBytes;
        }
        catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
        wake.notify_all();
        return Result::FromValue(std::move(timer));
    }
    bool Cancel(const std::shared_ptr<TimerState>& timer) noexcept
    {
        {
            const std::lock_guard guard(mutex);
            if (timer->cancelled || timer->retiring) return false;
            // A completed handle can outlive its scheduler's active list.
            { const std::lock_guard resultGuard(timer->resultMutex); if (timer->terminal) return false; }
            timer->cancelled = true;
        }
        wake.notify_all();
        return true;
    }
    void RequestStop() noexcept
    {
        {
            const std::lock_guard guard(mutex);
            stopping = true;
            for (const auto& timer : timers) timer->cancelled = true;
        }
        wake.notify_all();
    }
    Status Stop()
    {
        if (Forbidden()) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard lifecycleGuard(lifecycle);
        RequestStop();
        if (worker.joinable()) worker.join();
        { const std::lock_guard guard(mutex); executor = nullptr; ready = false; }
        return Status::Ok();
    }
    static Clock::time_point After(Clock::time_point now, Clock::duration interval) noexcept
    {
        return interval > Clock::time_point::max() - now ? Clock::time_point::max() : now + interval;
    }
    void Retire(const std::shared_ptr<TimerState>& timer, ErrorCode code)
    {
        TimerScheduler::Callback callback;
        std::unique_ptr<std::stop_callback<CancelTimer>> parent;
        // Retirement was claimed under mutex with no invocation in flight.
        // Moving a small move_only_function target can itself run user code.
        callback = std::move(timer->callback);
        parent = std::move(timer->parent);
        timer->taskCompletion.Reset();
        // The timing-thread scope rejects recursive Stop/Wait from user capture
        // destructors. No scheduling lock is held while they execute.
        callback = {};
        parent.reset();
        {
            const std::lock_guard guard(mutex);
            timers.remove(timer);
            retained -= timer->options.retainedBytes;
            const std::lock_guard resultGuard(timer->resultMutex);
            timer->result = code;
            timer->terminal = true;
        }
        timer->finished.notify_all();
        timer->completionSignal.Complete(code);
        wake.notify_all();
    }
    void Dispatch(const std::shared_ptr<TimerState>& timer)
    {
        timer->taskCompletion.Reset();
        auto result = [&]() -> Core::Result<TaskHandle>
        {
            try
            {
                TaskOptions taskOptions;
                taskOptions.retainedBytes = timer->options.retainedBytes;
                taskOptions.parentToken = timer->cancellation.get_token();
                auto completion = Core::CompletionSubscription::Create(
                    [weak = weak_from_this(), weakTimer = std::weak_ptr(timer)](Status status)
                    {
                        const auto completedTimer = weakTimer.lock();
                        if (const auto state = weak.lock(); state && completedTimer)
                        {
                            {
                                const std::lock_guard guard(state->mutex);
                                completedTimer->completionCode = status.Code();
                                completedTimer->completionReady = true;
                            }
                            state->wake.notify_all();
                        }
                    });
                if (!completion.IsOk()) return Core::Result<TaskHandle>::FromStatus(std::move(completion).TakeStatus());
                timer->taskCompletion = std::move(completion.Value());
                return executor->Submit(
                    [timer](std::stop_token token) { return timer->callback(token); }, taskOptions);
            }
            catch (...) { return Core::Result<TaskHandle>::FromStatus(Status::AllocationFailure()); }
        }();
        {
            const std::lock_guard guard(mutex);
            if (result.IsOk())
            {
                timer->task = result.Value();
                timer->published = true;
            }
            else
            {
                timer->inFlight = false;
                timer->completionCode = result.GetStatus().Code();
                if (result.GetStatus().Code() == ErrorCode::WouldBlock)
                {
                    timer->due = After(Clock::now(), std::chrono::milliseconds(1));
                    timer->completionCode = ErrorCode::Ok;
                }
                else timer->completionReady = true;
            }
        }
        if (result.IsOk())
        {
            // A fresh handle has no external observers yet; attaching this
            // preallocated signal cannot fail or miss an already-ended task.
            const auto observed = result.Value().ObserveCompletion(timer->taskCompletion.GetSource());
            SERVERCORE_ASSERT(observed.IsOk(), "Fresh timer task completion slot must be available");
        }
        else timer->taskCompletion.Reset();
        wake.notify_all();
    }
    void Run()
    {
        const TimerScope scope(this);
        for (;;)
        {
            enum class Action { None, Signal, Dispatch, Retire };
            Action action = Action::None;
            ErrorCode finalCode = ErrorCode::Ok;
            std::shared_ptr<TimerState> selected;
            {
                std::unique_lock guard(mutex);
                if (stopping && timers.empty()) break;
                auto nextWake = Clock::time_point::max();
                const auto now = Clock::now();
                for (const auto& timer : timers)
                {
                    if (timer->cancelled && !timer->signalSent)
                    {
                        timer->signalSent = true;
                        selected = timer;
                        action = Action::Signal;
                        break;
                    }
                    if (timer->inFlight && timer->published && timer->completionReady)
                    {
                        timer->inFlight = false;
                        timer->task = {};
                        if (!timer->cancelled && timer->completionCode == ErrorCode::Ok &&
                            timer->options.repeatInterval > Clock::duration::zero())
                        {
                            timer->completionReady = false;
                            timer->due = After(now, timer->options.repeatInterval);
                        }
                    }
                    if (!timer->inFlight && (timer->cancelled || timer->completionReady))
                    {
                        timer->retiring = true;
                        finalCode = timer->cancelled ? ErrorCode::Cancelled : timer->completionCode;
                        selected = timer;
                        action = Action::Retire;
                        break;
                    }
                    if (!timer->inFlight)
                    {
                        if (timer->due <= now)
                        {
                            timer->inFlight = true;
                            timer->published = false;
                            timer->completionReady = false;
                            selected = timer;
                            action = Action::Dispatch;
                            break;
                        }
                        nextWake = (std::min)(nextWake, timer->due);
                    }
                }
                if (action == Action::None)
                {
                    wake.wait_until(guard, nextWake);
                    continue;
                }
            }
            if (action == Action::Signal) (void)selected->cancellation.request_stop();
            else if (action == Action::Dispatch) Dispatch(selected);
            else if (action == Action::Retire) Retire(selected, finalCode);
        }
    }
};
void CancelTimer::operator()() const noexcept
{
    if (const auto state = timer.lock())
        if (const auto owner = state->owner.lock()) (void)owner->Cancel(state);
}
Status TimerResult(const TimerState& timer) noexcept
{ return timer.result == ErrorCode::Ok ? Status::Ok() : Status::FailWithoutMessage(timer.result); }
}
TimerHandle::TimerHandle(std::shared_ptr<Detail::TimerState> state) noexcept : mState(std::move(state)) {}
bool TimerHandle::IsValid() const noexcept { return mState != nullptr; }
bool TimerHandle::RequestCancel() const noexcept
{
    if (mState) if (const auto owner = mState->owner.lock()) return owner->Cancel(mState);
    return false;
}
Core::Status TimerHandle::Reschedule(std::chrono::steady_clock::time_point due) const noexcept
{
    if (!mState) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    if (const auto owner = mState->owner.lock())
    {
        {
            const std::lock_guard guard(owner->mutex);
            if (owner->stopping || mState->cancelled || mState->retiring ||
                (!mState->inFlight && mState->completionReady))
                return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
            if (mState->inFlight) return Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock);
            mState->due = due;
        }
        owner->wake.notify_all();
        return Core::Status::Ok();
    }
    return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
}
bool TimerHandle::IsFinished() const noexcept
{
    if (!mState) return false;
    const std::lock_guard guard(mState->resultMutex);
    return mState->terminal;
}
Core::Status TimerHandle::GetStatus() const noexcept
{
    if (!mState) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    const std::lock_guard guard(mState->resultMutex);
    return Detail::TimerResult(*mState);
}
Core::Status TimerHandle::Wait() const { return WaitUntil(std::chrono::steady_clock::time_point::max()); }
Core::Result<Core::CompletionSubscription> TimerHandle::WaitForCompletion(
    std::function<void(Core::Status)> callback, std::stop_token cancellation) const
{
    if (!mState) return Core::Result<Core::CompletionSubscription>::FromStatus(
        Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
    return mState->completionSignal.Subscribe(std::move(callback), cancellation);
}
Core::Status TimerHandle::WaitUntil(std::chrono::steady_clock::time_point deadline) const
{
    if (!mState) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    const auto owner = mState->owner.lock();
    const bool forbidden = owner && owner->Forbidden();
    std::unique_lock guard(mState->resultMutex);
    if (!mState->terminal)
    {
        if (forbidden) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        if (!mState->finished.wait_until(guard, deadline, [this] { return mState->terminal; }))
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout);
    }
    return Detail::TimerResult(*mState);
}
TimerScheduler::TimerScheduler() : mState(std::make_shared<Detail::TimerSchedulerState>()) {}
TimerScheduler::~TimerScheduler()
{
    SERVERCORE_ASSERT(!mState->Forbidden(), "TimerScheduler must be destroyed outside its timer/executor threads");
    (void)mState->Stop();
}
Core::Status TimerScheduler::Start(TaskExecutor& executor, const TimerSchedulerOptions& options)
{ return mState->Start(executor, options); }
Core::Result<TimerHandle> TimerScheduler::Schedule(Callback callback, const TimerOptions& options)
{
    auto result = mState->Schedule(std::move(callback), options);
    if (!result.IsOk()) return Core::Result<TimerHandle>::FromStatus(std::move(result).TakeStatus());
    return Core::Result<TimerHandle>::FromValue(TimerHandle(std::move(result.Value())));
}
void TimerScheduler::RequestStop() noexcept { mState->RequestStop(); }
Core::Status TimerScheduler::Stop() { return mState->Stop(); }
std::size_t TimerScheduler::TimerCount() const noexcept
{ const std::lock_guard guard(mState->mutex); return mState->timers.size(); }
std::size_t TimerScheduler::RetainedBytes() const noexcept
{ const std::lock_guard guard(mState->mutex); return mState->retained; }
}
