#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Observability/Metrics.h"
#include "ServerCore/Observability/RequestTrace.h"

#include "ServerCore/Core/CompletionSubscription.h"
#include "ServerCore/Core/Endpoint.h"
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
#include <stop_token>
#include <string>
#include <string_view>
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
    // Transport endpoints, independent of untrusted forwarding headers.
    Core::IpEndpoint localEndpoint{}, remoteEndpoint{};

    [[nodiscard]] SERVERCORE_API std::string_view Path() const noexcept;
    [[nodiscard]] SERVERCORE_API std::string_view Header(std::string_view name) const noexcept;
    // Case-sensitive name; an absent parameter returns an empty view. Views
    // remain valid only while the request's corresponding storage is unchanged.
    [[nodiscard]] SERVERCORE_API std::string_view PathParameter(
        std::string_view name) const noexcept;
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
// HTTP/1.0 요청에는 청크를 쓰지 않는다. 길이가 없으면 본문 끝을 연결 종료로 알린다.
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
    // One advisory one-shot waiter; ready data/EOF/errors notify inline.
    // Callbacks run outside parser/body locks. Reset is a quiescence boundary.
    virtual Core::Result<Core::CompletionSubscription> WaitForReadReady(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {})
    {
        (void)callback;
        (void)cancellation;
        return Core::Result<Core::CompletionSubscription>::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::Unimplemented));
    }
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
    // responseHeaders는 응답의 기본값이다. 핸들러가 같은 이름을 주면 핸들러 값이 나가지만, 목록인 Vary와
    // Set-Cookie는 둘 다 나간다(Web.PolicyHeadersKeepListFields).
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

enum class WebSocketMessageType
{
    Text,
    Binary
};

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
    // These single-frame sends must fit maxWebSocketFrameBytes. The optional
    // message writer sends larger messages up to maxWebSocketMessageBytes.
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
    // 이 연결의 수신을 멈춘다(수신 흐름 제어). 소켓에서 새로 읽지 않으므로 상대는 TCP 창이 차면 막힌다.
    // 이미 받아 둔 프레임과 진행 중이던 수신 하나(최대 16 KiB)는 버퍼에 남았다가 ResumeReceive 뒤에 받은
    // 순서대로 onMessage로 온다(Web.WebSocketReceivePauseAndResume).
    // - onMessage 안에서 부르면 그 콜백이 돌아온 뒤 재개 전까지 onMessage가 오지 않는다. 다른 스레드에서
    //   부르면 onMessage가 최대 하나 더 올 수 있다. 피어 잠금을 잡지 않으므로 앱의 잠금을 쥔 채 불러도 된다.
    // - 멈춘 동안에는 ping·pong·close 프레임도 처리하지 않는다. 서버 heartbeat는 멈춘 동안 ping을 보내지
    //   않고 pong 기한도 세지 않으며, 재개하면 걸려 있던 pong 기한을 그때부터 다시 센다. 멈춘 채로는 pong을
    //   읽을 수 없어 살아 있는 상대를 끊게 되기 때문이다(Web.WebSocketReceivePauseSuspendsHeartbeat).
    // - Close(서버 drain의 1001 포함)는 멈춤을 풀어 closing handshake의 답을 읽는다.
    // - idleTimeout은 멈춘 연결에도 마지막 수신·송신부터 적용된다. 멈춤은 소비를 늦추는 수단이지 연결을
    //   기한 없이 붙잡는 수단이 아니고, 멈춘 연결에서 끊긴 상대를 찾아낼 다른 수단이 없다. 오래 멈출 앱은
    //   idleTimeout을 늘린다(같은 시험이 고정한다).
    // - 열린 WebSocket이 아니면 Closed다. 멈춤과 재개는 여러 번 불러도 된다. 스레드 안전하다.
    virtual Core::Status PauseReceive() = 0;
    // 멈춘 수신을 다시 연다. 버퍼에 쌓인 프레임은 서버의 진행 스레드가 이어서 처리한다.
    virtual Core::Status ResumeReceive() = 0;
    // 앱이 PauseReceive로 멈췄고 아직 재개하지 않았으면 참이다. Close가 멈춤을 풀면 거짓이 된다.
    [[nodiscard]] virtual bool IsReceivePaused() const noexcept = 0;
};
[[nodiscard]] SERVERCORE_API std::shared_ptr<WebSocketFlowControl> GetWebSocketFlowControl(
    const std::shared_ptr<WebSocketConnection>& connection) noexcept;

