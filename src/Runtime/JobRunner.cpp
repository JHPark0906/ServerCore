#include "ServerCore/Runtime/JobRunner.h"
#include "Observability/MetricsInternal.h"
#include <atomic>
#include <limits>
#include <list>
#include <utility>

namespace ServerCore::Runtime
{
namespace
{
using Core::ErrorCode;
using Core::Status;
struct QueuedJob
{
    JobRunner::Job callback;
    std::size_t bytes = 0;
    bool control = false;
    std::chrono::steady_clock::time_point admitted{};
};
}
class JobRunner::ReservationState
{
public:
    ~ReservationState();
    std::shared_ptr<SharedState> owner;
    std::list<QueuedJob> node;
};
class JobRunner::SharedState : public std::enable_shared_from_this<SharedState>
{
public:
    Status Configure(const JobRunnerOptions& options)
    {
        if (!options.maxOutstandingJobs || !options.maxRetainedBytes || !options.maxControlJobs ||
            !options.maxControlBytes ||
            options.maxControlJobs >
                (std::numeric_limits<std::size_t>::max)() - options.maxOutstandingJobs ||
            options.maxControlBytes >
                (std::numeric_limits<std::size_t>::max)() - options.maxRetainedBytes)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard guard(mMutex);
        if (mOptions.maxOutstandingJobs == options.maxOutstandingJobs &&
            mOptions.maxRetainedBytes == options.maxRetainedBytes &&
            mOptions.maxControlJobs == options.maxControlJobs &&
            mOptions.maxControlBytes == options.maxControlBytes)
            return Status::Ok();
        if (mUsed || mRunActive || mAdmissionClosed || mStopRequested)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        mOptions = options;
        return Status::Ok();
    }
    Status Admission(std::size_t bytes, bool control) const noexcept
    {
        if (mStopRequested || (!control && mAdmissionClosed))
            return Status::FailWithoutMessage(ErrorCode::Closed);
        const auto limit = control ? mOptions.maxControlBytes : mOptions.maxRetainedBytes;
        if (bytes > limit)
            return Status::FailWithoutMessage(ErrorCode::TooLarge);
        if ((control ? mControlCount : mCount) >=
                (control ? mOptions.maxControlJobs : mOptions.maxOutstandingJobs) ||
            bytes > limit - (control ? mControlBytes : mBytes))
            return Status::FailWithoutMessage(ErrorCode::WouldBlock);
        return Status::Ok();
    }
    void Charge(std::size_t bytes, bool control) noexcept
    {
        ++(control ? mControlCount : mCount);
        (control ? mControlBytes : mBytes) += bytes;
        mUsed = true;
    }
    void Release(std::size_t bytes, bool control) noexcept
    {
        const std::lock_guard guard(mMutex);
        --(control ? mControlCount : mCount);
        (control ? mControlBytes : mBytes) -= bytes;
    }
    Status Post(Job callback, std::size_t bytes, bool control)
    {
        if (!callback)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        // Capture destruction is arbitrary code; allocate outside the lock.
        std::list<QueuedJob> node;
        try
        {
            node.push_back({ std::move(callback), bytes, control });
        }
        catch (...)
        {
            return Status::AllocationFailure();
        }
        {
            const std::lock_guard guard(mMutex);
            auto admitted = Admission(bytes, control);
            if (!admitted.IsOk())
                return admitted;
            Charge(bytes, control);
            node.front().admitted = std::chrono::steady_clock::now();
            Observability::Detail::Add(mMetrics.acceptedJobs);
            mQueue.splice(mQueue.end(), node);
        }
        mWake.notify_one();
        return Status::Ok();
    }
    Core::Result<Reservation> Reserve(std::size_t bytes)
    {
        using Result = Core::Result<Reservation>;
        std::shared_ptr<ReservationState> reserved;
        try
        {
            reserved = std::make_shared<ReservationState>();
            reserved->node.push_back({ {}, bytes, false });
        }
        catch (...)
        {
            return Result::FromStatus(Status::AllocationFailure());
        }
        {
            const std::lock_guard guard(mMutex);
            auto admitted = Admission(bytes, false);
            if (!admitted.IsOk())
                return Result::FromStatus(std::move(admitted));
            Charge(bytes, false);
            reserved->owner = shared_from_this();
        }
        return Result::FromValue(Reservation(std::move(reserved)));
    }
    Status PostReserved(ReservationState& reserved, Job callback)
    {
        if (!callback)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        if (reserved.node.empty())
            return Status::FailWithoutMessage(ErrorCode::Closed);
        // Operations on one reservation are caller-serialized. Fill its private
        // node before locking: an inline callable's move/destructor may reenter
        // this runner. Committing the node below needs no further callable move.
        reserved.node.front().callback = std::move(callback);
        bool posted = false;
        {
            const std::lock_guard guard(mMutex);
            if (!mStopRequested)
            {
                reserved.node.front().admitted = std::chrono::steady_clock::now();
                Observability::Detail::Add(mMetrics.acceptedJobs);
                mQueue.splice(mQueue.end(), reserved.node);
                posted = true;
            }
        }
        if (!posted)
        {
            // Destroy captures outside the lock while their reservation is
            // still charged; Reservation::Post releases that capacity next.
            reserved.node.front().callback = {};
            return Status::FailWithoutMessage(ErrorCode::Closed);
        }
        mWake.notify_one();
        return Status::Ok();
    }
    void RunUntilStopped()
    {
        {
            const std::lock_guard guard(mMutex);
            SERVERCORE_ASSERT(!mRunActive, "RunUntilStopped may have only one consumer");
            mUsed = true;
            mRunActive = true;
            mRunnerThreadId = std::this_thread::get_id();
        }
        for (;;)
        {
            std::list<QueuedJob> current;
            {
                std::unique_lock guard(mMutex);
                mWake.wait(guard, [this] { return mStopRequested || !mQueue.empty(); });
                if (mQueue.empty())
                {
                    mRunActive = false;
                    mRunnerThreadId = {};
                    return;
                }
                current.splice(current.end(), mQueue, mQueue.begin());
            }
            const auto bytes = current.front().bytes;
            const auto control = current.front().control;
            const auto admitted = current.front().admitted;
            try
            {
                current.front().callback();
            }
            catch (...)
            {
                Core::ReportAssertFailure("JobRunner callback did not throw", __FILE__, __LINE__,
                    "JobRunner callbacks must contain exceptions");
            }
            // Captures remain charged through their destructor, outside locks.
            current.clear();
            Release(bytes, control);
            {
                const std::lock_guard guard(mMutex);
                const auto elapsed = Observability::Detail::Elapsed(admitted);
                Observability::Detail::Add(mMetrics.completedJobs);
                Observability::Detail::Add(mMetrics.totalLatencyNanoseconds, elapsed);
                if (elapsed > mMetrics.maxLatencyNanoseconds)
                    mMetrics.maxLatencyNanoseconds = elapsed;
                Observability::Detail::ObserveLatency(mMetrics.latencyHistogram, elapsed);
            }
        }
    }
    void CloseAdmission() noexcept
    {
        const std::lock_guard guard(mMutex);
        mAdmissionClosed = true;
    }
    void RequestStop()
    {
        {
            const std::lock_guard guard(mMutex);
            mAdmissionClosed = true;
            mStopRequested = true;
        }
        mWake.notify_all();
    }
    bool IsCurrentThread() const noexcept
    {
        const std::lock_guard guard(mMutex);
        return mRunActive && mRunnerThreadId == std::this_thread::get_id();
    }
    bool IsStopRequested(bool control = false) const noexcept
    {
        const std::lock_guard guard(mMutex);
        return control ? mStopRequested : mAdmissionClosed;
    }
    std::size_t PendingCount() const noexcept
    {
        const std::lock_guard guard(mMutex);
        return mQueue.size();
    }
    std::size_t OutstandingCount() const noexcept
    {
        const std::lock_guard guard(mMutex);
        return mCount + mControlCount;
    }
    std::size_t RetainedBytes() const noexcept
    {
        const std::lock_guard guard(mMutex);
        return mBytes + mControlBytes;
    }
    Observability::JobRunnerMetricsSnapshot GetMetrics() const noexcept
    {
        const std::lock_guard guard(mMutex);
        auto value = mMetrics;
        value.pendingJobs = mQueue.size();
        value.outstandingJobs = mCount + mControlCount;
        value.retainedBytes = mBytes + mControlBytes;
        return value;
    }
    void RecordSkippedPeriodicPeriods(std::uint64_t count) noexcept
    {
        mSkippedPeriodicCount.fetch_add(count, std::memory_order_relaxed);
    }
    std::uint64_t PeriodicSkippedCount() const noexcept
    {
        return mSkippedPeriodicCount.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mWake;
    JobRunnerOptions mOptions;
    Observability::JobRunnerMetricsSnapshot mMetrics;
    std::list<QueuedJob> mQueue;
    std::size_t mCount = 0, mBytes = 0, mControlCount = 0, mControlBytes = 0;
    bool mAdmissionClosed = false, mStopRequested = false, mRunActive = false, mUsed = false;
    std::thread::id mRunnerThreadId;
    std::atomic<std::uint64_t> mSkippedPeriodicCount{ 0 };
};
JobRunner::ReservationState::~ReservationState()
{
    if (owner && !node.empty())
        owner->Release(node.front().bytes, false);
}
JobRunner::Reservation::Reservation(std::shared_ptr<ReservationState> state) noexcept
    : mState(std::move(state))
{
}
bool JobRunner::Reservation::IsValid() const noexcept
{
    return mState && !mState->node.empty();
}
Core::Status JobRunner::Reservation::Post(Job job)
{
    const auto state = mState;
    if (!state)
        return Status::FailWithoutMessage(ErrorCode::Closed);
    auto result = state->owner->PostReserved(*state, std::move(job));
    if (result.IsOk() || result.Code() == ErrorCode::Closed)
        mState.reset();
    return result;
}
void JobRunner::Reservation::Cancel() noexcept
{
    mState.reset();
}
JobRunner::Lease::Lease(std::shared_ptr<SharedState> state, bool control) noexcept
    : mState(std::move(state))
    , mControl(control)
{
}
Core::Status JobRunner::Lease::Post(Job job, std::size_t bytes) const
{
    return mState ? mState->Post(std::move(job), bytes, mControl)
                  : Status::FailWithoutMessage(ErrorCode::Closed);
}
Core::Result<JobRunner::Reservation> JobRunner::Lease::Reserve(std::size_t bytes) const
{
    return mState ? mState->Reserve(bytes)
                  : Core::Result<Reservation>::FromStatus(
                        Status::FailWithoutMessage(ErrorCode::Closed));
}
bool JobRunner::Lease::IsStopRequested() const noexcept
{
    return !mState || mState->IsStopRequested(mControl);
}
void JobRunner::Lease::RecordSkippedPeriodicPeriods(std::uint64_t count) const noexcept
{
    if (mState)
        mState->RecordSkippedPeriodicPeriods(count);
}
JobRunner::JobRunner()
    : mState(std::make_shared<SharedState>())
{
}
JobRunner::~JobRunner()
{
    mState->RequestStop();
}
JobRunner::Lease JobRunner::AcquireLease() const noexcept
{
    return Lease(mState);
}
JobRunner::Lease JobRunner::AcquireControlLease() const noexcept
{
    return Lease(mState, true);
}
Core::Status JobRunner::Configure(const JobRunnerOptions& options)
{
    return mState->Configure(options);
}
Core::Status JobRunner::Post(Job job, std::size_t bytes)
{
    return mState->Post(std::move(job), bytes, false);
}
Core::Status JobRunner::PostControl(Job job, std::size_t bytes)
{
    return mState->Post(std::move(job), bytes, true);
}
Core::Result<JobRunner::Reservation> JobRunner::Reserve(std::size_t bytes)
{
    return mState->Reserve(bytes);
}
void JobRunner::RunUntilStopped()
{
    const auto state = mState;
    state->RunUntilStopped();
}
void JobRunner::CloseAdmission() noexcept
{
    mState->CloseAdmission();
}
void JobRunner::RequestStop()
{
    mState->RequestStop();
}
void JobRunner::Stop()
{
    RequestStop();
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
std::size_t JobRunner::OutstandingCount() const noexcept
{
    return mState->OutstandingCount();
}
std::size_t JobRunner::RetainedBytes() const noexcept
{
    return mState->RetainedBytes();
}
Observability::JobRunnerMetricsSnapshot JobRunner::GetMetrics() const noexcept
{
    return mState->GetMetrics();
}
std::uint64_t JobRunner::PeriodicSkippedCount() const noexcept
{
    return mState->PeriodicSkippedCount();
}
}
