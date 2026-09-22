#pragma once

#include "ServerCore/Core/Error.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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

    [[nodiscard]] std::string_view Path() const noexcept;
    [[nodiscard]] std::string_view Header(std::string_view name) const noexcept;
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
};

// HTTP/1.1 origin-form routes, persistent connections, Content-Length and bounded
// chunked request decoding. No TLS, HTTP/2, HTTP/3, WebSocket compression or
// subprotocol negotiation. TLS can terminate at a reverse proxy.
//
// Register routes before Start. Callbacks run synchronously and must finish
// promptly; each connection's callbacks are serialized, different connections
// can run concurrently. Callback exceptions close/fail that connection.
// Stop waits for callbacks and all transport completions. Call Stop and destroy
// the server outside callbacks/I/O threads. A stopped/failed server is single-use.
class HttpServer
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Register before Start. Calls from this server's callbacks return Closed.
    // Other calls validate arguments first (InvalidArgument), then reject a used
    // server (Closed) or a duplicate key (AlreadyExists). Routes are never replaced.
    Core::Status RegisterRoute(std::string_view method, std::string_view path, Handler handler);
    Core::Status RegisterWebSocket(std::string_view path, WebSocketCallbacks callbacks);
    [[deprecated("Use RegisterRoute instead")]]
    Core::Status Route(std::string_view method, std::string_view path, Handler handler);
    [[deprecated("Use RegisterWebSocket instead")]]
    Core::Status WebSocket(std::string_view path, WebSocketCallbacks callbacks);
    Core::Status Start(const HttpServerOptions& options);
    // InvalidArgument from a callback/I/O thread, rather than self-joining.
    Core::Status Stop();
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::uint16_t Port() const noexcept;

private:
    class State;
    std::shared_ptr<State> mState;
};
}
