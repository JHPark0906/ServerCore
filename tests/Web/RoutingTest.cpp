#include "TestHarness.h"
#include "SocketTestSupport.h"
#include "ServerCore/Web/HttpServer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
namespace Web = ServerCore::Web;
using ServerCore::Core::ErrorCode;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;

bool Check(bool condition, std::string_view description)
{
    ExpectTrue(condition, description);
    return condition;
}

Web::HttpResponse Reply(std::string body)
{
    return {200, {}, std::move(body)};
}

Web::HttpServer::Handler Named(std::string name)
{
    return [name = std::move(name)](const Web::HttpRequest&) {
        return Web::HttpResponse{200, {{"X-Route", name}}, name};
    };
}

bool Start(Web::HttpServer& server)
{
    const auto probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!Check(probe != ServerCoreTest::InvalidSocket, "routing port probe opens")) return false;
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int bound = bind(probe, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
    ServerCoreTest::SocketLength size = sizeof(endpoint);
    const int named = getsockname(probe, reinterpret_cast<sockaddr*>(&endpoint), &size);
    ServerCoreTest::CloseSocket(probe);
    if (!Check(bound == 0 && named == 0, "routing port probe binds")) return false;
    Web::HttpServerOptions options;
    options.port = ntohs(endpoint.sin_port);
    return Check(server.Start(options).IsOk(), "routing server starts");
}

bool EqualHeaderName(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto lower = [](char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (lower(left[index]) != lower(right[index])) return false;
    }
    return true;
}

struct Response
{
    unsigned int status = 0;
    std::string headers;
    std::string body;

    std::string Header(std::string_view name) const
    {
        std::size_t begin = headers.find("\r\n");
        if (begin == std::string::npos) return {};
        begin += 2;
        while (begin < headers.size())
        {
            const auto end = headers.find("\r\n", begin);
            if (end == std::string::npos || end == begin) break;
            const std::string_view line(headers.data() + begin, end - begin);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos && EqualHeaderName(line.substr(0, colon), name))
            {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
                return std::string(value);
            }
            begin = end + 2;
        }
        return {};
    }
};

class Client
{
public:
    explicit Client(std::uint16_t port)
    {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!Check(socket_ != ServerCoreTest::InvalidSocket, "routing client opens")) return;
        ExpectTrue(ServerCoreTest::SetSocketTimeouts(socket_, 3000), "routing socket timeout is bounded");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = htons(port);
        connected_ = connect(socket_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0;
        ExpectTrue(connected_, "routing client connects");
    }

