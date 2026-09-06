#include "ServerCore/Runtime/PeriodicRunner.h"

#include "ServerCore/Core/Assert.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace ServerCore::Runtime
{
class PeriodicRunner::State : public std::enable_shared_from_this<State>
{
public:
    State(JobRunner::Lease runnerValue, const Period periodValue, Callback callbackValue)
        : runner(std::move(runnerValue))
        , period(periodValue)
        , callback(std::move(callbackValue))
    {
    }

    void RunTimer() noexcept;
    void RunCallback() noexcept;

    std::optional<JobRunner::Lease> runner;
    Period period;
    Callback callback;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable joined;
    std::thread worker;
    std::atomic<std::uint64_t> skippedCount = 0;
    bool started = false;
    bool stopRequested = false;
    bool running = false;
    bool joining = false;
    bool callbackOutstanding = false;
    std::thread::id timerThreadId;
};

namespace
{
using Clock = std::chrono::steady_clock;

[[nodiscard]] Core::Status PlatformFailureFrom(const std::system_error& error) noexcept
{
    try
    {
        return Core::Status::Fail(Core::ErrorCode::PlatformError, error.what());
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}
}

PeriodicRunner::PeriodicRunner(JobRunner::Lease runner, const Period period, Callback callback)
    : mState(std::make_shared<State>(std::move(runner), period, std::move(callback)))
{
}

PeriodicRunner::PeriodicRunner(JobRunner& runner, const Period period, Callback callback)
    : PeriodicRunner(runner.AcquireLease(), period, std::move(callback))
{
}

PeriodicRunner::~PeriodicRunner()
{
    Stop();
}

Core::Status PeriodicRunner::Start()
{
    const std::shared_ptr<State> state = mState;
    std::lock_guard<std::mutex> guard(state->mutex);

    if (state->started)
    {
        return Core::Status::Fail(
            Core::ErrorCode::AlreadyExists, "PeriodicRunner has already been started");
    }
    if (state->stopRequested || !state->runner.has_value() || state->runner->IsStopRequested())
    {
        return Core::Status::Fail(
            Core::ErrorCode::Closed, "PeriodicRunner cannot start with a stopped JobRunner");
    }
    if (state->period <= Period::zero())
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "PeriodicRunner period must be positive");
    }
    if (!state->callback)
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "PeriodicRunner callback cannot be empty");
    }

    try
    {
        state->started = true;
        state->running = true;
        state->worker = std::thread([state]() { state->RunTimer(); });
        state->timerThreadId = state->worker.get_id();
    }
    catch (const std::system_error& error)
    {
        state->started = false;
        state->running = false;
        state->timerThreadId = std::thread::id();
        return PlatformFailureFrom(error);
    }
    catch (const std::bad_alloc&)
    {
        state->started = false;
        state->running = false;
        state->timerThreadId = std::thread::id();
        return Core::Status::AllocationFailure();
    }

    return Core::Status::Ok();
}

void PeriodicRunner::Stop()
{
    const std::shared_ptr<State> state = mState;
    std::thread worker;
    {
        std::unique_lock<std::mutex> guard(state->mutex);
        state->stopRequested = true;
        state->wake.notify_all();

        SERVERCORE_ASSERT(state->timerThreadId != std::this_thread::get_id(),
            "PeriodicRunner::Stop() cannot be called by its timer thread");

        if (!state->worker.joinable())
        {
            // 다른 Stop 호출자가 worker를 가져가 join 중이면, 그 호출자가 실제 join을 끝낼
            // 때까지 기다린다. worker가 단순히 비어 있다는 것만으로는 종료가 완료된 것이 아니다.
            state->joined.wait(guard, [&state]() { return !state->joining; });
            state->runner.reset();
            return;
        }

        SERVERCORE_ASSERT(!state->joining, "PeriodicRunner worker has only one joining caller");
        state->joining = true;
        worker = std::move(state->worker);
    }

    worker.join();

    {
        const std::lock_guard<std::mutex> guard(state->mutex);
        // 실행되지 않은 주기 callback은 JobRunner 큐에서 State를 계속 소유할 수 있다. State도
        // Lease로 같은 큐를 소유하면 둘 다 소멸하지 못하므로, timer join 뒤 역방향 소유를 끊는다.
        state->runner.reset();
        state->joining = false;
        state->timerThreadId = std::thread::id();
    }
    state->joined.notify_all();
}

