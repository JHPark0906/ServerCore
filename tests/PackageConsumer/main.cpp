#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/ByteBuffer.h"
#include "ServerCore/Core/Clock.h"
#include "ServerCore/Core/Config.h"
#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/JobQueue.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Core/Version.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/ConnectionFlowControl.h"
#include "ServerCore/Net/IoContext.h"
#include "ServerCore/Protocol/DatagramCodec.h"
#include "ServerCore/Protocol/FrameCodec.h"
#include "ServerCore/Protocol/Framing.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Runtime/DatagramTransport.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/Metrics.h"
#include "ServerCore/Runtime/PeriodicRunner.h"
#include "ServerCore/Runtime/ServerHost.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Runtime/TaskGroup.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#include "ServerCore/Runtime/KeyedExecutor.h"
#include "ServerCore/Runtime/Channel.h"
#include "ServerCore/Session/Session.h"
#include "ServerCore/Session/SessionRegistry.h"
#include "ServerCore/Web/HttpServer.h"
#include "ServerCore/Web/HttpStreaming.h"
#include "ServerCore/Core/Endpoint.h"
#include "ServerCore/Runtime/RequestLimiter.h"
#include "ServerCore/Web/HttpPolicy.h"
#include "ServerCore/Web/RequestData.h"
#include "ServerCore/Web/Multipart.h"
#include "ServerCore/Protocol/BinaryIO.h"
#include "ServerCore/Core/AtomicFile.h"
#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Runtime/TickRunner.h"
#include "ServerCore/Runtime/OutboundQueue.h"

#include <concepts>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>

static_assert(std::same_as<int, int>);

