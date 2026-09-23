#include "ServerCore/Web/HttpPolicy.h"
#include "TestHarness.h"
namespace
{
using namespace ServerCore;
using ServerCoreTest::ExpectTrue;
std::string Header(const Web::HttpHeaders& headers, std::string_view name)
{
    for (const auto& h : headers)
        if (h.first == name)
            return h.second;
    return {};
}
void GroupedPoliciesAndJson()
{
    auto headers = Web::CommonHeadersPolicy({ { "x-policy", "group" }, { "vary", "Accept" } });
    auto ids = Web::RequestIdPolicy();
    auto pipeline =
        Web::HttpPolicyPipeline::Create({ { "/api", headers.Value() }, { "/api", ids.Value() } })
            .Value();
    Web::HttpRequest request;
    request.method = "GET";
    request.target = "/%61pi/item?test=1";
    auto result = pipeline.Evaluate(request);
    ExpectTrue(result.IsOk() && Header(result.Value().responseHeaders, "x-policy") == "group" &&
                   !Header(result.Value().attributes, "request_id").empty(),
        "decoded segment group receives ordered policies");
    request.target = "/apix/item";
    ExpectTrue(pipeline.Evaluate(request).Value().responseHeaders.empty(),
        "prefix never matches partial segment");
    request.target = "/api%2fitem";
    ExpectTrue(!pipeline.Evaluate(request).IsOk(),
        "encoded separator fails closed instead of bypassing group");
    request.target = "/api/../public";
    ExpectTrue(!pipeline.Evaluate(request).IsOk(), "dot segments rejected");
    ExpectTrue(!Web::CommonHeadersPolicy({ { "content-length", "1" } }).IsOk(),
        "policy cannot override framework framing");
    auto acceptId = Web::RequestIdPolicy({ "x-request-id", true, 128 }).Value();
    request.target = "/api";
    request.headers = { { "x-request-id", "trusted-token" } };
    auto id = acceptId(request, false);
    ExpectTrue(id.IsOk() && Header(id.Value().attributes, "request_id") == "trusted-token",
        "incoming ID explicit opt-in");
    request.headers.push_back({ "X-Request-ID", "other" });
    ExpectTrue(!acceptId(request, false).IsOk(), "duplicate IDs rejected");
    request.headers = { { "content-type", "application/json; charset=utf-8" } };
    request.body = "{\"value\":18446744073709551615}";
    auto parsed = Web::ParseJsonBody(request);
    ExpectTrue(parsed.IsOk(), "JSON request helper preserves exact uint64");
    auto response = Web::JsonResponse(parsed.Value());
    ExpectTrue(response.IsOk() && response.Value().body == "{\"value\": 18446744073709551615}" &&
                   Header(response.Value().headers, "content-type") == "application/json",
        "native JSON response helper");
    auto error = Web::ErrorResponse(Core::ErrorCode::WouldBlock, "req-1");
    ExpectTrue(error.IsOk() && error.Value().status == 429 &&
                   Header(error.Value().headers, "content-type") == "application/problem+json" &&
                   Protocol::JsonValue::Parse(error.Value().body).IsOk(),
        "structured error mapping");
}
void CorsPreflightAndCredentials()
{
    Web::CorsOptions options;
    options.allowedOrigins = { "https://app.example" };
    options.allowCredentials = true;
    options.allowedHeaders = { "authorization", "x-action" };
    auto cors = Web::CorsPolicy(options);
    ExpectTrue(cors.IsOk(), "exact origin credentials configured");
    auto pipeline = Web::HttpPolicyPipeline::Create(
        { { "/", Web::CommonHeadersPolicy({ { "vary", "Accept" } }).Value() },
            { "/api", cors.Value() } })
                        .Value();
    Web::HttpRequest request;
    request.method = "OPTIONS";
    request.target = "/api/upload";
    request.headers = { { "origin", "https://app.example" },
        { "access-control-request-method", "POST" },
        { "access-control-request-headers", "Authorization, X-Action" } };
    auto result = pipeline.Evaluate(request);
    ExpectTrue(result.IsOk() && result.Value().response && result.Value().response->status == 204,
        "matching preflight returns empty response");
    if (result.IsOk() && result.Value().response)
    {
        const auto& headers = result.Value().response->headers;
        ExpectTrue(Header(headers, "access-control-allow-origin") == "https://app.example" &&
                       Header(headers, "access-control-allow-credentials") == "true",
            "credential headers use exact origin");
        ExpectTrue(Header(headers, "vary").find("Origin") != std::string::npos &&
                       Header(headers, "vary").find("Accept") != std::string::npos,
            "Vary fields accumulate across rules");
    }
    request.headers[0].second = "https://evil.example";
    ExpectTrue(
        pipeline.Evaluate(request).Value().response->status == 403, "unapproved origin rejected");
    request.headers[0].second = "https://app.example";
    request.headers[2].second = "x-unapproved";
    ExpectTrue(pipeline.Evaluate(request).Value().response->status == 403,
        "unapproved requested header rejected");
    request.headers.push_back({ "origin", "https://app.example" });
    ExpectTrue(!pipeline.Evaluate(request).IsOk(), "duplicate Origin cannot bypass policy");
    options.allowedOrigins = { "*" };
    ExpectTrue(!Web::CorsPolicy(options).IsOk(), "wildcard credentials configuration rejected");
    options.allowCredentials = false;
    auto wildcard = Web::CorsPolicy(options).Value();
    request.headers = { { "origin", "null" } };
    request.method = "GET";
    ExpectTrue(Header(wildcard(request, false).Value().responseHeaders,
                   "access-control-allow-origin") == "*",
        "noncredential wildcard is explicit");
    ExpectTrue(wildcard(request, true).Value().responseHeaders.empty(),
        "WebSocket authorization is separate from browser CORS");
    request.headers.push_back({ "Origin", "not a serialized origin" });
    auto upgrade = wildcard(request, true);
    ExpectTrue(upgrade.IsOk() && upgrade.Value().responseHeaders.empty() &&
                   upgrade.Value().attributes.empty() && !upgrade.Value().response,
        "WebSocket upgrade bypasses CORS even with duplicate malformed Origin fields");
}
void TrustedProxyBoundaries()
{
    Web::TrustedProxyOptions options;
    options.trustedNetworks = { Core::IpNetwork::Parse("10.0.0.0/8").Value(),
        Core::IpNetwork::Parse("2001:db8:ffff::/48").Value() };
    options.acceptProto = true;
    options.acceptHost = true;
    auto policy = Web::TrustedProxyPolicy::Create(options).Value();
    auto peer = Core::IpEndpoint::Parse("10.0.0.1", 4567).Value();
    auto headers = Web::HttpHeaders{ { "forwarded",
        "for=192.0.2.1;proto=https;host=app.example, for=10.0.0.2;proto=http" } };
    auto result = policy.Resolve(peer, headers);
    ExpectTrue(result.IsOk() && result.Value().client.address.ToString() == "192.0.2.1" &&
                   result.Value().acceptedHops == 2 && result.Value().proto == "https" &&
                   result.Value().transportPeer == peer,
        "trusted chain follows nearest-to-farthest and preserves socket peer");
    headers = { { "forwarded", "for=203.0.113.1;proto=https, for=192.0.2.9;proto=http" } };
    result = policy.Resolve(peer, headers);
    ExpectTrue(result.IsOk() && result.Value().client.address.ToString() == "192.0.2.9" &&
                   result.Value().acceptedHops == 1 && result.Value().proto == "http",
        "forged leftmost values ignored past untrusted boundary");
    headers = { { "forwarded", "malformed garbage" } };
    auto direct = Core::IpEndpoint::Parse("192.0.2.22", 444).Value();
    ExpectTrue(policy.Resolve(direct, headers).Value().client == direct,
        "untrusted socket ignores even malformed forwarding headers");
    ExpectTrue(
        !policy.Resolve(peer, headers).IsOk(), "malformed header from configured proxy rejected");
    for (const auto field : { "for=192.0.2.1;For=198.51.100.1", "for=192.0.2.1,", "for=\"[:::1]\"",
             "for=192.0.2.1;proto=javascript", "for=192.0.2.1;host=\"user@app.example\"" })
        ExpectTrue(!policy.Resolve(peer, { { "forwarded", field } }).IsOk(),
            "ambiguous or invalid forwarding metadata rejected");
    result = policy.Resolve(peer, { { "forwarded", "for=\"[2001:db8::123]:443\";proto=https" } });
    ExpectTrue(result.IsOk() && result.Value().client.port == 443 &&
                   result.Value().client.address.Family() == Core::IpFamily::V6,
        "quoted IPv6 and port parsed");
    result = policy.Resolve(peer, { { "forwarded", "for=192.0.2.1,for=unknown" } });
    ExpectTrue(result.IsOk() && result.Value().acceptedHops == 0, "unknown node stops trust walk");
    options.mode = Web::ProxyHeaderMode::XForwarded;
    auto legacy = Web::TrustedProxyPolicy::Create(options).Value();
    result = legacy.Resolve(peer,
        { { "x-forwarded-for", "192.0.2.1, 10.0.0.2" }, { "x-forwarded-proto", "https, http" },
            { "x-forwarded-host", "app.example, proxy.example" } });
    ExpectTrue(
        result.IsOk() && result.Value().acceptedHops == 2 && result.Value().host == "app.example",
        "explicit legacy lists keep hop correlation");
    ExpectTrue(!legacy
                   .Resolve(peer, { { "x-forwarded-for", "192.0.2.1,10.0.0.2" },
                                      { "x-forwarded-proto", "https" } })
                   .IsOk(),
        "misaligned legacy metadata rejected");
    options.maxHops = 1;
    auto bounded = Web::TrustedProxyPolicy::Create(options).Value();
    auto tooMany = bounded.Resolve(peer, { { "x-forwarded-for", "192.0.2.1,10.0.0.2" } });
    ExpectTrue(!tooMany.IsOk() && tooMany.GetStatus().Code() == Core::ErrorCode::TooLarge,
        "hop storage is bounded with a distinct limit error");
}
ServerCoreTest::CheckRegistration a("Web.GroupedPoliciesAndJson", GroupedPoliciesAndJson);
ServerCoreTest::CheckRegistration b("Web.CorsPreflightAndCredentials", CorsPreflightAndCredentials);
ServerCoreTest::CheckRegistration c("Web.TrustedProxyBoundaries", TrustedProxyBoundaries);
}
