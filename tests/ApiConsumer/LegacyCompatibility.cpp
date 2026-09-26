#include "ServerCore/Runtime/DatagramTransport.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Web/HttpServer.h"

#include <cstddef>
#include <utility>

static_assert(noexcept(std::declval<ServerCore::Runtime::DatagramTransport&>().Send(
    ServerCore::Session::SessionId::Invalid, {})));

int main()
{
    using ServerCore::Core::ErrorCode;

    ServerCore::Web::HttpServer http;
    const auto handler = [](const ServerCore::Web::HttpRequest&)
    { return ServerCore::Web::HttpResponse{ 200, {}, "ok" }; };
    if (!http.Route("GET", "/legacy", handler).IsOk() ||
        http.RegisterRoute("GET", "/legacy", handler).Code() != ErrorCode::AlreadyExists ||
        !http.RegisterRoute("GET", "/canonical", handler).IsOk() ||
        http.Route("GET", "/canonical", handler).Code() != ErrorCode::AlreadyExists)
        return 1;
    if (!http.WebSocket("/legacy-socket", {}).IsOk() ||
        http.RegisterWebSocket("/legacy-socket", {}).Code() != ErrorCode::AlreadyExists ||
        !http.RegisterWebSocket("/canonical-socket", {}).IsOk() ||
        http.WebSocket("/canonical-socket", {}).Code() != ErrorCode::AlreadyExists)
        return 1;
    if (http.Route("GET", "invalid-path", handler).Code() != ErrorCode::InvalidArgument ||
        http.WebSocket("invalid-path", {}).Code() != ErrorCode::InvalidArgument)
        return 1;

    ServerCore::Runtime::DatagramTransport datagrams;
    const auto id = static_cast<ServerCore::Session::SessionId>(1);
    if (datagrams.Send(id, {}).Code() != ErrorCode::Closed ||
        datagrams.SendSerialized(id, {}).Code() != ErrorCode::Closed)
        return 1;
    if (!datagrams.Bind("127.0.0.1", 0).IsOk())
        return 1;
    const auto registered = datagrams.RegisterSession(id);
    if (!registered.IsOk() || datagrams.Send(id, {}).Code() != ErrorCode::WouldBlock ||
        datagrams.SendSerialized(id, {}).Code() != ErrorCode::WouldBlock)
        return 1;

    ServerCore::Runtime::JobRunner runner;
    unsigned int completed = 0;
    if (!runner.Post([&completed] { ++completed; }).IsOk())
        return 1;
    runner.Stop();
    if (!runner.IsStopRequested() || runner.PendingCount() != std::size_t{ 1 } ||
        runner.Post([] {}).Code() != ErrorCode::Closed)
        return 1;
    // Stop is a request. The accepted job still drains on the owning context.
    runner.RunUntilStopped();
    runner.Stop();
    runner.RequestStop();
    return completed == 1 && runner.PendingCount() == 0 ? 0 : 1;
}
