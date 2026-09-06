#include "ServerCore/Runtime/JobRunner.h"

#include "ServerCore/Core/Assert.h"

#include <atomic>
#include <memory>
#include <utility>

namespace ServerCore::Runtime
{
class JobRunner::SharedState
{
public:
    Core::Status Post(std::function<void()> job)
    {
        Core::Status queued = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            if (mStopRequested)
            {
                return Core::Status::Fail(
                    Core::ErrorCode::Closed, "JobRunner no longer accepts jobs after Stop");
            }

            // Stop 판정과 큐 수락을 같은 잠금에 묶는다. 정지 요청 직전에 수락한 작업이
            // 실행자의 마지막 빈 큐 검사 뒤로 밀려 남는 틈을 만들지 않는다.
            queued = mQueue.Post(std::move(job));
        }

        if (queued.IsOk())
        {
            mWake.notify_one();
        }
        return queued;
    }

    void RunUntilStopped()
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            SERVERCORE_ASSERT(!mRunActive, "RunUntilStopped() may run on only one thread at a time");
            mRunActive = true;
            mRunnerThreadId = std::this_thread::get_id();
        }

        const auto clearRunnerThread = [this]() {
            const std::lock_guard<std::mutex> guard(mMutex);
            mRunActive = false;
            mRunnerThreadId = std::thread::id();
        };

        try
        {
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> guard(mMutex);
                    mWake.wait(guard, [this]() { return mStopRequested || mQueue.PendingCount() != 0; });
                }

                (void)mQueue.DrainOnce();

                {
                    const std::lock_guard<std::mutex> guard(mMutex);
                    if (!mStopRequested || mQueue.PendingCount() != 0)
                    {
                        continue;
                    }

                    mRunActive = false;
                    mRunnerThreadId = std::thread::id();
                    return;
                }
            }
        }
        catch (...)
        {
            clearRunnerThread();
            Core::ReportAssertFailure(
                "JobRunner execution did not throw",
                __FILE__,
                __LINE__,
                "JobRunner encountered an exception while it was running");
        }
    }

    void Stop()
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mStopRequested = true;
        }
        mWake.notify_all();
    }

    [[nodiscard]] bool IsCurrentThread() const noexcept
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mRunActive && mRunnerThreadId == std::this_thread::get_id();
    }

    [[nodiscard]] bool IsStopRequested() const noexcept
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mStopRequested;
    }

    [[nodiscard]] std::size_t PendingCount() const
    {
        return mQueue.PendingCount();
    }

    void RecordSkippedPeriodicPeriods(const std::uint64_t count) noexcept
    {
        mSkippedPeriodicCount.fetch_add(count, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t PeriodicSkippedCount() const noexcept
    {
        return mSkippedPeriodicCount.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mWake;
    Core::JobQueue mQueue;
    bool mStopRequested = false;
    bool mRunActive = false;
    std::thread::id mRunnerThreadId;
    std::atomic<std::uint64_t> mSkippedPeriodicCount{ 0 };
};

JobRunner::Lease::Lease(std::shared_ptr<SharedState> state) noexcept
    : mState(std::move(state))
{
}

Core::Status JobRunner::Lease::Post(std::function<void()> job) const
{
    const std::shared_ptr<SharedState> state = mState;
    if (state == nullptr)
    {
        return Core::Status::Fail(Core::ErrorCode::Closed, "JobRunner lease is closed");
    }

    return state->Post(std::move(job));
}

bool JobRunner::Lease::IsStopRequested() const noexcept
{
    const std::shared_ptr<SharedState> state = mState;
    return state == nullptr || state->IsStopRequested();
}

void JobRunner::Lease::RecordSkippedPeriodicPeriods(const std::uint64_t count) const noexcept
{
    const std::shared_ptr<SharedState> state = mState;
    if (state != nullptr)
    {
        state->RecordSkippedPeriodicPeriods(count);
    }
}

JobRunner::JobRunner()
    : mState(std::make_shared<SharedState>())
{
}

JobRunner::~JobRunner()
{
    // RunUntilStopped와 Lease는 모두 SharedState의 자기 소유 복사본을 잡고 실행한다. 따라서
    // 여기서 정지를 요청한 뒤 JobRunner 외피가 사라져도 그 경로들이 this를 다시 읽지 않는다.
    mState->Stop();
}

JobRunner::Lease JobRunner::AcquireLease() const noexcept
{
    return Lease(mState);
}

Core::Status JobRunner::Post(std::function<void()> job)
{
    return mState->Post(std::move(job));
}

void JobRunner::RunUntilStopped()
{
    const std::shared_ptr<SharedState> state = mState;
    state->RunUntilStopped();
}

void JobRunner::Stop()
{
    mState->Stop();
}

bool JobRunner::IsCurrentThread() const noexcept
{
    return mState->IsCurrentThread();
}

bool JobRunner::IsStopRequested() const noexcept
{
    return mState->IsStopRequested();
}

std::size_t JobRunner::PendingCount() const
{
    return mState->PendingCount();
}

std::uint64_t JobRunner::PeriodicSkippedCount() const noexcept
{
    return mState->PeriodicSkippedCount();
}
}