    ~Client()
    {
        if (socket_ != ServerCoreTest::InvalidSocket) ServerCoreTest::CloseSocket(socket_);
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Send(std::string_view bytes)
    {
        if (!connected_) return false;
        while (!bytes.empty())
        {
            const int count = ServerCoreTest::Send(socket_, bytes.data(),
                static_cast<int>(std::min<std::size_t>(bytes.size(), 4096)));
            if (!Check(count > 0, "routing request sends")) return false;
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }

    Response ReadResponse(bool includeBody = true)
    {
        Response response;
        if (!connected_) return response;
        while (!response.headers.ends_with("\r\n\r\n") && response.headers.size() < 65536)
        {
            char byte = 0;
            if (!Check(ServerCoreTest::Receive(socket_, &byte, 1) == 1, "routing response header arrives"))
                return response;
            response.headers.push_back(byte);
        }
        if (!Check(response.headers.starts_with("HTTP/1.1 ") && response.headers.size() >= 12,
                "routing response status line is valid")) return response;
        const auto status = std::from_chars(response.headers.data() + 9, response.headers.data() + 12, response.status);
        ExpectTrue(status.ec == std::errc{}, "routing status parses");
        if (!includeBody) return response;
        const auto lengthText = response.Header("Content-Length");
        std::size_t length = 0;
        const auto parsed = std::from_chars(lengthText.data(), lengthText.data() + lengthText.size(), length);
        if (!Check(parsed.ec == std::errc{} && parsed.ptr == lengthText.data() + lengthText.size() &&
                length <= 65536, "routing response has bounded content length")) return response;
        response.body.resize(length);
        std::size_t offset = 0;
        while (offset < length)
        {
            const int count = ServerCoreTest::Receive(socket_, response.body.data() + offset,
                static_cast<int>(length - offset));
            if (!Check(count > 0, "routing response body arrives"))
            {
                response.body.resize(offset);
                return response;
            }
            offset += static_cast<std::size_t>(count);
        }
        return response;
    }

    bool Closed()
    {
        char byte = 0;
        const int count = ServerCoreTest::Receive(socket_, &byte, 1);
        if (count == 0) return true;
        if (count > 0) return false;
        const auto error = ServerCoreTest::LastSocketError();
#if defined(_WIN32)
        return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN;
#else
        return error == ECONNRESET || error == ENOTCONN;
#endif
    }

private:
    ServerCoreTest::Socket socket_ = ServerCoreTest::InvalidSocket;
    bool connected_ = false;
};

Response Request(const Web::HttpServer& server, std::string_view method, std::string_view target)
{
    Client client(server.Port());
    if (!client.Send(std::string(method) + " " + std::string(target) +
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")) return {};
    auto response = client.ReadResponse(method != "HEAD");
    ExpectTrue(client.Closed(), "response framing and HEAD suppression leave no extra body");
    return response;
}

Response Upgrade(const Web::HttpServer& server, std::string_view target)
{
    Client client(server.Port());
    if (!client.Send("GET " + std::string(target) +
            " HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n")) return {};
    return client.ReadResponse(false);
}

void RoutePatternRegistration()
{
    Web::HttpServer server;
    const std::array<std::string_view, 17> invalid{
        "", "plugins/{id}", "/plugins/{}", "/plugins/{1id}", "/plugins/{bad-name}",
        "/plugins/{id", "/plugins/id}", "/plugins/pre{id}", "/plugins/{id}post",
        "/{id}/{id}", "/plugins/{id}?q=x", "/plugins/{id}#x", "/plugins/{\xC3\xA9}",
        "/literal/%GG/{id}", "/literal/%2F/{id}", "/literal/../{id}", "/literal/\xC3\xA9/{id}"
    };
    for (const auto pattern : invalid)
    {
        ExpectTrue(server.RegisterRoutePattern("GET", pattern, Named("invalid")).Code() == ErrorCode::InvalidArgument,
            "HTTP pattern registration rejects invalid grammar or encoding");
        ExpectTrue(server.RegisterWebSocketPattern(pattern, {}).Code() == ErrorCode::InvalidArgument,
            "WebSocket pattern registration shares grammar validation");
    }
    ExpectTrue(server.RegisterRoutePattern("bad method", "/valid/{id}", Named("invalid")).Code() ==
        ErrorCode::InvalidArgument, "pattern methods remain HTTP tokens");
    ExpectTrue(server.RegisterRoutePattern("GET", "/valid/{id}", {}).Code() == ErrorCode::InvalidArgument,
        "HTTP pattern requires a handler");
    ExpectTrue(server.RegisterRoutePattern("GET", "/plugins/{id}", Named("get")).IsOk(), "pattern registers");
    ExpectTrue(server.RegisterRoutePattern("GET", "/plugins/{name}", Named("duplicate")).Code() ==
        ErrorCode::AlreadyExists, "renaming captures does not create a second route");
    ExpectTrue(server.RegisterRoutePattern("POST", "/plugins/{name}", Named("post")).IsOk(),
        "different methods may use different parameter names");
    ExpectTrue(server.RegisterRoutePattern("GET", "/views/{_id}/actions/{action_2}", Named("action")).IsOk(),
        "multiple distinct ASCII identifiers register");
    ExpectTrue(server.RegisterRoutePattern("GET", "/static/%66ixed", Named("static")).IsOk(),
        "zero-capture patterns decode static segments");
    ExpectTrue(server.RegisterRoutePattern("GET", "/static/fixed", Named("duplicate")).Code() ==
        ErrorCode::AlreadyExists, "decoded static route collisions are detected");
    ExpectTrue(server.RegisterWebSocketPattern("/plugins/{socket_id}", {}).IsOk(),
        "HTTP and WebSocket patterns coexist");
    ExpectTrue(server.RegisterWebSocketPattern("/plugins/{name}", {}).Code() == ErrorCode::AlreadyExists,
        "WebSocket structural duplicates are rejected");
    ExpectTrue(server.RegisterRoute("GET", "/plugins/{id}", Named("literal")).IsOk(),
        "legacy exact routes retain literal braces independently");
    Web::WebSocketCallbacks conflicting;
    conflicting.onOpen = [](const auto&) {};
    conflicting.onOpenWithRequest = [](const auto&, const Web::HttpRequest&) {};
    ExpectTrue(server.RegisterWebSocket("/conflicting", conflicting).Code() == ErrorCode::InvalidArgument,
        "exact WebSocket registration rejects two open callbacks");
    ExpectTrue(server.RegisterWebSocketPattern("/conflicting/{id}", conflicting).Code() == ErrorCode::InvalidArgument,
        "pattern WebSocket registration rejects two open callbacks");
}

void RoutePatternDecodingAndOwnership()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "routing socket runtime initializes")) return;
    std::mutex savedMutex;
    std::optional<Web::HttpRequest> saved;
    Web::HttpServer server;
    ExpectTrue(server.RegisterRoutePattern("GET", "/plugins/{id}", [&](const Web::HttpRequest& request) {
        if (request.PathParameter("id") == "saved value")
        {
            std::lock_guard lock(savedMutex);
            saved = request;
        }
        return Reply(std::string(request.PathParameter("id")));
    }).IsOk(), "capture route registers");
    ExpectTrue(server.RegisterRoutePattern("GET", "/views/{id}/actions/{action_id}", [](const Web::HttpRequest& request) {
        return Reply(std::string(request.PathParameter("id")) + ":" +
            std::string(request.PathParameter("action_id")) + ":" + std::to_string(request.pathParameters.size()));
    }).IsOk(), "nested capture route registers");
    ExpectTrue(server.RegisterRoutePattern("GET", "/encoded/%66ixed/{id}", [](const Web::HttpRequest& request) {
        return Reply(std::string(request.PathParameter("id")));
    }).IsOk(), "decoded static segment route registers");
    ExpectTrue(server.RegisterRoute("GET", "/legacy/%GG", [](const Web::HttpRequest& request) {
        return Reply(request.pathParameters.empty() ? "raw-exact" : "unexpected captures");
    }).IsOk(), "raw exact route registers");
    ExpectTrue(server.RegisterRoutePattern("GET", "/literal/%7Bid%7D/{value}", [](const Web::HttpRequest& request) {
        return Reply(std::string(request.PathParameter("value")));
    }).IsOk(), "encoded braces register as a literal segment");
    ExpectTrue(server.RegisterRoutePattern("GET", "/stars/*/{id}", Named("star-literal")).IsOk(),
        "asterisk inside a pattern is literal");
    if (!Start(server)) return;
    const std::array<std::pair<std::string_view, std::string_view>, 10> valid{{
        {"/plugins/hello%20world", "hello world"},
        {"/plugins/a+b", "a+b"},
        {"/plugins/%252F", "%2F"},
        {"/plugins/%ED%95%9C%EA%B8%80", "\xED\x95\x9C\xEA\xB8\x80"},
        {"/plugins/saved%20value?q=%GG", "saved value"},
        {"/views/view%201/actions/run%2Bnow", "view 1:run+now:2"},
        {"/encoded/fixed/7", "7"},
        {"/legacy/%GG", "raw-exact"},
        {"/literal/%7Bid%7D/ok", "ok"},
        {"/stars/*/ok", "star-literal"}
    }};
    for (const auto& [target, expected] : valid)
    {
        const auto response = Request(server, "GET", target);
        ExpectEqual(200u, response.status, "valid decoded path resolves");
        ExpectEqual(std::string(expected), response.body, "captures decode exactly once and exclude query");
    }
    const std::array<std::string_view, 19> invalid{
        "/plugins/%", "/plugins/%0", "/plugins/%GG", "/plugins/%00", "/plugins/%1f",
        "/plugins/%7F", "/plugins/%2F", "/plugins/%5c", "/plugins/\\", "/plugins/%2e",
        "/plugins/%2E%2e", "/plugins/.", "/plugins/..", "/plugins/%C0%AF",
        "/plugins/%ED%A0%80", "/plugins/%F4%90%80%80", "/plugins/%E2%82",
        "/unmatched/%GG", "/plugins/%0A"
    };
    for (const auto target : invalid)
        ExpectEqual(400u, Request(server, "GET", target).status, "malformed pattern path fails with 400");
    for (const auto target : {"/Plugins/a", "/plugins/", "/plugins/a/"})
        ExpectEqual(404u, Request(server, "GET", target).status, "case, nonempty captures and trailing slash matter");
    {
        Client pipeline(server.Port());
        if (pipeline.Send("GET /plugins/first HTTP/1.1\r\nHost: localhost\r\n\r\n"
                "GET /legacy/%GG HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"))
        {
            ExpectEqual(std::string("first"), pipeline.ReadResponse().body, "pipeline captures first request");
            ExpectEqual(std::string("raw-exact"), pipeline.ReadResponse().body, "next exact request has no stale captures");
            ExpectTrue(pipeline.Closed(), "pipeline closes after second response");
        }
    }
    ExpectTrue(server.Stop().IsOk(), "decoding server stops");
    std::lock_guard lock(savedMutex);
    if (Check(saved.has_value(), "handler copied a request"))
    {
        auto moved = std::move(*saved);
        saved.reset();
        ExpectEqual(std::string("saved value"), std::string(moved.PathParameter("id")),
            "owned captures survive request copying, moving and server shutdown");
        ExpectEqual(std::string("/plugins/saved%20value?q=%GG"), moved.target, "raw request target is preserved");
        ExpectTrue(moved.PathParameter("missing").empty() && moved.PathParameter("ID").empty(),
            "parameter lookup is case-sensitive and absent values are empty");
    }
    static_assert(noexcept(std::declval<const Web::HttpRequest&>().PathParameter(std::declval<std::string_view>())));
}

void RoutePatternPrecedenceAndMethods()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "routing socket runtime initializes")) return;
    for (const bool reverse : {false, true})
    {
        Web::HttpServer server;
        const std::array<std::pair<std::string_view, std::string_view>, 3> routes{{
            {"/{first}/{second}", "generic"}, {"/{first}/settings", "right-static"}, {"/plugins/{id}", "left-static"}
        }};
        for (std::size_t index = 0; index < routes.size(); ++index)
        {
            const auto& [pattern, name] = routes[reverse ? routes.size() - 1 - index : index];
            ExpectTrue(server.RegisterRoutePattern("GET", pattern, Named(std::string(name))).IsOk(), "overlapping pattern registers");
        }
        ExpectTrue(server.RegisterRoute("GET", "/plugins/exact", Named("exact")).IsOk(), "exact route registers");
        ExpectTrue(server.RegisterRoute("GET", "/plugins/{id}", Named("literal-braces")).IsOk(), "literal braces register");
        ExpectTrue(server.RegisterRoutePattern("HEAD", "/plugins/{head_id}", Named("explicit-head")).IsOk(), "explicit HEAD registers");
        ExpectTrue(server.RegisterRoutePattern("GET", "/resources/fixed", Named("static-get")).IsOk(), "static resource registers");
        ExpectTrue(server.RegisterRoutePattern("HEAD", "/resources/{id}", Named("broad-head")).IsOk(), "broad HEAD registers");
        ExpectTrue(server.RegisterRoutePattern("POST", "/resources/{id}", Named("broad-post")).IsOk(), "broad POST registers");
        ExpectTrue(server.RegisterRoutePattern("GET", "/verbs/{get_id}", [](const Web::HttpRequest& request) {
            return Reply("get:" + std::string(request.PathParameter("get_id")));
        }).IsOk(), "GET capture name registers");
        ExpectTrue(server.RegisterRoutePattern("POST", "/verbs/{post_id}", [](const Web::HttpRequest& request) {
            return Reply("post:" + std::string(request.PathParameter("post_id")));
        }).IsOk(), "POST capture name registers");
        ExpectTrue(server.RegisterRoutePattern("PUT", "/verbs/{put_id}", Named("put")).IsOk(), "PUT registers");
        ExpectTrue(server.RegisterRoute("OPTIONS", "*", Named("star")).IsOk(), "asterisk exact target registers");
        if (!Start(server)) return;
        ExpectEqual(std::string("left-static"), Request(server, "GET", "/plugins/settings").body,
            "static precedence is evaluated left-to-right independent of registration");
        ExpectEqual(std::string("right-static"), Request(server, "GET", "/other/settings").body, "later static wins common prefix");
        ExpectEqual(std::string("generic"), Request(server, "GET", "/other/value").body, "generic pattern remains reachable");
        ExpectEqual(std::string("exact"), Request(server, "GET", "/plugins/exact").body, "raw exact route wins");
        ExpectEqual(std::string("literal-braces"), Request(server, "GET", "/plugins/{id}").body, "exact braces remain literal");
        ExpectEqual(std::string("explicit-head"), Request(server, "HEAD", "/plugins/value").Header("X-Route"),
            "explicit HEAD handler takes precedence within matching path");
        const auto exactHead = Request(server, "HEAD", "/plugins/exact");
        ExpectEqual(std::string("exact"), exactHead.Header("X-Route"), "exact GET fallback wins over broader explicit HEAD");
        ExpectEqual(std::string("5"), exactHead.Header("Content-Length"), "HEAD preserves GET representation length");
        ExpectEqual(std::string("static-get"), Request(server, "HEAD", "/resources/fixed").Header("X-Route"),
            "most-specific path is chosen before HEAD fallback");
        const auto wrongStatic = Request(server, "POST", "/resources/fixed");
        ExpectEqual(405u, wrongStatic.status, "wrong method on static path cannot fall back to broad POST");
        ExpectEqual(std::string("GET, HEAD"), wrongStatic.Header("Allow"), "GET implies HEAD in Allow");
        ExpectEqual(405u, Request(server, "POST", "/plugins/exact").status, "wrong method on exact path cannot fall back");
        ExpectEqual(std::string("get:item"), Request(server, "GET", "/verbs/item").body, "GET uses its capture name");
        ExpectEqual(std::string("post:item"), Request(server, "POST", "/verbs/item").body, "POST uses its capture name");
        const auto wrongMethod = Request(server, "DELETE", "/verbs/item");
        ExpectEqual(405u, wrongMethod.status, "known path with unsupported method gives 405");
        ExpectEqual(std::string("GET, HEAD, POST, PUT"), wrongMethod.Header("Allow"), "Allow is sorted and complete");
        ExpectEqual(std::string("GET, HEAD"), Request(server, "DELETE", "/plugins/value").Header("Allow"),
            "explicit and implied HEAD occur once in Allow");
        ExpectEqual(404u, Request(server, "GET", "/unknown").status, "unknown path gives 404");
        ExpectEqual(std::string("star"), Request(server, "OPTIONS", "*").body, "asterisk bypasses pattern path validation");
        ExpectTrue(server.Stop().IsOk(), "precedence server stops");
    }
}

