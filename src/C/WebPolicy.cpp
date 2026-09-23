#include "ServerCore/C/WebPolicy.h"
#include "C/EndpointInternal.h"
#include "C/Internal.h"
#include "ServerCore/Web/HttpPolicy.h"
#include <mutex>
namespace C = ServerCore::CDetail;
namespace W = ServerCore::Web;
namespace Core = ServerCore::Core;
struct sc_trusted_proxy
{
    W::TrustedProxyPolicy value;
    size_t charge = 0;
};
struct sc_proxy_peer
{
    W::ProxyPeer value;
};
struct sc_http_policy
{
    W::HttpPolicyLimits limits;
    std::mutex mutex;
    std::vector<W::HttpPolicyRule> rules;
    std::optional<W::HttpPolicyPipeline> compiled;
    size_t configBytes = 0;
};
struct sc_http_policy_result
{
    W::HttpPolicyResult value;
    std::vector<sc_header> headers, attributes;
    explicit sc_http_policy_result(W::HttpPolicyResult result)
        : value(std::move(result))
    {
        const auto& source = value.response ? value.response->headers : value.responseHeaders;
        headers.reserve(source.size());
        attributes.reserve(value.attributes.size());
        for (const auto& [n, v] : source)
            headers.push_back({ C::View(n), C::View(v) });
        for (const auto& [n, v] : value.attributes)
            attributes.push_back({ C::View(n), C::View(v) });
    }
};
namespace
{
sc_status Headers(const sc_header* input, size_t count, W::HttpHeaders& out)
{
    if ((count && !input) || count > 256)
        return SC_INVALID_ARGUMENT;
    size_t bytes = 0;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (!C::Valid(input[i].name) || !C::Valid(input[i].value))
            return SC_INVALID_ARGUMENT;
        if (!C::Add(bytes, input[i].name.len) || !C::Add(bytes, input[i].value.len) ||
            bytes > 1024 * 1024)
            return SC_TOO_LARGE;
        out.emplace_back(C::Text(input[i].name), C::Text(input[i].value));
    }
    return SC_OK;
}
sc_status Strings(const sc_bytes* input, size_t count, std::vector<std::string>& out)
{
    if ((count && !input) || count > 256)
        return SC_INVALID_ARGUMENT;
    size_t bytes = 0;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (!C::Valid(input[i]))
            return SC_INVALID_ARGUMENT;
        if (!C::Add(bytes, input[i].len) || bytes > 16384)
            return SC_TOO_LARGE;
        out.emplace_back(C::Text(input[i]));
    }
    return SC_OK;
}
sc_status Add(
    sc_http_policy* policy, sc_bytes prefix, Core::Result<W::HttpPolicyStep> step, size_t charge)
{
    if (!policy || !C::Valid(prefix))
        return SC_INVALID_ARGUMENT;
    if (!step.IsOk())
        return C::Code(step.GetStatus());
    if (prefix.len > 16384 || !C::Add(charge, prefix.len))
        return SC_TOO_LARGE;
    W::HttpPolicyRule rule{ std::string(C::Text(prefix)), std::move(step.Value()) };
    // Validate prefix and the individual step before changing builder state.
    auto valid = W::HttpPolicyPipeline::Create({ rule }, policy->limits);
    if (!valid.IsOk())
        return C::Code(valid.GetStatus());
    std::lock_guard lock(policy->mutex);
    if (policy->compiled)
        return SC_CLOSED;
    if (policy->rules.size() >= policy->limits.maxRules ||
        charge > policy->limits.maxMetadataBytes - policy->configBytes)
        return SC_TOO_LARGE;
    policy->rules.push_back(std::move(rule));
    policy->configBytes += charge;
    return SC_OK;
}
sc_status Export(W::HttpPolicyResult value, sc_http_policy_result** out)
{
    *out = new sc_http_policy_result(std::move(value));
    return SC_OK;
}
}
extern "C"
{
    sc_status sc_http_policy_options_init(sc_http_policy_options* o, size_t size)
    {
        if (!o || size < sizeof(*o))
            return SC_INVALID_ARGUMENT;
        *o = { SC_ABI_VERSION, sizeof(*o), 64, 16384, 256 * 1024 };
        return SC_OK;
    }
    sc_status sc_cors_options_init(sc_cors_options* o, size_t size)
    {
        if (!o || size < sizeof(*o))
            return SC_INVALID_ARGUMENT;
        *o = { SC_ABI_VERSION, sizeof(*o), nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0, 600 };
        return SC_OK;
    }
    sc_status sc_proxy_options_init(sc_proxy_options* o, size_t size)
    {
        if (!o || size < sizeof(*o))
            return SC_INVALID_ARGUMENT;
        *o = { SC_ABI_VERSION, sizeof(*o), nullptr, 0, SC_PROXY_FORWARDED, 0, 0, 1, 32, 8192 };
        return SC_OK;
    }
    sc_status sc_trusted_proxy_create(const sc_proxy_options* o, sc_trusted_proxy** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Version(o) || o->mode > 1 || o->accept_proto > 1 || o->accept_host > 1 ||
            o->normalize_mapped_ipv4 > 1)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                std::vector<std::string> networks;
                auto code = Strings(o->trusted_cidrs, o->trusted_cidr_count, networks);
                if (code != SC_OK)
                    return code;
                W::TrustedProxyOptions options;
                for (const auto& text : networks)
                {
                    auto parsed = Core::IpNetwork::Parse(text);
                    if (!parsed.IsOk())
                        return C::Code(parsed.GetStatus());
                    options.trustedNetworks.push_back(parsed.Value());
                }
                options.mode = static_cast<W::ProxyHeaderMode>(o->mode);
                options.maxHops = o->max_hops;
                options.maxHeaderBytes = o->max_header_bytes;
                options.acceptProto = o->accept_proto != 0;
                options.acceptHost = o->accept_host != 0;
                options.normalizeMappedIpv4 = o->normalize_mapped_ipv4 != 0;
                auto value = W::TrustedProxyPolicy::Create(std::move(options));
                if (!value.IsOk())
                    return C::Code(value.GetStatus());
                size_t charge = 0;
                for (const auto& text : networks)
                    charge += text.size();
                *out = new sc_trusted_proxy{ std::move(value.Value()), charge };
                return SC_OK;
            });
    }
    void sc_trusted_proxy_destroy(sc_trusted_proxy* value)
    {
        delete value;
    }
    sc_status sc_trusted_proxy_resolve(const sc_trusted_proxy* proxy, const sc_ip_endpoint* peer,
        const sc_header* headers, size_t count, sc_proxy_peer** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!proxy || !peer)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                Core::IpEndpoint endpoint;
                if (!C::ToEndpoint(*peer, endpoint))
                    return SC_INVALID_ARGUMENT;
                W::HttpHeaders fields;
                auto code = Headers(headers, count, fields);
                if (code != SC_OK)
                    return code;
                auto resolved = proxy->value.Resolve(endpoint, fields);
                if (!resolved.IsOk())
                    return C::Code(resolved.GetStatus());
                *out = new sc_proxy_peer{ std::move(resolved.Value()) };
                return SC_OK;
            });
    }
    sc_status sc_proxy_peer_get(const sc_proxy_peer* peer, sc_proxy_peer_view* out)
    {
        if (!peer || !C::Version(out))
            return SC_INVALID_ARGUMENT;
        out->transport_peer = C::FromEndpoint(peer->value.transportPeer);
        out->client = C::FromEndpoint(peer->value.client);
        out->accepted_hops = peer->value.acceptedHops;
        out->proto = C::View(peer->value.proto);
        out->host = C::View(peer->value.host);
        return SC_OK;
    }
    void sc_proxy_peer_destroy(sc_proxy_peer* value)
    {
        delete value;
    }
    sc_status sc_http_policy_create(const sc_http_policy_options* o, sc_http_policy** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Version(o))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                W::HttpPolicyLimits limits{ o->max_rules, o->max_metadata_bytes,
                    o->max_response_body_bytes };
                auto valid = W::HttpPolicyPipeline::Create({}, limits);
                if (!valid.IsOk())
                    return C::Code(valid.GetStatus());
                auto value = std::make_unique<sc_http_policy>();
                value->limits = limits;
                value->rules.reserve(limits.maxRules);
                *out = value.release();
                return SC_OK;
            });
    }
    sc_status sc_http_policy_add_headers(
        sc_http_policy* p, sc_bytes prefix, const sc_header* headers, size_t count)
    {
        return C::Protect(
            [&]() -> sc_status
            {
                W::HttpHeaders fields;
                auto code = Headers(headers, count, fields);
                if (code != SC_OK)
                    return code;
                size_t bytes = 0;
                for (const auto& f : fields)
                    bytes += f.first.size() + f.second.size();
                return Add(p, prefix, W::CommonHeadersPolicy(std::move(fields)), bytes);
            });
    }
    sc_status sc_http_policy_add_cors(sc_http_policy* p, sc_bytes prefix, const sc_cors_options* o)
    {
        if (!C::Version(o) || o->allow_credentials > 1)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                W::CorsOptions options;
                options.allowedMethods.clear();
                auto code = Strings(o->origins, o->origin_count, options.allowedOrigins);
                if (code != SC_OK)
                    return code;
                code = Strings(o->methods, o->method_count, options.allowedMethods);
                if (code != SC_OK)
                    return code;
                code = Strings(o->allowed_headers, o->allowed_header_count, options.allowedHeaders);
                if (code != SC_OK)
                    return code;
                code = Strings(o->exposed_headers, o->exposed_header_count, options.exposedHeaders);
                if (code != SC_OK)
                    return code;
                if (options.allowedMethods.empty())
                    options.allowedMethods = { "GET", "HEAD", "POST" };
                size_t bytes = 0;
                for (const auto* list : { &options.allowedOrigins, &options.allowedMethods,
                         &options.allowedHeaders, &options.exposedHeaders })
                    for (const auto& s : *list)
                        bytes += s.size();
                options.allowCredentials = o->allow_credentials != 0;
                options.maxAge = std::chrono::seconds(o->max_age_seconds);
                return Add(p, prefix, W::CorsPolicy(std::move(options)), bytes);
            });
    }
    sc_status sc_http_policy_add_request_id(
        sc_http_policy* p, sc_bytes prefix, sc_bytes header, uint32_t incoming, size_t maxBytes)
    {
        if (!C::Valid(header) || header.len > 128 || incoming > 1)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                return Add(p, prefix,
                    W::RequestIdPolicy({ std::string(C::Text(header)), incoming != 0, maxBytes }),
                    header.len);
            });
    }
    sc_status sc_http_policy_add_proxy(
        sc_http_policy* p, sc_bytes prefix, const sc_trusted_proxy* proxy)
    {
        if (!proxy)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                return Add(p, prefix,
                    Core::Result<W::HttpPolicyStep>::FromValue(
                        W::ProxyMetadataPolicy(proxy->value)),
                    proxy->charge);
            });
    }
    sc_status sc_http_policy_evaluate(sc_http_policy* p, const sc_request_view* request,
        const sc_ip_endpoint* peer, uint32_t upgrade, sc_http_policy_result** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!p || !C::Version(request) || !C::Valid(request->method) ||
            !C::Valid(request->target) || upgrade > 1)
            return SC_INVALID_ARGUMENT;
        if (request->method.len > 64 || request->target.len > 16384)
            return SC_TOO_LARGE;
        return C::Protect(
            [&]() -> sc_status
            {
                W::HttpRequest input;
                input.method = C::Text(request->method);
                input.target = C::Text(request->target);
                auto code = Headers(request->headers, request->header_count, input.headers);
                if (code != SC_OK)
                    return code;
                if (peer && !C::ToEndpoint(*peer, input.remoteEndpoint))
                    return SC_INVALID_ARGUMENT;
                auto pipeline = [&]() -> Core::Result<W::HttpPolicyPipeline>
                {
                    std::lock_guard lock(p->mutex);
                    if (!p->compiled)
                    {
                        auto compiled = W::HttpPolicyPipeline::Create(p->rules, p->limits);
                        if (!compiled.IsOk())
                            return compiled;
                        p->compiled = std::move(compiled.Value());
                    }
                    return Core::Result<W::HttpPolicyPipeline>::FromValue(*p->compiled);
                }();
                if (!pipeline.IsOk())
                    return C::Code(pipeline.GetStatus());
                auto result = pipeline.Value().Evaluate(input, upgrade != 0);
                if (!result.IsOk())
                    return C::Code(result.GetStatus());
                return Export(std::move(result.Value()), out);
            });
    }
    sc_status sc_http_policy_evaluate_event(
        sc_http_policy* policy, const sc_web_event* event, sc_http_policy_result** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        sc_request_view request{};
        request.abi_version = SC_ABI_VERSION;
        request.struct_size = sizeof(request);
        auto status = sc_web_event_request(event, &request);
        if (status != SC_OK)
            return status;
        sc_request_endpoints endpoints{};
        endpoints.abi_version = SC_ABI_VERSION;
        endpoints.struct_size = sizeof(endpoints);
        status = sc_web_event_endpoints(event, &endpoints);
        if (status != SC_OK)
            return status;
        return sc_http_policy_evaluate(policy, &request, &endpoints.remote_endpoint,
            sc_web_event_policy_is_websocket(event), out);
    }
    sc_status sc_http_policy_result_get(
        const sc_http_policy_result* value, sc_http_policy_view* out)
    {
        if (!value || !C::Version(out))
            return SC_INVALID_ARGUMENT;
        const auto& result = value->value;
        out->has_response = result.response ? 1u : 0u;
        out->status = result.response ? result.response->status : 0;
        out->close = result.response && result.response->close ? 1u : 0u;
        out->response_headers = value->headers.data();
        out->response_header_count = value->headers.size();
        out->attributes = value->attributes.data();
        out->attribute_count = value->attributes.size();
        out->body = result.response ? C::View(result.response->body) : sc_bytes{};
        return SC_OK;
    }
    sc_status sc_http_policy_result_apply(
        const sc_http_policy_result* result, sc_request_decision* decision)
    {
        if (!result || !decision)
            return SC_INVALID_ARGUMENT;
        if (result->value.response)
        {
            sc_response_head head{};
            auto status = sc_response_head_init(&head, sizeof(head));
            if (status != SC_OK)
                return status;
            head.status = result->value.response->status;
            head.flags = result->value.response->close ? SC_RESPONSE_CLOSE : 0;
            head.headers = result->headers.data();
            head.header_count = result->headers.size();
            return sc_request_decision_reject(
                decision, &head, C::View(result->value.response->body));
        }
        sc_policy_allow allow{ SC_ABI_VERSION, sizeof(sc_policy_allow), result->headers.data(),
            result->headers.size(), result->attributes.data(), result->attributes.size() };
        return sc_request_decision_allow(decision, &allow);
    }
    void sc_http_policy_result_destroy(sc_http_policy_result* value)
    {
        delete value;
    }
    void sc_http_policy_destroy(sc_http_policy* value)
    {
        delete value;
    }
    sc_status sc_http_json_response(
        sc_bytes json, uint32_t status, size_t maxBytes, sc_http_policy_result** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Valid(json) || !maxBytes || maxBytes > 16 * 1024 * 1024)
            return SC_INVALID_ARGUMENT;
        if (json.len > maxBytes)
            return SC_TOO_LARGE;
        return C::Protect(
            [&]() -> sc_status
            {
                auto parsed = ServerCore::Protocol::JsonValue::Parse(C::Text(json));
                if (!parsed.IsOk())
                    return C::Code(parsed.GetStatus());
                auto response = W::JsonResponse(parsed.Value(), status, maxBytes);
                if (!response.IsOk())
                    return C::Code(response.GetStatus());
                W::HttpPolicyResult result;
                result.response = std::move(response.Value());
                return Export(std::move(result), out);
            });
    }
    sc_status sc_http_error_response(
        sc_status error, sc_bytes id, uint32_t status, sc_http_policy_result** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Valid(id) || error <= SC_OK || error > SC_CANCELLED)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto response =
                    W::ErrorResponse(static_cast<Core::ErrorCode>(error), C::Text(id), status);
                if (!response.IsOk())
                    return C::Code(response.GetStatus());
                W::HttpPolicyResult result;
                result.response = std::move(response.Value());
                return Export(std::move(result), out);
            });
    }
}