std::uint64_t PeriodicRunner::SkippedCount() const noexcept
{
    return mState->skippedCount.load(std::memory_order_relaxed);
}

bool PeriodicRunner::IsRunning() const noexcept
{
    const std::lock_guard<std::mutex> guard(mState->mutex);
    return mState->running;
}

void PeriodicRunner::RecordSkippedPeriods(
    const JobRunner::Lease& runner, const std::uint64_t count) noexcept
{
    runner.RecordSkippedPeriodicPeriods(count);
}

void PeriodicRunner::State::RunTimer() noexcept
{
    std::unique_lock<std::mutex> guard(mutex);
    SERVERCORE_ASSERT(runner.has_value(), "PeriodicRunner timer started without a JobRunner lease");
    JobRunner::Lease& targetRunner = *runner;
    Clock::time_point nextDue = Clock::now() + period;

    while (!stopRequested)
    {
        const bool wasStopped = wake.wait_until(guard, nextDue, [this]() { return stopRequested; });
        if (wasStopped)
        {
            break;
        }

        const Clock::time_point now = Clock::now();
        if (now < nextDue)
        {
            continue;
        }

        const Clock::duration lateness = now - nextDue;
        if (lateness >= period)
        {
            const auto fullPeriodsLate = static_cast<std::uint64_t>(lateness / period);
            const std::uint64_t skipped = fullPeriodsLate + 1;
            skippedCount.fetch_add(skipped, std::memory_order_relaxed);
            PeriodicRunner::RecordSkippedPeriods(targetRunner, skipped);

            const Clock::duration remainder = lateness % period;
            nextDue = now + (remainder == Clock::duration::zero() ? period : period - remainder);
            continue;
        }

        nextDue += period;
        // 실행 대기 중인 콜백도 한 자리로 센다. JobRunner가 늦어질 때 타이머 호출이
        // 무한히 쌓이거나 지연된 횟수만큼 한꺼번에 게임 상태를 갱신하지 않게 한다.
        if (callbackOutstanding)
        {
            skippedCount.fetch_add(1, std::memory_order_relaxed);
            PeriodicRunner::RecordSkippedPeriods(targetRunner, 1);
            continue;
        }

        callbackOutstanding = true;
        const std::shared_ptr<State> state = shared_from_this();
        // state 잠금을 든 채 넣으면 Stop이 새 예약을 막은 뒤에는 새 콜백이 더 이상
        // JobRunner에 들어갈 수 없다. Post는 콜백을 즉시 실행하지 않고 큐에만 넣는다.
        Core::Status posted = Core::Status::Ok();
        try
        {
            posted = targetRunner.Post([state]() { state->RunCallback(); });
        }
        catch (const std::bad_alloc&)
        {
            // lambda를 std::function으로 소유하는 과정은 Post() 본문에 들어가기 전에도
            // 할당할 수 있다. timer thread의 noexcept 경계를 넘기지 않는다.
            posted = Core::Status::AllocationFailure();
        }
        catch (...)
        {
            posted = Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }

        if (!posted.IsOk())
        {
            callbackOutstanding = false;
            skippedCount.fetch_add(1, std::memory_order_relaxed);
            PeriodicRunner::RecordSkippedPeriods(targetRunner, 1);
            if (posted.Code() == Core::ErrorCode::Closed)
            {
                stopRequested = true;
            }
        }
    }

    running = false;
    wake.notify_all();
}

void PeriodicRunner::State::RunCallback() noexcept
{
    try
    {
        callback();
    }
    catch (...)
    {
        {
            const std::lock_guard<std::mutex> guard(mutex);
            callbackOutstanding = false;
        }
        Core::ReportAssertFailure("PeriodicRunner callback did not throw", __FILE__, __LINE__,
            "A periodic callback threw an exception");
    }

    const std::lock_guard<std::mutex> guard(mutex);
    callbackOutstanding = false;
}
}
