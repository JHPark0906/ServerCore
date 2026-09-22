#include "TestHarness.h"
#include "SocketTestSupport.h"
#include "ServerCore/Web/HttpServer.h"
#include "ServerCore/Web/HttpStreaming.h"
#include "ServerCore/Runtime/TaskExecutor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace Web = ServerCore::Web;
namespace Runtime = ServerCore::Runtime;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using namespace std::chrono_literals;
constexpr auto WaitLimit = 10s;
constexpr std::size_t MaximumResponseBytes = 16 * 1024 * 1024;

bool Check(const bool condition, const std::string_view message)
{
    ExpectTrue(condition, message);
    return condition;
}

std::span<const std::byte> Bytes(const std::string_view value)
{
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::string Payload(const std::size_t size)
{
    std::string bytes(size, '\0');
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<char>((index * 37 + index / 251) % 256);
    return bytes;
}

template <typename T> class Inbox final
{
public:
    void Publish(T item)
    {
        {
            const std::lock_guard lock(mMutex);
            mItems.push_back(std::move(item));
        }
        mChanged.notify_all();
    }

    T Next()
    {
        std::unique_lock lock(mMutex);
        if (!mChanged.wait_for(lock, WaitLimit, [this] { return !mItems.empty(); })) return {};
        auto item = std::move(mItems.front());
        mItems.pop_front();
        return item;
    }

private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::deque<T> mItems;
};

using Request = std::shared_ptr<const Web::HttpRequestContext>;

class Completion final
{
public:
    void Complete(const Status& status)
    {
        {
            const std::lock_guard lock(mMutex);
            ++mCalls;
            mCode = status.Code();
        }
        mChanged.notify_all();
    }

    bool Wait()
    {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, WaitLimit, [this] { return mCalls != 0; });
    }

    ErrorCode Code() const
    {
        const std::lock_guard lock(mMutex);
        return mCode;
    }

    unsigned Calls() const
    {
        const std::lock_guard lock(mMutex);
        return mCalls;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    unsigned mCalls = 0;
    ErrorCode mCode = ErrorCode::WouldBlock;
};

class HandlerGate final
{
public:
    void Enter()
    {
        std::unique_lock lock(mMutex);
        mEntered = true;
        mChanged.notify_all();
        if (!mChanged.wait_for(lock, WaitLimit, [this] { return mReleased; })) mTimedOut = true;
    }
    bool WaitForEntry()
    {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, WaitLimit, [this] { return mEntered; });
    }
    void Release()
    {
        {
            const std::lock_guard lock(mMutex);
            mReleased = true;
        }
        mChanged.notify_all();
    }
    bool TimedOut() const
    {
        const std::lock_guard lock(mMutex);
        return mTimedOut;
    }
private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    bool mEntered = false;
    bool mReleased = false;
    bool mTimedOut = false;
};

struct ReleaseGateOnExit
{
    std::shared_ptr<HandlerGate> gate;
    ~ReleaseGateOnExit() { gate->Release(); }
};

bool WaitCancelled(const std::stop_token token)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool cancelled = false;
    std::stop_callback callback(token, [&] {
        {
            const std::lock_guard lock(mutex);
            cancelled = true;
        }
        changed.notify_all();
    });
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, WaitLimit, [&] { return cancelled; });
}

std::uint16_t FreePort()
{
    const auto probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!Check(probe != ServerCoreTest::InvalidSocket, "async port probe opens")) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool bound = ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ServerCoreTest::SocketLength size = sizeof(address);
    const bool named = bound && ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    ServerCoreTest::CloseSocket(probe);
    return Check(named, "async ephemeral port is discovered") ? ntohs(address.sin_port) : 0;
}

bool Start(Web::HttpServer& server, Web::HttpServerOptions options = {})
{
    options.port = FreePort();
    return Check(server.Start(options).IsOk(), "async/streaming server starts");
}

bool EqualHeaderName(const std::string_view left, const std::string_view right)
{
    if (left.size() != right.size()) return false;
    const auto lower = [](const char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    };
    for (std::size_t index = 0; index < left.size(); ++index)
        if (lower(left[index]) != lower(right[index])) return false;
    return true;
}

struct Response
{
    unsigned status = 0;
    std::string headers;
    std::string body;

    std::string Header(const std::string_view name) const
    {
        auto begin = headers.find("\r\n");
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
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
                return std::string(value);
            }
            begin = end + 2;
        }
        return {};
    }
};

