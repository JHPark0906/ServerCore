#include "Runtime/ParseWorkerPoolInternal.h"

#include "ServerCore/Core/Assert.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ServerCore::Runtime
{
namespace
{
thread_local const void* gCurrentParseWorkerState = nullptr;

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

class ParseWorkerPool::State
{
public:
    [[nodiscard]] Core::Status Start(int workerThreadCount);
    [[nodiscard]] Core::Status Post(std::function<void()> job);
    void StopAndDiscard();
    [[nodiscard]] bool IsCurrentThread() const noexcept;

private:
    void RunWorker() noexcept;

    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::deque<std::function<void()>> mJobs;
    std::vector<std::thread> mWorkers;
    bool mStarted = false;
    bool mAccepting = false;
    bool mStopRequested = false;
};

ParseWorkerPool::ParseWorkerPool()
    : mState(std::make_unique<State>())
{
}

ParseWorkerPool::~ParseWorkerPool()
{
    StopAndDiscard();
}

Core::Status ParseWorkerPool::Start(const int workerThreadCount)
{
    return mState->Start(workerThreadCount);
}

Core::Status ParseWorkerPool::Post(std::function<void()> job)
{
    return mState->Post(std::move(job));
}

void ParseWorkerPool::StopAndDiscard()
{
    SERVERCORE_ASSERT(
        !IsCurrentThread(), "ParseWorkerPool::StopAndDiscard() cannot run from one of its workers");
    mState->StopAndDiscard();
}

bool ParseWorkerPool::IsCurrentThread() const noexcept
{
    return mState->IsCurrentThread();
}

Core::Status ParseWorkerPool::State::Start(const int workerThreadCount)
{
    if (workerThreadCount <= 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ParseWorkerPool worker thread count must be positive");
    }

    std::unique_lock<std::mutex> guard(mMutex);
    if (mStarted)
    {
        return Core::Status::Fail(
            Core::ErrorCode::AlreadyExists, "ParseWorkerPool can be started only once");
    }

    try
    {
        mWorkers.reserve(static_cast<std::size_t>(workerThreadCount));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }

    mStarted = true;
    mAccepting = true;
    for (int index = 0; index < workerThreadCount; ++index)
    {
        try
        {
            mWorkers.emplace_back([this]() { RunWorker(); });
        }
        catch (const std::system_error& error)
        {
            mAccepting = false;
            mStopRequested = true;
            std::vector<std::thread> workers;
            workers.swap(mWorkers);
            guard.unlock();
            mWake.notify_all();
            for (std::thread& worker : workers)
            {
                worker.join();
            }
            return PlatformFailureFrom(error);
        }
        catch (const std::bad_alloc&)
        {
            mAccepting = false;
            mStopRequested = true;
            std::vector<std::thread> workers;
            workers.swap(mWorkers);
            guard.unlock();
            mWake.notify_all();
            for (std::thread& worker : workers)
            {
                worker.join();
            }
            return Core::Status::AllocationFailure();
        }
    }

    return Core::Status::Ok();
}

Core::Status ParseWorkerPool::State::Post(std::function<void()> job)
{
    if (!job)
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "ParseWorkerPool cannot post an empty job");
    }

    try
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            if (!mAccepting)
            {
                return Core::Status::Fail(Core::ErrorCode::Closed,
                    "ParseWorkerPool no longer accepts jobs after StopAndDiscard");
            }
            mJobs.emplace_back(std::move(job));
        }
        mWake.notify_one();
        return Core::Status::Ok();
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::system_error& error)
    {
        return PlatformFailureFrom(error);
    }
}

void ParseWorkerPool::State::StopAndDiscard()
{
    std::vector<std::thread> workers;
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        if (!mStarted)
        {
            return;
        }

        mAccepting = false;
        mStopRequested = true;
        // 대기 lambda가 소유한 ParseWorkItem을 여기서 파기해 예약을 반환한다. 이미 worker가
        // 꺼낸 작업은 따로 끝까지 실행되므로, 그 payload의 예약은 아래 join 이전에 빼앗지 않는다.
        mJobs.clear();
        workers.swap(mWorkers);
    }
    mWake.notify_all();

    for (std::thread& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

bool ParseWorkerPool::State::IsCurrentThread() const noexcept
{
    return gCurrentParseWorkerState == this;
}

void ParseWorkerPool::State::RunWorker() noexcept
{
    gCurrentParseWorkerState = this;
    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> guard(mMutex);
            mWake.wait(guard, [this]() { return mStopRequested || !mJobs.empty(); });
            if (mStopRequested && mJobs.empty())
            {
                break;
            }

            job = std::move(mJobs.front());
            mJobs.pop_front();
        }

        try
        {
            job();
        }
        catch (...)
        {
            Core::ReportAssertFailure("ParseWorkerPool job did not throw", __FILE__, __LINE__,
                "A ParseWorkerPool job threw an exception");
        }
    }
    gCurrentParseWorkerState = nullptr;
}
}
