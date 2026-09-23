#include "AbiSocketTestSupport.h"
#include "ServerCore/C/RequestLimiter.h"
#include "ServerCore/C/WebPolicy.h"
namespace
{
using namespace AbiTest;
using ServerCoreTest::ExpectTrue;
using Policy = Handle<sc_http_policy, sc_http_policy_destroy>;
using Result = Handle<sc_http_policy_result, sc_http_policy_result_destroy>;
void PolicyOwnershipAndLimits()
{
    sc_http_policy_options options{};
    ExpectTrue(sc_http_policy_options_init(&options, sizeof(options)) == SC_OK,
        "policy options initialize");
    sc_http_policy* raw = nullptr;
    ExpectTrue(sc_http_policy_create(&options, &raw) == SC_OK, "policy create");
    Policy policy(raw, sc_http_policy_destroy);
    const std::string oversizedName(129, 'x');
    ExpectTrue(sc_http_policy_add_request_id(raw, Bytes("/api"), Bytes(oversizedName), 0, 128) ==
                   SC_INVALID_ARGUMENT,
        "oversized request ID header name rejected before copying");
    ExpectTrue(
        sc_http_policy_add_request_id(raw, Bytes("/api"), Bytes("x-request-id"), 0, 128) == SC_OK,
        "request ID rule copies configuration");
    sc_header common{ Bytes("x-server"), Bytes("test") };
    ExpectTrue(sc_http_policy_add_headers(raw, Bytes("/api"), &common, 1) == SC_OK,
        "common response header rule");
    auto request = View<sc_request_view>();
    request.method = Bytes("GET");
    request.target = Bytes("/api/items");
    sc_http_policy_result* resultRaw = nullptr;
    ExpectTrue(sc_http_policy_evaluate(raw, &request, nullptr, 0, &resultRaw) == SC_OK,
        "owned policy evaluation");
    Result result(resultRaw, sc_http_policy_result_destroy);
    ExpectTrue(sc_http_policy_add_headers(raw, Bytes("/"), &common, 1) == SC_CLOSED,
        "first evaluation freezes config");
    policy.reset();
    auto view = View<sc_http_policy_view>();
    ExpectTrue(sc_http_policy_result_get(result.get(), &view) == SC_OK && view.has_response == 0 &&
                   view.response_header_count == 2 && view.attribute_count == 1,
        "result owns metadata after policy destruction");
    ExpectTrue(Text(view.attributes[0].value).starts_with("sc-"),
        "request identifier exposed as attribute");
    resultRaw = nullptr;
    ExpectTrue(sc_http_json_response(Bytes("{\"a\":1}"), 201, 100, &resultRaw) == SC_OK,
        "native JSON helper");
    Result json(resultRaw, sc_http_policy_result_destroy);
    view = View<sc_http_policy_view>();
    ExpectTrue(sc_http_policy_result_get(json.get(), &view) == SC_OK && view.status == 201 &&
                   Text(view.body) == "{\"a\": 1}",
        "JSON owned response view");
    ExpectTrue(sc_http_json_response(Bytes("{bad}"), 200, 100, &resultRaw) == SC_INVALID_FORMAT &&
                   !resultRaw,
        "malformed JSON never emitted");
    ExpectTrue(sc_http_error_response(SC_WOULD_BLOCK, Bytes("req-7"), 0, &resultRaw) == SC_OK,
        "structured error helper");
    Result error(resultRaw, sc_http_policy_result_destroy);
    view = View<sc_http_policy_view>();
    ExpectTrue(sc_http_policy_result_get(error.get(), &view) == SC_OK && view.status == 429 &&
                   Text(view.body).find("req-7") != std::string::npos,
        "error mapping keeps supplied correlation");

    sc_proxy_options proxyOptions{};
    sc_proxy_options_init(&proxyOptions, sizeof(proxyOptions));
    sc_bytes cidr = Bytes("10.0.0.0/8");
    proxyOptions.trusted_cidrs = &cidr;
    proxyOptions.trusted_cidr_count = 1;
    sc_trusted_proxy* proxyRaw = nullptr;
    ExpectTrue(sc_trusted_proxy_create(&proxyOptions, &proxyRaw) == SC_OK, "trusted CIDR config");
    Handle<sc_trusted_proxy, sc_trusted_proxy_destroy> proxy(proxyRaw, sc_trusted_proxy_destroy);
    sc_ip_endpoint peer{};
    sc_ip_endpoint_parse(Bytes("10.1.2.3"), 999, &peer);
    sc_header forwarding{ Bytes("forwarded"), Bytes("for=192.0.2.1") };
    sc_proxy_peer* peerRaw = nullptr;
    ExpectTrue(sc_trusted_proxy_resolve(proxy.get(), &peer, &forwarding, 1, &peerRaw) == SC_OK,
        "C proxy resolution");
    Handle<sc_proxy_peer, sc_proxy_peer_destroy> resolved(peerRaw, sc_proxy_peer_destroy);
    proxy.reset();
    auto peerView = View<sc_proxy_peer_view>();
    ExpectTrue(sc_proxy_peer_get(resolved.get(), &peerView) == SC_OK &&
                   peerView.accepted_hops == 1 && peerView.client.address[0] == 192,
        "proxy result outlives config");

    sc_request_limiter_options limits{};
    sc_request_limiter_options_init(&limits, sizeof(limits));
    limits.max_concurrent_per_key = 1;
    sc_request_limiter* limiterRaw = nullptr;
    ExpectTrue(
        sc_request_limiter_create(&limits, &limiterRaw) == SC_OK, "limiter options and create");
    Handle<sc_request_limiter, sc_request_limiter_destroy> limiter(
        limiterRaw, sc_request_limiter_destroy);
    auto decision = View<sc_request_limit_decision>();
    sc_request_permit* permitRaw = nullptr;
    ExpectTrue(sc_request_limiter_acquire(limiter.get(), Bytes("key"), 1, &decision, &permitRaw) ==
                       SC_OK &&
                   permitRaw,
        "C limiter concurrency permit");
    Handle<sc_request_permit, sc_request_permit_destroy> permit(
        permitRaw, sc_request_permit_destroy);
    permitRaw = nullptr;
    ExpectTrue(sc_request_limiter_acquire(limiter.get(), Bytes("key"), 1, &decision, &permitRaw) ==
                       SC_OK &&
                   !permitRaw && decision.reason == SC_LIMIT_KEY_CONCURRENCY,
        "C denial reason distinct from call error");
    limiter.reset();
    permit.reset();
}
void PolicyPreflightSocket()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "socket runtime ready"))
        return;
    sc_web_options options{};
    sc_web_options_init(&options, sizeof(options));
    options.port = FreePort();
    sc_web_server* serverRaw = nullptr;
    if (!Check(sc_web_server_create(&options, &serverRaw) == SC_OK, "preflight server creates"))
        return;
    Handle<sc_web_server, sc_web_server_destroy> server(serverRaw, sc_web_server_destroy);
    ExpectTrue(
        sc_web_server_route(server.get(), Bytes("OPTIONS"), Bytes("/api/{id}"), 1) == SC_OK &&
            sc_web_server_route(server.get(), Bytes("GET"), Bytes("/api/{id}"), 1) == SC_OK &&
            sc_web_server_enable_policy(server.get()) == SC_OK,
        "explicit OPTIONS route and policy registration");
    sc_http_policy_options p{};
    sc_http_policy_options_init(&p, sizeof(p));
    sc_http_policy* policyRaw = nullptr;
    sc_http_policy_create(&p, &policyRaw);
    Policy policy(policyRaw, sc_http_policy_destroy);
    ExpectTrue(sc_http_policy_add_request_id(
                   policy.get(), Bytes("/api"), Bytes("x-request-id"), 0, 128) == SC_OK,
        "request ID before preflight");
    sc_cors_options cors{};
    sc_cors_options_init(&cors, sizeof(cors));
    sc_bytes origin = Bytes("https://app.example");
    cors.origins = &origin;
    cors.origin_count = 1;
    cors.allow_credentials = 1;
    ExpectTrue(
        sc_http_policy_add_cors(policy.get(), Bytes("/api"), &cors) == SC_OK, "CORS rule added");
    ExpectTrue(sc_web_server_start(server.get()) == SC_OK, "preflight server starts");
    Peer peer;
    if (!peer.Connect(options.port))
        return;
    peer.Send(
        "OPTIONS /api/item HTTP/1.1\r\nHost: localhost\r\nOrigin: "
        "https://app.example\r\nAccess-Control-Request-Method: GET\r\nConnection: close\r\n\r\n");
    sc_web_event* eventRaw = nullptr;
    if (!Check(sc_web_server_next(server.get(), 5000, &eventRaw) == SC_OK,
            "preflight delivered as policy event"))
        return;
    Handle<sc_web_event, sc_web_event_destroy> event(eventRaw, sc_web_event_destroy);
    ExpectTrue(sc_web_event_kind(event.get()) == SC_WEB_POLICY, "policy runs before handler");
    sc_http_policy_result* resultRaw = nullptr;
    ExpectTrue(sc_http_policy_evaluate_event(policy.get(), event.get(), &resultRaw) == SC_OK,
        "event native metadata evaluates");
    Result result(resultRaw, sc_http_policy_result_destroy);
    sc_request_decision* decisionRaw = nullptr;
    ExpectTrue(sc_web_event_decision(event.get(), &decisionRaw) == SC_OK, "claim policy decision");
    Handle<sc_request_decision, sc_request_decision_destroy> decision(
        decisionRaw, sc_request_decision_destroy);
    event.reset();
    ExpectTrue(sc_http_policy_result_apply(result.get(), decision.get()) == SC_OK,
        "preflight short-circuit applies after event release");
    auto wire = peer.UntilClosed();
    ExpectTrue(
        wire.starts_with("HTTP/1.1 204") &&
            wire.find("access-control-allow-origin: https://app.example") != std::string::npos &&
            wire.find("x-request-id: sc-") != std::string::npos,
        "204 CORS response and common ID reach real socket");
    ExpectTrue(sc_web_server_next(server.get(), 0, &eventRaw) == SC_WOULD_BLOCK,
        "short circuit does not invoke application handler");
    ExpectTrue(sc_web_server_stop(server.get()) == SC_OK, "preflight server joins");
}
ServerCoreTest::CheckRegistration a("CAbi.PolicyOwnershipAndLimits", PolicyOwnershipAndLimits);
ServerCoreTest::CheckRegistration b("CAbi.PolicyPreflightSocket", PolicyPreflightSocket);
}