class Client final
{
public:
    explicit Client(const std::uint16_t port, const int receiveBuffer = 0)
    {
        mSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!Check(mSocket != ServerCoreTest::InvalidSocket, "async client socket opens")) return;
        ExpectTrue(ServerCoreTest::SetSocketTimeouts(mSocket, 10000), "async client I/O has deadlines");
        if (receiveBuffer != 0)
            ExpectTrue(::setsockopt(mSocket, SOL_SOCKET, SO_RCVBUF,
                reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer)) == 0,
                "slow reader has a bounded receive window");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        mConnected = Check(::connect(mSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "async client connects");
    }

    ~Client() { Close(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void Close()
    {
        if (mSocket != ServerCoreTest::InvalidSocket)
        {
            (void)::shutdown(mSocket, ServerCoreTest::ShutdownBoth);
            ServerCoreTest::CloseSocket(mSocket);
            mSocket = ServerCoreTest::InvalidSocket;
            mConnected = false;
        }
    }

    bool Send(std::string_view bytes)
    {
        if (!mConnected) return false;
        while (!bytes.empty())
        {
            const int sent = ServerCoreTest::Send(mSocket, bytes.data(),
                static_cast<int>(std::min<std::size_t>(bytes.size(), 4096)));
            if (!Check(sent > 0, "async request sends completely")) return false;
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    std::string Read(const std::size_t size)
    {
        if (!Check(size <= MaximumResponseBytes, "test response allocation is bounded")) return {};
        std::string result(size, '\0');
        std::size_t offset = 0;
        while (offset < size)
        {
            const int count = ServerCoreTest::Receive(mSocket, result.data() + offset,
                static_cast<int>(std::min<std::size_t>(size - offset, 16384)));
            if (!Check(count > 0, "async response bytes arrive before the socket deadline"))
            {
                result.resize(offset);
                return result;
            }
            offset += static_cast<std::size_t>(count);
        }
        return result;
    }

    std::optional<std::string> Line()
    {
        std::string line;
        while (line.size() < 65536)
        {
            const auto byte = Read(1);
            if (byte.empty()) return std::nullopt;
            line += byte;
            if (line.ends_with("\r\n")) { line.resize(line.size() - 2); return line; }
        }
        ExpectTrue(false, "response line is bounded");
        return std::nullopt;
    }

    Response ReadHead()
    {
        Response response;
        while (response.headers.size() < 65536)
        {
            const auto line = Line();
            if (!line) return response;
            response.headers += *line + "\r\n";
            if (line->empty()) break;
        }
        if (!Check(response.headers.starts_with("HTTP/1.1 ") && response.headers.size() >= 12,
                "stream response has an HTTP/1.1 status line")) return response;
        const auto parsed = std::from_chars(response.headers.data() + 9, response.headers.data() + 12, response.status);
        ExpectTrue(parsed.ec == std::errc{}, "response status is numeric");
        return response;
    }

    std::optional<std::string> Chunk()
    {
        const auto line = Line();
        if (!line) return std::nullopt;
        std::size_t size = 0;
        const auto parsed = std::from_chars(line->data(), line->data() + line->size(), size, 16);
        if (!Check(parsed.ec == std::errc{} && parsed.ptr == line->data() + line->size() &&
                size <= MaximumResponseBytes, "stream chunk has a valid bounded hexadecimal size")) return std::nullopt;
        if (size == 0)
        {
            const auto trailerEnd = Line();
            ExpectTrue(trailerEnd && trailerEnd->empty(), "final chunk ends with an empty trailer section");
            return std::string{};
        }
        auto bytes = Read(size);
        ExpectEqual(std::string("\r\n"), Read(2), "each chunk has its required terminator");
        return bytes;
    }

    Response ReadResponse(const bool head = false)
    {
        auto response = ReadHead();
        if (head || response.status < 200 || response.status == 204 || response.status == 205 || response.status == 304)
            return response;
        if (EqualHeaderName(response.Header("Transfer-Encoding"), "chunked"))
        {
            while (response.body.size() <= MaximumResponseBytes)
            {
                auto chunk = Chunk();
                if (!chunk || chunk->empty()) break;
                response.body += *chunk;
            }
            ExpectTrue(response.body.size() <= MaximumResponseBytes, "aggregate streamed response is bounded in the test client");
        }
        else
        {
            const auto value = response.Header("Content-Length");
            std::size_t size = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), size);
            if (Check(!value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
                    "fixed response supplies an unambiguous content length")) response.body = Read(size);
        }
        return response;
    }

    std::pair<unsigned, std::string> Frame()
    {
        const auto header = Read(2);
        if (header.size() != 2) return {};
        const auto first = static_cast<unsigned char>(header[0]);
        const auto second = static_cast<unsigned char>(header[1]);
        ExpectTrue((first & 128U) != 0 && (second & 128U) == 0, "upgraded server frame is final and unmasked");
        std::uint64_t size = second & 127U;
        if (size >= 126)
        {
            const auto extended = Read(size == 126 ? 2 : 8);
            size = 0;
            for (const char byte : extended) size = (size << 8) | static_cast<unsigned char>(byte);
        }
        if (!Check(size <= MaximumResponseBytes, "upgraded frame allocation is bounded")) return {};
        return {first & 15U, Read(static_cast<std::size_t>(size))};
    }

    bool DrainUntilClosed(const std::size_t maximum = 65536)
    {
        std::array<char, 4096> buffer{};
        std::size_t received = 0;
        while (received <= maximum)
        {
            const int count = ServerCoreTest::Receive(mSocket, buffer.data(), static_cast<int>(buffer.size()));
            if (count == 0) return true;
            if (count < 0)
            {
                const int error = ServerCoreTest::LastSocketError();
#ifdef _WIN32
                return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN;
#else
                return error == ECONNRESET || error == ENOTCONN;
#endif
            }
            received += static_cast<std::size_t>(count);
        }
        return false;
    }

private:
    ServerCoreTest::Socket mSocket = ServerCoreTest::InvalidSocket;
    bool mConnected = false;
};

std::string Get(const std::string_view path, const bool close = false)
{
    return "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n" +
        (close ? "Connection: close\r\n" : "") + "\r\n";
}

void AsyncRequestOwnershipAndIoProgress()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "async socket runtime starts")) return;
    Inbox<Request> requests;
    const auto gate = std::make_shared<HandlerGate>();
    Web::HttpServer server;
    const ReleaseGateOnExit release{gate};
    ExpectTrue(server.RegisterAsyncRoutePattern("POST", "/plugins/{id}", [&](Request request) {
        requests.Publish(std::move(request));
    }).IsOk(), "async pattern route registers");
    ExpectTrue(server.RegisterRoute("GET", "/health", [](const Web::HttpRequest&) {
        return Web::HttpResponse{200, {}, "ready"};
    }).IsOk(), "synchronous adapter route registers alongside async route");
    ExpectTrue(server.RegisterRoute("GET", "/blocking", [gate](const Web::HttpRequest&) {
        gate->Enter();
        return Web::HttpResponse{200, {}, "unblocked"};
    }).IsOk(), "blocking synchronous adapter route registers");
    ExpectTrue(server.RegisterRoute("GET", "/counter", [count = 0](const Web::HttpRequest&) mutable {
        return Web::HttpResponse{200, {}, std::to_string(++count)};
    }).IsOk(), "synchronous route retains its mutable callable state");
    ExpectTrue(server.RegisterAsyncRoutePattern("GET", "/counter/{id}", [count = 0](Request request) mutable {
        (void)request->response->Complete({200, {}, std::to_string(++count)});
    }).IsOk(), "async pattern route retains its mutable callable state");
    Web::HttpServerOptions options;
    options.ioWorkerThreadCount = 1;
    options.handlerWorkerThreadCount = 2;
    if (!Start(server, options)) return;
    Client delayed(server.Port());
    std::string wire = "POST /plugins/saved%20value?q=one HTTP/1.1\r\nHost: localhost\r\nX-Owned: yes\r\nContent-Length: 3\r\n\r\n";
    wire.append("a\0b", 3);
    if (!delayed.Send(wire)) return;
    const auto request = requests.Next();
    if (!Check(request != nullptr, "deferred request context is delivered")) return;
    Client blocking(server.Port());
    if (!blocking.Send(Get("/blocking"))) return;
    if (!Check(gate->WaitForEntry(), "synchronous handler occupies one handler worker")) return;
    Client independent(server.Port());
    if (!independent.Send(Get("/health", true))) return;
    ExpectEqual(std::string("ready"), independent.ReadResponse().body,
        "deferred response and blocked synchronous handler leave the sole I/O worker available");
    gate->Release();
    ExpectEqual(std::string("unblocked"), blocking.ReadResponse().body, "synchronous adapter completes after its worker gate opens");
    ExpectTrue(!gate->TimedOut(), "other connection progressed while the synchronous handler was still blocked");
    ExpectEqual(std::string("saved value"), std::string(request->request.PathParameter("id")), "async context owns decoded path captures");
    ExpectEqual(std::string("/plugins/saved%20value?q=one"), request->request.target, "async context retains the original request target");
    ExpectEqual(std::string("yes"), std::string(request->request.Header("x-owned")), "async context owns request headers after handler return");
    ExpectEqual(std::string("a\0b", 3), request->request.body, "async context owns binary request body");
    ExpectTrue(request->response->Complete({200, {}, "deferred"}).IsOk(), "response can complete outside the handler call");
    ExpectEqual(std::string("deferred"), delayed.ReadResponse().body, "deferred response reaches its original connection");
    ExpectTrue(request->response->Complete({200, {}, "duplicate"}).Code() == ErrorCode::Closed,
        "a completed request cannot publish a second response");
    Client counter(server.Port());
    for (const auto path : {"/counter", "/counter/value"})
        for (unsigned expected = 1; expected <= 2; ++expected)
        {
            if (!counter.Send(Get(path))) return;
            ExpectEqual(std::to_string(expected), counter.ReadResponse().body,
                "sequential requests invoke the same registered callable instead of a fresh capture copy");
        }
    ExpectTrue(server.Stop().IsOk(), "async ownership server stops");
    ExpectEqual(std::string("saved value"), std::string(request->request.PathParameter("id")), "owned context remains readable after server shutdown");
}

