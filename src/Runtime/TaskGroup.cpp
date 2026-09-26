#include "ServerCore/Runtime/TaskGroup.h"
#include "Runtime/CompletionSignalInternal.h"
#include <condition_variable>
#include <limits>
#include <list>
#include <mutex>
#include <utility>

namespace ServerCore::Runtime
{
namespace Detail
{
using Core::ErrorCode;
using Core::Status;
using Clock = std::chrono::steady_clock;
class GroupScope;
thread_local const GroupScope* currentGroupScope = nullptr;
class GroupScope
{
public:
    explicit GroupScope(const TaskGroupState* state) noexcept
        : mState(state)
        , mPrevious(currentGroupScope)
    {
        currentGroupScope = this;
    }
    ~GroupScope() { currentGroupScope = mPrevious; }
    static bool Contains(const TaskGroupState* state) noexcept
    {
        for (auto scope = currentGroupScope; scope; scope = scope->mPrevious)
            if (scope->mState == state)
                return true;
        return false;
    }

private:
    const TaskGroupState* mState;
    const GroupScope* mPrevious;
};
struct GroupChild;
struct CancelChild
{
    std::weak_ptr<GroupChild> child;
    void operator()() const noexcept;
};
struct GroupChild
{
    std::uint64_t id = 0;
    std::size_t bytes = 0;
    TaskHandle handle;
    Core::CompletionSubscription completion;
    std::stop_source cancellation;
    std::unique_ptr<std::stop_callback<CancelChild>> groupRegistration, callerRegistration;
    bool published = false, completed = false;
    ErrorCode code = ErrorCode::WouldBlock;
};
void CancelChild::operator()() const noexcept
{
    if (const auto value = child.lock())
        (void)value->cancellation.request_stop();
}
struct CancelGroup
{
    std::weak_ptr<TaskGroupState> group;
    void operator()() const noexcept;
};
class TaskGroupState : public std::enable_shared_from_this<TaskGroupState>
{
public:
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable wake;
    TaskExecutor* executor = nullptr;
    TaskGroupOptions options;
    std::stop_source cancellation;
    std::unique_ptr<std::stop_callback<CancelGroup>> parent;
    std::list<std::shared_ptr<GroupChild>> children;
    bool started = false, ready = false, closed = false;
    std::size_t retained = 0, submitting = 0;
    std::uint64_t nextId = 1;
    CompletionSignal completionSignal;

    bool FinishedLocked() const noexcept
    {
        if (!closed || submitting != 0)
            return false;
        for (const auto& child : children)
            if (!child->published || !child->completed)
                return false;
        return true;
    }
    void NotifyCompletion() noexcept
    {
        bool complete;
        {
            const std::lock_guard guard(mutex);
            complete = FinishedLocked();
        }
        wake.notify_all();
        if (complete)
            completionSignal.Complete();
    }
    void CloseAdmission() noexcept
    {
        {
            const std::lock_guard guard(mutex);
            closed = true;
        }
        NotifyCompletion();
    }

