#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include <cstdint>
#include <vector>

namespace ServerCore::Runtime
{
namespace Detail
{
class TaskGroupState;
}
struct TaskGroupOptions
{
    // Counts active children AND completions not yet taken, avoiding an
    // unbounded completion history. TakeCompletions returns count capacity.
    std::size_t maxChildren = 128;
    std::size_t maxRetainedBytes = 4 * 1024 * 1024;
    std::stop_token parentToken{};
    std::optional<std::chrono::steady_clock::time_point> deadline{};
};
struct TaskGroupTask
{
    std::uint64_t id = 0;
    TaskHandle task;
};
struct TaskGroupCompletion
{
    std::uint64_t id = 0;
    // Fixed-size result collection. Detailed diagnostics remain on TaskHandle.
    Core::ErrorCode code = Core::ErrorCode::Ok;
};
// No group threads: work and deadlines use an existing TaskExecutor. It must
// outlive Stop/destruction. Parent/group cancellation is linked to every child;
// each child's deadline is the earlier of its own and the group's deadline.
// Running cancellation is cooperative. Stop/destruction cancel and join; never
// destroy from the bound executor or this group's submission/cancellation scope.
class TaskGroup
{
public:
    SERVERCORE_API TaskGroup();
    SERVERCORE_API ~TaskGroup();
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    SERVERCORE_API Core::Status Start(TaskExecutor& executor, const TaskGroupOptions& options = {});
    SERVERCORE_API Core::Result<TaskGroupTask> Submit(
        TaskExecutor::Task task, const TaskOptions& options = {});
    SERVERCORE_API void CloseAdmission() noexcept;
    SERVERCORE_API void RequestCancel() noexcept;
    // Wait closes admission. It returns Ok when all children are terminal;
    // inspect individual results, including cancellation/errors, separately.
    // A wait timeout does not cancel children. Wait from bound executor is
    // InvalidArgument while unfinished, including unrelated jobs on that pool.
    SERVERCORE_API Core::Status Wait();
    SERVERCORE_API Core::Status WaitUntil(std::chrono::steady_clock::time_point deadline);
    SERVERCORE_API Core::Status Stop();
    // Closes admission; fires once all accepted children are truly terminal.
    // At most 16 pending observers. Cancellation affects only this observation.
    SERVERCORE_API Core::Result<Core::CompletionSubscription> WaitForCompletion(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {});
    [[nodiscard]] SERVERCORE_API Core::Status GetStatus() const noexcept;
    SERVERCORE_API Core::Result<std::vector<TaskGroupCompletion>> TakeCompletions();
    [[nodiscard]] SERVERCORE_API std::size_t OutstandingCount() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RetainedBytes() const noexcept;

private:
    std::shared_ptr<Detail::TaskGroupState> mState;
};
}