void WebSocketMaintenanceCloseSerialization()
{
    Inbox<Request> pending;
    Inbox<std::shared_ptr<Web::WebSocketConnection>> opened;
    const auto gate = std::make_shared<HandlerGate>();
    std::atomic<ErrorCode> callbackSend{ErrorCode::WouldBlock};
    std::atomic<unsigned> closes{0};
    Web::HttpServer server;
    const ReleaseGateOnExit release{gate};
    ExpectTrue(server.RegisterAsyncRoute("GET", "/deferred", [&](Request request) {
        pending.Publish(std::move(request));
    }).IsOk(), "maintenance ordering route registers");
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&](const auto& socket) {
        opened.Publish(socket);
        gate->Enter();
        callbackSend.store(socket->SendText("from-open").Code());
    };
    callbacks.onClose = [&](std::uint64_t, std::uint16_t, std::string_view) { ++closes; };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "maintenance ordering WebSocket registers");
    if (!Start(server)) return;
    Client client(server.Port());
    const std::string upgrade = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    if (!client.Send(Get("/deferred") + upgrade)) return;
    const auto request = pending.Next();
    if (!Check(request != nullptr, "deferred request holds the queued WebSocket upgrade")) return;
    ExpectTrue(request->response->Complete({200, {}, "done"}).IsOk(), "maintenance may advance the queued upgrade");
    ExpectEqual(200U, client.ReadResponse().status, "deferred response arrives before the upgrade");
    ExpectEqual(101U, client.ReadHead().status, "queued upgrade enters its callback");
    const auto socket = opened.Next();
    if (!Check(socket != nullptr && gate->WaitForEntry(), "onOpen holds the Peer operation lock")) return;
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool finished = false;
    ErrorCode closeCode = ErrorCode::WouldBlock;
    std::thread closer([&] {
        {
            const std::lock_guard lock(mutex);
            started = true;
        }
        changed.notify_all();
        const auto result = socket->Close();
        {
            const std::lock_guard lock(mutex);
            closeCode = result.Code();
            finished = true;
        }
        changed.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        ExpectTrue(changed.wait_for(lock, WaitLimit, [&] { return started; }), "external WebSocket Close starts");
        ExpectTrue(!changed.wait_for(lock, 50ms, [&] { return finished; }), "external Close cannot overtake an active maintenance callback");
    }
    gate->Release();
    closer.join();
    ExpectTrue(closeCode == ErrorCode::Ok && callbackSend.load() == ErrorCode::Ok,
        "callback's reentrant send and external Close finish in lock order");
    const auto frame = client.Frame();
    ExpectTrue(frame.first == 1 && frame.second == "from-open", "callback send precedes the concurrent close frame");
    client.Close();
    ExpectTrue(server.Stop().IsOk(), "maintenance ordering server shuts down without a lock inversion");
    ExpectTrue(!gate->TimedOut(), "maintenance callback was released within its bound");
    ExpectEqual(1U, closes.load(), "concurrent close still notifies exactly once");
}