    bool Forbidden() const noexcept
    {
        const std::lock_guard guard(mutex);
        return GroupScope::Contains(this) || (executor && executor->IsCurrentThreadWorker());
    }
    Status Start(TaskExecutor& target, const TaskGroupOptions& configuration)
    {
        if (configuration.maxChildren == 0 || configuration.maxRetainedBytes == 0)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard lifecycleGuard(lifecycle);
        {
            const std::lock_guard guard(mutex);
            if (started)
                return Status::FailWithoutMessage(ErrorCode::Closed);
            started = true;
            options = configuration;
            executor = &target;
        }
        try
        {
            if (configuration.parentToken.stop_possible())
                parent = std::make_unique<std::stop_callback<CancelGroup>>(
                    configuration.parentToken, CancelGroup{ weak_from_this() });
        }
        catch (...)
        {
            const std::lock_guard guard(mutex);
            closed = true;
            executor = nullptr;
            return Status::AllocationFailure();
        }
        {
            const std::lock_guard guard(mutex);
            ready = true;
        }
        return Status::Ok();
    }
    void Cancel() noexcept
    {
        const GroupScope scope(this);
        {
            const std::lock_guard guard(mutex);
            closed = true;
        }
        (void)cancellation.request_stop();
        NotifyCompletion();
    }
    Core::Result<TaskGroupTask> Submit(TaskExecutor::Task task, TaskOptions configuration)
    {
        using Result = Core::Result<TaskGroupTask>;
        const GroupScope scope(this);
        if (!task)
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        std::shared_ptr<GroupChild> child;
        TaskExecutor* target = nullptr;
        try
        {
            child = std::make_shared<GroupChild>();
            child->bytes = configuration.retainedBytes;
            auto completion = Core::CompletionSubscription::Create(
                [weak = weak_from_this(), weakChild = std::weak_ptr(child)](Status status)
                {
                    const auto completedChild = weakChild.lock();
                    if (const auto state = weak.lock(); state && completedChild)
                    {
                        {
                            const std::lock_guard guard(state->mutex);
                            completedChild->code = status.Code();
                            completedChild->completed = true;
                            state->retained -= completedChild->bytes;
                        }
                        state->NotifyCompletion();
                    }
                });
            if (!completion.IsOk())
                return Result::FromStatus(std::move(completion).TakeStatus());
            child->completion = std::move(completion.Value());
            child->groupRegistration = std::make_unique<std::stop_callback<CancelChild>>(
                cancellation.get_token(), CancelChild{ child });
            if (configuration.parentToken.stop_possible())
                child->callerRegistration = std::make_unique<std::stop_callback<CancelChild>>(
                    configuration.parentToken, CancelChild{ child });
            const std::lock_guard guard(mutex);
            if (!ready || closed)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
            if (options.deadline && Clock::now() >= *options.deadline)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Timeout));
            if (configuration.retainedBytes > options.maxRetainedBytes || nextId == 0)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
            if (children.size() >= options.maxChildren ||
                configuration.retainedBytes > options.maxRetainedBytes - retained)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::WouldBlock));
            if (options.deadline &&
                (!configuration.deadline || *options.deadline < *configuration.deadline))
                configuration.deadline = options.deadline;
            configuration.parentToken = child->cancellation.get_token();
            child->id = nextId;
            children.push_back(child);
            ++nextId;
            ++submitting;
            retained += configuration.retainedBytes;
            target = executor;
        }
        catch (...)
        {
            return Result::FromStatus(Status::AllocationFailure());
        }

        auto submitted = [&]() -> Core::Result<TaskHandle>
        {
            try
            {
                return target->Submit(std::move(task), configuration);
            }
            catch (...)
            {
                return Core::Result<TaskHandle>::FromStatus(Status::AllocationFailure());
            }
        }();
        // Failed argument construction may leave the task here. Retire its user
        // captures before returning admission bytes, outside every group lock.
        task = {};
        {
            const std::lock_guard guard(mutex);
            --submitting;
            if (submitted.IsOk())
            {
                child->handle = submitted.Value();
                child->published = true;
            }
            else
            {
                children.remove(child);
                retained -= child->bytes;
            }
        }
        if (!submitted.IsOk())
        {
            NotifyCompletion();
            return Result::FromStatus(std::move(submitted).TakeStatus());
        }
        const auto observed = submitted.Value().ObserveCompletion(child->completion.GetSource());
        SERVERCORE_ASSERT(observed.IsOk(), "Fresh group task completion slot must be available");
        NotifyCompletion();
        return Result::FromValue({ child->id, std::move(submitted.Value()) });
    }
    Status WaitUntil(Clock::time_point deadline)
    {
        CloseAdmission();
        std::unique_lock guard(mutex);
        if (FinishedLocked())
            return Status::Ok();
        if (GroupScope::Contains(this) || (executor && executor->IsCurrentThreadWorker()))
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        // Wait()·Stop()의 time_point::max()는 wait_until 대신 wait로 기다린다. glibc 2.30 미만의
        // libstdc++에서 wait_until(max)가 넘쳐 바쁘게 돈다(EXEC-8).
        if (deadline == Clock::time_point::max())
            wake.wait(guard, [this] { return FinishedLocked(); });
        else if (!wake.wait_until(guard, deadline, [this] { return FinishedLocked(); }))
            return Status::FailWithoutMessage(ErrorCode::Timeout);
        return Status::Ok();
    }
    Status Stop()
    {
        if (Forbidden())
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard lifecycleGuard(lifecycle);
        Cancel();
        auto status = WaitUntil(Clock::time_point::max());
        if (!status.IsOk())
            return status;
        parent.reset();
        {
            const std::lock_guard guard(mutex);
            executor = nullptr;
            ready = false;
        }
        return Status::Ok();
    }
};
void CancelGroup::operator()() const noexcept
{
    if (const auto state = group.lock())
        state->Cancel();
}
}
TaskGroup::TaskGroup()
    : mState(std::make_shared<Detail::TaskGroupState>())
{
}
TaskGroup::~TaskGroup()
{
    SERVERCORE_ASSERT(
        !mState->Forbidden(), "TaskGroup must be destroyed outside its executor and callbacks");
    (void)mState->Stop();
}
Core::Status TaskGroup::Start(TaskExecutor& executor, const TaskGroupOptions& options)
{
    return mState->Start(executor, options);
}
Core::Result<TaskGroupTask> TaskGroup::Submit(TaskExecutor::Task task, const TaskOptions& options)
{
    return mState->Submit(std::move(task), options);
}
void TaskGroup::CloseAdmission() noexcept
{
    mState->CloseAdmission();
}
Core::Status TaskGroup::GetStatus() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->FinishedLocked() ? Core::Status::Ok()
                                    : Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock);
}
Core::Result<Core::CompletionSubscription> TaskGroup::WaitForCompletion(
    std::function<void(Core::Status)> callback, std::stop_token cancellation)
{
    mState->CloseAdmission();
    return mState->completionSignal.Subscribe(std::move(callback), cancellation);
}
void TaskGroup::RequestCancel() noexcept
{
    mState->Cancel();
}
Core::Status TaskGroup::Wait()
{
    return WaitUntil(std::chrono::steady_clock::time_point::max());
}
Core::Status TaskGroup::WaitUntil(std::chrono::steady_clock::time_point deadline)
{
    return mState->WaitUntil(deadline);
}
Core::Status TaskGroup::Stop()
{
    return mState->Stop();
}
std::size_t TaskGroup::OutstandingCount() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->children.size();
}
std::size_t TaskGroup::RetainedBytes() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->retained;
}
Core::Result<std::vector<TaskGroupCompletion>> TaskGroup::TakeCompletions()
{
    using Result = Core::Result<std::vector<TaskGroupCompletion>>;
    std::vector<TaskGroupCompletion> result;
    std::unique_ptr<std::list<std::shared_ptr<Detail::GroupChild>>> retired;
    try
    {
        retired = std::make_unique<std::list<std::shared_ptr<Detail::GroupChild>>>();
        const std::lock_guard guard(mState->mutex);
        result.reserve(mState->children.size());
        for (auto iterator = mState->children.begin(); iterator != mState->children.end();)
        {
            const auto current = iterator++;
            const auto& child = *current;
            if (child->published && child->completed && child->handle.IsFinished())
            {
                result.push_back({ child->id, child->code });
                retired->splice(retired->end(), mState->children, current);
            }
        }
    }
    catch (...)
    {
        return Result::FromStatus(Core::Status::AllocationFailure());
    }
    return Result::FromValue(std::move(result));
}
}