// One owned writer reserves the data-message lane. Every successful Write
// admits one frame (first Text/Binary, then continuations); final releases the
// lane. Control frames may interleave. Another writer or ordinary data send
// returns AlreadyExists while reserved. WouldBlock accepts no bytes/state change.
// Text is checked incrementally, including code points split across frames.
// Invalid input is retryable; dropping/aborting after any admitted fragment
// closes the connection, since an incomplete wire message cannot be withdrawn.
class WebSocketMessageWriter
{
public:
    virtual ~WebSocketMessageWriter() = default;
    virtual Core::Status Write(std::span<const std::byte> bytes, bool final) = 0;
    virtual void Abort() noexcept = 0;
    [[nodiscard]] virtual std::size_t MaxWriteBytes() const noexcept = 0;
    // Reentry during a Write transaction returns WouldBlock without installing
    // a waiter. Capacity itself is advisory; recheck Write after notification.
    virtual Core::Result<Net::SendCapacitySubscription> WaitForWriteCapacity(std::size_t bytes,
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) = 0;
};
// Optional extension preserves existing WebSocketConnection implementers.
class WebSocketMessageControl
{
public:
    virtual ~WebSocketMessageControl() = default;
    [[nodiscard]] virtual std::string_view Subprotocol() const noexcept = 0;
    virtual Core::Result<std::shared_ptr<WebSocketMessageWriter>> BeginMessage(
        WebSocketMessageType type) = 0;
};
[[nodiscard]] SERVERCORE_API std::shared_ptr<WebSocketMessageControl> GetWebSocketMessageControl(
    const std::shared_ptr<WebSocketConnection>& connection) noexcept;

struct WebSocketCallbacks
{
    // Runs before the upgrade. Use it to enforce Origin, authentication and other
    // application policy. An empty predicate accepts the handshake.
    std::function<bool(const HttpRequest&)> accept;
    std::function<void(const std::shared_ptr<WebSocketConnection>&)> onOpen;
    std::function<void(const std::shared_ptr<WebSocketConnection>&, const WebSocketMessage&)>
        onMessage;
    // Called exactly once for an upgraded connection, including server shutdown.
    // 1006 means the transport ended without receiving a valid close frame;
    // 1005 means a received close frame had no status. Neither is sent on the wire.
    std::function<void(std::uint64_t, std::uint16_t, std::string_view)> onClose;
    // Alternative to onOpen when the connection needs the handshake request,
    // including route parameters. Setting both is InvalidArgument at registration.
    // The request is borrowed only for this callback; copy anything retained.
    std::function<void(const std::shared_ptr<WebSocketConnection>&, const HttpRequest&)>
        onOpenWithRequest{};
    // Runs before the synchronous accept predicate and before any 101 bytes.
    // The server-wide policy, when set, is evaluated first.
    RequestPolicy authorize{};
    // Server preference order; unique case-sensitive ASCII tokens. At most 64
    // tokens/4096 bytes (also enforced for offered request tokens). A missing
    // match omits Sec-WebSocket-Protocol. Repeated
    // request fields combine normally; duplicate offered tokens are rejected.
    std::vector<std::string> subprotocols{};
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
    // 예외: 스트리밍 경로(RegisterStreamingRoute)의 본문은 디스패치 뒤 마지막으로 본문 바이트가 읽는 쪽에
    // 넘어간 때부터 센다. 진행하는 큰 업로드는 끊지 않고, 멈춘 업로드만 닫는다
    // (Web.StreamingUploadOutlivesRequestDeadline).
    std::chrono::milliseconds requestTimeout{ 30000 };
    std::chrono::milliseconds idleTimeout{ 120000 };
    // 새 연결의 첫 요청은 수락 시각부터 requestTimeout을 센다. 그와 별개로 첫 바이트가 이 기한 안에 오지
    // 않으면 닫는다. 아무것도 보내지 않는 연결이 유휴 기한까지 연결 자리를 쥐지 못하게 한다
    // (Web.SilentConnectionsExpire). keep-alive 연결의 요청 사이 대기에는 idleTimeout을 쓴다.
    std::chrono::milliseconds firstByteTimeout{ 10000 };
    // Absolute limit for draining an HTTP Connection: close response. Incoming
    // traffic cannot prolong ownership of its connection slot and send queue.
    std::chrono::milliseconds responseDrainTimeout{ 5000 };
    std::chrono::milliseconds webSocketCloseTimeout{ 3000 };
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
    // 스트리밍 경로의 핸들러 기한은 요청 본문을 다 받은 때부터 센다. 받는 동안에는 requestTimeout이 진행을 본다.
    std::chrono::milliseconds handlerTimeout{ 30000 };
    std::chrono::milliseconds sendStallTimeout{ 30000 };
    std::chrono::milliseconds streamIdleTimeout{ 120000 };
    // 스트리밍 업로드 한 건이 읽는 쪽에 아직 넘기지 않았거나 읽는 쪽이 쥐고 있는 본문의 몫이다. 조각마다
    // 페이로드에 더해 장부 비용(약 160바이트, 어림값)을 이 몫에서 뺀다. 몫이 한 조각의 장부 비용보다
    // 작아도 설정은 유효하다. 그때 업로드는 한 조각씩 나아간다. 빈 몫은 조각 하나를 받고, 그 조각을
    // 놓을 때까지 다음 조각을 받지 않는다. 연결당 메모리는 몫과 조각 하나의 장부 비용을 더한 정도로
    // 묶이고 처리량이 줄어든다(Web.StreamingBodyChargesPieceBookkeeping, Web.StreamingUploadBackpressure).
    std::size_t maxRequestBodyBufferBytes = 64 * 1024;
    std::size_t maxStreamedBodyBytes = 1024 * 1024 * 1024;
    // Disabled at zero. The shared maintenance tick sends correlated Pings;
    // only the matching Pong satisfies the positive timeout. No per-peer thread.
    // Deadlines begin at local queue admission, not remote delivery.
    std::chrono::milliseconds webSocketPingInterval{ 0 };
    std::chrono::milliseconds webSocketPongTimeout{ 10000 };
    // IPv6 listeners default to V6ONLY. Explicit false accepts mapped IPv4 too.
    bool ipv6Only = true;
};