void AsyncPipelineOrderAndWebSocketUpgrade()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "pipeline socket runtime starts")) return;
    Inbox<Request> pending;
    Inbox<std::shared_ptr<Web::WebSocketConnection>> upgraded;
    std::atomic<unsigned> secondCalls{0};
    std::atomic<unsigned> closes{0};
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/first", [&](Request request) { pending.Publish(std::move(request)); }).IsOk(), "deferred pipeline route registers");
    ExpectTrue(server.RegisterRoute("GET", "/second", [&](const Web::HttpRequest&) {
        secondCalls.fetch_add(1);
        return Web::HttpResponse{200, {}, "second"};
    }).IsOk(), "second pipeline route registers");
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&](const auto& connection) { upgraded.Publish(connection); };
    callbacks.onClose = [&](std::uint64_t, std::uint16_t, std::string_view) { closes.fetch_add(1); };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "pipeline upgrade route registers");
    Web::HttpServerOptions options;
    options.ioWorkerThreadCount = 1;
    if (!Start(server, options)) return;
    Client client(server.Port());
    const std::string handshake = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    if (!client.Send(Get("/first") + Get("/second") + handshake)) return;
    const auto first = pending.Next();
    if (!Check(first != nullptr, "first pipelined request is dispatched")) return;
    std::this_thread::sleep_for(100ms);
    ExpectEqual(0U, secondCalls.load(), "only one HTTP request per connection executes before response completion");
    ExpectTrue(first->response->Complete({200, {}, "first"}).IsOk(), "first pipeline response completes");
    ExpectEqual(std::string("first"), client.ReadResponse().body, "first response precedes later ready routes");
    ExpectEqual(std::string("second"), client.ReadResponse().body, "second response follows the completed async response");
    ExpectEqual(101U, client.ReadHead().status, "WebSocket upgrade follows both HTTP responses");
    const auto socket = upgraded.Next();
    if (!Check(socket != nullptr, "WebSocket opens after pipelined HTTP responses")) return;
    const auto flow = Web::GetWebSocketFlowControl(socket);
    if (!Check(flow != nullptr, "server WebSocket exposes optional send flow control")) return;
    const auto completion = std::make_shared<Completion>();
    auto wait = flow->WaitForSendCapacity(8, [completion](Status status) { completion->Complete(status); }, {});
    if (Check(wait.IsOk(), "WebSocket capacity notification registration succeeds"))
    {
        ExpectTrue(completion->Wait() && completion->Code() == ErrorCode::Ok, "WebSocket capacity becomes available without polling its queue");
        wait.Value().Reset();
    }
    ExpectTrue(socket->SendText("upgraded").IsOk(), "WebSocket sends after the mixed-protocol pipeline");
    const auto frame = client.Frame();
    ExpectTrue(frame.first == 1 && frame.second == "upgraded", "upgrade leaves no HTTP framing bytes in the WebSocket stream");
    ExpectTrue(first->response->Write(Bytes("late")).Code() == ErrorCode::Closed, "retained old HTTP writer cannot write into the upgraded socket");
    ExpectTrue(server.Stop().IsOk(), "mixed pipeline server stops");
    ExpectEqual(1U, secondCalls.load(), "second pipelined handler executes exactly once");
    ExpectEqual(1U, closes.load(), "upgraded connection closes exactly once");
    WebSocketMaintenanceCloseSerialization();
}

