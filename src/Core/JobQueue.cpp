#include "ServerCore/Core/JobQueue.h"

#include "ServerCore/Core/Assert.h"

#include <exception>
#include <new>
#include <system_error>
#include <utility>

namespace ServerCore::Core
{
Status JobQueue::Post(std::function<void()> job)
{
    if (!job)
    {
        return Status::Fail(ErrorCode::InvalidArgument, "JobQueue cannot post an empty job");
    }

    try
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mJobs.emplace_back(std::move(job));
    }
    catch (const std::bad_alloc&)
    {
        return Status::AllocationFailure();
    }
    catch (const std::system_error& error)
    {
        try
        {
            return Status::Fail(ErrorCode::PlatformError, error.what());
        }
        catch (...)
        {
            return Status::AllocationFailure();
        }
    }

    return Status::Ok();
}

std::size_t JobQueue::DrainOnce()
{
    std::deque<std::function<void()>> pending;
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        // 이번 배치만 떼어 잠금 밖에서 호출한다. 작업이 다시 Post한 일은 다음 DrainOnce로
        // 넘어가므로 재진입 잠금이나 한 번의 drain이 끝없이 늘어나는 상황을 피한다.
        pending.swap(mJobs);
    }

    for (std::function<void()>& job : pending)
    {
        try
        {
            job();
        }
        catch (...)
        {
            ReportAssertFailure("JobQueue job did not throw", __FILE__, __LINE__,
                "A JobQueue job threw an exception");
        }
    }

    return pending.size();
}

std::size_t JobQueue::PendingCount() const
{
    const std::lock_guard<std::mutex> guard(mMutex);
    return mJobs.size();
}
}