int main()
{
    auto binary = ServerCore::Protocol::BinaryWriter::Create(64);
    if (!binary.IsOk() || !binary.Value().WriteSigned(-23, 2).IsOk()) return 1;
    auto binaryRead = ServerCore::Protocol::BinaryReader::Create(binary.Value().Bytes());
    if (!binaryRead.IsOk() || binaryRead.Value().ReadSigned(2).Value() != -23) return 1;
    (void)ServerCore::Core::AtomicFile::SupportsDirectorySync();
    auto endpoint = ServerCore::Core::IpEndpoint::Parse("::1", 8080);
    if (!endpoint.IsOk() || endpoint.Value().ToString() != "[::1]:8080") return 1;
    auto limiter = ServerCore::Runtime::RequestLimiter::Create();
    if (!limiter.IsOk() || !limiter.Value().TryAcquire("consumer").Value().Allowed()) return 1;
    auto fields = ServerCore::Web::ParseFormUrlEncoded("a=one+two");
    if (!fields.IsOk()) return 1;
    auto multipart = ServerCore::Web::MultipartParser::Create("multipart/form-data; boundary=test");
    if (!multipart.IsOk()) return 1;
    multipart.Value()->Cancel();
    auto common = ServerCore::Web::CommonHeadersPolicy({{"x-consumer", "yes"}});
    if (!common.IsOk()) return 1;
    auto policies = ServerCore::Web::HttpPolicyPipeline::Create({{"/api", std::move(common.Value())}});
    if (!policies.IsOk() || !policies.Value().Evaluate({"GET", "/api/item", {}, {}}).IsOk()) return 1;
    if (!ServerCore::Web::ErrorResponse(ServerCore::Core::ErrorCode::NotFound).IsOk()) return 1;
    // The consumer starts in C++17; linking ServerCore must enable C++23.
    std::expected<std::unique_ptr<int>, int> value(std::make_unique<int>(23));
    auto result = ServerCore::Core::Result<std::unique_ptr<int>>::FromValue(std::move(*value));
    if (!result.IsOk() || *result.Value() != 23) return 1;
    ServerCore::Runtime::TaskExecutor executor;
    if (!executor.Start({1, 2, 1024}).IsOk()) return 1;
    if (!ServerCore::Observability::Observe(executor).Has(ServerCore::Observability::PendingWork)) return 1;
    std::move_only_function<ServerCore::Core::Status(std::stop_token)> work =
        [owned = std::move(result.Value())](std::stop_token) {
            return *owned == 23 ? ServerCore::Core::Status::Ok() :
                ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::InvalidArgument);
        };
    auto task = executor.Submit(std::move(work));
    if (!task.IsOk() || !task.Value().Wait().IsOk()) return 1;
    ServerCore::Runtime::TimerScheduler scheduler;
    if (!scheduler.Start(executor).IsOk()) return 1;
    auto timer = scheduler.Schedule([](std::stop_token) { return ServerCore::Core::Status::Ok(); });
    if (!timer.IsOk() || !timer.Value().Wait().IsOk() || !scheduler.Stop().IsOk()) return 1;
    ServerCore::Runtime::TaskGroup group;
    if (!group.Start(executor).IsOk()) return 1;
    auto child = group.Submit([](std::stop_token) { return ServerCore::Core::Status::Ok(); });
    if (!child.IsOk() || !group.Wait().IsOk() || !group.TakeCompletions().IsOk() || !group.Stop().IsOk()) return 1;
    if (!executor.Stop().IsOk()) return 1;
    ServerCore::Runtime::KeyedExecutor keyed;
    if (!keyed.Start().IsOk()) return 1;
    auto keyedTask = keyed.Submit(7, [](std::stop_token) { return ServerCore::Core::Status::Ok(); });
    if (!keyedTask.IsOk() || !keyedTask.Value().Wait().IsOk() || !keyed.Stop().IsOk()) return 1;
    auto channel = ServerCore::Runtime::BoundedChannel<int>::Create();
    if (!channel.IsOk() || !channel.Value().TrySend(std::make_shared<const int>(23), sizeof(int)).IsOk()) return 1;
    auto received = channel.Value().TryReceive();
    if (!received.IsOk() || *received.Value() != 23) return 1;
    ServerCore::Net::SendCapacitySubscription subscription;
    if (subscription.IsPending() || subscription.Cancel() ||
        ServerCore::Net::GetConnectionFlowControl({})) return 1;
    ServerCore::Web::HttpServer http;
    const auto route = http.RegisterRoute("GET", "/health", [](const ServerCore::Web::HttpRequest&) {
        return ServerCore::Web::HttpResponse{200, {}, "ok"};
    });
    if (!route.IsOk() || http.IsRunning()) return 1;
    if (!http.RegisterWebSocket("/events", {}).IsOk()) return 1;
    const ServerCore::Web::HttpRequest legacyRequest{"GET", "/", {}, ""};
    const ServerCore::Web::WebSocketCallbacks legacyCallbacks{{}, {}, {}, {}};
    if (!legacyRequest.PathParameter("id").empty() || legacyCallbacks.onOpenWithRequest) return 1;
    if (!http.RegisterRoutePattern("GET", "/plugins/{id}", [](const ServerCore::Web::HttpRequest& request) {
        return ServerCore::Web::HttpResponse{200, {}, std::string(request.PathParameter("id"))};
    }).IsOk()) return 1;
    if (!http.RegisterWebSocketPattern("/events/{id}", {}).IsOk()) return 1;
    if (!http.RegisterAsyncRoute("GET", "/async",
        [](std::shared_ptr<const ServerCore::Web::HttpRequestContext> context) {
            (void)context->response->Complete({200, {}, "async"});
        }).IsOk()) return 1;
    if (!http.RegisterAsyncRoutePattern("GET", "/files/{id}",
        [](std::shared_ptr<const ServerCore::Web::HttpRequestContext> context) {
            (void)context->response->Complete({200, {}, std::string(context->request.PathParameter("id"))});
        }).IsOk()) return 1;
    auto event = ServerCore::Web::EncodeSseEvent({"ready", "", {}, {}}, 128);
    if (!event.IsOk() || event.Value() != "data: ready\n\n" ||
        ServerCore::Web::GetWebSocketFlowControl({})) return 1;

    ServerCore::Runtime::DatagramTransport datagrams;
    const ServerCore::Core::Status invalidDatagramEndpoint = datagrams.Bind("not-an-ipv4", 0);
    if (invalidDatagramEndpoint.Code() != ServerCore::Core::ErrorCode::InvalidArgument ||
        datagrams.Port() != 0)
    {
        return 1;
    }
    datagrams.Close();
    if (datagrams.SendSerialized(ServerCore::Session::SessionId::Invalid, {}).Code() !=
        ServerCore::Core::ErrorCode::Closed)
        return 1;

    ServerCore::Runtime::JobRunner runner;
    unsigned int completed = 0;
    if (!runner.Post([&completed] { ++completed; }).IsOk()) return 1;
    runner.RequestStop();
    if (!runner.IsStopRequested() || runner.Post([] {}).Code() != ServerCore::Core::ErrorCode::Closed)
        return 1;
    runner.RunUntilStopped();
    if (completed != 1 || runner.PendingCount() != 0) return 1;

    ServerCore::Net::Acceptor acceptor;
    if (!acceptor.SetSendQueueLimits({1024, 4096}).IsOk()) return 1;
    const ServerCore::Core::Status invalidEndpoint = acceptor.Listen("not-an-ipv4", 1, 1);

    if (invalidEndpoint.Code() != ServerCore::Core::ErrorCode::InvalidArgument)
    {
        return 1;
    }

    return ServerCore::GetVersionString().empty() ? 1 : 0;
}