void AsyncCancellationAndShutdown()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "cancellation socket runtime starts")) return;
    Inbox<Request> requests;
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/pending", [&](Request request) { requests.Publish(std::move(request)); }).IsOk(), "cancellable async route registers");
    if (!Start(server)) return;
    Client disconnected(server.Port());
    if (!disconnected.Send(Get("/pending"))) return;
    const auto first = requests.Next();
    if (!Check(first != nullptr, "disconnect test has an outstanding response")) return;
    std::atomic<unsigned> disconnectCancellation{0};
    std::stop_callback firstCancellation(first->response->GetCancellationToken(), [&] { disconnectCancellation.fetch_add(1); });
    disconnected.Close();
    ExpectTrue(WaitCancelled(first->response->GetCancellationToken()), "peer disconnect cancels a deferred request without a new application write");
    ExpectTrue(first->response->Complete({200, {}, "late"}).Code() == ErrorCode::Closed, "completion after disconnect reports Closed");
    ExpectTrue(first->response->Write(Bytes("late")).Code() == ErrorCode::Closed, "write after disconnect reports Closed");
    Client stopping(server.Port());
    if (!stopping.Send(Get("/pending"))) return;
    const auto second = requests.Next();
    if (!Check(second != nullptr, "shutdown test has an outstanding response")) return;
    Web::HttpResponseHead head;
    head.contentLength = 100;
    ExpectTrue(second->response->Start(head).IsOk() && second->response->Write(Bytes("partial")).IsOk(), "stream begins before shutdown cancellation");
    std::atomic<unsigned> shutdownCancellation{0};
    std::stop_callback secondCancellation(second->response->GetCancellationToken(), [&] { shutdownCancellation.fetch_add(1); });
    ExpectTrue(server.Stop().IsOk(), "server stops with unfinished deferred and streaming writers retained externally");
    ExpectTrue(second->response->GetCancellationToken().stop_requested(), "Stop requests cancellation before returning");
    ExpectTrue(second->response->Finish().Code() == ErrorCode::Closed, "unfinished writer cannot finish after Stop");
    ExpectTrue(second->response->Start({}).Code() == ErrorCode::Closed, "late headers cannot restart a stopped request");
    ExpectEqual(1U, disconnectCancellation.load(), "disconnect cancellation callback runs once");
    ExpectEqual(1U, shutdownCancellation.load(), "shutdown cancellation callback runs once");
}

void AsyncHandlerDeadline()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "deadline socket runtime starts")) return;
    Inbox<Request> requests;
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/never", [&](Request request) { requests.Publish(std::move(request)); }).IsOk(), "deadline route registers");
    Web::HttpServerOptions options;
    options.handlerTimeout = 150ms;
    options.idleTimeout = 5s;
    options.requestTimeout = 5s;
    if (!Start(server, options)) return;
    Client client(server.Port());
    if (!client.Send(Get("/never"))) return;
    const auto request = requests.Next();
    if (!Check(request != nullptr, "handler deadline applies to an admitted request")) return;
    ExpectTrue(WaitCancelled(request->response->GetCancellationToken()), "returning without headers is bounded by handlerTimeout");
    ExpectTrue(request->response->Start({}).Code() == ErrorCode::Closed, "expired handler cannot begin a late response");
    ExpectTrue(request->response->Complete({200, {}, "late"}).Code() == ErrorCode::Closed, "expired handler cannot publish a late complete response");
    ExpectTrue(client.DrainUntilClosed(), "expired header deadline releases its TCP connection");
    ExpectTrue(server.Stop().IsOk(), "deadline server stops");
}

