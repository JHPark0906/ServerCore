#include "ServerCore/Runtime/SessionTask.h"
#include <utility>

namespace ServerCore::Runtime
{
namespace
{
struct ForwardCancellation
{
    std::stop_source source;
    void operator()() noexcept { (void)source.request_stop(); }
};
struct LinkedCancellation
{
    std::stop_source source;
    std::stop_callback<ForwardCancellation> session;
    std::stop_callback<ForwardCancellation> caller;
    LinkedCancellation(std::stop_token sessionToken, std::stop_token callerToken)
        : session(sessionToken, ForwardCancellation{ source })
        , caller(callerToken, ForwardCancellation{ source })
    {
    }
};
struct Payload
{
    std::weak_ptr<Session::Session> session;
    std::stop_token sessionToken;
    std::function<void(Session::Session&, const Core::Status&)> apply;
    Core::Status result = Core::Status::Ok();
};
struct Bridge
{
    JobRunner::Reservation reservation;
    std::shared_ptr<Payload> payload;
    std::function<void()> invoke;
    std::shared_ptr<LinkedCancellation> cancellation;
};
}
Core::Result<TaskHandle> SubmitSessionTask(TaskExecutor& executor, JobRunner::Lease runner,
    const std::shared_ptr<Session::Session>& session, TaskExecutor::Task work,
    std::function<void(Session::Session&, const Core::Status&)> apply,
    const SessionTaskOptions& options)
{
    using Result = Core::Result<TaskHandle>;
    if (!session || !work || !apply)
        return Result::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
    auto reserved = runner.Reserve(options.completionRetainedBytes);
    if (!reserved.IsOk())
        return Result::FromStatus(std::move(reserved).TakeStatus());
    try
    {
        auto bridge = std::make_shared<Bridge>();
        bridge->reservation = std::move(reserved.Value());
        bridge->payload = std::make_shared<Payload>();
        auto& payload = *bridge->payload;
        payload.session = session;
        payload.sessionToken = session->GetCancellationToken();
        payload.apply = std::move(apply);
        bridge->cancellation =
            std::make_shared<LinkedCancellation>(payload.sessionToken, options.task.parentToken);
        bridge->invoke = [value = bridge->payload]
        {
            const auto target = value->session.lock();
            if (!target || value->sessionToken.stop_requested())
                return;
            const auto state = target->State();
            if (state != Session::SessionState::Connected &&
                state != Session::SessionState::Authenticated)
                return;
            value->apply(*target, value->result);
        };
        auto taskOptions = options.task;
        taskOptions.parentToken = bridge->cancellation->source.get_token();
        return executor.SubmitWithCompletion(
            std::move(work),
            [bridge](const Core::Status& result)
            {
                try
                {
                    bridge->payload->result = result;
                }
                catch (...)
                {
                    bridge->payload->result = Core::Status::AllocationFailure();
                }
                // The callback and queue node were allocated before work started.
                // Only forced runner stop can reject this reserved completion.
                (void)bridge->reservation.Post(std::move(bridge->invoke));
            },
            taskOptions);
    }
    catch (...)
    {
        return Result::FromStatus(Core::Status::AllocationFailure());
    }
}
}
