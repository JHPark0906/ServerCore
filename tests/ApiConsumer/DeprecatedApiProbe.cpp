#include "ServerCore/Runtime/DatagramTransport.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Web/HttpServer.h"

// Each target selects exactly one old call. Its build must fail specifically
// because that declaration is deprecated, not because another alias warns.
#if SERVERCORE_DEPRECATED_API == 1
void VerifyDeprecatedApi(ServerCore::Web::HttpServer& server)
{
    (void)server.Route("GET", "/", {});
}
#elif SERVERCORE_DEPRECATED_API == 2
void VerifyDeprecatedApi(ServerCore::Web::HttpServer& server)
{
    (void)server.WebSocket("/", {});
}
#elif SERVERCORE_DEPRECATED_API == 3
void VerifyDeprecatedApi(ServerCore::Runtime::DatagramTransport& transport)
{
    (void)transport.Send(ServerCore::Session::SessionId::Invalid, {});
}
#elif SERVERCORE_DEPRECATED_API == 4
void VerifyDeprecatedApi(ServerCore::Runtime::JobRunner& runner)
{
    runner.Stop();
}
#else
#error A deprecated API probe selector is required.
#endif