void RoutePatternWebSocketCoexistence()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "routing socket runtime initializes")) return;
    std::optional<Web::HttpRequest> openedRequest;
    bool openedConnectionWasLive = false;
    Web::HttpServer server;
    ExpectTrue(server.RegisterRoute("GET", "/shared", Named("ordinary-exact")).IsOk(), "exact HTTP route registers");
    ExpectTrue(server.RegisterWebSocket("/shared", {}).IsOk(), "exact WebSocket route shares path");
    ExpectTrue(server.RegisterRoutePattern("GET", "/views/{http_id}", [](const Web::HttpRequest& request) {
        return Reply("http:" + std::string(request.PathParameter("http_id")));
    }).IsOk(), "HTTP pattern registers");
    Web::WebSocketCallbacks views;
    views.accept = [](const Web::HttpRequest& request) {
        return request.PathParameter("ws_id") == "view 1" && request.PathParameter("http_id").empty() &&
            request.pathParameters.size() == 1;
    };
    views.onOpenWithRequest = [&](const auto& connection, const Web::HttpRequest& request) {
        openedRequest = request;
        openedConnectionWasLive = connection->IsOpen();
    };
    ExpectTrue(server.RegisterWebSocketPattern("/views/{ws_id}", std::move(views)).IsOk(), "WebSocket shares pattern shape");
    Web::WebSocketCallbacks sockets;
    sockets.accept = [](const Web::HttpRequest& request) { return request.PathParameter("id") == "%2F"; };
    ExpectTrue(server.RegisterWebSocketPattern("/sockets/{id}", std::move(sockets)).IsOk(), "WebSocket-only pattern registers");
    ExpectTrue(server.RegisterWebSocket("/sockets/fixed", {}).IsOk(), "exact WebSocket route registers");
    if (!Start(server)) return;
    ExpectEqual(std::string("ordinary-exact"), Request(server, "GET", "/shared").body, "ordinary exact request selects HTTP");
    ExpectEqual(101u, Upgrade(server, "/shared").status, "upgrade selects coexisting exact WebSocket");
    ExpectEqual(std::string("http:view 1"), Request(server, "GET", "/views/view%201").body,
        "ordinary patterned request selects HTTP captures");
    ExpectEqual(101u, Upgrade(server, "/views/view%201?q=ignored").status, "WebSocket accept sees decoded own captures");
    ExpectEqual(400u, Request(server, "GET", "/sockets/plain").status, "WebSocket-only path retains missing-upgrade error");
    ExpectEqual(101u, Upgrade(server, "/sockets/%252F").status, "WebSocket captures decode once");
    ExpectEqual(101u, Upgrade(server, "/sockets/fixed").status, "exact WebSocket wins before pattern predicate");
    ExpectEqual(400u, Upgrade(server, "/sockets/%2F").status, "WebSocket resolution rejects decoded separators");
    ExpectEqual(404u, Upgrade(server, "/missing").status, "unknown WebSocket route gives 404");
    ExpectTrue(server.Stop().IsOk(), "coexistence server stops");
    if (Check(openedRequest.has_value(), "request-aware open callback ran before shutdown completed"))
    {
        ExpectTrue(openedConnectionWasLive, "request-aware callback receives the live upgraded connection");
        ExpectEqual(std::string("view 1"), std::string(openedRequest->PathParameter("ws_id")),
            "request-aware open callback receives owned decoded captures");
        ExpectEqual(std::string("/views/view%201?q=ignored"), openedRequest->target,
            "request-aware open callback receives the original target");
    }
}

