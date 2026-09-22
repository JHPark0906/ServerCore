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
#include "ServerCore/Session/Session.h"
#include "ServerCore/Session/SessionRegistry.h"
#include "ServerCore/Web/HttpServer.h"

#include <concepts>

static_assert(std::same_as<int, int>);

int main()
{
    ServerCore::Web::HttpServer http;
    const auto route = http.RegisterRoute("GET", "/health", [](const ServerCore::Web::HttpRequest&) {
        return ServerCore::Web::HttpResponse{200, {}, "ok"};
    });
    if (!route.IsOk() || http.IsRunning()) return 1;
    if (!http.RegisterWebSocket("/events", {}).IsOk()) return 1;

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
    const ServerCore::Core::Status invalidEndpoint = acceptor.Listen("not-an-ipv4", 1, 1);

    if (invalidEndpoint.Code() != ServerCore::Core::ErrorCode::InvalidArgument)
    {
        return 1;
    }

    return ServerCore::GetVersionString().empty() ? 1 : 0;
}
