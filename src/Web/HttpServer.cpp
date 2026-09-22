#include "ServerCore/Web/HttpServer.h"

#include "Core/Utf8Internal.h"
#include "ServerCore/Core/Assert.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/IoContext.h"
#include "Web/WebProtocol.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ServerCore::Web
{
namespace
{
using Core::ErrorCode;
using Core::Status;
using Clock = std::chrono::steady_clock;

std::int64_t Now() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::span<const std::byte> Bytes(std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string_view Text(std::span<const std::byte> bytes) noexcept
{
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

thread_local const void* activeWebCallback = nullptr;
class CallbackScope
{
public:
    explicit CallbackScope(const void* owner) noexcept : mPrevious(activeWebCallback) { activeWebCallback = owner; }
    ~CallbackScope() { activeWebCallback = mPrevious; }
private:
    const void* mPrevious;
};

bool ValidOptions(const HttpServerOptions& options) noexcept
{
    constexpr std::size_t maximumBody = 16 * 1024 * 1024;
    return options.port != 0 && options.ioWorkerThreadCount > 0 && options.ioWorkerThreadCount <= 64 &&
        options.acceptBacklog > 0 && options.maxConnections > 0 && options.maxConnections <= 65536 &&
        options.maxHeaderBytes >= 256 && options.maxHeaderBytes <= 64 * 1024 &&
        options.maxBodyBytes <= maximumBody && options.maxResponseBodyBytes > 0 &&
        options.maxResponseBodyBytes <= Net::SendQueueLimitBytes - options.maxHeaderBytes &&
        options.maxWebSocketFrameBytes > 0 && options.maxWebSocketFrameBytes <= Net::SendQueueLimitBytes - 14 &&
        options.maxWebSocketMessageBytes >= options.maxWebSocketFrameBytes && options.maxWebSocketMessageBytes <= maximumBody &&
        options.requestTimeout.count() > 0 && options.idleTimeout.count() > 0 &&
        options.responseDrainTimeout.count() > 0 && options.webSocketCloseTimeout.count() > 0;
}

bool ValidPath(std::string_view path) noexcept
{
    if (path.empty() || (path.front() != '/' && path != "*")) return false;
    for (char ch : path)
        if (static_cast<unsigned char>(ch) < 33 || static_cast<unsigned char>(ch) >= 127 || ch == '?' || ch == '#') return false;
    return true;
}
}

class HttpServer::State : public std::enable_shared_from_this<HttpServer::State>
{
public:
    class Peer;
    Status Start(const HttpServerOptions& value);
    Status Stop();
    void Shutdown();
    void Accepted(std::shared_ptr<Net::Connection> connection) noexcept;
    void Disconnected(std::uint64_t id) noexcept;
    void Sweep() noexcept;

    std::mutex lifecycleMutex;
    bool used = false;
    bool stopped = false;
    HttpServerOptions options;
    std::map<std::pair<std::string, std::string>, Handler> routes;
    std::map<std::string, std::shared_ptr<const WebSocketCallbacks>, std::less<>> sockets;
    Net::IoContext io;
    Net::Acceptor acceptor;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::mutex peersMutex;
    std::condition_variable peersChanged;
    std::unordered_map<std::uint64_t, std::shared_ptr<Peer>> peers;
    std::uint64_t nextId = 1;
    std::mutex sweepMutex;
    std::condition_variable sweepChanged;
    std::thread sweepThread;
};

class HttpServer::State::Peer final : public Net::IConnectionObserver,
                                    public WebSocketConnection,
                                    public std::enable_shared_from_this<Peer>
{
public:
    Peer(std::weak_ptr<State> owner, std::uint64_t id, std::shared_ptr<Net::Connection> connection,
        const HttpServerOptions& options)
        : mOwner(std::move(owner)), mId(id), mConnection(std::move(connection)), mOptions(options),
          mParser(options.maxHeaderBytes, options.maxBodyBytes), mLastActivity(Now()) {}

    std::uint64_t Id() const noexcept override { return mId; }
    bool IsOpen() const noexcept override { return mPhase.load() == Phase::WebSocket && mConnection->IsOpen(); }

    Status SendText(std::string_view text) override
    {
        return SendFrame(1, Bytes(text));
    }

    Status SendBinary(std::span<const std::byte> bytes) override { return SendFrame(2, bytes); }

    Status Ping(std::span<const std::byte> bytes) override
    {
        return SendFrame(9, bytes);
    }

    Status Close(std::uint16_t code, std::string_view reason) override
    {
        const std::lock_guard<std::recursive_mutex> guard(mSendMutex);
        const auto phase = mPhase.load();
        if (phase != Phase::WebSocket && phase != Phase::WsClosing)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        if (reason.size() > 123) return Status::FailWithoutMessage(ErrorCode::TooLarge);
        if (!Detail::ValidCloseCode(code) || code == 1010)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        if (!Core::Detail::IsValidUtf8(reason)) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        try
        {
            std::vector<std::byte> payload;
            payload.reserve(reason.size() + 2);
            payload.push_back(static_cast<std::byte>(code >> 8));
            payload.push_back(static_cast<std::byte>(code & 255u));
            const auto reasonBytes = Bytes(reason);
            payload.insert(payload.end(), reasonBytes.begin(), reasonBytes.end());
            return SendClose(payload);
        }
        catch (...) { mConnection->Close(); return Status::AllocationFailure(); }
    }

    void ForceClose() noexcept { mConnection->Close(); }

    void CheckTimeout(std::int64_t now) noexcept
    {
        const auto phase = mPhase.load();
        if (phase == Phase::Closed) return;
        const auto requestStarted = mRequestStarted.load();
        const auto closeStarted = mCloseStarted.load();
        if (now - mLastActivity.load() >= mOptions.idleTimeout.count() ||
            (phase == Phase::Http && requestStarted != 0 && now - requestStarted >= mOptions.requestTimeout.count()) ||
            (phase == Phase::HttpClosing && closeStarted != 0 && now - closeStarted >= mOptions.responseDrainTimeout.count()) ||
            (phase == Phase::WsClosing && closeStarted != 0 && now - closeStarted >= mOptions.webSocketCloseTimeout.count()))
            mConnection->Close();
    }

    void OnBytesReceived(std::span<const std::byte> bytes) override
    {
        try
        {
            mLastActivity.store(Now());
            while (!bytes.empty())
            {
                const auto phase = mPhase.load();
                if (phase == Phase::Closed || phase == Phase::HttpClosing || mIgnoreInput) return;
                if (phase == Phase::Http && mRequestStarted.load() == 0) mRequestStarted.store(Now());
                const auto limit = phase == Phase::Http ? mOptions.maxHeaderBytes :
                    (std::max)(mOptions.maxWebSocketFrameBytes, std::size_t{125}) + 14;
                if (mInput.size() >= limit)
                {
                    if (phase == Phase::Http) HttpError(431, mParser.IsHeadRequest());
                    else WebSocketError(1009);
                    return;
                }
                const auto count = (std::min)(bytes.size(), limit - mInput.size());
                mInput.append(Text(bytes.first(count)));
                bytes = bytes.subspan(count);
                ProcessInput();
            }
        }
        catch (...) { mConnection->Close(); }
    }

    void OnDisconnected(Status reason) override
    {
        (void)reason;
        mPhase.store(Phase::Closed);
        const auto owner = mOwner.lock();
        const auto callbacks = std::move(mCallbacks);
        std::string{}.swap(mInput);
        std::vector<std::byte>{}.swap(mMessage);
        mParser = Detail::HttpParser(mOptions.maxHeaderBytes, mOptions.maxBodyBytes);
        if (mUpgraded && callbacks && callbacks->onClose)
        {
            const CallbackScope callback(owner.get());
            try { callbacks->onClose(mId, mReceivedCloseCode, mReceivedCloseReason); }
            catch (...) { /* User exceptions never escape the transport observer. */ }
        }
        if (owner) owner->Disconnected(mId);
    }

private:
    enum class Phase { Http, WebSocket, WsClosing, HttpClosing, Closed };

    Status SendFrame(std::uint8_t opcode, std::span<const std::byte> payload, bool closingControl = false)
    {
        try
        {
            const std::lock_guard<std::recursive_mutex> guard(mSendMutex);
            const auto phase = mPhase.load();
            if (phase != Phase::WebSocket && !(closingControl && phase == Phase::WsClosing))
                return Status::FailWithoutMessage(ErrorCode::Closed);
            if (payload.size() > (opcode < 8 ? mOptions.maxWebSocketFrameBytes : std::size_t{125}))
                return Status::FailWithoutMessage(ErrorCode::TooLarge);
            if (opcode == 1 && !Core::Detail::IsValidUtf8(Text(payload)))
                return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
            const auto wire = Detail::EncodeServerFrame(opcode, payload);
            return mConnection->Send(wire);
        }
        catch (...) { return Status::AllocationFailure(); }
    }

    Status SendClose(std::span<const std::byte> payload)
    {
        const std::lock_guard<std::recursive_mutex> guard(mSendMutex);
        const auto phase = mPhase.load();
        if (phase == Phase::WsClosing) return Status::Ok();
        if (phase != Phase::WebSocket) return Status::FailWithoutMessage(ErrorCode::Closed);
        mCloseStarted.store(Now());
        mPhase.store(Phase::WsClosing);
        try
        {
            const auto wire = Detail::EncodeServerFrame(8, payload);
            auto result = mConnection->Send(wire);
            if (!result.IsOk()) mConnection->Close();
            return result;
        }
        catch (...) { mConnection->Close(); return Status::AllocationFailure(); }
    }

    void WebSocketError(std::uint16_t code)
    {
        mIgnoreInput = true;
        (void)Close(code, {});
        mConnection->CloseAfterSend();
    }

    void HttpError(unsigned int status, bool head = false)
    {
        HttpResponse response;
        response.status = status;
        response.body = std::string(Detail::ReasonPhrase(status)) + "\n";
        if (response.body.size() > mOptions.maxResponseBodyBytes) response.body.clear();
        response.close = true;
        if (status == 426)
        {
            response.headers.emplace_back("Upgrade", "websocket");
            response.headers.emplace_back("Sec-WebSocket-Version", "13");
        }
        Respond(response, head, true);
    }

    void Respond(const HttpResponse& response, bool head, bool close)
    {
        std::string wire;
        if (!Detail::SerializeResponse(response, head, close, mOptions.maxHeaderBytes,
                mOptions.maxResponseBodyBytes, wire))
        {
            wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            close = true;
        }
        if (close)
        {
            mCloseStarted.store(Now());
            mPhase.store(Phase::HttpClosing);
        }
        const auto sent = mConnection->Send(Bytes(wire));
        if (!sent.IsOk()) mConnection->Close();
        else if (close) mConnection->CloseAfterSend();
    }

    void ProcessInput()
    {
        while (true)
        {
            const auto phase = mPhase.load();
            if (phase == Phase::Closed || phase == Phase::HttpClosing || mIgnoreInput) return;
            if (phase != Phase::Http)
            {
                if (!ProcessWebSocketFrame()) return;
                continue;
            }
            if (!mInput.empty() && mRequestStarted.load() == 0) mRequestStarted.store(Now());
            HttpRequest request;
            const auto result = mParser.Parse(mInput, request);
            if (result.kind == Detail::HttpParseKind::NeedMore) return;
            if (result.kind == Detail::HttpParseKind::Error) { HttpError(result.status, mParser.IsHeadRequest()); return; }
            if (result.kind == Detail::HttpParseKind::Continue)
            {
                const auto sent = mConnection->Send(Bytes("HTTP/1.1 100 Continue\r\n\r\n"));
                if (!sent.IsOk()) { mConnection->Close(); return; }
                continue;
            }
            mRequestStarted.store(0);
            HandleRequest(request);
        }
    }

    void HandleRequest(const HttpRequest& request)
    {
        const auto owner = mOwner.lock();
        if (!owner || owner->stopping.load()) { mConnection->Close(); return; }
        const auto socket = owner->sockets.find(request.Path());
        if (socket != owner->sockets.end())
        {
            Upgrade(request, socket->second, owner.get());
            return;
        }
        const bool head = request.method == "HEAD";
        if (Detail::HasToken(request, "upgrade", "websocket")) { HttpError(404, head); return; }
        auto route = owner->routes.find({request.method, std::string(request.Path())});
        if (route == owner->routes.end() && head) route = owner->routes.find({"GET", std::string(request.Path())});
        HttpResponse response;
        if (route == owner->routes.end())
        {
            response.status = 404;
            response.body = "Not Found\n";
        }
        else
        {
            const CallbackScope callback(owner.get());
            try { response = route->second(request); }
            catch (...) { HttpError(500, head); return; }
        }
        Respond(response, head, response.close || Detail::HasToken(request, "connection", "close"));
    }

    void Upgrade(const HttpRequest& request, std::shared_ptr<const WebSocketCallbacks> callbacks,
        const State* owner)
    {
        if (request.method != "GET" || !request.body.empty() || !request.Header("transfer-encoding").empty() ||
            !Detail::HasToken(request, "connection", "upgrade") || Detail::HasToken(request, "connection", "close") ||
            !Detail::HasToken(request, "upgrade", "websocket")) { HttpError(400, request.method == "HEAD"); return; }
        std::size_t keys = 0;
        std::size_t versions = 0;
        for (const auto& [name, value] : request.headers)
        {
            (void)value;
            if (name == "sec-websocket-key") ++keys;
            if (name == "sec-websocket-version") ++versions;
        }
        if (keys != 1 || versions != 1) { HttpError(400); return; }
        if (request.Header("sec-websocket-version") != "13") { HttpError(426); return; }
        std::string accept;
        if (!Detail::WebSocketAccept(request.Header("sec-websocket-key"), accept)) { HttpError(400); return; }
        if (callbacks->accept)
        {
            const CallbackScope callback(owner);
            try { if (!callbacks->accept(request)) { HttpError(403); return; } }
            catch (...) { HttpError(500); return; }
        }
        const std::string wire = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        const auto sent = mConnection->Send(Bytes(wire));
        if (!sent.IsOk()) { mConnection->Close(); return; }
        mCallbacks = std::move(callbacks);
        mUpgraded = true;
        mPhase.store(Phase::WebSocket);
        if (mCallbacks->onOpen)
        {
            const CallbackScope callback(owner);
            try { mCallbacks->onOpen(shared_from_this()); }
            catch (...) { WebSocketError(1011); }
        }
    }

    bool ProcessWebSocketFrame()
    {
        Detail::WebSocketFrame frame;
        const auto result = Detail::ParseClientFrame(Bytes(mInput), mOptions.maxWebSocketFrameBytes, frame);
        if (result.kind == Detail::FrameParseKind::NeedMore) return false;
        if (result.kind == Detail::FrameParseKind::Error) { WebSocketError(result.closeCode); return false; }
        mInput.erase(0, result.consumed);
        if (frame.opcode == 8)
        {
            if (frame.payload.size() == 1) { WebSocketError(1002); return false; }
            std::uint16_t code = 1005;
            std::string_view reason;
            if (frame.payload.size() >= 2)
            {
                code = static_cast<std::uint16_t>((std::to_integer<unsigned int>(frame.payload[0]) << 8) |
                    std::to_integer<unsigned int>(frame.payload[1]));
                if (!Detail::ValidCloseCode(code)) { WebSocketError(1002); return false; }
                reason = Text(std::span(frame.payload).subspan(2));
                if (!Core::Detail::IsValidUtf8(reason)) { WebSocketError(1007); return false; }
            }
            mReceivedCloseReason.assign(reason);
            mReceivedCloseCode = code;
            mIgnoreInput = true;
            // 1010 is a client-only status. A server acknowledges it with normal closure.
            if (code == 1010) { frame.payload[0] = std::byte{0x03}; frame.payload[1] = std::byte{0xe8}; }
            (void)SendClose(frame.payload);
            mConnection->CloseAfterSend();
            return false;
        }
        if (frame.opcode == 9)
        {
            if (!SendFrame(10, frame.payload, true).IsOk()) mConnection->Close();
            return true;
        }
        if (frame.opcode == 10) return true;
        if (mPhase.load() == Phase::WsClosing) return true;
        if (frame.opcode == 0)
        {
            if (mFragmentOpcode == 0) { WebSocketError(1002); return false; }
        }
        else
        {
            if (mFragmentOpcode != 0) { WebSocketError(1002); return false; }
            mFragmentOpcode = frame.opcode;
        }
        if (frame.payload.size() > mOptions.maxWebSocketMessageBytes - mMessage.size())
        { WebSocketError(1009); return false; }
        mMessage.insert(mMessage.end(), frame.payload.begin(), frame.payload.end());
        if (!frame.final) return true;
        if (mFragmentOpcode == 1 && !Core::Detail::IsValidUtf8(Text(mMessage)))
        { WebSocketError(1007); return false; }
        const auto type = mFragmentOpcode == 1 ? WebSocketMessageType::Text : WebSocketMessageType::Binary;
        mFragmentOpcode = 0;
        if (mCallbacks && mCallbacks->onMessage)
        {
            const auto owner = mOwner.lock();
            const CallbackScope callback(owner.get());
            try { mCallbacks->onMessage(shared_from_this(), WebSocketMessage{type, mMessage}); }
            catch (...) { WebSocketError(1011); return false; }
        }
        mMessage.clear();
        return true;
    }

    std::weak_ptr<State> mOwner;
    const std::uint64_t mId;
    std::shared_ptr<Net::Connection> mConnection;
    const HttpServerOptions mOptions;
    Detail::HttpParser mParser;
    std::string mInput;
    std::atomic<Phase> mPhase{Phase::Http};
    std::atomic<std::int64_t> mLastActivity;
    std::atomic<std::int64_t> mRequestStarted{0};
    std::atomic<std::int64_t> mCloseStarted{0};
    // A transport failure can synchronously reenter OnDisconnected from Send.
    std::recursive_mutex mSendMutex;
    std::shared_ptr<const WebSocketCallbacks> mCallbacks;
    bool mUpgraded = false;
    bool mIgnoreInput = false;
    std::uint16_t mReceivedCloseCode = 1006;
    std::string mReceivedCloseReason;
    std::uint8_t mFragmentOpcode = 0;
    std::vector<std::byte> mMessage;
};

Status HttpServer::State::Start(const HttpServerOptions& value)
{
    const std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    if (used) return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!ValidOptions(value)) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    used = true;
    try
    {
        options = value;
        peers.reserve(options.maxConnections);
        acceptor.SetConnectionHandler([weak = weak_from_this()](std::shared_ptr<Net::Connection> connection)
        {
            if (const auto state = weak.lock()) state->Accepted(std::move(connection));
            else connection->Close();
        });
        auto result = acceptor.Listen(options.listenAddress, options.port, options.acceptBacklog);
        if (!result.IsOk()) return result;
        result = io.Start(options.ioWorkerThreadCount);
        if (!result.IsOk()) { Shutdown(); stopped = true; return result; }
        // Start the sweep before accepts, so a thread allocation failure has no peers to drain.
        sweepThread = std::thread([this] { Sweep(); });
        result = acceptor.Start(io);
        if (!result.IsOk())
        {
            Shutdown();
            stopped = true;
            return result;
        }
        running.store(true);
        return Status::Ok();
    }
    catch (...)
    {
        Shutdown();
        stopped = true;
        return Status::AllocationFailure();
    }
}

Status HttpServer::State::Stop()
{
    if (activeWebCallback == this || io.IsCurrentThreadIoThread())
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    const std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    if (!used || stopped) return Status::Ok();
    Shutdown();
    stopped = true;
    return Status::Ok();
}

void HttpServer::State::Shutdown()
{
    stopping.store(true);
    running.store(false);
    acceptor.Stop();
    sweepChanged.notify_all();
    if (sweepThread.joinable()) sweepThread.join();
    // Avoid allocating a shutdown snapshot: close one retained peer at a time,
    // then wait for its observer to release the registry entry and callback.
    while (true)
    {
        std::shared_ptr<Peer> peer;
        std::uint64_t id = 0;
        {
            const std::lock_guard<std::mutex> guard(peersMutex);
            if (peers.empty()) break;
            id = peers.begin()->first;
            peer = peers.begin()->second;
        }
        peer->ForceClose();
        std::unique_lock<std::mutex> lock(peersMutex);
        peersChanged.wait(lock, [this, id] { return peers.find(id) == peers.end(); });
    }
    io.Stop();
}

void HttpServer::State::Accepted(std::shared_ptr<Net::Connection> connection) noexcept
{
    std::shared_ptr<Peer> peer;
    try
    {
        {
            const std::lock_guard<std::mutex> guard(peersMutex);
            if (!stopping.load() && peers.size() < options.maxConnections && nextId != 0)
            {
                const auto id = nextId++;
                peer = std::make_shared<Peer>(weak_from_this(), id, connection, options);
                peers.emplace(id, peer);
            }
        }
        if (!peer) { connection->Close(); return; }
        connection->SetObserver(peer);
    }
    catch (...)
    {
        connection->Close();
        if (peer) Disconnected(peer->Id());
    }
}

void HttpServer::State::Disconnected(std::uint64_t id) noexcept
{
    const std::lock_guard<std::mutex> guard(peersMutex);
    peers.erase(id);
    peersChanged.notify_all();
}

void HttpServer::State::Sweep() noexcept
{
    std::unique_lock<std::mutex> waitLock(sweepMutex);
    while (!stopping.load())
    {
        sweepChanged.wait_for(waitLock, std::chrono::milliseconds(25), [this] { return stopping.load(); });
        if (stopping.load()) break;
        waitLock.unlock();
        try
        {
            std::vector<std::shared_ptr<Peer>> snapshot;
            {
                const std::lock_guard<std::mutex> guard(peersMutex);
                snapshot.reserve(peers.size());
                for (const auto& [id, peer] : peers) { (void)id; snapshot.push_back(peer); }
            }
            const auto now = Now();
            for (const auto& peer : snapshot) peer->CheckTimeout(now);
        }
        catch (...) { /* A later sweep retries if a temporary snapshot cannot be allocated. */ }
        waitLock.lock();
    }
}

HttpServer::HttpServer() : mState(std::make_shared<State>()) {}

HttpServer::~HttpServer()
{
    const auto result = mState->Stop();
    SERVERCORE_ASSERT(result.IsOk(), "HttpServer must be destroyed outside its callbacks and I/O threads");
}

Status HttpServer::RegisterRoute(std::string_view method, std::string_view path, Handler handler)
{
    if (activeWebCallback == mState.get()) return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!Detail::IsToken(method) || !ValidPath(path) || !handler)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);
        if (mState->used) return Status::FailWithoutMessage(ErrorCode::Closed);
        const auto inserted = mState->routes.emplace(std::make_pair(std::string(method), std::string(path)), std::move(handler));
        return inserted.second ? Status::Ok() : Status::FailWithoutMessage(ErrorCode::AlreadyExists);
    }
    catch (...) { return Status::AllocationFailure(); }
}

Status HttpServer::RegisterWebSocket(std::string_view path, WebSocketCallbacks callbacks)
{
    if (activeWebCallback == mState.get()) return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!ValidPath(path)) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);
        if (mState->used) return Status::FailWithoutMessage(ErrorCode::Closed);
        const auto inserted = mState->sockets.emplace(std::string(path), std::make_shared<const WebSocketCallbacks>(std::move(callbacks)));
        return inserted.second ? Status::Ok() : Status::FailWithoutMessage(ErrorCode::AlreadyExists);
    }
    catch (...) { return Status::AllocationFailure(); }
}

Status HttpServer::Route(std::string_view method, std::string_view path, Handler handler)
{
    return RegisterRoute(method, path, std::move(handler));
}

Status HttpServer::WebSocket(std::string_view path, WebSocketCallbacks callbacks)
{
    return RegisterWebSocket(path, std::move(callbacks));
}

Status HttpServer::Start(const HttpServerOptions& options)
{
    if (activeWebCallback == mState.get()) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    return mState->Start(options);
}
Status HttpServer::Stop() { return mState->Stop(); }
bool HttpServer::IsRunning() const noexcept { return mState->running.load(); }
std::uint16_t HttpServer::Port() const noexcept { return mState->acceptor.Port(); }
}
