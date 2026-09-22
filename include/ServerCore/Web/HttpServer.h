#pragma once
#include "ServerCore/Observability/Metrics.h"
#include "ServerCore/Observability/RequestTrace.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Net/ConnectionFlowControl.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stop_token>
#include <utility>
#include <vector>

namespace ServerCore::Web
{
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest
{
    std::string method;
    std::string target;
    // Names are normalized to lowercase. Repeated ordinary fields remain separate.
    HttpHeaders headers;
    std::string body;
    // Owned, decoded captures from the selected pattern, in path order. Empty
    // for exact routes. Copying the request also copies these values.
    std::vector<std::pair<std::string, std::string>> pathParameters{};

    [[nodiscard]] std::string_view Path() const noexcept;
    [[nodiscard]] std::string_view Header(std::string_view name) const noexcept;
    // Case-sensitive name; an absent parameter returns an empty view. Views
    // remain valid only while the request's corresponding storage is unchanged.
    [[nodiscard]] std::string_view PathParameter(std::string_view name) const noexcept;
};

struct HttpResponse
{
    unsigned int status = 200;
    HttpHeaders headers;
    std::string body;
    // HEAD and 204/205/304 suppress content. Date is generated if absent;
    // response framing fields remain owned by HttpServer. An Upgrade advertisement
    // automatically adds its Connection option; status 426 requires that header.
    bool close = false;
};

// Framework-owned framing: absent length selects HTTP/1.1 chunked transfer.
// HEAD and 204/205/304 suppress body bytes. Header fields cannot override framing.
struct HttpResponseHead
{
    unsigned int status = 200;
    HttpHeaders headers;
    std::optional<std::uint64_t> contentLength{};
    bool close = false;
};

// One response per request. Thread-safe operations; concurrent writes are ordered
// by admission, so applications must serialize writes when their order matters.
// Success means locally queued. WouldBlock accepts none and permits retry.
// Complete is the bounded buffered form; Start/Write/Finish stream arbitrary
// total lengths in bounded chunks. Finish does not wait for remote delivery.
class HttpResponseWriter
{
public:
    virtual ~HttpResponseWriter() = default;
    virtual Core::Status Start(const HttpResponseHead& head) = 0;
    virtual Core::Status Write(std::span<const std::byte> bytes) = 0;
    virtual Core::Status Finish() = 0;
    virtual Core::Status Complete(const HttpResponse& response) = 0;
    virtual void Abort() noexcept = 0;
    [[nodiscard]] virtual std::stop_token GetCancellationToken() const noexcept = 0;
    [[nodiscard]] virtual bool IsHeadRequest() const noexcept = 0;
    [[nodiscard]] virtual std::size_t MaxWriteBytes() const noexcept = 0;
    [[nodiscard]] virtual std::size_t RetainedSendBytes() const noexcept = 0;
    // One pending waiter, advisory availability, callback may run inline.
    // bytes=0 waits for header/Finish framing capacity. Keep the subscription.
    // Callbacks run outside response/transport locks and must finish promptly.
    // Serialize application producers and retries for each response. Capacity
    // does not serialize writers: a concurrent/reentrant transaction returns
    // WouldBlock without registering a wait or invoking its callback.
    // Finish retires pending waits with Cancelled without cancelling user work;
    // Abort/disconnect cancel both. New waits after either return Closed.
    virtual Core::Result<Net::SendCapacitySubscription> WaitForWriteCapacity(
        std::size_t bodyBytes, std::function<void(Core::Status)> callback) = 0;
};

// Bounded owned upload chunks. Read returns WouldBlock while awaiting input,
// a non-null chunk on progress, and null on clean EOF. Retaining a returned
// chunk retains its byte charge and can pause socket reads. Cancel aborts an
// unfinished upload; after EOF it is harmless. Safe to retain after server Stop.
class HttpRequestBody
{
public:
    virtual ~HttpRequestBody() = default;
    virtual Core::Result<std::shared_ptr<const std::vector<std::byte>>> Read() = 0;
    virtual void Cancel() noexcept = 0;
    [[nodiscard]] virtual std::stop_token GetCancellationToken() const noexcept = 0;
    [[nodiscard]] virtual std::size_t RetainedBytes() const noexcept = 0;
};

struct HttpRequestContext
{
    HttpRequest request;
    std::shared_ptr<HttpResponseWriter> response;
    std::shared_ptr<HttpRequestBody> body{};
    HttpHeaders attributes{};
};

// Asynchronous admission on the bounded handler pool. Exactly one Allow/Reject
// succeeds. Owned metadata is bounded by maxHeaderBytes; rejected content is
// bounded by maxResponseBodyBytes. Callbacks can retain the decision and resolve
// later; disconnect, Stop and handlerTimeout cancel unresolved decisions.
class HttpRequestDecision
{
public:
    virtual ~HttpRequestDecision() = default;
    virtual Core::Status Allow(HttpHeaders responseHeaders = {}, HttpHeaders attributes = {}) = 0;
    virtual Core::Status Reject(const HttpResponse& response) = 0;
    virtual void Abort() noexcept = 0;
    [[nodiscard]] virtual std::stop_token GetCancellationToken() const noexcept = 0;
};
struct HttpPolicyContext
{
    HttpRequest request;
    bool webSocketUpgrade = false;
    std::shared_ptr<HttpRequestDecision> decision;
};
using RequestPolicy = std::function<void(std::shared_ptr<const HttpPolicyContext>)>;

enum class WebSocketMessageType { Text, Binary };

struct WebSocketMessage
{
    WebSocketMessageType type = WebSocketMessageType::Text;
    // Borrowed for the duration of onMessage. Copy before retaining it.
    std::span<const std::byte> bytes;
};

class WebSocketConnection
{
public:
    virtual ~WebSocketConnection() = default;
    [[nodiscard]] virtual std::uint64_t Id() const noexcept = 0;
    // Thread-safe. Success means queued locally, not delivered to the peer.
    // Outgoing messages must fit maxWebSocketFrameBytes; incoming fragmented
    // messages may be larger, up to maxWebSocketMessageBytes.
    // Closed state is checked before payload validation. Live sends enforce size
    // before inspecting UTF-8; malformed caller text/close reasons return
    // InvalidArgument. Invalid received UTF-8 instead closes with wire code 1007.
    virtual Core::Status SendText(std::string_view text) = 0;
    virtual Core::Status SendBinary(std::span<const std::byte> bytes) = 0;
    virtual Core::Status Ping(std::span<const std::byte> bytes = {}) = 0;
    // Starts the closing handshake. The close deadline bounds an unresponsive peer.
    virtual Core::Status Close(std::uint16_t code = 1000, std::string_view reason = {}) = 0;
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
};

class WebSocketFlowControl
{
public:
    virtual ~WebSocketFlowControl() = default;
    [[nodiscard]] virtual std::size_t RetainedSendBytes() const noexcept = 0;
    // Advisory capacity for a complete frame, including its wire header.
    // A zero-byte payload is valid. The returned subscription owns the wait.
    virtual Core::Result<Net::SendCapacitySubscription> WaitForSendCapacity(
        std::size_t payloadBytes, std::function<void(Core::Status)> callback,
        std::stop_token cancellation = {}) = 0;
};
[[nodiscard]] std::shared_ptr<WebSocketFlowControl> GetWebSocketFlowControl(
    const std::shared_ptr<WebSocketConnection>& connection) noexcept;

struct WebSocketCallbacks
{
    // Runs before the upgrade. Use it to enforce Origin, authentication and other
    // application policy. An empty predicate accepts the handshake.
    std::function<bool(const HttpRequest&)> accept;
    std::function<void(const std::shared_ptr<WebSocketConnection>&)> onOpen;
    std::function<void(const std::shared_ptr<WebSocketConnection>&, const WebSocketMessage&)> onMessage;
    // Called exactly once for an upgraded connection, including server shutdown.
    // 1006 means the transport ended without receiving a valid close frame;
    // 1005 means a received close frame had no status. Neither is sent on the wire.
    std::function<void(std::uint64_t, std::uint16_t, std::string_view)> onClose;
    // Alternative to onOpen when the connection needs the handshake request,
    // including route parameters. Setting both is InvalidArgument at registration.
    // The request is borrowed only for this callback; copy anything retained.
    std::function<void(const std::shared_ptr<WebSocketConnection>&, const HttpRequest&)> onOpenWithRequest{};
    // Runs before the synchronous accept predicate and before any 101 bytes.
    // The server-wide policy, when set, is evaluated first.
    RequestPolicy authorize{};
};

struct HttpServerOptions
{
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = 0;
    int ioWorkerThreadCount = 2;
    int acceptBacklog = 64;
    std::size_t maxConnections = 256;
    std::size_t maxHeaderBytes = 16 * 1024;
    std::size_t maxBodyBytes = 64 * 1024;
    std::size_t maxResponseBodyBytes = 256 * 1024;
    std::size_t maxWebSocketFrameBytes = 64 * 1024;
    std::size_t maxWebSocketMessageBytes = 256 * 1024;
    // Absolute deadline for a complete HTTP request, including its body; trickle
    // traffic does not extend it. The idle deadline also applies to WebSockets.
    std::chrono::milliseconds requestTimeout{30000};
    std::chrono::milliseconds idleTimeout{120000};
    // Absolute limit for draining an HTTP Connection: close response. Incoming
    // traffic cannot prolong ownership of its connection slot and send queue.
    std::chrono::milliseconds responseDrainTimeout{5000};
    std::chrono::milliseconds webSocketCloseTimeout{3000};
    // Retained send payloads across HTTP and WebSocket connections together.
    // Positive, at most 512 MiB. Each connection is still limited to 1 MiB.
    std::size_t maxTotalSendQueueCapacityBytes = 256 * 1024 * 1024;
    std::size_t handlerWorkerThreadCount = 2;
    std::size_t maxPendingHandlers = 128;
    std::size_t maxHandlerRetainedBytes = 8 * 1024 * 1024;
    std::size_t maxActiveHttpRequests = 256;
    // Owned request storage remains charged while an application retains its
    // shared request context, including after response completion/cancellation.
    std::size_t maxTotalRequestBytes = 16 * 1024 * 1024;
    std::size_t maxPipelinedBytes = 64 * 1024;
    std::size_t maxStreamChunkBytes = 64 * 1024;
    // Handler deadline ends when response headers are committed. A stream uses
    // independent idle and queued-send stall deadlines; output refreshes idle.
    std::chrono::milliseconds handlerTimeout{30000};
    std::chrono::milliseconds sendStallTimeout{30000};
    std::chrono::milliseconds streamIdleTimeout{120000};
    std::size_t maxRequestBodyBufferBytes = 64 * 1024;
    std::size_t maxStreamedBodyBytes = 1024 * 1024 * 1024;
};

// HTTP/1.1 exact and pattern routes, persistent connections, Content-Length and bounded
// chunked request decoding. No TLS, HTTP/2, HTTP/3, WebSocket compression or
// subprotocol negotiation. TLS can terminate at a reverse proxy.
//
// Register routes before Start. HTTP handlers run on the bounded handler pool;
// async handlers may retain their owning context and complete later. One response
// is active per connection. WebSocket callbacks run on transport/maintenance
// threads, serialize per connection, and must finish promptly.
// Stop waits for callbacks and all transport completions. Call Stop and destroy
// the server outside callbacks/I/O threads. A stopped/failed server is single-use.
class HttpServer
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    using AsyncHandler = std::function<void(std::shared_ptr<const HttpRequestContext>)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Register before Start. Calls from this server's callbacks return Closed.
    // Other calls validate arguments first (InvalidArgument), then reject a used
    // server (Closed) or a duplicate key (AlreadyExists). Routes are never replaced.
    Core::Status RegisterRoute(std::string_view method, std::string_view path, Handler handler);
    Core::Status RegisterAsyncRoute(std::string_view method, std::string_view path, AsyncHandler handler);
    Core::Status RegisterStreamingRoute(std::string_view method, std::string_view path, AsyncHandler handler);
    Core::Status RegisterWebSocket(std::string_view path, WebSocketCallbacks callbacks);
    // Opt-in patterns: /plugins/{id}, with whole, nonempty segment captures.
    // Names use [A-Za-z_][A-Za-z0-9_]* and cannot repeat within one pattern.
    // Exact raw paths win first; patterns prefer a static segment over a capture
    // at the first difference, independent of registration order. Select the
    // path before its method; HEAD falls back to GET, other mismatches are 405.
    // Equivalent patterns for the same method are AlreadyExists even if their
    // parameter names differ. RegisterRoute keeps its literal path semantics.
    // Pattern matching percent-decodes each segment once and validates UTF-8;
    // malformed escapes, controls, separators and dot segments are rejected.
    // Query strings are excluded; case, '+' and trailing slashes are preserved.
    Core::Status RegisterRoutePattern(std::string_view method, std::string_view pattern, Handler handler);
    Core::Status RegisterAsyncRoutePattern(std::string_view method, std::string_view pattern, AsyncHandler handler);
    Core::Status RegisterStreamingRoutePattern(std::string_view method, std::string_view pattern, AsyncHandler handler);
    // Uses the same pattern rules. Captures are available in accept(request)
    // and onOpenWithRequest(connection, request).
    // HTTP and WebSocket routes can share a path; Upgrade: websocket requests
    // select the WebSocket table, ordinary requests select the HTTP table first.
    Core::Status RegisterWebSocketPattern(std::string_view pattern, WebSocketCallbacks callbacks);
    [[deprecated("Use RegisterRoute instead")]]
    Core::Status Route(std::string_view method, std::string_view path, Handler handler);
    [[deprecated("Use RegisterWebSocket instead")]]
    Core::Status WebSocket(std::string_view path, WebSocketCallbacks callbacks);
    Core::Status Start(const HttpServerOptions& options);
    Core::Status SetRequestPolicy(RequestPolicy policy);
    Core::Status SetLogger(std::shared_ptr<Core::ILogger> logger);
    // Configure before Start. One owned terminal event per admitted routed HTTP
    // request, delivered on a dedicated bounded worker. No request content is
    // collected. Full queues drop events and increment droppedTraceEvents.
    // Callbacks run outside internal locks, must finish promptly, and may query
    // metrics. Stop from this callback is InvalidArgument. Exceptions are counted.
    Core::Status SetRequestTraceHandler(Observability::RequestTraceHandler handler,
        std::size_t maxPendingEvents = 256);
    // InvalidArgument from a callback/I/O thread, rather than self-joining.
    Core::Status Stop();
    // Stops new accepts/requests; admitted responses may finish. Idle HTTP peers
    // close and WebSockets begin a 1001 closing handshake. BeginDrain is
    // idempotent on a control thread; callbacks/I/O threads return InvalidArgument.
    // DrainStatus is nonblocking (WouldBlock/Ok).
    Core::Status BeginDrain();
    [[nodiscard]] Core::Status DrainStatus() const noexcept;
    // At deadline, cancel remaining connections then perform normal Stop.
    // Returns Timeout when forcing was necessary. Joining noncooperative user
    // callbacks retains the same limitation as Stop; call on a control thread.
    Core::Status StopGracefully(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::uint16_t Port() const noexcept;
    [[nodiscard]] Observability::HttpServerMetricsSnapshot GetMetrics() const noexcept;

private:
    class State;
    std::shared_ptr<State> mState;
};
}
