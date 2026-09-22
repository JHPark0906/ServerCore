#pragma once
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Session/Session.h"

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
Core::Result<TaskHandle> SubmitSessionTask(TaskExecutor& executor, JobRunner::Lease runner,
    const std::shared_ptr<Session::Session>& session, TaskExecutor::Task work,
    std::function<void(Session::Session&, const Core::Status&)> apply,
    const SessionTaskOptions& options = {});
}