void StreamingFramingAndHead()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "framing socket runtime starts")) return;
    Inbox<Request> requests;
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/stream", [&](Request request) { requests.Publish(std::move(request)); }).IsOk(), "streaming route registers");
    if (!Start(server)) return;
    {
        Client client(server.Port());
        if (!client.Send(Get("/stream", true))) return;
        const auto request = requests.Next();
        if (!Check(request != nullptr, "chunked request is admitted")) return;
        Web::HttpResponseHead head;
        head.headers = {{"Content-Type", "application/octet-stream"}};
        ExpectTrue(request->response->Start(head).IsOk(), "unknown content length begins chunked transfer");
        ExpectTrue(request->response->Start(head).Code() == ErrorCode::AlreadyExists, "second live Start cannot emit another header block");
        const std::string first("a\0b", 3);
        ExpectTrue(request->response->Write(Bytes(first)).IsOk() && request->response->Write(Bytes("tail")).IsOk(), "binary stream chunks are admitted");
        ExpectTrue(request->response->Finish().IsOk(), "chunked stream emits its terminal chunk");
        ExpectTrue(request->response->Finish().Code() == ErrorCode::Closed, "second Finish is rejected");
        ExpectTrue(request->response->Write(Bytes("late")).Code() == ErrorCode::Closed, "finished stream rejects late bytes");
        const auto response = client.ReadResponse();
        ExpectEqual(std::string("chunked"), response.Header("Transfer-Encoding"), "stream owns chunked transfer framing");
        ExpectTrue(response.Header("Content-Length").empty(), "chunked stream never emits conflicting content length");
        ExpectEqual(first + "tail", response.body, "chunk framing preserves binary payloads exactly");
        ExpectTrue(client.DrainUntilClosed(0), "terminal chunk is followed by close without extra body data");
    }
    {
        Client client(server.Port());
        if (!client.Send(Get("/stream", true))) return;
        const auto request = requests.Next();
        if (!Check(request != nullptr, "fixed stream request is admitted")) return;
        Web::HttpResponseHead head;
        head.contentLength = 5;
        ExpectTrue(request->response->Start(head).IsOk(), "fixed stream begins with declared content length");
        ExpectTrue(request->response->Write(Bytes("excess")).Code() == ErrorCode::InvalidArgument, "fixed stream rejects a write larger than remaining length atomically");
        ExpectTrue(request->response->Write(Bytes("he")).IsOk(), "fixed stream accepts a prefix");
        ExpectTrue(request->response->Finish().Code() == ErrorCode::InvalidArgument, "short fixed stream cannot finish prematurely");
        ExpectTrue(request->response->Write(Bytes("llo")).IsOk() && request->response->Finish().IsOk(), "caller can correct a premature Finish by completing the body");
        const auto response = client.ReadResponse();
        ExpectEqual(std::string("5"), response.Header("Content-Length"), "fixed response length matches admitted bytes");
        ExpectTrue(response.Header("Transfer-Encoding").empty(), "fixed response never emits chunked framing");
        ExpectEqual(std::string("hello"), response.body, "rejected fixed writes add no wire bytes");
        ExpectTrue(client.DrainUntilClosed(0), "fixed response ends exactly at content length");
    }
    {
        Client client(server.Port());
        if (!client.Send("HEAD /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")) return;
        const auto request = requests.Next();
        if (!Check(request != nullptr, "HEAD falls back to the async GET route")) return;
        ExpectTrue(request->response->IsHeadRequest(), "writer exposes HEAD semantics to streaming producers");
        Web::HttpResponseHead head;
        head.contentLength = std::uint64_t{4} * 1024 * 1024 * 1024 + 1;
        ExpectTrue(request->response->Start(head).IsOk() && request->response->Finish().IsOk(), "HEAD can report a large representation without writing its body");
        const auto response = client.ReadResponse(true);
        ExpectEqual(std::string("4294967297"), response.Header("Content-Length"), "HEAD length is not truncated to 32 bits");
        ExpectTrue(client.DrainUntilClosed(0), "HEAD writes neither body bytes nor a terminal chunk");
    }
    for (const unsigned status : {204U, 205U, 304U})
    {
        Client client(server.Port());
        if (!client.Send(Get("/stream", true))) return;
        const auto request = requests.Next();
        if (!Check(request != nullptr, "bodyless status request is admitted")) return;
        Web::HttpResponseHead head;
        head.status = status;
        if (status == 304) head.contentLength = 123;
        ExpectTrue(request->response->Start(head).IsOk() && request->response->Finish().IsOk(), "bodyless status finishes without payload writes");
        const auto response = client.ReadResponse();
        ExpectEqual(status, response.status, "streaming preserves the bodyless status code");
        ExpectTrue(response.Header("Transfer-Encoding").empty(), "bodyless status omits chunked transfer coding");
        ExpectTrue(client.DrainUntilClosed(0), "bodyless status adds no forbidden payload or chunk terminator");
    }
    ExpectTrue(server.Stop().IsOk(), "framing server stops");
}

void StreamingSseOutlivesInboundIdle()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "SSE socket runtime starts")) return;
    Inbox<Request> requests;
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/events", [&](Request request) { requests.Publish(std::move(request)); }).IsOk(), "SSE route registers");
    Web::HttpServerOptions options;
    options.idleTimeout = 100ms;
    options.streamIdleTimeout = 2s;
    options.handlerTimeout = 100ms;
    if (!Start(server, options)) return;
    Client client(server.Port());
    if (!client.Send(Get("/events", true))) return;
    const auto request = requests.Next();
    if (!Check(request != nullptr, "SSE request is admitted")) return;
    Web::HttpResponseHead head;
    head.headers = {{"Content-Type", "text/event-stream"}, {"Cache-Control", "no-cache"}};
    if (!Check(request->response->Start(head).IsOk(), "SSE headers begin a chunked response")) return;
    const auto response = client.ReadHead();
    ExpectEqual(std::string("text/event-stream"), response.Header("Content-Type"), "SSE MIME type survives streaming headers");
    ExpectEqual(std::string("chunked"), response.Header("Transfer-Encoding"), "SSE uses bounded chunk framing");
    for (unsigned index = 0; index < 8; ++index)
    {
        std::this_thread::sleep_for(50ms);
        Web::SseEvent event;
        event.data = "tick-" + std::to_string(index);
        event.event = "update";
        event.id = std::to_string(index);
        if (!Check(Web::WriteSseEvent(*request->response, event).IsOk(), "active SSE output survives the ordinary inbound idle deadline")) break;
        const auto chunk = client.Chunk();
        ExpectTrue(chunk && chunk->find("data: tick-" + std::to_string(index) + "\n") != std::string::npos &&
            chunk->ends_with("\n\n"), "each SSE event arrives complete in order without new client input");
    }
    ExpectTrue(!request->response->GetCancellationToken().stop_requested(), "outgoing stream activity keeps the request live beyond handler and inbound idle deadlines");
    ExpectTrue(request->response->Finish().IsOk(), "SSE stream can finish explicitly");
    const auto finalChunk = client.Chunk();
    ExpectTrue(finalChunk && finalChunk->empty(), "SSE Finish emits exactly one terminal chunk");
    ExpectTrue(client.DrainUntilClosed(0), "completed SSE response closes after its terminal chunk");
    ExpectTrue(server.Stop().IsOk(), "SSE server stops");
}

