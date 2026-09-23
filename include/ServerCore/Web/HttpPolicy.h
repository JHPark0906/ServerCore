#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Web/HttpServer.h"
#include "ServerCore/Web/TrustedProxy.h"

namespace ServerCore::Web
{
struct HttpPolicyResult
{
    HttpHeaders responseHeaders;
    HttpHeaders attributes;
    std::optional<HttpResponse> response;
};
using HttpPolicyStep =
    std::function<Core::Result<HttpPolicyResult>(const HttpRequest&, bool webSocketUpgrade)>;
struct HttpPolicyRule
{
    std::string pathPrefix = "/";
    HttpPolicyStep step;
};
struct HttpPolicyLimits
{
    std::size_t maxRules = 64;
    std::size_t maxMetadataBytes = 16384;
    std::size_t maxResponseBodyBytes = 256 * 1024;
};
// Immutable chain; custom steps may run concurrently and must synchronize their captures.
// Limits bound returned logical metadata, not arbitrary callback captures or allocator overhead.
// Matching rules run in registration order;
// prefixes match decoded whole path segments. Invalid/ambiguous paths fail closed.
// First complete response short-circuits. Other results accumulate, later headers
// replace earlier names (Set-Cookie appends, Vary combines). Steps must be short;
// retain decisions using the existing asynchronous RequestPolicy when required.
class HttpPolicyPipeline
{
public:
    SERVERCORE_API static Core::Result<HttpPolicyPipeline> Create(
        std::vector<HttpPolicyRule> rules, HttpPolicyLimits limits = {});
    SERVERCORE_API Core::Result<HttpPolicyResult> Evaluate(const HttpRequest& request, bool upgrade = false) const;
    // Suitable for HttpServer::SetRequestPolicy and WebSocketCallbacks::authorize.
    SERVERCORE_API RequestPolicy AsRequestPolicy() const;

private:
    struct State;
    explicit HttpPolicyPipeline(std::shared_ptr<const State> state)
        : mState(std::move(state))
    {
    }
    std::shared_ptr<const State> mState;
};
struct CorsOptions
{
    // Exact serialized origins. "*" is allowed only without credentials.
    // Without "*", opaque "null" must be explicitly listed. No suffix/domain matching.
    std::vector<std::string> allowedOrigins;
    std::vector<std::string> allowedMethods{ "GET", "HEAD", "POST" };
    std::vector<std::string> allowedHeaders;
    std::vector<std::string> exposedHeaders;
    bool allowCredentials = false;
    std::chrono::seconds maxAge{ 600 };
};
struct RequestIdOptions
{
    std::string headerName = "x-request-id";
    bool acceptIncoming = false;
    std::size_t maxBytes = 128;
};
SERVERCORE_API Core::Result<HttpPolicyStep> CorsPolicy(CorsOptions options);
SERVERCORE_API Core::Result<HttpPolicyStep> CommonHeadersPolicy(HttpHeaders headers);
SERVERCORE_API Core::Result<HttpPolicyStep> RequestIdPolicy(RequestIdOptions options = {});
SERVERCORE_API HttpPolicyStep ProxyMetadataPolicy(TrustedProxyPolicy policy);
// Register OPTIONS explicitly so existing path/method resolution still precedes
// policy admission. Register the pipeline on the server before Start.
SERVERCORE_API Core::Status RegisterCorsPreflight(HttpServer& server, std::string_view path, bool pattern = false);
SERVERCORE_API Core::Result<Protocol::JsonValue> ParseJsonBody(
    const HttpRequest& request, std::size_t maxBytes = 65536);
SERVERCORE_API Core::Result<HttpResponse> JsonResponse(
    const Protocol::JsonValue& value, unsigned status = 200, std::size_t maxBytes = 256 * 1024);
// RFC 9457-shaped error JSON; does not expose internal Status messages.
SERVERCORE_API Core::Result<HttpResponse> ErrorResponse(
    Core::ErrorCode error, std::string_view requestId = {}, unsigned status = 0);
}