void RoutePatternLifecycle()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "routing socket runtime initializes")) return;
    Web::HttpServer server;
    ExpectTrue(server.RegisterRoutePattern("GET", "/lifecycle/{id}", [&](const Web::HttpRequest&) {
        const auto http = server.RegisterRoutePattern("bad method", "invalid", {});
        const auto webSocket = server.RegisterWebSocketPattern("invalid", {});
        return Reply(http.Code() == ErrorCode::Closed && webSocket.Code() == ErrorCode::Closed ?
            "closed" : "wrong precedence");
    }).IsOk(), "lifecycle route registers");
    if (!Start(server)) return;
    ExpectTrue(server.RegisterRoutePattern("GET", "/new/{id}", Named("late")).Code() == ErrorCode::Closed,
        "live server rejects valid HTTP registration");
    ExpectTrue(server.RegisterWebSocketPattern("/new/{id}", {}).Code() == ErrorCode::Closed,
        "live server rejects valid WebSocket registration");
    ExpectTrue(server.RegisterRoutePattern("bad method", "/new/{id}", Named("bad")).Code() == ErrorCode::InvalidArgument,
        "external invalid HTTP arguments are checked before used-server state");
    ExpectTrue(server.RegisterWebSocketPattern("invalid", {}).Code() == ErrorCode::InvalidArgument,
        "external invalid WebSocket arguments are checked before used-server state");
    ExpectEqual(std::string("closed"), Request(server, "GET", "/lifecycle/1").body,
        "callback registration rejects before argument validation");
    ExpectTrue(server.Stop().IsOk(), "lifecycle server stops");
    ExpectTrue(server.RegisterRoutePattern("GET", "/new/{id}", Named("late")).Code() == ErrorCode::Closed,
        "stopped server rejects HTTP registration");
    ExpectTrue(server.RegisterWebSocketPattern("/new/{id}", {}).Code() == ErrorCode::Closed,
        "stopped server rejects WebSocket registration");
}

const ServerCoreTest::CheckRegistration registration("Web.RoutePatternRegistration", RoutePatternRegistration);
const ServerCoreTest::CheckRegistration decoding("Web.RoutePatternDecodingAndOwnership", RoutePatternDecodingAndOwnership);
const ServerCoreTest::CheckRegistration precedence("Web.RoutePatternPrecedenceAndMethods", RoutePatternPrecedenceAndMethods);
const ServerCoreTest::CheckRegistration coexistence("Web.RoutePatternWebSocketCoexistence", RoutePatternWebSocketCoexistence);
const ServerCoreTest::CheckRegistration lifecycle("Web.RoutePatternLifecycle", RoutePatternLifecycle);
}
