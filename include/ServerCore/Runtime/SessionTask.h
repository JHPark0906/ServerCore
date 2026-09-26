#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Session/Session.h"
#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace ServerCore::Runtime
{
struct SessionTaskOptions
{
    TaskOptions task;
    // Captures/results retained by the runner continuation, including the
    // maximum result size produced by work. Admission reserves this up front.
    std::size_t completionRetainedBytes = 0;
};

// Schedule background work, then apply its status/result to a live session on
// its owning JobRunner. Reserve completion capacity before submitting the work;
// WouldBlock never starts work. Both caller cancellation and session closure
// cancel work. The continuation is discarded after session close/runner stop.
// Capture an owned result container shared by work and completion; include its
// maximum storage in BOTH declared budgets for their respective lifetimes.
// The handle observes background completion/posting, not execution of apply.
// Work must not access game-thread state; apply must not throw. Custom Sessions
// must provide GetCancellationToken() to cancel promptly on disconnect.
SERVERCORE_API Core::Result<TaskHandle> SubmitSessionTask(TaskExecutor& executor,
    JobRunner::Lease runner, const std::shared_ptr<Session::Session>& session,
    TaskExecutor::Task work, std::function<void(Session::Session&, const Core::Status&)> apply,
    const SessionTaskOptions& options = {});

// Preserve empty legacy work validation before wrapping its callable object.
template <class Work>
    requires std::same_as<std::remove_cvref_t<Work>, std::function<Core::Status(std::stop_token)>>
Core::Result<TaskHandle> SubmitSessionTask(TaskExecutor& executor, JobRunner::Lease runner,
    const std::shared_ptr<Session::Session>& session, Work&& work,
    std::function<void(Session::Session&, const Core::Status&)> apply,
    const SessionTaskOptions& options = {})
{
    auto task = work ? TaskExecutor::Task(std::forward<Work>(work)) : TaskExecutor::Task{};
    return SubmitSessionTask(
        executor, std::move(runner), session, std::move(task), std::move(apply), options);
}
}