void StreamingBackpressureAndStall()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "stall socket runtime starts")) return;
    Inbox<Request> requests;
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/unread", [&](Request request) { requests.Publish(std::move(request)); }).IsOk(), "bounded stream route registers");
    Web::HttpServerOptions options;
    options.maxTotalSendQueueCapacityBytes = 16 * 1024;
    options.maxStreamChunkBytes = 4096;
    options.sendStallTimeout = 250ms;
    options.streamIdleTimeout = 5s;
    options.idleTimeout = 5s;
    if (!Start(server, options)) return;
    Client unread(server.Port(), 1024);
    if (!unread.Send(Get("/unread"))) return;
    const auto request = requests.Next();
    if (!Check(request != nullptr, "slow-consumer stream is admitted")) return;
    if (!Check(request->response->Start({}).IsOk(), "slow-consumer response begins")) return;
    const auto writer = request->response;
    ExpectTrue(writer->MaxWriteBytes() > 0 && writer->MaxWriteBytes() <= options.maxStreamChunkBytes,
        "writer advertises its bounded atomic write size");
    const auto chunk = Payload(std::min<std::size_t>(4096, writer->MaxWriteBytes()));
    const auto deadline = std::chrono::steady_clock::now() + WaitLimit;
    std::size_t accepted = 0;
    bool blocked = false;
    bool bounded = true;
    bool invalidFailure = false;
    while (!writer->GetCancellationToken().stop_requested() && accepted < 64 * 1024 * 1024 &&
        std::chrono::steady_clock::now() < deadline)
    {
        const auto status = writer->Write(Bytes(chunk));
        bounded = bounded && writer->RetainedSendBytes() <= options.maxTotalSendQueueCapacityBytes;
        if (!bounded) break;
        if (status.IsOk()) { accepted += chunk.size(); continue; }
        if (status.Code() == ErrorCode::Closed) break;
        if (status.Code() != ErrorCode::WouldBlock) { invalidFailure = true; break; }
        blocked = true;
        const auto completion = std::make_shared<Completion>();
        auto wait = writer->WaitForWriteCapacity(chunk.size(), [completion](Status ready) { completion->Complete(ready); });
        if (!wait.IsOk())
        {
            if (wait.GetStatus().Code() != ErrorCode::Closed) invalidFailure = true;
            break;
        }
        const bool notified = completion->Wait();
        wait.Value().Reset();
        if (!notified) { invalidFailure = true; break; }
        ExpectEqual(1U, completion->Calls(), "each write-capacity subscription completes once");
        if (completion->Code() == ErrorCode::Closed || completion->Code() == ErrorCode::Cancelled) break;
        if (completion->Code() != ErrorCode::Ok) { invalidFailure = true; break; }
    }
    ExpectTrue(accepted > 0 && blocked, "slow reader eventually applies backpressure after real bytes are admitted");
    ExpectTrue(bounded && !invalidFailure, "stream retains only the configured send budget while capacity is exhausted");
    ExpectTrue(WaitCancelled(writer->GetCancellationToken()), "send stall cancels a producer whose peer never consumes response bytes");
    ExpectTrue(writer->Write(Bytes("late")).Code() == ErrorCode::Closed, "stalled stream rejects subsequent writes");
    ExpectTrue(writer->Finish().Code() == ErrorCode::Closed, "stalled stream cannot finish after transport teardown");
    ExpectTrue(server.Stop().IsOk(), "stalled stream server stops without an unbounded producer queue");
    ExpectEqual(std::size_t{0}, writer->RetainedSendBytes(), "teardown releases all retained stream buffers");
}