// HTTP/1.1 exact and pattern routes, persistent connections, Content-Length and bounded
// chunked request decoding. No TLS, HTTP/2, HTTP/3 or WebSocket compression.
// TLS can terminate at a reverse proxy.
// HTTP/1.0 요청(예: nginx의 기본 proxy_http_version)도 받는다. 이때는 Host를 요구하지 않고,
// 100 Continue와 WebSocket 전환을 하지 않으며, keep-alive 요청과 관계없이 응답 뒤에 연결을 닫는다.
// 요청 줄 앞의 빈 줄은 무시하고, absolute-form 대상은 origin-form으로 줄여 그 authority를 Host로 쓴다.
// 짝 없는 CR·LF는 기다리지 않고 400으로 거절한다. 이 동작은 Web.Http10AndLenientRequestLines가 고정한다.
//
// Register routes before Start. HTTP handlers run on the bounded handler pool;
// async handlers may retain their owning context and complete later. One response
// is active per connection. WebSocket callbacks run on transport/maintenance
// threads, serialize per connection, and must finish promptly.
// 정책을 거친 업그레이드의 accept·onOpen(과 이미 받아 둔 프레임의 onMessage), 응용 스레드나 기한 집행에서
// 난 끊김의 onClose는 서버의 진행 스레드에서 돈다. 기한을 집행하는 유지보수 스레드는 WebSocket 콜백을
// 부르지 않고 콜백이 쥔 연결을 건너뛰므로, 느린 콜백이 다른 연결의 기한을 멈추지 않는다
// (Web.PolicyCallbacksDoNotStallDeadlines). 다만 기한이 지난 응답을 취소할 때 그 취소 토큰에 걸린
// stop_callback과 쓰기 용량 대기 콜백은 유지보수 스레드에서 돈다.
// Stop waits for callbacks and all transport completions. Call Stop and destroy
// the server outside callbacks/I/O threads. A stopped/failed server is single-use.
class HttpServer
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    using AsyncHandler = std::function<void(std::shared_ptr<const HttpRequestContext>)>;

    SERVERCORE_API HttpServer();
    SERVERCORE_API ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Register before Start. Calls from this server's callbacks return Closed.
    // Other calls validate arguments first (InvalidArgument), then reject a used
    // server (Closed) or a duplicate key (AlreadyExists). Routes are never replaced.
    SERVERCORE_API Core::Status RegisterRoute(
        std::string_view method, std::string_view path, Handler handler);
    SERVERCORE_API Core::Status RegisterAsyncRoute(
        std::string_view method, std::string_view path, AsyncHandler handler);
    SERVERCORE_API Core::Status RegisterStreamingRoute(
        std::string_view method, std::string_view path, AsyncHandler handler);
    SERVERCORE_API Core::Status RegisterWebSocket(
        std::string_view path, WebSocketCallbacks callbacks);
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
    // 캡처를 파일 경로에 그대로 붙여도 안전하다는 뜻은 아니다. ':'·끝의 점과 공백·장치 이름은 거절하지
    // 않으며 Windows에서는 다른 드라이브·스트림·파일이 된다. SendFile이 Windows에서 그런 경로를 거절한다.
    SERVERCORE_API Core::Status RegisterRoutePattern(
        std::string_view method, std::string_view pattern, Handler handler);
    SERVERCORE_API Core::Status RegisterAsyncRoutePattern(
        std::string_view method, std::string_view pattern, AsyncHandler handler);
    SERVERCORE_API Core::Status RegisterStreamingRoutePattern(
        std::string_view method, std::string_view pattern, AsyncHandler handler);
    // Uses the same pattern rules. Captures are available in accept(request)
    // and onOpenWithRequest(connection, request).
    // HTTP and WebSocket routes can share a path; Upgrade: websocket requests
    // select the WebSocket table, ordinary requests select the HTTP table first.
    SERVERCORE_API Core::Status RegisterWebSocketPattern(
        std::string_view pattern, WebSocketCallbacks callbacks);
    [[deprecated("Use RegisterRoute instead")]]
    SERVERCORE_API Core::Status Route(
        std::string_view method, std::string_view path, Handler handler);
    [[deprecated("Use RegisterWebSocket instead")]]
    SERVERCORE_API Core::Status WebSocket(std::string_view path, WebSocketCallbacks callbacks);
    SERVERCORE_API Core::Status Start(const HttpServerOptions& options);
    // 정책은 경로와 메서드가 맞은 요청에서 돈다. 예외로 CORS preflight(Origin과 Access-Control-Request-Method가
    // 있는 OPTIONS)는 경로가 있으면 메서드 판정(405)보다 먼저 정책에 보인다. 정책이 응답하지 않고 허용만 하면
    // 그 경로의 405를 보낸다(Web.CorsPreflightReachesPolicyBeforeMethodCheck).
    SERVERCORE_API Core::Status SetRequestPolicy(RequestPolicy policy);
    SERVERCORE_API Core::Status SetLogger(std::shared_ptr<Core::ILogger> logger);
    // Configure before Start. One owned terminal event per admitted routed HTTP
    // request, delivered on a dedicated bounded worker. No request content is
    // collected. Full queues drop events and increment droppedTraceEvents.
    // Callbacks run outside internal locks, must finish promptly, and may query
    // metrics. Stop from this callback is InvalidArgument. Exceptions are counted.
    SERVERCORE_API Core::Status SetRequestTraceHandler(
        Observability::RequestTraceHandler handler, std::size_t maxPendingEvents = 256);
    // InvalidArgument from a callback/I/O thread, rather than self-joining.
    SERVERCORE_API Core::Status Stop();
    // Stops new accepts/requests; admitted responses may finish. Idle HTTP peers
    // close and WebSockets begin a 1001 closing handshake. BeginDrain is
    // idempotent on a control thread; callbacks/I/O threads return InvalidArgument.
    // DrainStatus is nonblocking (WouldBlock/Ok).
    // drain 완료는 상대가 연결을 닫기를 기다린다. 전송은 보내기 쪽을 닫은 뒤 상대의 EOF까지 남은 입력을
    // 읽어 버리고 나서야 연결을 거두기 때문이다(Net CloseAfterSend). EOF 뒤에도 닫지 않는 상대는 연결의
    // 상태에 따라 다른 기한이 끝낸다: HTTP 연결은 응답 뒤에 닫든 요청 사이에 쉬다가 drain으로 닫히든
    // responseDrainTimeout(닫기 시작부터, Web.DrainBoundsIdleKeepAliveConnections), WebSocket은
    // webSocketCloseTimeout, 그리고 모든 연결은 StopGracefully의 기한이 끝낸다.
    SERVERCORE_API Core::Status BeginDrain();
    [[nodiscard]] SERVERCORE_API Core::Status DrainStatus() const noexcept;
    // Call after BeginDrain. One waiter; completion means no admitted peers
    // remain. Does not stop/join workers. Callback may run inline.
    // 닫지 않는 상대가 있으면 BeginDrain에 적은 기한이 지날 때까지 완료되지 않는다.
    SERVERCORE_API Core::Result<Core::CompletionSubscription> WaitForDrain(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {});
    // At deadline, cancel remaining connections then perform normal Stop.
    // Returns Timeout when forcing was necessary. Joining noncooperative user
    // callbacks retains the same limitation as Stop; call on a control thread.
    // EOF를 받고도 닫지 않는 상대 때문에 기한 전에 끝나지 못하면 Timeout을 돌려준다.
    SERVERCORE_API Core::Status StopGracefully(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] SERVERCORE_API bool IsRunning() const noexcept;
    [[nodiscard]] SERVERCORE_API std::uint16_t Port() const noexcept;
    [[nodiscard]] SERVERCORE_API Observability::HttpServerMetricsSnapshot GetMetrics()
        const noexcept;

private:
    class State;
    std::shared_ptr<State> mState;
};
}
