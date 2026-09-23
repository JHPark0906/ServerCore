#include "ServerCore/Web/HttpPolicy.h"
#include "ServerCore/Web/RequestData.h"
#include "Web/PolicyInternal.h"
#include "Web/WebProtocol.h"
#include <atomic>
#include <set>

namespace ServerCore::Web
{
namespace
{
using Core::ErrorCode;
using Core::Result;
using Core::Status;
template <class T> Result<T> Fail(ErrorCode code)
{
    return Result<T>::FromStatus(Status::FailWithoutMessage(code));
}
Result<std::string> CanonicalPath(std::string_view raw)
{
    if (raw.empty() || raw.front() != '/' || raw.size() > 16384 ||
        raw.find_first_of("?#") != std::string_view::npos)
        return Fail<std::string>(ErrorCode::InvalidFormat);
    std::string path;
    std::size_t begin = 1;
    path.push_back('/');
    while (begin <= raw.size())
    {
        auto end = raw.find('/', begin);
        if (end == std::string_view::npos)
            end = raw.size();
        auto decoded = PercentDecodeComponent(raw.substr(begin, end - begin), false, 16384);
        if (!decoded.IsOk())
            return Fail<std::string>(decoded.GetStatus().Code());
        const auto& part = decoded.Value();
        if (part == "." || part == "..")
            return Fail<std::string>(ErrorCode::InvalidFormat);
        for (unsigned char c : part)
            if (c < 32 || c == 127 || c == '/' || c == '\\')
                return Fail<std::string>(ErrorCode::InvalidFormat);
        path.append(part);
        if (end == raw.size())
            break;
        path.push_back('/');
        begin = end + 1;
    }
    return Result<std::string>::FromValue(std::move(path));
}
bool Prefix(std::string_view path, std::string_view prefix) noexcept
{
    return prefix == "/" || (path.starts_with(prefix) &&
                                (path.size() == prefix.size() || path[prefix.size()] == '/'));
}
bool PolicyHeader(std::string_view name, std::string_view value) noexcept
{
    return Detail::Token(name) && Detail::HeaderValue(value) &&
           !Detail::Same(name, "content-length") && !Detail::Same(name, "transfer-encoding") &&
           !Detail::Same(name, "connection") && !Detail::Same(name, "trailer") &&
           !Detail::Same(name, "upgrade");
}
bool Account(const HttpHeaders& fields, std::size_t& bytes, std::size_t max, bool headers)
{
    if (fields.size() > 256)
        return false;
    for (const auto& [name, value] : fields)
    {
        if (headers && !PolicyHeader(name, value))
            return false;
        if (name.size() > max - bytes)
            return false;
        bytes += name.size();
        if (value.size() > max - bytes)
            return false;
        bytes += value.size();
    }
    return true;
}
void Merge(HttpHeaders& target, HttpHeaders source, bool headers)
{
    for (auto& [name, value] : source)
    {
        if (headers && Detail::Same(name, "vary"))
        {
            for (const auto& previous : target)
                if (Detail::Same(previous.first, "vary"))
                    value = previous.second + ", " + value;
        }
        if (headers)
            Detail::SetHeader(target, std::move(name), std::move(value));
        else
        {
            std::erase_if(target, [&](const auto& item) { return item.first == name; });
            target.emplace_back(std::move(name), std::move(value));
        }
    }
}
Result<std::optional<std::string_view>> Single(const HttpHeaders& fields, std::string_view name)
{
    std::optional<std::string_view> value;
    for (const auto& field : fields)
        if (Detail::Same(field.first, name))
        {
            if (value)
                return Fail<std::optional<std::string_view>>(ErrorCode::InvalidFormat);
            value = Detail::Trim(field.second);
        }
    return Result<std::optional<std::string_view>>::FromValue(value);
}
bool Origin(std::string_view text)
{
    if (text == "null")
        return true;
    auto split = text.find("://");
    return split != std::string_view::npos &&
           (text.substr(0, split) == "http" || text.substr(0, split) == "https") &&
           Detail::Authority(text.substr(split + 3));
}
std::string Join(const std::vector<std::string>& values)
{
    std::string out;
    for (const auto& value : values)
    {
        if (!out.empty())
            out += ", ";
        out += value;
    }
    return out;
}
HttpPolicyResult Rejected(unsigned status)
{
    HttpPolicyResult out;
    out.responseHeaders.emplace_back("vary", "Origin");
    out.response = HttpResponse{ status, {}, {}, false };
    return out;
}
}
struct HttpPolicyPipeline::State
{
    std::vector<HttpPolicyRule> rules;
    HttpPolicyLimits limits;
};
Result<HttpPolicyPipeline> HttpPolicyPipeline::Create(
    std::vector<HttpPolicyRule> rules, HttpPolicyLimits limits)
{
    try
    {
        if (!limits.maxRules || limits.maxRules > 256 || rules.size() > limits.maxRules ||
            !limits.maxMetadataBytes || limits.maxMetadataBytes > 1024 * 1024 ||
            !limits.maxResponseBodyBytes || limits.maxResponseBodyBytes > 16 * 1024 * 1024)
            return Fail<HttpPolicyPipeline>(ErrorCode::InvalidArgument);
        std::size_t bytes = 0;
        for (auto& rule : rules)
        {
            if (!rule.step)
                return Fail<HttpPolicyPipeline>(ErrorCode::InvalidArgument);
            auto path = CanonicalPath(rule.pathPrefix);
            if (!path.IsOk())
                return Fail<HttpPolicyPipeline>(ErrorCode::InvalidArgument);
            rule.pathPrefix = std::move(path.Value());
            if (rule.pathPrefix.size() > 1 && rule.pathPrefix.back() == '/')
                rule.pathPrefix.pop_back();
            if (rule.pathPrefix.size() > limits.maxMetadataBytes - bytes)
                return Fail<HttpPolicyPipeline>(ErrorCode::TooLarge);
            bytes += rule.pathPrefix.size();
        }
        return Result<HttpPolicyPipeline>::FromValue(
            HttpPolicyPipeline(std::make_shared<const State>(State{ std::move(rules), limits })));
    }
    catch (...)
    {
        return Fail<HttpPolicyPipeline>(ErrorCode::PlatformError);
    }
}
Result<HttpPolicyResult> HttpPolicyPipeline::Evaluate(
    const HttpRequest& request, bool upgrade) const
{
    try
    {
        auto path = CanonicalPath(request.Path());
        if (!path.IsOk())
            return Fail<HttpPolicyResult>(path.GetStatus().Code());
        HttpPolicyResult result;
        for (const auto& rule : mState->rules)
        {
            if (!Prefix(path.Value(), rule.pathPrefix))
                continue;
            auto evaluated = rule.step(request, upgrade);
            if (!evaluated.IsOk())
                return Fail<HttpPolicyResult>(evaluated.GetStatus().Code());
            auto value = std::move(evaluated.Value());
            std::size_t size = 0;
            if (!Account(value.responseHeaders, size, mState->limits.maxMetadataBytes, true) ||
                !Account(value.attributes, size, mState->limits.maxMetadataBytes, false))
                return Fail<HttpPolicyResult>(ErrorCode::TooLarge);
            Merge(result.responseHeaders, std::move(value.responseHeaders), true);
            Merge(result.attributes, std::move(value.attributes), false);
            size = 0;
            if (!Account(result.responseHeaders, size, mState->limits.maxMetadataBytes, true) ||
                !Account(result.attributes, size, mState->limits.maxMetadataBytes, false))
                return Fail<HttpPolicyResult>(ErrorCode::TooLarge);
            if (value.response)
            {
                if (value.response->body.size() > mState->limits.maxResponseBodyBytes ||
                    !Account(value.response->headers, size, mState->limits.maxMetadataBytes, true))
                    return Fail<HttpPolicyResult>(ErrorCode::TooLarge);
                auto headers = std::move(result.responseHeaders);
                Merge(headers, std::move(value.response->headers), true);
                value.response->headers = std::move(headers);
                result.responseHeaders.clear();
                result.response = std::move(value.response);
                break;
            }
        }
        return Result<HttpPolicyResult>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Fail<HttpPolicyResult>(ErrorCode::PlatformError);
    }
}
RequestPolicy HttpPolicyPipeline::AsRequestPolicy() const
{
    return [pipeline = *this](std::shared_ptr<const HttpPolicyContext> context)
    {
        if (!context || !context->decision)
            return;
        auto value = pipeline.Evaluate(context->request, context->webSocketUpgrade);
        if (!value.IsOk())
        {
            auto response = ErrorResponse(value.GetStatus().Code());
            if (!response.IsOk() || !context->decision->Reject(response.Value()).IsOk())
                context->decision->Abort();
            return;
        }
        auto& result = value.Value();
        auto status = result.response ? context->decision->Reject(*result.response)
                                      : context->decision->Allow(std::move(result.responseHeaders),
                                            std::move(result.attributes));
        if (!status.IsOk())
            context->decision->Abort();
    };
}
Result<HttpPolicyStep> CommonHeadersPolicy(HttpHeaders headers)
{
    std::size_t bytes = 0;
    if (!Account(headers, bytes, 16384, true))
        return Fail<HttpPolicyStep>(ErrorCode::InvalidArgument);
    try
    {
        return Result<HttpPolicyStep>::FromValue(
            [headers = std::move(headers)](const HttpRequest&, bool)
            {
                HttpPolicyResult result;
                result.responseHeaders = headers;
                return Result<HttpPolicyResult>::FromValue(std::move(result));
            });
    }
    catch (...)
    {
        return Fail<HttpPolicyStep>(ErrorCode::PlatformError);
    }
}
Result<HttpPolicyStep> RequestIdPolicy(RequestIdOptions options)
{
    if (!Detail::Token(options.headerName) || !PolicyHeader(options.headerName, "id") ||
        options.headerName.size() > 128 || options.maxBytes < 24 || options.maxBytes > 1024)
        return Fail<HttpPolicyStep>(ErrorCode::InvalidArgument);
    try
    {
        return Result<HttpPolicyStep>::FromValue(
            [options = std::move(options)](const HttpRequest& request, bool)
            {
                std::string id;
                if (options.acceptIncoming)
                {
                    auto incoming = Single(request.headers, options.headerName);
                    if (!incoming.IsOk())
                        return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                    if (incoming.Value())
                    {
                        if (incoming.Value()->size() > options.maxBytes ||
                            !Detail::Token(*incoming.Value()))
                            return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                        id = *incoming.Value();
                    }
                }
                if (id.empty())
                {
                    static std::atomic<std::uint64_t> counter{ 1 };
                    auto next = counter.load(std::memory_order_relaxed);
                    do
                    {
                        if (next == UINT64_MAX)
                            return Fail<HttpPolicyResult>(ErrorCode::TooLarge);
                    } while (
                        !counter.compare_exchange_weak(next, next + 1, std::memory_order_relaxed));
                    id = "sc-" + std::to_string(next);
                }
                HttpPolicyResult result;
                result.responseHeaders.emplace_back(options.headerName, id);
                result.attributes.emplace_back("request_id", std::move(id));
                return Result<HttpPolicyResult>::FromValue(std::move(result));
            });
    }
    catch (...)
    {
        return Fail<HttpPolicyStep>(ErrorCode::PlatformError);
    }
}
Result<HttpPolicyStep> CorsPolicy(CorsOptions options)
{
    try
    {
        if (options.allowedOrigins.empty() || options.allowedOrigins.size() > 128 ||
            options.allowedMethods.empty() || options.allowedMethods.size() > 64 ||
            options.allowedHeaders.size() > 128 || options.exposedHeaders.size() > 128 ||
            options.maxAge.count() < 0 || options.maxAge > std::chrono::hours(24))
            return Fail<HttpPolicyStep>(ErrorCode::InvalidArgument);
        bool wildcard = false;
        std::size_t bytes = 0;
        for (const auto& origin : options.allowedOrigins)
        {
            bytes += origin.size();
            if (origin == "*")
                wildcard = true;
            else if (!Origin(origin))
                return Fail<HttpPolicyStep>(ErrorCode::InvalidArgument);
        }
        if (wildcard && (options.allowCredentials || options.allowedOrigins.size() != 1))
            return Fail<HttpPolicyStep>(ErrorCode::InvalidArgument);
        for (auto* values :
            { &options.allowedMethods, &options.allowedHeaders, &options.exposedHeaders })
            for (auto& value : *values)
            {
                bytes += value.size();
                if (!Detail::Token(value) || value == "*")
                    return Fail<HttpPolicyStep>(ErrorCode::InvalidArgument);
            }
        if (bytes > 16384)
            return Fail<HttpPolicyStep>(ErrorCode::TooLarge);
        for (auto& value : options.allowedHeaders)
            value = Detail::Lower(value);
        return Result<HttpPolicyStep>::FromValue(
            [options = std::move(options), wildcard](const HttpRequest& request, bool upgrade)
            {
                HttpPolicyResult result;
                // CORS is HTTP browser policy; WebSocket Origin authorization is an
                // application rule and is deliberately not inferred from CORS.
                if (upgrade)
                    return Result<HttpPolicyResult>::FromValue(std::move(result));
                auto origin = Single(request.headers, "origin");
                if (!origin.IsOk())
                    return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                result.responseHeaders.emplace_back("vary", "Origin");
                if (!origin.Value())
                    return Result<HttpPolicyResult>::FromValue(std::move(result));
                if (!Origin(*origin.Value()))
                    return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                if (!wildcard &&
                    std::find(options.allowedOrigins.begin(), options.allowedOrigins.end(),
                        *origin.Value()) == options.allowedOrigins.end())
                    return Result<HttpPolicyResult>::FromValue(Rejected(403));
                result.responseHeaders.emplace_back(
                    "access-control-allow-origin", wildcard ? "*" : std::string(*origin.Value()));
                if (options.allowCredentials)
                    result.responseHeaders.emplace_back("access-control-allow-credentials", "true");
                auto method = Single(request.headers, "access-control-request-method");
                if (!method.IsOk())
                    return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                if (request.method == "OPTIONS" && method.Value())
                {
                    if (!Detail::Token(*method.Value()))
                        return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                    if (std::find(options.allowedMethods.begin(), options.allowedMethods.end(),
                            *method.Value()) == options.allowedMethods.end())
                        return Result<HttpPolicyResult>::FromValue(Rejected(403));
                    auto requested = Single(request.headers, "access-control-request-headers");
                    if (!requested.IsOk())
                        return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                    std::vector<std::string> allowed;
                    if (requested.Value())
                    {
                        auto remaining = *requested.Value();
                        if (remaining.size() > 8192)
                            return Fail<HttpPolicyResult>(ErrorCode::TooLarge);
                        while (true)
                        {
                            const auto comma = remaining.find(',');
                            auto name = Detail::Lower(Detail::Trim(remaining.substr(0, comma)));
                            if (!Detail::Token(name) || allowed.size() >= 128)
                                return Fail<HttpPolicyResult>(ErrorCode::InvalidFormat);
                            if (std::find(options.allowedHeaders.begin(),
                                    options.allowedHeaders.end(),
                                    name) == options.allowedHeaders.end())
                                return Result<HttpPolicyResult>::FromValue(Rejected(403));
                            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
                                allowed.push_back(std::move(name));
                            if (comma == std::string_view::npos)
                                break;
                            remaining.remove_prefix(comma + 1);
                        }
                    }
                    result.responseHeaders.emplace_back(
                        "access-control-allow-methods", Join(options.allowedMethods));
                    if (!allowed.empty())
                        result.responseHeaders.emplace_back(
                            "access-control-allow-headers", Join(allowed));
                    result.responseHeaders.emplace_back(
                        "access-control-max-age", std::to_string(options.maxAge.count()));
                    result.responseHeaders.emplace_back(
                        "vary", "Access-Control-Request-Method, Access-Control-Request-Headers");
                    result.response = HttpResponse{ 204, {}, {}, false };
                }
                else if (!options.exposedHeaders.empty())
                    result.responseHeaders.emplace_back(
                        "access-control-expose-headers", Join(options.exposedHeaders));
                return Result<HttpPolicyResult>::FromValue(std::move(result));
            });
    }
    catch (...)
    {
        return Fail<HttpPolicyStep>(ErrorCode::PlatformError);
    }
}
HttpPolicyStep ProxyMetadataPolicy(TrustedProxyPolicy policy)
{
    return [policy = std::move(policy)](const HttpRequest& request, bool)
    {
        auto resolved = policy.Resolve(request);
        if (!resolved.IsOk())
            return Fail<HttpPolicyResult>(resolved.GetStatus().Code());
        const auto& peer = resolved.Value();
        HttpPolicyResult result;
        result.attributes.emplace_back("client_ip", peer.client.address.ToString());
        result.attributes.emplace_back("client_port", std::to_string(peer.client.port));
        if (!peer.proto.empty())
            result.attributes.emplace_back("forwarded_proto", peer.proto);
        if (!peer.host.empty())
            result.attributes.emplace_back("forwarded_host", peer.host);
        return Result<HttpPolicyResult>::FromValue(std::move(result));
    };
}
Core::Status RegisterCorsPreflight(HttpServer& server, std::string_view path, bool pattern)
{
    auto handler = [](const HttpRequest&) { return HttpResponse{ 204, {}, {}, false }; };
    return pattern ? server.RegisterRoutePattern("OPTIONS", path, handler)
                   : server.RegisterRoute("OPTIONS", path, handler);
}
Result<Protocol::JsonValue> ParseJsonBody(const HttpRequest& request, std::size_t maxBytes)
{
    if (!maxBytes || request.body.size() > maxBytes)
        return Fail<Protocol::JsonValue>(ErrorCode::TooLarge);
    auto contentType = Single(request.headers, "content-type");
    if (!contentType.IsOk() || !contentType.Value())
        return Fail<Protocol::JsonValue>(ErrorCode::InvalidFormat);
    auto value = *contentType.Value();
    auto semi = value.find(';');
    if (!Detail::Same(Detail::Trim(value.substr(0, semi)), "application/json"))
        return Fail<Protocol::JsonValue>(ErrorCode::InvalidFormat);
    if (semi != std::string_view::npos)
    {
        auto parameter = Detail::Trim(value.substr(semi + 1));
        if (!Detail::Same(parameter, "charset=utf-8") &&
            !Detail::Same(parameter, "charset=\"utf-8\""))
            return Fail<Protocol::JsonValue>(ErrorCode::InvalidFormat);
    }
    return Protocol::JsonValue::Parse(request.body);
}
Result<HttpResponse> JsonResponse(
    const Protocol::JsonValue& value, unsigned status, std::size_t maxBytes)
{
    if (status < 200 || status > 599 || !maxBytes)
        return Fail<HttpResponse>(ErrorCode::InvalidArgument);
    auto encoded = value.Dump();
    if (!encoded.IsOk())
        return Fail<HttpResponse>(encoded.GetStatus().Code());
    if (encoded.Value().size() > maxBytes)
        return Fail<HttpResponse>(ErrorCode::TooLarge);
    try
    {
        return Result<HttpResponse>::FromValue(HttpResponse{ status,
            { { "content-type", "application/json" } }, std::move(encoded.Value()), false });
    }
    catch (...)
    {
        return Fail<HttpResponse>(ErrorCode::PlatformError);
    }
}
Result<HttpResponse> ErrorResponse(
    Core::ErrorCode error, std::string_view requestId, unsigned status)
{
    if (error == ErrorCode::Ok || requestId.size() > 1024)
        return Fail<HttpResponse>(ErrorCode::InvalidArgument);
    if (!status)
    {
        switch (error)
        {
        case ErrorCode::InvalidArgument:
        case ErrorCode::InvalidFormat:
            status = 400;
            break;
        case ErrorCode::TooLarge:
            status = 413;
            break;
        case ErrorCode::NotFound:
            status = 404;
            break;
        case ErrorCode::AlreadyExists:
            status = 409;
            break;
        case ErrorCode::Closed:
            status = 503;
            break;
        case ErrorCode::WouldBlock:
            status = 429;
            break;
        case ErrorCode::Timeout:
            status = 504;
            break;
        case ErrorCode::Cancelled:
            status = 408;
            break;
        case ErrorCode::Unimplemented:
            status = 501;
            break;
        default:
            status = 500;
            break;
        }
    }
    if (status < 400 || status > 599)
        return Fail<HttpResponse>(ErrorCode::InvalidArgument);
    try
    {
        Protocol::JsonValue::Object body;
        body.emplace("type", Protocol::JsonValue(std::string("about:blank")));
        const auto title = status == 409   ? std::string_view("Conflict")
                           : status == 429 ? std::string_view("Too Many Requests")
                           : status == 504 ? std::string_view("Gateway Timeout")
                                           : Detail::ReasonPhrase(status);
        body.emplace("title", Protocol::JsonValue(std::string(title)));
        body.emplace("status", Protocol::JsonValue(status));
        body.emplace("code", Protocol::JsonValue(static_cast<unsigned>(error)));
        if (!requestId.empty())
            body.emplace("request_id", Protocol::JsonValue(std::string(requestId)));
        auto response = JsonResponse(Protocol::JsonValue(std::move(body)), status, 16384);
        if (response.IsOk())
            response.Value().headers[0].second = "application/problem+json";
        return response;
    }
    catch (...)
    {
        return Fail<HttpResponse>(ErrorCode::PlatformError);
    }
}
}