class TemporaryFile final
{
public:
    TemporaryFile()
    {
        path = std::filesystem::temp_directory_path() /
            ("servercore-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    }
    ~TemporaryFile()
    {
        std::error_code ignored;
        (void)std::filesystem::remove(path, ignored);
    }
    std::filesystem::path path;
};

void StreamingFileIntegrityAndHead()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "file socket runtime starts")) return;
    TemporaryFile file;
    const auto bytes = Payload(2 * 1024 * 1024 + 37);
    {
        std::ofstream output(file.path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!Check(output.good(), "large binary fixture is written")) return;
    }
    Runtime::TaskExecutor executor;
    if (!Check(executor.Start().IsOk(), "file executor starts")) return;
    Inbox<Runtime::TaskHandle> tasks;
    std::atomic<unsigned> admissionFailures{0};
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/file", [&](Request request) {
        Web::HttpResponseHead head;
        head.headers = {{"Content-Type", "application/octet-stream"}};
        auto result = Web::SendFile(executor, request, file.path, std::move(head));
        if (result.IsOk()) tasks.Publish(result.Value());
        else { admissionFailures.fetch_add(1); request->response->Abort(); }
    }).IsOk(), "file helper route registers");
    ExpectTrue(server.RegisterRoute("GET", "/after", [](const Web::HttpRequest&) {
        return Web::HttpResponse{200, {}, "after-head"};
    }).IsOk(), "post-HEAD framing probe registers");
    Web::HttpServerOptions options;
    options.maxResponseBodyBytes = 1024;
    options.maxStreamChunkBytes = 16 * 1024;
    options.maxTotalSendQueueCapacityBytes = 128 * 1024;
    if (!Start(server, options)) return;
    Client client(server.Port());
    if (!client.Send(Get("/file"))) return;
    const auto response = client.ReadResponse();
    ExpectEqual(200U, response.status, "large file returns a successful response");
    ExpectEqual(std::to_string(bytes.size()), response.Header("Content-Length"), "file response declares its full size beyond 1 MiB");
    ExpectTrue(response.body == bytes, "file streaming preserves every binary byte across bounded writes");
    auto first = tasks.Next();
    ExpectTrue(first.IsValid() && first.WaitUntil(std::chrono::steady_clock::now() + WaitLimit).IsOk(), "file producer completes after the final admitted chunk");
    if (!client.Send("HEAD /file HTTP/1.1\r\nHost: localhost\r\n\r\n" + Get("/after", true))) return;
    const auto head = client.ReadResponse(true);
    ExpectEqual(200U, head.status, "file HEAD request succeeds");
    ExpectEqual(std::to_string(bytes.size()), head.Header("Content-Length"), "file HEAD reports the same representation length");
    ExpectEqual(std::string("after-head"), client.ReadResponse().body, "file HEAD emits no body before the next pipelined response");
    auto second = tasks.Next();
    ExpectTrue(second.IsValid() && second.WaitUntil(std::chrono::steady_clock::now() + WaitLimit).IsOk(), "HEAD file task finishes without consuming file body bytes");
    ExpectTrue(server.Stop().IsOk(), "file HTTP server stops before its executor");
    ExpectTrue(executor.Stop().IsOk(), "file executor drains all producer tasks");
    ExpectEqual(0U, admissionFailures.load(), "both file requests were admitted to the bounded executor");
}

void AsyncRetainedRequestBudget()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "retained request budget socket runtime starts")) return;
    Inbox<Request> requests;
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("POST", "/keep", [&](Request request) {
        requests.Publish(std::move(request));
    }).IsOk(), "retained request route registers");
    ExpectTrue(server.RegisterRoute("GET", "/small", [](const Web::HttpRequest&) {
        return Web::HttpResponse{200, {}, "small"};
    }).IsOk(), "small request budget probe registers");
    Web::HttpServerOptions options;
    options.maxActiveHttpRequests = 1;
    options.maxTotalRequestBytes = 5000;
    if (!Start(server, options)) return;
    const std::string body(3000, 'r');
    const std::string wire = "POST /keep HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    Client first(server.Port());
    if (!first.Send(wire)) return;
    auto retained = requests.Next();
    if (!Check(retained != nullptr, "first request context is retained by the application")) return;
    ExpectTrue(retained->response->Complete({200, {}, "done"}).IsOk(), "retained request response finishes");
    ExpectEqual(200U, first.ReadResponse().status, "first response arrives");
    if (!first.Send(Get("/small"))) return;
    ExpectEqual(200U, first.ReadResponse().status, "terminal response returns active count while retaining only its byte charge");
    Client rejected(server.Port());
    if (!rejected.Send(wire)) return;
    ExpectEqual(503U, rejected.ReadResponse().status, "completed but retained request body still consumes global request bytes");
    ExpectEqual(body, retained->request.body, "retained request body remains intact after response completion");
    retained.reset();
    Client accepted(server.Port());
    if (!accepted.Send(wire)) return;
    auto recovered = requests.Next();
    if (!Check(recovered != nullptr, "dropping the last owning context returns its request byte charge")) return;
    recovered->response->Abort();
    Client cancelled(server.Port());
    if (!cancelled.Send(wire)) return;
    ExpectEqual(503U, cancelled.ReadResponse().status, "cancelled but retained context also keeps its request byte charge");
    ExpectTrue(server.Stop().IsOk(), "request budget server stops with cancelled context retained");
}

const ServerCoreTest::CheckRegistration retainedBudget{
    "Web.AsyncRetainedRequestBudget", &AsyncRetainedRequestBudget};
const ServerCoreTest::CheckRegistration asyncOwnership{
    "Web.AsyncRequestOwnershipAndIoProgress", &AsyncRequestOwnershipAndIoProgress};
const ServerCoreTest::CheckRegistration asyncPipeline{
    "Web.AsyncPipelineOrderAndWebSocketUpgrade", &AsyncPipelineOrderAndWebSocketUpgrade};
const ServerCoreTest::CheckRegistration asyncCancellation{
    "Web.AsyncCancellationAndShutdown", &AsyncCancellationAndShutdown};
const ServerCoreTest::CheckRegistration asyncDeadline{
    "Web.AsyncHandlerDeadline", &AsyncHandlerDeadline};
const ServerCoreTest::CheckRegistration streamFraming{
    "Web.StreamingFramingAndHead", &StreamingFramingAndHead};
const ServerCoreTest::CheckRegistration streamSse{
    "Web.StreamingSseOutlivesInboundIdle", &StreamingSseOutlivesInboundIdle};
const ServerCoreTest::CheckRegistration streamStall{
    "Web.StreamingBackpressureAndStall", &StreamingBackpressureAndStall};
const ServerCoreTest::CheckRegistration streamFile{
    "Web.StreamingFileIntegrityAndHead", &StreamingFileIntegrityAndHead};
}
