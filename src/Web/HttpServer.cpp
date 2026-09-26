#include "ServerCore/Web/HttpServer.h"

#include "Core/Utf8Internal.h"
#include "Observability/MetricsInternal.h"
#include "ServerCore/Core/Assert.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/IoContext.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "Web/HttpObservationInternal.h"
#include "Web/HttpResponseInternal.h"
#include "Web/RequestInputInternal.h"
#include "Web/RoutePattern.h"
#include "Web/WebCallbackInternal.h"
#include "Web/WebProtocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

std::span<const std::byte> Bytes(std::string_view text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string_view Text(std::span<const std::byte> bytes) noexcept
{
    if (bytes.empty())
        return {};
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

using CallbackScope = Detail::WebCallbackScope;

// 유지보수(sweep) 스레드에서만 참이다. 그 스레드에서 난 끊김의 마무리와 onClose를 진행 스레드로 넘겨,
// onClose가 기한 집행을 붙잡지 못하게 한다(WEB-5).
thread_local bool onMaintenanceThread = false;

// WebSocket 수신을 멈춘 뒤 버퍼가 프레임 한도를 넘어도 되는 여유다. Net은 멈춘 뒤에도 이미 걸린 수신
// 하나(최대 16 KiB)를 올릴 수 있다고 적는다(ConnectionFlowControl::PauseReceive). 그 넉 배를 둔다.
constexpr std::size_t HeldReceiveSlack = 64 * 1024;

bool ValidOptions(const HttpServerOptions& options) noexcept
{
    constexpr std::size_t maximumBody = 16 * 1024 * 1024;
    return options.port != 0 && options.ioWorkerThreadCount > 0 &&
           options.ioWorkerThreadCount <= 64 && options.acceptBacklog > 0 &&
           options.maxConnections > 0 && options.maxConnections <= 65536 &&
           options.maxHeaderBytes >= 256 && options.maxHeaderBytes <= 64 * 1024 &&
           options.maxBodyBytes <= maximumBody && options.maxResponseBodyBytes > 0 &&
           options.maxResponseBodyBytes <= Net::SendQueueLimitBytes - options.maxHeaderBytes &&
           options.maxWebSocketFrameBytes > 0 &&
           options.maxWebSocketFrameBytes <= Net::SendQueueLimitBytes - 14 &&
           options.maxWebSocketMessageBytes >= options.maxWebSocketFrameBytes &&
           options.maxWebSocketMessageBytes <= maximumBody && options.requestTimeout.count() > 0 &&
           options.idleTimeout.count() > 0 && options.firstByteTimeout.count() > 0 &&
           options.responseDrainTimeout.count() > 0 && options.webSocketCloseTimeout.count() > 0 &&
           options.maxTotalSendQueueCapacityBytes > 0 &&
           options.maxTotalSendQueueCapacityBytes <= 512 * 1024 * 1024 &&
           options.handlerWorkerThreadCount > 0 && options.handlerWorkerThreadCount <= 64 &&
           options.maxPendingHandlers > 0 && options.maxPendingHandlers <= 65536 &&
           options.maxHandlerRetainedBytes > 0 && options.maxActiveHttpRequests > 0 &&
           options.maxActiveHttpRequests <= 65536 && options.maxTotalRequestBytes > 0 &&
           options.maxPipelinedBytes >= 32 * 1024 &&
           options.maxPipelinedBytes <= 16 * 1024 * 1024 && options.maxStreamChunkBytes > 0 &&
           options.maxStreamChunkBytes <= Net::SendQueueLimitBytes - 32 &&
           options.handlerTimeout.count() > 0 && options.sendStallTimeout.count() > 0 &&
           options.streamIdleTimeout.count() > 0 && options.maxRequestBodyBufferBytes > 0 &&
           options.maxRequestBodyBufferBytes <= maximumBody &&
           options.maxStreamedBodyBytes >= options.maxBodyBytes &&
           options.webSocketPingInterval.count() >= 0 && options.webSocketPongTimeout.count() > 0;
}

bool ValidPath(std::string_view path) noexcept
{
    if (path.empty() || (path.front() != '/' && path != "*"))
        return false;
    for (char ch : path)
        if (static_cast<unsigned char>(ch) < 33 || static_cast<unsigned char>(ch) >= 127 ||
            ch == '?' || ch == '#')
            return false;
    return true;
}

bool ValidWebSocketCallbacks(const WebSocketCallbacks& callbacks) noexcept
{
    if ((callbacks.onOpen && callbacks.onOpenWithRequest) || callbacks.subprotocols.size() > 64)
        return false;
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < callbacks.subprotocols.size(); ++index)
    {
        const auto& token = callbacks.subprotocols[index];
        if (!Detail::IsToken(token) || token.size() > 4096 - bytes)
            return false;
        bytes += token.size();
        for (std::size_t previous = 0; previous < index; ++previous)
            if (callbacks.subprotocols[previous] == token)
                return false;
    }
    return true;
}

// Both protocols use the same immutable-after-Start routing rules. A path group
// is selected before its method, so a generic route cannot bypass a more
// specific resource's method restrictions. Capture names belong to each method.
template <class Target> class RouteTable
{
public:
    struct Entry
    {
        Target target;
        Detail::RoutePattern pattern;
    };
    using Methods = std::map<std::string, Entry, std::less<>>;
    struct Match
    {
        const Methods* methods = nullptr;
        Detail::RoutePath path;
        bool invalid = false;
    };

    Status AddExact(std::string_view method, std::string_view path, Target target)
    {
        const auto existing = mExact.find(path);
        if (existing != mExact.end())
        {
            const auto added =
                existing->second.emplace(std::string(method), Entry{ std::move(target), {} });
            return added.second ? Status::Ok()
                                : Status::FailWithoutMessage(ErrorCode::AlreadyExists);
        }
        Methods methods;
        methods.emplace(std::string(method), Entry{ std::move(target), {} });
        mExact.emplace(std::string(path), std::move(methods));
        return Status::Ok();
    }

    Status AddPattern(std::string_view method, Detail::RoutePattern pattern, Target target)
    {
        for (auto& group : mPatterns)
        {
            if (!Detail::SameRouteShape(group.shape, pattern))
                continue;
            const auto added = group.methods.emplace(
                std::string(method), Entry{ std::move(target), std::move(pattern) });
            return added.second ? Status::Ok()
                                : Status::FailWithoutMessage(ErrorCode::AlreadyExists);
        }
        PatternGroup group;
        group.shape = pattern;
        group.methods.emplace(std::string(method), Entry{ std::move(target), std::move(pattern) });
        mPatterns.push_back(std::move(group));
        return Status::Ok();
    }

    Match Find(std::string_view path) const
    {
        Match result;
        const auto exact = mExact.find(path);
        if (exact != mExact.end())
        {
            result.methods = &exact->second;
            return result;
        }
        // The asterisk-form OPTIONS target is not a slash-delimited path.
        if (mPatterns.empty() || path == "*")
            return result;
        if (!Detail::DecodeRoutePath(path, result.path))
        {
            result.invalid = true;
            return result;
        }
        const PatternGroup* best = nullptr;
        for (const auto& group : mPatterns)
        {
            if (Detail::MatchesRoutePattern(group.shape, result.path) &&
                (!best || Detail::MoreSpecificRoutePattern(group.shape, best->shape)))
                best = &group;
        }
        if (best)
            result.methods = &best->methods;
        return result;
    }

private:
    struct PatternGroup
    {
        Detail::RoutePattern shape;
        Methods methods;
    };
    std::map<std::string, Methods, std::less<>> mExact;
    std::vector<PatternGroup> mPatterns;
};

template <class Methods> std::string AllowedMethods(const Methods& methods)
{
    std::set<std::string_view> names;
    for (const auto& [method, entry] : methods)
    {
        (void)entry;
        names.insert(method);
        if (method == "GET")
            names.insert("HEAD");
    }
    std::string result;
    for (const auto name : names)
    {
        if (!result.empty())
            result += ", ";
        result += name;
    }
    return result;
}

template <class Methods> HttpResponse MethodNotAllowed(const Methods& methods)
{
    HttpResponse response;
    response.status = 405;
    response.headers.emplace_back("Allow", AllowedMethods(methods));
    response.body = "Method Not Allowed\n";
    return response;
}

/// <summary>CORS preflight인지 본다. Origin과 Access-Control-Request-Method가 있는 OPTIONS다.</summary>
bool IsCorsPreflight(const HttpRequest& request) noexcept
{
    if (request.method != "OPTIONS")
        return false;
    bool origin = false, method = false;
    for (const auto& [name, value] : request.headers)
    {
        (void)value;
        origin = origin || name == "origin";
        method = method || name == "access-control-request-method";
    }
    return origin && method;
}

struct RequestBudget : std::enable_shared_from_this<RequestBudget>
{
    struct Reservation
    {
        Reservation(std::shared_ptr<RequestBudget> value, std::size_t size, bool count)
            : owner(std::move(value))
            , bytes(size)
            , activeCount(count)
        {
        }
        std::shared_ptr<RequestBudget> owner;
        std::size_t bytes;
        bool activeCount;
        bool charged = false;
        ~Reservation()
        {
            if (!charged)
                return;
            const std::lock_guard guard(owner->mutex);
            if (activeCount)
                --owner->active;
            else
                owner->retained -= bytes;
        }
    };
    struct Reservations
    {
        std::shared_ptr<void> active;
        std::shared_ptr<void> storage;
    };
    Reservations Acquire(std::size_t bytes)
    {
        const std::lock_guard guard(mutex);
        if (active >= maxActive || bytes > maximum - retained)
            return {};
        // Construct both before charging. An allocation failure destroys only
        // uncharged reservations and therefore never reenters this mutex.
        auto response = std::make_shared<Reservation>(shared_from_this(), 0, true);
        auto storage = std::make_shared<Reservation>(shared_from_this(), bytes, false);
        ++active;
        retained += bytes;
        response->charged = storage->charged = true;
        return { std::move(response), std::move(storage) };
    }
    std::shared_ptr<void> AcquireStorage(std::size_t bytes)
    {
        const std::lock_guard guard(mutex);
        if (bytes > maximum - retained)
            return {};
        auto storage = std::make_shared<Reservation>(shared_from_this(), bytes, false);
        retained += bytes;
        storage->charged = true;
        return storage;
    }
    std::mutex mutex;
    std::size_t active = 0, retained = 0, maxActive = 0, maximum = 0;
};

struct RequestContextStorage
{
    // Declared first so the request is destroyed before its byte charge returns.
    std::shared_ptr<void> reservation;
    HttpRequestContext context;
};
struct PolicyContextStorage
{
    std::shared_ptr<void> reservation;
    HttpPolicyContext context;
};
struct HttpRoute
{
    HttpRoute(std::shared_ptr<const HttpServer::AsyncHandler> value, bool stream = false)
        : handler(std::move(value))
        , streaming(stream)
    {
    }
    std::shared_ptr<const HttpServer::AsyncHandler> handler;
    bool streaming;
};

std::size_t RequestStorage(const HttpRequest& request) noexcept
{
    // Input fields were bounded by the parser. Account both text and containers.
    std::size_t bytes = sizeof(HttpRequestContext) + request.method.size() + request.target.size() +
                        request.body.size();
    for (const auto& [name, value] : request.headers)
        bytes += sizeof(std::pair<std::string, std::string>) + name.size() + value.size();
    for (const auto& [name, value] : request.pathParameters)
        bytes += sizeof(std::pair<std::string, std::string>) + name.size() + value.size();
    return bytes;
}

HttpServer::AsyncHandler AdaptHandler(HttpServer::Handler handler)
{
    return [handler = std::move(handler)](std::shared_ptr<const HttpRequestContext> context)
    {
        const auto response = handler(context->request);
        const auto result = context->response->Complete(response);
        if (result.IsOk())
            return;
        if (result.Code() == ErrorCode::InvalidArgument || result.Code() == ErrorCode::TooLarge)
        {
            HttpResponse error;
            error.status = 500;
            error.close = true;
            if (context->response->Complete(error).IsOk())
                return;
        }
        context->response->Abort();
    };
}
}

class HttpServer::State : public std::enable_shared_from_this<HttpServer::State>
{
public:
    class Peer;
    State()
        : requestBudget(std::make_shared<RequestBudget>())
        , observation(std::make_shared<Detail::HttpObservation>(this))
    {
    }
    Status Start(const HttpServerOptions& value);
    Status Stop();
    Status BeginDrain();
    Status DrainStatus() const noexcept;
    Core::Result<Core::CompletionSubscription> WaitForDrain(
        std::function<void(Status)> callback, std::stop_token cancellation);
    Status StopGracefully(Clock::time_point deadline);
    void Shutdown();
    void Accepted(std::shared_ptr<Net::Connection> connection) noexcept;
    void Disconnected(std::uint64_t id) noexcept;
    /// <summary>미룬 끊김 통지를 진행 스레드에 맡긴다(WEB-3).</summary>
    void DeferDisconnect(std::shared_ptr<Peer> peer) noexcept;
    /// <summary>맡겨 둔 끊김 통지 하나를 이 스레드에서 마무리한다. 없으면 거짓이다.</summary>
    bool RunDeferredDisconnect() noexcept;
    /// <summary>피어 하나를 진행 스레드의 차례에 올린다. 피어마다 한 번만 줄에 선다(WEB-5).</summary>
    void QueueProgress(std::shared_ptr<Peer> peer) noexcept;
    /// <summary>진행 스레드 본체다. 정책 결정·응답 완료·몫 반환과 미룬 끊김 통지를 처리한다.</summary>
    void Progress() noexcept;
    void Sweep() noexcept;
    Observability::HttpServerMetricsSnapshot GetMetrics() const noexcept;
    std::uint64_t RequestId() noexcept
    {
        auto id = nextRequestId.load();
        while (id != 0)
            if (nextRequestId.compare_exchange_weak(id, id + 1))
                return id;
        return 0;
    }

    std::mutex lifecycleMutex;
    bool used = false;
    bool stopped = false;
    HttpServerOptions options;
    RouteTable<HttpRoute> routes;
    RouteTable<std::shared_ptr<const WebSocketCallbacks>> sockets;
    Net::IoContext io;
    Net::Acceptor acceptor;
    Runtime::TaskExecutor handlers;
    std::shared_ptr<RequestBudget> requestBudget;
    const std::shared_ptr<Detail::HttpObservation> observation;
    std::atomic<std::uint64_t> nextRequestId{ 1 };
    std::atomic<bool> running{ false };
    std::atomic<bool> stopping{ false };
    std::atomic<bool> draining{ false };
    std::atomic<bool> stopComplete{ false };
    std::shared_ptr<const RequestPolicy> policy;
    std::shared_ptr<Core::ILogger> logger;
    mutable std::mutex peersMutex;
    std::condition_variable peersChanged;
    Core::CompletionSource drainWait;
    std::unordered_map<std::uint64_t, std::shared_ptr<Peer>> peers;
    // peersMutex가 지킨다. 피어마다 한 번만 들어오므로 maxConnections만큼 미리 잡아 두면 넣을 때 할당하지 않는다.
    std::vector<std::shared_ptr<Peer>> deferredDisconnects;
    std::uint64_t nextId = 1;
    std::mutex sweepMutex;
    std::condition_variable sweepChanged;
    std::thread sweepThread;
    // 진행 스레드의 상태다. progressMutex가 지킨다. 유지보수 스레드는 기한만 집행하고, 연결을 다시
    // 진행시키는 일과 그 안의 사용자 콜백(정책 뒤의 accept·onOpen, 미룬 onClose)은 이 스레드가 맡는다.
    std::mutex progressMutex;
    std::condition_variable progressChanged;
    std::vector<std::shared_ptr<Peer>> progressQueue, progressBatch;
    bool deferredPending = false;
    std::thread progressThread;
};

class HttpServer::State::Peer final : public Net::IConnectionObserver,
                                      public WebSocketConnection,
                                      public WebSocketFlowControl,
                                      public WebSocketMessageControl,
                                      public std::enable_shared_from_this<Peer>
{
public:
    /// <summary>mInputMutex를 잡고, 이 스레드가 이 피어를 몇 겹으로 잡았는지 센다.</summary>
    /// <remarks>
    /// 전송은 진행 중인 I/O가 없으면(Linux는 I/O 콜백이 없을 때, IOCP는 걸린 작업이 없을 때) Close를
    /// 부른 스레드에서 끊김 통지를 동기로 보낸다. 그 통지가 이 잠금 안쪽에서 오면 OnDisconnected는
    /// 상태만 Closed로 바꾸고 나머지를 미루며, 가장 바깥 겹이 풀릴 때 마무리한다(WEB-3). 바깥 겹이
    /// 응용 스레드가 들어온 공개 API이거나 유지보수 스레드면 onClose를 진행 스레드로 넘겨, 호출자가 쥔
    /// 잠금 아래나 기한 집행 중에 onClose가 불리지 않게 한다. 그 밖의 서버 스레드면 잠금을 푼 뒤 그
    /// 스레드에서 마무리한다.
    /// 유지보수 스레드는 std::try_to_lock으로 잡아, 콜백이 쥔 피어에서 멈추지 않고 다음 주기로 넘긴다.
    /// </remarks>
    class InputLock
    {
    public:
        enum class Caller
        {
            Server,
            Application
        };
        explicit InputLock(Peer& peer, Caller caller = Caller::Server)
            : mPeer(peer)
            , mCaller(caller)
            , mOwns(true)
        {
            mPeer.mInputMutex.lock();
            ++mPeer.mInputDepth;
        }
        InputLock(Peer& peer, std::try_to_lock_t)
            : mPeer(peer)
            , mCaller(Caller::Server)
            , mOwns(peer.mInputMutex.try_lock())
        {
            if (mOwns)
                ++mPeer.mInputDepth;
        }
        ~InputLock()
        {
            if (mOwns)
                mPeer.ReleaseInput(mCaller);
        }
        InputLock(const InputLock&) = delete;
        InputLock& operator=(const InputLock&) = delete;
        [[nodiscard]] bool OwnsLock() const noexcept { return mOwns; }

    private:
        Peer& mPeer;
        const Caller mCaller;
        const bool mOwns;
    };
    class MessageWriter final : public WebSocketMessageWriter
    {
    public:
        MessageWriter(std::shared_ptr<Peer> owner, std::uint64_t id)
            : mPeer(std::move(owner))
            , mId(id)
        {
        }
        ~MessageWriter() override { Abort(); }
        Status Write(std::span<const std::byte> bytes, bool final) override
        {
            // A capacity callback can release the public writer/socket owner.
            const auto peer = mPeer;
            const auto id = mId;
            return peer->WriteMessage(id, bytes, final);
        }
        void Abort() noexcept override
        {
            const auto peer = mPeer;
            peer->AbortMessage(mId);
        }
        std::size_t MaxWriteBytes() const noexcept override
        {
            return mPeer->mOptions.maxWebSocketFrameBytes;
        }
        Core::Result<Net::SendCapacitySubscription> WaitForWriteCapacity(std::size_t bytes,
            std::function<void(Status)> callback, std::stop_token cancellation) override
        {
            const auto peer = mPeer;
            const auto id = mId;
            {
                const InputLock guard(*peer, InputLock::Caller::Application);
                if (peer->mOutgoingId != id || !peer->IsOpen())
                    return Core::Result<Net::SendCapacitySubscription>::FromStatus(
                        Status::FailWithoutMessage(ErrorCode::Closed));
                if (peer->mOutgoingWriting)
                    return Core::Result<Net::SendCapacitySubscription>::FromStatus(
                        Status::FailWithoutMessage(ErrorCode::WouldBlock));
            }
            return peer->WaitForSendCapacity(bytes, std::move(callback), cancellation);
        }

    private:
        const std::shared_ptr<Peer> mPeer;
        const std::uint64_t mId;
    };
    Peer(std::weak_ptr<State> owner, std::uint64_t id, std::shared_ptr<Net::Connection> connection,
        const HttpServerOptions& options)
        : mOwner(std::move(owner))
        , mId(id)
        , mConnection(std::move(connection))
        , mSender(std::make_shared<Detail::TrackedSender>(mConnection))
        , mOptions(options)
        , mParser(options.maxHeaderBytes, options.maxStreamedBodyBytes, true)
        , mLastActivity(Now())
        , mAccepted(mLastActivity.load())
        , mLastSendProgress(Now())
    {
        // 첫 요청의 기한은 첫 바이트가 아니라 수락 시각부터 센다(WEB-8).
        mRequestStarted.store(mAccepted);
    }

    std::uint64_t Id() const noexcept override { return mId; }
    std::string_view Subprotocol() const noexcept override { return mSubprotocol; }
    Core::Result<std::shared_ptr<WebSocketMessageWriter>> BeginMessage(
        WebSocketMessageType type) override
    {
        using Result = Core::Result<std::shared_ptr<WebSocketMessageWriter>>;
        try
        {
            const InputLock guard(*this, InputLock::Caller::Application);
            if (!IsOpen())
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
            if (type != WebSocketMessageType::Text && type != WebSocketMessageType::Binary)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
            if (mOutgoingId != 0 || mOutgoingWriting)
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::AlreadyExists));
            if (++mNextOutgoingId == 0)
                ++mNextOutgoingId;
            auto writer = std::make_shared<MessageWriter>(shared_from_this(), mNextOutgoingId);
            mOutgoingId = mNextOutgoingId;
            mOutgoingBytes = 0;
            mOutgoingStarted = false;
            mOutgoingOpcode = type == WebSocketMessageType::Text ? 1 : 2;
            mOutgoingUtf8 = {};
            return Result::FromValue(std::move(writer));
        }
        catch (...)
        {
            return Result::FromStatus(Status::AllocationFailure());
        }
    }
    bool IsOpen() const noexcept override
    {
        return mPhase.load() == Phase::WebSocket && mConnection->IsOpen();
    }
    std::size_t RetainedSendBytes() const noexcept override
    {
        return mSender->flow->RetainedSendBytes();
    }
    Core::Result<Net::SendCapacitySubscription> WaitForSendCapacity(std::size_t payloadBytes,
        std::function<void(Status)> callback, std::stop_token cancellation) override
    {
        using Result = Core::Result<Net::SendCapacitySubscription>;
        if (!callback)
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        if (!IsOpen())
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
        if (payloadBytes > mOptions.maxWebSocketFrameBytes)
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        const auto owner = mOwner.lock();
        const auto ownerId = owner.get();
        try
        {
            return mSender->flow->WaitForSendCapacity(
                payloadBytes + 14,
                [ownerId, callback = std::move(callback)](Status status) mutable
                {
                    const CallbackScope scope(ownerId);
                    callback(std::move(status));
                },
                cancellation);
        }
        catch (...)
        {
            return Result::FromStatus(Status::AllocationFailure());
        }
    }

    // 수신 멈춤은 피어 잠금을 잡지 않는다. 앱이 자기 잠금을 쥔 채 불러도 onMessage를 기다리지 않는다.
    Status PauseReceive() override
    {
        if (!IsOpen())
            return Status::FailWithoutMessage(ErrorCode::Closed);
        mReceiveHeld.store(true);
        auto status = mSender->flow->PauseReceive();
        // 그 사이 Close가 멈춤을 풀었으면 closing handshake의 답을 읽도록 되돌린다.
        if (!IsOpen() && mReceiveHeld.exchange(false))
        {
            (void)mSender->flow->ResumeReceive();
            RequestProgress();
            return Status::FailWithoutMessage(ErrorCode::Closed);
        }
        return status;
    }

    Status ResumeReceive() override
    {
        if (!IsOpen())
            return Status::FailWithoutMessage(ErrorCode::Closed);
        if (mReceiveHeld.exchange(false))
            mHeartbeatResumed.store(true);
        auto status = mSender->flow->ResumeReceive();
        // 멈춘 동안 버퍼에 남은 프레임은 새 바이트가 없어도 진행 스레드가 이어서 처리한다.
        RequestProgress();
        return status;
    }

    bool IsReceivePaused() const noexcept override { return mReceiveHeld.load(); }

    Status SendText(std::string_view text) override { return SendFrame(1, Bytes(text)); }

    Status SendBinary(std::span<const std::byte> bytes) override { return SendFrame(2, bytes); }

    Status Ping(std::span<const std::byte> bytes) override { return SendFrame(9, bytes); }

    Status Close(std::uint16_t code, std::string_view reason) override
    {
        const InputLock guard(*this, InputLock::Caller::Application);
        const auto phase = mPhase.load();
        if (phase != Phase::WebSocket && phase != Phase::WsClosing)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        if (reason.size() > 123)
            return Status::FailWithoutMessage(ErrorCode::TooLarge);
        if (!Detail::ValidCloseCode(code) || code == 1010)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        if (!Core::Detail::IsValidUtf8(reason))
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
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
        catch (...)
        {
            mConnection->Close();
            return Status::AllocationFailure();
        }
    }

    void ForceClose() noexcept { mConnection->Close(); }

    void BeginDrain()
    {
        const InputLock guard(*this, std::try_to_lock);
        if (!guard.OwnsLock())
            return;
        const auto phase = mPhase.load();
        if (phase == Phase::WebSocket)
        {
            (void)Close(1001, "server draining");
            return;
        }
        if (phase == Phase::Http && !mActiveResponse && !mPolicyDecision)
            CloseIdleForDrain();
    }

    /// <summary>drain이 요청 사이에 쉬던 HTTP 연결을 닫는다.</summary>
    /// <remarks>
    /// CloseAfterSend는 상대가 닫을 때까지 소켓을 쥔다(Net의 lingering close). HttpClosing으로 두어
    /// 닫지 않는 상대를 idleTimeout이 아니라 responseDrainTimeout이 끝내게 한다
    /// (Web.DrainBoundsIdleKeepAliveConnections).
    /// </remarks>
    void CloseIdleForDrain()
    {
        mCloseStarted.store(Now());
        mPhase.store(Phase::HttpClosing);
        mConnection->CloseAfterSend();
    }

    void CheckTimeout(std::int64_t now) noexcept
    {
        const Detail::WebCompletionScope completions;
        bool expired = false;
        std::shared_ptr<Detail::HttpResponseState> timedOutResponse;
        try
        {
            // 콜백이 이 피어를 쥐고 있으면 기다리지 않고 다음 주기에 본다. 입력 처리는 진행 스레드가
            // 사건(정책 결정·응답 완료·몫 반환)을 받아 하므로 여기서 ProcessInput을 부르지 않는다.
            const InputLock guard(*this, std::try_to_lock);
            if (!guard.OwnsLock())
                return;
            auto phase = mPhase.load();
            if (phase == Phase::Closed)
                return;
            if (phase == Phase::WebSocket)
                CheckHeartbeat(now);
            phase = mPhase.load();
            const auto queued = mConnection->QueuedSendBytes();
            const auto admitted = mSender->admitted.load();
            const auto drained =
                admitted - (std::min)(admitted, static_cast<std::uint64_t>(queued));
            if (queued == 0 || !mHadQueued || drained > mLastDrained)
                mLastSendProgress = now;
            mHadQueued = queued != 0;
            mLastDrained = (std::max)(mLastDrained, drained);
            const auto requestStarted = mRequestStarted.load();
            const auto closeStarted = mCloseStarted.load();
            const auto activity = (std::max)(mLastActivity.load(), mSender->lastWrite.load());
            expired =
                (queued != 0 && now - mLastSendProgress >= mOptions.sendStallTimeout.count()) ||
                (mPolicyDecision && now - mPolicyStarted >= mOptions.handlerTimeout.count()) ||
                (mActiveResponse && mActiveResponse->TimedOut(now, !mReceivingBody)) ||
                // 끝난 응답이 아직 거둬지지 않았어도 유휴 기한은 적용한다. 놓친 사건이 연결을 붙잡지 못한다.
                ((!mActiveResponse || mActiveResponse->IsFinished()) &&
                    now - activity >= mOptions.idleTimeout.count()) ||
                (phase == Phase::Http && (!mActiveResponse || mReceivingBody) &&
                    requestStarted != 0 &&
                    now - requestStarted >= mOptions.requestTimeout.count()) ||
                (phase == Phase::Http && !mReceivedInput.load() &&
                    now - mAccepted >= mOptions.firstByteTimeout.count()) ||
                (phase == Phase::HttpClosing && closeStarted != 0 &&
                    now - closeStarted >= mOptions.responseDrainTimeout.count()) ||
                (phase == Phase::WsClosing && closeStarted != 0 &&
                    now - closeStarted >= mOptions.webSocketCloseTimeout.count());
            if (expired)
                timedOutResponse = mActiveResponse;
        }
        catch (...)
        {
            expired = true;
        }
        if (expired)
        {
            if (timedOutResponse)
                timedOutResponse->Cancel(ErrorCode::Timeout);
            mConnection->Close();
        }
    }

    void OnBytesReceived(std::span<const std::byte> bytes) override
    {
        const Detail::WebCompletionScope completions;
        try
        {
            const InputLock guard(*this);
            mLastActivity.store(Now());
            mReceivedInput.store(true);
            while (!bytes.empty())
            {
                const auto phase = mPhase.load();
                if (phase == Phase::Closed || phase == Phase::HttpClosing || mIgnoreInput)
                    return;
                if (phase == Phase::Http && !mActiveResponse && mRequestStarted.load() == 0)
                    mRequestStarted.store(Now());
                // 앱이 수신을 멈춘 뒤에도 이미 걸려 있던 수신 하나(최대 16 KiB)는 올 수 있다. 버퍼에 남은 프레임과
                // 함께 담을 여유를 두어, 멈춤이 1009로 끝나지 않게 한다.
                const auto limit =
                    phase == Phase::Http
                        ? ((mActiveResponse || mPolicyDecision) ? mOptions.maxPipelinedBytes
                                                                : mOptions.maxHeaderBytes)
                        : (std::max)(mOptions.maxWebSocketFrameBytes, std::size_t{ 125 }) + 14 +
                              (mReceiveHeld.load() ? HeldReceiveSlack : 0);
                if (mInput.size() >= limit)
                {
                    if (mActiveResponse)
                        mConnection->Close();
                    else if (phase == Phase::Http)
                        HttpError(431, mParser.IsHeadRequest());
                    else
                        WebSocketError(1009);
                    return;
                }
                const auto count = (std::min)(bytes.size(), limit - mInput.size());
                mInput.append(Text(bytes.first(count)));
                bytes = bytes.subspan(count);
                ProcessInput();
                if ((mActiveResponse || mPolicyDecision) &&
                    mInput.size() >= mOptions.maxPipelinedBytes - 16 * 1024)
                    (void)mSender->flow->PauseReceive();
            }
        }
        catch (...)
        {
            mConnection->Close();
        }
    }

    void OnDisconnected(Status reason) override
    {
        {
            const InputLock guard(*this);
            mPhase.store(Phase::Closed);
            mDisconnectReason = std::move(reason);
            if (mInputDepth > 1)
            {
                // 이 스레드가 이미 이 피어 안에 있다(예: onMessage가 부른 Close). 여기서 버퍼를 비우거나
                // onClose를 부르면 바깥 호출이 빌려 준 메시지가 사라지고 호출자의 잠금 아래에서 사용자
                // 코드가 돈다. 가장 바깥 InputLock이 풀릴 때 마무리한다.
                mDisconnectPending = true;
                return;
            }
        }
        HandOffDisconnect(onMaintenanceThread);
    }

    /// <summary>미룬 끊김 통지를 마무리한다. 진행 스레드나 바깥 겹의 서버 스레드가 부른다.</summary>
    void CompleteDeferredDisconnect() noexcept
    {
        Status reason = Status::Ok();
        {
            const InputLock guard(*this);
            reason = std::move(mDisconnectReason);
        }
        FinishDisconnect(std::move(reason));
    }

    /// <summary>진행 스레드에 이 피어의 차례를 청한다. 이미 줄에 서 있으면 아무것도 하지 않는다(WEB-5).</summary>
    void RequestProgress() noexcept
    {
        if (mProgressQueued.exchange(true))
            return;
        if (const auto owner = mOwner.lock())
            owner->QueueProgress(shared_from_this());
        else
            mProgressQueued.store(false);
    }

    /// <summary>줄을 세우지 못했을 때 표시를 되돌려, 다음 사건이 다시 청할 수 있게 한다.</summary>
    void ClearProgressRequest() noexcept { mProgressQueued.store(false); }

    /// <summary>진행 스레드가 부른다. 사건 뒤에 남은 입력·정책·응답 처리를 이어 간다.</summary>
    void RunProgress() noexcept
    {
        mProgressQueued.store(false);
        const Detail::WebCompletionScope completions;
        try
        {
            const InputLock guard(*this);
            // 재개 뒤 pong 기한은 지금부터 센다. 멈춘 동안에는 pong을 읽을 수 없었다.
            if (mHeartbeatResumed.exchange(false) && mHeartbeatPending)
                mHeartbeatSent = Now();
            const auto phase = mPhase.load();
            if (phase == Phase::Http || phase == Phase::WebSocket || phase == Phase::WsClosing)
                ProcessInput();
        }
        catch (...)
        {
            mConnection->Close();
        }
    }

private:
    void ReleaseInput(InputLock::Caller caller) noexcept
    {
        const bool finish = --mInputDepth == 0 && std::exchange(mDisconnectPending, false);
        mInputMutex.unlock();
        if (finish)
            HandOffDisconnect(caller == InputLock::Caller::Application || onMaintenanceThread);
    }

    /// <summary>끊김의 마무리를 이 스레드에서 하거나, handOff면 진행 스레드에 넘긴다.</summary>
    void HandOffDisconnect(bool handOff) noexcept
    {
        if (handOff)
        {
            if (const auto owner = mOwner.lock())
            {
                owner->DeferDisconnect(shared_from_this());
                return;
            }
        }
        CompleteDeferredDisconnect();
    }

    /// <summary>업로드 조각이 놓였을 때 불린다. 몫을 기다리며 멈춘 경우에만 진행을 청한다.</summary>
    void CreditReleased() noexcept
    {
        if (mAwaitingCredit.exchange(false))
            RequestProgress();
    }

    void FinishDisconnect(Status reason) noexcept
    {
        const auto owner = mOwner.lock();
        std::shared_ptr<const WebSocketCallbacks> callbacks;
        std::shared_ptr<Detail::HttpResponseState> active;
        std::shared_ptr<Detail::RequestBodyState> body;
        std::shared_ptr<Detail::RequestDecisionState> decision;
        {
            const InputLock guard(*this);
            mPhase.store(Phase::Closed);
            mOutgoingId = 0;
            mHeartbeatPending = false;
            active = std::move(mActiveResponse);
            body = std::move(mBodyReader);
            decision = std::move(mPolicyDecision);
            mPolicyContext.reset();
            mPolicyActive.reset();
            mRequestContext.reset();
            callbacks = std::move(mCallbacks);
            std::string{}.swap(mInput);
            mInputOffset = 0;
            std::vector<std::byte>{}.swap(mMessage);
            mParser =
                Detail::HttpParser(mOptions.maxHeaderBytes, mOptions.maxStreamedBodyBytes, true);
        }
        if (body)
            body->End(ErrorCode::Cancelled);
        if (decision)
            decision->Abort();
        if (active)
            active->Cancel(reason.Code() == ErrorCode::Closed || reason.IsOk()
                               ? ErrorCode::Cancelled
                               : reason.Code());
        if (mUpgraded && callbacks && callbacks->onClose)
        {
            const CallbackScope callback(owner.get());
            try
            {
                callbacks->onClose(mId, mReceivedCloseCode, mReceivedCloseReason);
            }
            catch (...)
            { /* User exceptions never escape the transport observer. */
            }
        }
        if (owner)
            owner->Disconnected(mId);
    }

    enum class Phase
    {
        Http,
        WebSocket,
        WsClosing,
        HttpClosing,
        Closed
    };

    Status WriteMessage(std::uint64_t id, std::span<const std::byte> bytes, bool final)
    {
        try
        {
            const InputLock guard(*this, InputLock::Caller::Application);
            if (!IsOpen() || id != mOutgoingId)
                return Status::FailWithoutMessage(ErrorCode::Closed);
            if (mOutgoingWriting)
                return Status::FailWithoutMessage(ErrorCode::WouldBlock);
            if (bytes.size() > mOptions.maxWebSocketFrameBytes ||
                bytes.size() > mOptions.maxWebSocketMessageBytes - mOutgoingBytes)
                return Status::FailWithoutMessage(ErrorCode::TooLarge);
            auto utf8 = mOutgoingUtf8;
            if (mOutgoingOpcode == 1 && !utf8.Append(bytes, final))
                return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
            // Shared-budget notifications may invoke user code inside Send.
            // A recursive input mutex alone does not serialize that reentry.
            mOutgoingWriting = true;
            struct EndWrite
            {
                bool& writing;
                ~EndWrite() { writing = false; }
            } endWrite{ mOutgoingWriting };
            const auto wire =
                Detail::EncodeServerFrame(mOutgoingStarted ? 0 : mOutgoingOpcode, bytes, final);
            const auto result = mSender->Send(wire);
            if (!result.IsOk())
                return result;
            if (mOutgoingId == id)
            {
                mOutgoingBytes += bytes.size();
                mOutgoingUtf8 = utf8;
                mOutgoingStarted = true;
                if (final)
                    mOutgoingId = 0;
            }
            return result;
        }
        catch (...)
        {
            return Status::AllocationFailure();
        }
    }
    void AbortMessage(std::uint64_t id) noexcept
    {
        const InputLock guard(*this, InputLock::Caller::Application);
        if (mOutgoingId != id)
            return;
        mOutgoingId = 0;
        if (mOutgoingStarted || mOutgoingWriting)
            mConnection->Close();
    }
    void CheckHeartbeat(std::int64_t now)
    {
        if (mOptions.webSocketPingInterval.count() == 0)
            return;
        // 수신을 멈춘 동안(과 재개 직후 진행 스레드가 기한을 되돌리기 전)에는 pong을 읽을 수 없으므로 세지 않는다.
        if (mReceiveHeld.load() || mHeartbeatResumed.load())
            return;
        if (mHeartbeatPending)
        {
            if (now - mHeartbeatSent >= mOptions.webSocketPongTimeout.count())
                (void)Close(1001, "pong timeout");
            return;
        }
        if (now - mHeartbeatSent < mOptions.webSocketPingInterval.count())
            return;
        std::array<std::byte, 16> payload{};
        const auto sequence = mHeartbeatSequence + 1;
        for (std::size_t index = 0; index < 8; ++index)
        {
            const auto shift = static_cast<unsigned int>((7 - index) * 8);
            payload[index] = static_cast<std::byte>((mId >> shift) & 255);
            payload[index + 8] = static_cast<std::byte>((sequence >> shift) & 255);
        }
        const auto result = SendFrame(9, payload);
        if (result.IsOk() && mPhase.load() == Phase::WebSocket)
        {
            mHeartbeatPayload = payload;
            mHeartbeatSequence = sequence;
            mHeartbeatSent = now;
            mHeartbeatPending = true;
        }
        else if (!result.IsOk() && result.Code() != ErrorCode::WouldBlock)
            mConnection->Close();
    }

    Status SendFrame(
        std::uint8_t opcode, std::span<const std::byte> payload, bool closingControl = false)
    {
        try
        {
            const InputLock guard(*this, InputLock::Caller::Application);
            const auto phase = mPhase.load();
            if (phase != Phase::WebSocket && !(closingControl && phase == Phase::WsClosing))
                return Status::FailWithoutMessage(ErrorCode::Closed);
            if (payload.size() >
                (opcode < 8 ? mOptions.maxWebSocketFrameBytes : std::size_t{ 125 }))
                return Status::FailWithoutMessage(ErrorCode::TooLarge);
            if (opcode == 1 && !Core::Detail::IsValidUtf8(Text(payload)))
                return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
            if (opcode < 8 && (mOutgoingId != 0 || mOutgoingWriting))
                return Status::FailWithoutMessage(ErrorCode::AlreadyExists);
            const auto wire = Detail::EncodeServerFrame(opcode, payload);
            return mSender->Send(wire);
        }
        catch (...)
        {
            return Status::AllocationFailure();
        }
    }

    Status SendClose(std::span<const std::byte> payload)
    {
        const InputLock guard(*this, InputLock::Caller::Application);
        const auto phase = mPhase.load();
        if (phase == Phase::WsClosing)
            return Status::Ok();
        if (phase != Phase::WebSocket)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        mCloseStarted.store(Now());
        mPhase.store(Phase::WsClosing);
        mOutgoingId = 0;
        mHeartbeatPending = false;
        // 앱이 멈춘 수신을 풀어 closing handshake의 답을 읽는다. 버퍼에 남은 프레임은 진행 스레드가 처리한다.
        if (mReceiveHeld.exchange(false))
        {
            (void)mSender->flow->ResumeReceive();
            RequestProgress();
        }
        try
        {
            const auto wire = Detail::EncodeServerFrame(8, payload);
            auto result = mSender->Send(wire);
            if (!result.IsOk())
                mConnection->Close();
            return result;
        }
        catch (...)
        {
            mConnection->Close();
            return Status::AllocationFailure();
        }
    }

    void WebSocketError(std::uint16_t code)
    {
        mIgnoreInput = true;
        (void)Close(code, {});
        mConnection->CloseAfterSend();
    }

    void HttpError(unsigned int status, bool head = false)
    {
        if (const auto owner = mOwner.lock(); owner && owner->logger)
        {
            const CallbackScope scope(owner.get());
            char code[16];
            const auto converted = std::to_chars(code, code + sizeof(code), status);
            const Core::LogField field{ "http_status",
                { code, static_cast<std::size_t>(converted.ptr - code) } };
            (void)Core::WriteLog(
                *owner->logger, { Core::LogLevel::Warn, Detail::ReasonPhrase(status), { &field, 1 },
                                    { 0, 0, 0, mId } });
        }
        HttpResponse response;
        response.status = status;
        response.body = std::string(Detail::ReasonPhrase(status)) + "\n";
        if (response.body.size() > mOptions.maxResponseBodyBytes)
            response.body.clear();
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
            wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: "
                   "close\r\n\r\n";
            close = true;
        }
        if (close)
        {
            mCloseStarted.store(Now());
            mPhase.store(Phase::HttpClosing);
        }
        const auto sent = mSender->Send(Bytes(wire));
        if (!sent.IsOk())
            mConnection->Close();
        else if (close)
            mConnection->CloseAfterSend();
    }

    /// <summary>mInput에서 아직 처리하지 않은 뒷부분이다. 오프셋이 크기를 넘어도 던지지 않는다.</summary>
    std::string_view PendingInput() const noexcept
    {
        return std::string_view(mInput).substr((std::min)(mInputOffset, mInput.size()));
    }

    void ProcessInput()
    {
        // 파서와 프레임 해석은 mInputOffset만 옮기고, 처리한 앞부분은 이 호출을 떠날 때 한 번 지운다.
        // 청크·프레임마다 앞을 지우면 남은 버퍼 전체를 매번 옮겨 입력 크기의 제곱이 된다(WEB-1).
        struct Compaction
        {
            std::string& input;
            std::size_t& offset;
            ~Compaction()
            {
                input.erase(0, (std::min)(offset, input.size()));
                offset = 0;
            }
        } compaction{ mInput, mInputOffset };
        while (true)
        {
            const auto phase = mPhase.load();
            if (phase == Phase::Closed || phase == Phase::HttpClosing || mIgnoreInput)
                return;
            if (phase != Phase::Http)
            {
                // 앱이 수신을 멈췄으면 버퍼에 남은 프레임도 재개까지 처리하지 않는다.
                if (mReceiveHeld.load() || !ProcessWebSocketFrame())
                    return;
                continue;
            }
            if (mPolicyDecision)
            {
                if (!ResolvePolicy())
                    return;
                continue;
            }
            if (mActiveResponse && mReceivingBody && mActiveResponse->IsAborted())
            {
                mConnection->Close();
                return;
            }
            if (mActiveResponse && !mReceivingBody)
            {
                if (mActiveResponse->IsAborted())
                {
                    mConnection->Close();
                    return;
                }
                if (!mActiveResponse->IsFinished())
                    return;
                const auto owner = mOwner.lock();
                const bool close =
                    mActiveResponse->ShouldClose() || (owner && owner->draining.load());
                mActiveResponse.reset();
                mRequestContext.reset();
                if (close)
                {
                    mCloseStarted.store(Now());
                    mPhase.store(Phase::HttpClosing);
                    mConnection->CloseAfterSend();
                    return;
                }
                mBodyReader.reset();
                if (mSender->flow->IsReceivePaused())
                    (void)mSender->flow->ResumeReceive();
                mLastActivity.store(Now());
            }
            if (!mReceivingBody && !mParser.Started())
            {
                const auto owner = mOwner.lock();
                if (owner && owner->draining.load())
                {
                    CloseIdleForDrain();
                    return;
                }
            }
            if (mReceivingBody && mBodyReader)
            {
                // 몫을 읽기 전에 기다림을 표시한다. 그 사이에 조각이 놓이면 CreditReleased가 진행을 청한다.
                mAwaitingCredit.store(true);
                const auto available = mBodyReader->Available();
                if (available != 0)
                    mAwaitingCredit.store(false);
                if (available == 0)
                    (void)mSender->flow->PauseReceive();
                (void)mParser.ConfigureBody(true, mOptions.maxStreamedBodyBytes,
                    (std::min)(available, mOptions.maxStreamChunkBytes));
                if (available != 0 && mSender->flow->IsReceivePaused())
                    (void)mSender->flow->ResumeReceive();
            }
            if (!PendingInput().empty() && mRequestStarted.load() == 0)
                mRequestStarted.store(Now());
            HttpRequest request;
            std::size_t consumed = 0;
            const auto result = mParser.Parse(PendingInput(), consumed, request);
            mInputOffset += consumed;
            if (result.kind == Detail::HttpParseKind::Headers ||
                result.kind == Detail::HttpParseKind::Complete)
            {
                request.localEndpoint = mConnection->LocalEndpoint();
                request.remoteEndpoint = mConnection->RemoteEndpoint();
            }
            if (result.kind == Detail::HttpParseKind::NeedMore)
            {
                if (mReceivingBody && mActiveResponse && mActiveResponse->IsFinished())
                    mConnection->Close();
                return;
            }
            if (result.kind == Detail::HttpParseKind::Error)
            {
                if (const auto owner = mOwner.lock())
                    Observability::Detail::Add(owner->observation->protocolErrors);
                if (mBodyReader)
                {
                    mBodyReader->End(
                        result.status == 413 ? ErrorCode::TooLarge : ErrorCode::InvalidFormat);
                    if (mActiveResponse)
                        mActiveResponse->Cancel(ErrorCode::InvalidFormat);
                    mConnection->Close();
                    return;
                }
                HttpError(result.status, mParser.IsHeadRequest());
                return;
            }
            if (result.kind == Detail::HttpParseKind::Continue)
            {
                if (mActiveResponse && mActiveResponse->IsFinished())
                {
                    mConnection->Close();
                    return;
                }
                const auto sent = mSender->Send(Bytes("HTTP/1.1 100 Continue\r\n\r\n"));
                if (!sent.IsOk())
                {
                    mConnection->Close();
                    return;
                }
                continue;
            }
            if (result.kind == Detail::HttpParseKind::Headers)
            {
                mIncomingRequest = std::move(request);
                mRequestHttp10 = mParser.IsHttp10();
                mPolicyHeaders.clear();
                mPolicyAttributes.clear();
                mPolicyStarted = 0;
                const auto owner = mOwner.lock();
                bool streaming = false;
                if (owner && !WantsWebSocket(mIncomingRequest))
                {
                    const auto match = owner->routes.Find(mIncomingRequest.Path());
                    if (match.methods)
                    {
                        auto method = match.methods->find(mIncomingRequest.method);
                        if (method == match.methods->end() && mIncomingRequest.method == "HEAD")
                            method = match.methods->find("GET");
                        streaming =
                            method != match.methods->end() && method->second.target.streaming;
                    }
                }
                if (!mParser.ConfigureBody(streaming,
                        streaming ? mOptions.maxStreamedBodyBytes : mOptions.maxBodyBytes,
                        mOptions.maxStreamChunkBytes))
                {
                    HttpError(413, mIncomingRequest.method == "HEAD");
                    return;
                }
                if (streaming && owner)
                {
                    auto reservation =
                        owner->requestBudget->AcquireStorage(mOptions.maxRequestBodyBufferBytes);
                    if (!reservation)
                    {
                        HttpError(503, mIncomingRequest.method == "HEAD");
                        return;
                    }
                    mBodyReader = std::make_shared<Detail::RequestBodyState>(
                        mOptions.maxRequestBodyBufferBytes, std::move(reservation),
                        [weak = weak_from_this()]
                        {
                            if (const auto peer = weak.lock())
                                peer->ForceClose();
                        },
                        owner.get(),
                        [weak = weak_from_this()]
                        {
                            if (const auto peer = weak.lock())
                                peer->CreditReleased();
                        });
                    mReceivingBody = true;
                    HandleRequest(mIncomingRequest);
                }
                continue;
            }
            if (result.kind == Detail::HttpParseKind::Body)
            {
                // 스트리밍 본문은 진행할 때마다 요청 기한을 다시 센다. 멈춘 업로드만 기한에 걸린다(WEB-10).
                mBodyReader->Push(Bytes(request.body));
                mRequestStarted.store(Now());
                continue;
            }
            mRequestStarted.store(0);
            mParser =
                Detail::HttpParser(mOptions.maxHeaderBytes, mOptions.maxStreamedBodyBytes, true);
            if (mReceivingBody)
            {
                mReceivingBody = false;
                mBodyReader->End();
                // 핸들러 기한은 본문을 다 받은 뒤부터 센다. 받는 동안에는 CheckTimeout이 보지 않는다.
                if (mActiveResponse)
                    mActiveResponse->RestartHandlerDeadline();
            }
            else
            {
                mIncomingRequest.body = std::move(request.body);
                HandleRequest(mIncomingRequest);
            }
            mIncomingRequest = {};
        }
    }

    void BeginPolicy(const HttpRequest& request, const std::shared_ptr<State>& owner,
        std::shared_ptr<const AsyncHandler> handler,
        std::shared_ptr<const WebSocketCallbacks> socket, bool global)
    {
        const auto selectedPolicy =
            global ? owner->policy
                   : std::shared_ptr<const RequestPolicy>(socket, &socket->authorize);
        // Reserve the maximum deferred decision as well as its owning request:
        // the application may resolve it while every handler worker is busy.
        const auto policyBytes = RequestStorage(request) + mOptions.maxHeaderBytes +
                                 mOptions.maxResponseBodyBytes +
                                 128 * sizeof(HttpHeaders::value_type);
        auto reservation = owner->requestBudget->Acquire(policyBytes);
        if (!reservation.active)
        {
            HttpError(503, request.method == "HEAD");
            return;
        }
        if (mPolicyStarted == 0)
            mPolicyStarted = Now();
        auto decision = std::make_shared<Detail::RequestDecisionState>(mOptions.maxHeaderBytes,
            mOptions.maxResponseBodyBytes, mPolicyStarted, mOptions.handlerTimeout, owner.get(),
            [weak = weak_from_this()]
            {
                if (const auto peer = weak.lock())
                    peer->RequestProgress();
            });
        auto storage = std::make_shared<PolicyContextStorage>(
            PolicyContextStorage{ std::move(reservation.storage),
                HttpPolicyContext{ request, socket != nullptr, decision } });
        auto context = std::shared_ptr<const HttpPolicyContext>(storage, &storage->context);
        mPolicyActive = std::move(reservation.active);
        mPolicyDecision = decision;
        mPolicyContext = context;
        mPolicyHandler = std::move(handler);
        mPolicySocket = std::move(socket);
        mPolicyWasGlobal = global;
        Runtime::TaskOptions taskOptions;
        taskOptions.retainedBytes = policyBytes;
        taskOptions.parentToken = decision->GetCancellationToken();
        auto submitted = owner->handlers.Submit(
            [selectedPolicy, context, ownerId = owner.get()](std::stop_token)
            {
                const CallbackScope scope(ownerId);
                try
                {
                    (*selectedPolicy)(context);
                }
                catch (...)
                {
                    HttpResponse failed;
                    failed.status = 500;
                    try
                    {
                        (void)context->decision->Reject(failed);
                    }
                    catch (...)
                    {
                        context->decision->Abort();
                    }
                }
                return Status::Ok();
            },
            taskOptions);
        if (!submitted.IsOk())
        {
            HttpResponse failed;
            failed.status = 503;
            (void)decision->Reject(failed);
        }
    }

    bool ResolvePolicy()
    {
        auto result = mPolicyDecision->Take();
        if (result.phase == Detail::RequestDecisionState::Phase::Pending)
            return false;
        auto context = std::move(mPolicyContext);
        auto socket = std::move(mPolicySocket);
        auto handler = std::move(mPolicyHandler);
        mPolicyActive.reset();
        mPolicyDecision.reset();
        if (result.phase == Detail::RequestDecisionState::Phase::Aborted)
        {
            mConnection->Close();
            return false;
        }
        if (result.phase == Detail::RequestDecisionState::Phase::Rejected)
        {
            // 정책이 돌려준 응답(예: CORS preflight 204)은 요청을 다 읽었으면 연결을 유지한다. 본문을 아직
            // 받는 중이거나 응답·요청이 닫기를 원하면 닫는다(WEB-10).
            const auto owner = mOwner.lock();
            const bool close = result.response.close || mReceivingBody || mRequestHttp10 ||
                               Detail::HasToken(context->request, "connection", "close") ||
                               !owner || owner->draining.load();
            Respond(result.response, context->request.method == "HEAD", close);
            if (close)
                return false;
            if (mSender->flow->IsReceivePaused())
                (void)mSender->flow->ResumeReceive();
            mLastActivity.store(Now());
            return true;
        }
        mPolicyHeaders.insert(mPolicyHeaders.end(),
            std::make_move_iterator(result.response.headers.begin()),
            std::make_move_iterator(result.response.headers.end()));
        mPolicyAttributes.insert(mPolicyAttributes.end(),
            std::make_move_iterator(result.attributes.begin()),
            std::make_move_iterator(result.attributes.end()));
        const auto owner = mOwner.lock();
        if (!owner || owner->stopping.load())
        {
            mConnection->Close();
            return false;
        }
        if (socket)
        {
            if (mPolicyWasGlobal && socket->authorize)
                BeginPolicy(context->request, owner, {}, socket, false);
            else
                Upgrade(context->request, socket, owner.get(), true);
        }
        else if (handler)
            DispatchRequest(context->request, handler, owner, true);
        else if (!RespondMethodNotAllowed(context->request, *owner))
            return false;
        return mPolicyDecision == nullptr;
    }

    /// <summary>
    /// 정책이 허용만 한 CORS preflight에 경로의 405를 보낸다. 정책이 준 헤더는 기본값으로 더한다.
    /// 연결을 계속 쓰면 참이다.
    /// </summary>
    bool RespondMethodNotAllowed(const HttpRequest& request, const State& owner)
    {
        const auto match = owner.routes.Find(request.Path());
        HttpResponse response;
        if (match.methods)
            response = MethodNotAllowed(*match.methods);
        else
        {
            response.status = 404;
            response.body = "Not Found\n";
        }
        Detail::MergePolicyHeaders(response.headers, mPolicyHeaders);
        const bool close = mRequestHttp10 || owner.draining.load() ||
                           Detail::HasToken(request, "connection", "close");
        Respond(response, false, close);
        if (close)
            return false;
        if (mSender->flow->IsReceivePaused())
            (void)mSender->flow->ResumeReceive();
        mLastActivity.store(Now());
        return true;
    }

    void HandleRequest(HttpRequest& request)
    {
        const auto owner = mOwner.lock();
        if (!owner || owner->stopping.load())
        {
            mConnection->Close();
            return;
        }
        const bool head = request.method == "HEAD";
        const auto upgrade = [&]()
        {
            const auto match = owner->sockets.Find(request.Path());
            if (match.invalid)
            {
                HttpError(400, head);
                return true;
            }
            if (!match.methods)
                return false;
            const auto& entry = match.methods->begin()->second;
            Detail::CaptureRouteParameters(entry.pattern, match.path, request.pathParameters);
            Upgrade(request, entry.target, owner.get());
            return true;
        };
        if (WantsWebSocket(request))
        {
            if (!upgrade())
                HttpError(404, head);
            return;
        }
        const auto match = owner->routes.Find(request.Path());
        if (match.invalid)
        {
            HttpError(400, head);
            return;
        }
        HttpResponse response;
        if (!match.methods)
        {
            // Preserve the existing missing-handshake error on WS-only paths.
            if (upgrade())
                return;
            response.status = 404;
            response.body = "Not Found\n";
        }
        else
        {
            auto route = match.methods->find(request.method);
            if (route == match.methods->end() && head)
                route = match.methods->find("GET");
            if (route == match.methods->end())
            {
                // 정책이 있으면 CORS preflight는 메서드 판정보다 먼저 정책에 보인다. 정책이 응답하지 않고
                // 허용만 하면 ResolvePolicy가 이 405를 보낸다(Web.CorsPreflightReachesPolicyBeforeMethodCheck).
                if (owner->policy && IsCorsPreflight(request))
                {
                    BeginPolicy(request, owner, {}, {}, true);
                    return;
                }
                response = MethodNotAllowed(*match.methods);
            }
            else
            {
                Detail::CaptureRouteParameters(
                    route->second.pattern, match.path, request.pathParameters);
                DispatchRequest(std::move(request), route->second.target.handler, owner);
                return;
            }
        }
        Respond(response, head,
            response.close || mRequestHttp10 || Detail::HasToken(request, "connection", "close"));
    }

    /// <summary>WebSocket 전환을 요청했는지 본다. HTTP/1.0의 Upgrade는 무시한다(RFC 9110 §7.8).</summary>
    bool WantsWebSocket(const HttpRequest& request) const
    {
        return !mRequestHttp10 && Detail::HasToken(request, "upgrade", "websocket");
    }

    void DispatchRequest(HttpRequest request, const std::shared_ptr<const AsyncHandler>& handler,
        const std::shared_ptr<State>& owner, bool authorized = false)
    {
        if (!authorized && owner->policy)
        {
            BeginPolicy(request, owner, handler, {}, true);
            return;
        }
        auto bytes = RequestStorage(request);
        for (const auto* fields : { &mPolicyHeaders, &mPolicyAttributes })
            for (const auto& [name, value] : *fields)
                bytes += sizeof(HttpHeaders::value_type) + name.size() + value.size();
        auto reservation = owner->requestBudget->Acquire(bytes);
        const auto requestId = owner->RequestId();
        if (!reservation.active || requestId == 0)
        {
            Observability::Detail::Add(owner->observation->rejectedRequests);
            HttpError(503, request.method == "HEAD");
            return;
        }
        auto observedRequest = std::make_shared<Detail::RequestObservation>(
            owner->observation, requestId, mId, request.method);
        // HTTP/1.0은 연결을 유지하지 않고, 길이 모르는 본문을 청크 대신 연결 종료로 끝낸다.
        auto response =
            std::make_shared<Detail::HttpResponseState>(mSender, mOptions, request.method == "HEAD",
                mRequestHttp10 || Detail::HasToken(request, "connection", "close"),
                request.method == "CONNECT", owner.get(), std::move(reservation.active),
                std::move(observedRequest), mPolicyHeaders, mRequestHttp10,
                [weak = weak_from_this()]
                {
                    if (const auto peer = weak.lock())
                        peer->RequestProgress();
                });
        auto storage = std::make_shared<RequestContextStorage>(RequestContextStorage{
            std::move(reservation.storage),
            HttpRequestContext{ std::move(request), response, mBodyReader, mPolicyAttributes } });
        const std::shared_ptr<const HttpRequestContext> context(storage, &storage->context);
        mActiveResponse = response;
        mRequestContext = context;
        // Queue 100 Continue before exposing the writer to a handler, so a
        // final response cannot overtake the interim response.
        if (mReceivingBody && mParser.TakeContinue())
        {
            const auto sent = mSender->Send(Bytes("HTTP/1.1 100 Continue\r\n\r\n"));
            if (!sent.IsOk())
            {
                mConnection->Close();
                return;
            }
        }
        Runtime::TaskOptions taskOptions;
        taskOptions.retainedBytes = bytes;
        taskOptions.parentToken = response->GetCancellationToken();
        auto task = owner->handlers.Submit(
            [context, handler, ownerId = owner.get()](std::stop_token)
            {
                const CallbackScope scope(ownerId);
                try
                {
                    (*handler)(context);
                }
                catch (...)
                {
                    HttpResponse error;
                    error.status = 500;
                    error.close = true;
                    if (!context->response->Complete(error).IsOk())
                        context->response->Abort();
                }
                return Status::Ok();
            },
            taskOptions);
        if (!task.IsOk())
        {
            Observability::Detail::Add(owner->observation->rejectedRequests);
            HttpResponse error;
            error.status = 503;
            error.close = true;
            if (!response->Complete(error).IsOk())
                response->Abort();
        }
    }

    void Upgrade(const HttpRequest& request, std::shared_ptr<const WebSocketCallbacks> callbacks,
        const State* owner, bool authorized = false)
    {
        if (mRequestHttp10 || request.method != "GET" || !request.body.empty() ||
            !request.Header("transfer-encoding").empty() ||
            !Detail::HasToken(request, "connection", "upgrade") ||
            Detail::HasToken(request, "connection", "close") ||
            !Detail::HasToken(request, "upgrade", "websocket"))
        {
            HttpError(400, request.method == "HEAD");
            return;
        }
        std::size_t keys = 0;
        std::size_t versions = 0;
        for (const auto& [name, value] : request.headers)
        {
            (void)value;
            if (name == "sec-websocket-key")
                ++keys;
            if (name == "sec-websocket-version")
                ++versions;
        }
        if (keys != 1 || versions != 1)
        {
            HttpError(400);
            return;
        }
        if (request.Header("sec-websocket-version") != "13")
        {
            HttpError(426);
            return;
        }
        std::string accept;
        if (!Detail::WebSocketAccept(request.Header("sec-websocket-key"), accept))
        {
            HttpError(400);
            return;
        }
        std::string subprotocol;
        if (!Detail::SelectWebSocketSubprotocol(request, callbacks->subprotocols, subprotocol))
        {
            HttpError(400);
            return;
        }
        if (!authorized)
        {
            const auto state = mOwner.lock();
            if (!state)
            {
                mConnection->Close();
                return;
            }
            if (state->policy)
            {
                BeginPolicy(request, state, {}, callbacks, true);
                return;
            }
            if (callbacks->authorize)
            {
                BeginPolicy(request, state, {}, callbacks, false);
                return;
            }
        }
        if (callbacks->accept)
        {
            const CallbackScope callback(owner);
            try
            {
                if (!callbacks->accept(request))
                {
                    HttpError(403);
                    return;
                }
            }
            catch (...)
            {
                HttpError(500);
                return;
            }
        }
        std::string wire = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                           "Upgrade\r\nSec-WebSocket-Accept: " +
                           accept + "\r\n";
        if (!subprotocol.empty())
            wire += "Sec-WebSocket-Protocol: " + subprotocol + "\r\n";
        for (const auto& [name, value] : mPolicyHeaders)
        {
            if (Detail::EqualInsensitive(name, "sec-websocket-accept") ||
                Detail::EqualInsensitive(name, "upgrade") ||
                Detail::EqualInsensitive(name, "sec-websocket-protocol") ||
                Detail::EqualInsensitive(name, "sec-websocket-extensions"))
            {
                HttpError(500);
                return;
            }
            wire += name + ": " + value + "\r\n";
        }
        wire += "\r\n";
        if (wire.size() > mOptions.maxHeaderBytes)
        {
            HttpError(500);
            return;
        }
        const auto sent = mSender->Send(Bytes(wire));
        if (!sent.IsOk())
        {
            mConnection->Close();
            return;
        }
        mCallbacks = std::move(callbacks);
        HttpHeaders{}.swap(mPolicyHeaders);
        HttpHeaders{}.swap(mPolicyAttributes);
        mUpgraded = true;
        mSubprotocol = std::move(subprotocol);
        mHeartbeatSent = Now();
        mPhase.store(Phase::WebSocket);
        // 정책을 기다리며 받아 둔 입력이 많아 멈춘 수신을 푼다. 받아 둔 프레임은 이 호출 뒤 ProcessInput이
        // 이어서 처리한다(WEB-10).
        if (mSender->flow->IsReceivePaused())
            (void)mSender->flow->ResumeReceive();
        if (mCallbacks->onOpenWithRequest || mCallbacks->onOpen)
        {
            const CallbackScope callback(owner);
            try
            {
                if (mCallbacks->onOpenWithRequest)
                    mCallbacks->onOpenWithRequest(shared_from_this(), request);
                else
                    mCallbacks->onOpen(shared_from_this());
            }
            catch (...)
            {
                WebSocketError(1011);
            }
        }
    }

    bool ProcessWebSocketFrame()
    {
        Detail::WebSocketFrame frame;
        const auto result =
            Detail::ParseClientFrame(Bytes(PendingInput()), mOptions.maxWebSocketFrameBytes, frame);
        if (result.kind == Detail::FrameParseKind::NeedMore)
            return false;
        if (result.kind == Detail::FrameParseKind::Error)
        {
            WebSocketError(result.closeCode);
            return false;
        }
        mInputOffset += result.consumed;
        if (frame.opcode == 8)
        {
            if (frame.payload.size() == 1)
            {
                WebSocketError(1002);
                return false;
            }
            std::uint16_t code = 1005;
            std::string_view reason;
            if (frame.payload.size() >= 2)
            {
                code = static_cast<std::uint16_t>(
                    (std::to_integer<unsigned int>(frame.payload[0]) << 8) |
                    std::to_integer<unsigned int>(frame.payload[1]));
                if (!Detail::ValidCloseCode(code))
                {
                    WebSocketError(1002);
                    return false;
                }
                reason = Text(std::span(frame.payload).subspan(2));
                if (!Core::Detail::IsValidUtf8(reason))
                {
                    WebSocketError(1007);
                    return false;
                }
            }
            mReceivedCloseReason.assign(reason);
            mReceivedCloseCode = code;
            mIgnoreInput = true;
            // 1010 is a client-only status. A server acknowledges it with normal closure.
            if (code == 1010)
            {
                frame.payload[0] = std::byte{ 0x03 };
                frame.payload[1] = std::byte{ 0xe8 };
            }
            (void)SendClose(frame.payload);
            mConnection->CloseAfterSend();
            return false;
        }
        if (frame.opcode == 9)
        {
            if (!SendFrame(10, frame.payload, true).IsOk())
                mConnection->Close();
            return true;
        }
        if (frame.opcode == 10)
        {
            if (mHeartbeatPending && frame.payload.size() == mHeartbeatPayload.size() &&
                std::equal(frame.payload.begin(), frame.payload.end(), mHeartbeatPayload.begin()))
            {
                mHeartbeatPending = false;
                mHeartbeatSent = Now();
            }
            return true;
        }
        if (mPhase.load() == Phase::WsClosing)
            return true;
        if (frame.opcode == 0)
        {
            if (mFragmentOpcode == 0)
            {
                WebSocketError(1002);
                return false;
            }
        }
        else
        {
            if (mFragmentOpcode != 0)
            {
                WebSocketError(1002);
                return false;
            }
            mFragmentOpcode = frame.opcode;
        }
        if (frame.payload.size() > mOptions.maxWebSocketMessageBytes - mMessage.size())
        {
            WebSocketError(1009);
            return false;
        }
        mMessage.insert(mMessage.end(), frame.payload.begin(), frame.payload.end());
        if (!frame.final)
            return true;
        if (mFragmentOpcode == 1 && !Core::Detail::IsValidUtf8(Text(mMessage)))
        {
            WebSocketError(1007);
            return false;
        }
        const auto type =
            mFragmentOpcode == 1 ? WebSocketMessageType::Text : WebSocketMessageType::Binary;
        mFragmentOpcode = 0;
        if (mCallbacks && mCallbacks->onMessage)
        {
            const auto owner = mOwner.lock();
            const CallbackScope callback(owner.get());
            try
            {
                mCallbacks->onMessage(shared_from_this(), WebSocketMessage{ type, mMessage });
            }
            catch (...)
            {
                WebSocketError(1011);
                return false;
            }
        }
        mMessage.clear();
        return true;
    }

    std::weak_ptr<State> mOwner;
    const std::uint64_t mId;
    std::shared_ptr<Net::Connection> mConnection;
    std::shared_ptr<Detail::TrackedSender> mSender;
    const HttpServerOptions mOptions;
    Detail::HttpParser mParser;
    std::string mInput;
    /// <summary>mInput 앞에서 이미 처리한 바이트 수다. ProcessInput을 떠날 때 0으로 돌아간다.</summary>
    std::size_t mInputOffset = 0;
    // One lock orders input, maintenance and public WebSocket operations. Send
    // failures can synchronously reenter OnDisconnected, requiring recursion;
    // separate input/send locks would invert that order across these callbacks.
    std::recursive_mutex mInputMutex;
    // 아래 셋은 mInputMutex가 지킨다. 깊이는 잠금을 쥔 스레드의 InputLock 겹 수다.
    std::size_t mInputDepth = 0;
    bool mDisconnectPending = false;
    Status mDisconnectReason = Status::Ok();
    // 진행 스레드의 줄에 서 있는지, 업로드가 몫을 기다리며 멈췄는지다. 잠금 없이 사건 쪽에서 읽는다.
    std::atomic<bool> mProgressQueued{ false };
    std::atomic<bool> mAwaitingCredit{ false };
    // 앱이 WebSocket 수신을 멈췄는지와, 재개 뒤 pong 기한을 아직 되돌리지 않았는지다.
    std::atomic<bool> mReceiveHeld{ false };
    std::atomic<bool> mHeartbeatResumed{ false };
    std::shared_ptr<Detail::HttpResponseState> mActiveResponse;
    std::shared_ptr<const HttpRequestContext> mRequestContext;
    HttpRequest mIncomingRequest;
    /// <summary>처리 중인 요청의 요청 줄이 HTTP/1.0이었는지다. 다음 요청의 머리를 해석할 때 바뀐다.</summary>
    bool mRequestHttp10 = false;
    std::shared_ptr<Detail::RequestBodyState> mBodyReader;
    bool mReceivingBody = false;
    std::shared_ptr<Detail::RequestDecisionState> mPolicyDecision;
    std::shared_ptr<const HttpPolicyContext> mPolicyContext;
    std::shared_ptr<void> mPolicyActive;
    std::shared_ptr<const AsyncHandler> mPolicyHandler;
    std::shared_ptr<const WebSocketCallbacks> mPolicySocket;
    HttpHeaders mPolicyHeaders, mPolicyAttributes;
    bool mPolicyWasGlobal = false;
    std::int64_t mPolicyStarted = 0;
    std::atomic<Phase> mPhase{ Phase::Http };
    std::atomic<std::int64_t> mLastActivity;
    std::atomic<std::int64_t> mRequestStarted{ 0 };
    // 수락 시각과, 이 연결에서 바이트를 한 번이라도 받았는지다. 첫 바이트 기한에 쓴다.
    const std::int64_t mAccepted;
    std::atomic<bool> mReceivedInput{ false };
    std::atomic<std::int64_t> mCloseStarted{ 0 };
    std::int64_t mLastSendProgress;
    std::uint64_t mLastDrained = 0;
    bool mHadQueued = false;
    std::shared_ptr<const WebSocketCallbacks> mCallbacks;
    bool mUpgraded = false;
    bool mIgnoreInput = false;
    std::uint16_t mReceivedCloseCode = 1006;
    std::string mReceivedCloseReason;
    std::uint8_t mFragmentOpcode = 0;
    std::vector<std::byte> mMessage;
    std::string mSubprotocol;
    std::uint64_t mNextOutgoingId = 0, mOutgoingId = 0;
    std::size_t mOutgoingBytes = 0;
    std::uint8_t mOutgoingOpcode = 0;
    bool mOutgoingStarted = false, mOutgoingWriting = false;
    Detail::Utf8FragmentState mOutgoingUtf8;
    bool mHeartbeatPending = false;
    std::uint64_t mHeartbeatSequence = 0;
    std::int64_t mHeartbeatSent = 0;
    std::array<std::byte, 16> mHeartbeatPayload{};
};

Status HttpServer::State::Start(const HttpServerOptions& value)
{
    const std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    if (used)
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!ValidOptions(value))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    used = true;
    try
    {
        options = value;
        requestBudget->maxActive = options.maxActiveHttpRequests;
        requestBudget->maximum = options.maxTotalRequestBytes;
        auto result = acceptor.SetSendQueueLimits(
            { Net::SendQueueLimitBytes, options.maxTotalSendQueueCapacityBytes });
        if (!result.IsOk())
            return result;
        peers.reserve(options.maxConnections);
        deferredDisconnects.reserve(options.maxConnections);
        progressQueue.reserve(options.maxConnections);
        progressBatch.reserve(options.maxConnections);
        acceptor.SetConnectionHandler(
            [weak = weak_from_this()](std::shared_ptr<Net::Connection> connection)
            {
                if (const auto state = weak.lock())
                    state->Accepted(std::move(connection));
                else
                    connection->Close();
            });
        const auto endpoint = Core::IpEndpoint::Parse(options.listenAddress, options.port);
        if (!endpoint.IsOk())
            return endpoint.GetStatus();
        result = acceptor.Listen(endpoint.Value(), options.acceptBacklog, options.ipv6Only);
        if (!result.IsOk())
            return result;
        result = io.Start(options.ioWorkerThreadCount);
        if (!result.IsOk())
        {
            Shutdown();
            stopped = true;
            return result;
        }
        result = handlers.Start({ options.handlerWorkerThreadCount, options.maxPendingHandlers,
            options.maxHandlerRetainedBytes });
        if (!result.IsOk())
        {
            Shutdown();
            stopped = true;
            return result;
        }
        result = observation->Start();
        if (!result.IsOk())
        {
            Shutdown();
            stopped = true;
            return result;
        }
        // Start the sweep before accepts, so a thread allocation failure has no peers to drain.
        sweepThread = std::thread([this] { Sweep(); });
        progressThread = std::thread([this] { Progress(); });
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
    if (CallbackScope::Contains(this) || io.IsCurrentThreadIoThread())
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    const std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    if (!used || stopped)
        return Status::Ok();
    Shutdown();
    stopped = true;
    return Status::Ok();
}

Status HttpServer::State::BeginDrain()
{
    if (CallbackScope::Contains(this) || io.IsCurrentThreadIoThread())
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    const std::lock_guard lifecycle(lifecycleMutex);
    if (draining.load() || stopping.load())
        return Status::Ok();
    if (!running.load())
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!draining.exchange(true))
        acceptor.Stop();
    sweepChanged.notify_all();
    return Status::Ok();
}
Status HttpServer::State::DrainStatus() const noexcept
{
    if (!draining.load() && !stopping.load())
        return Status::FailWithoutMessage(ErrorCode::Closed);
    const std::lock_guard guard(peersMutex);
    return peers.empty() ? Status::Ok() : Status::FailWithoutMessage(ErrorCode::WouldBlock);
}
Status HttpServer::State::StopGracefully(Clock::time_point deadline)
{
    if (CallbackScope::Contains(this) || io.IsCurrentThreadIoThread())
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    auto started = BeginDrain();
    if (!started.IsOk())
        return started;
    bool drained;
    {
        std::unique_lock guard(peersMutex);
        drained = peersChanged.wait_until(guard, deadline, [this] { return peers.empty(); });
    }
    auto stoppedResult = Stop();
    return !stoppedResult.IsOk()
               ? stoppedResult
               : (drained ? Status::Ok() : Status::FailWithoutMessage(ErrorCode::Timeout));
}

Core::Result<Core::CompletionSubscription> HttpServer::State::WaitForDrain(
    std::function<void(Status)> callback, std::stop_token cancellation)
{
    using Result = Core::Result<Core::CompletionSubscription>;
    if (!callback)
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    try
    {
        auto registered = Core::CompletionSubscription::Create(
            [owner = static_cast<const void*>(this), callback = std::move(callback)](Status status)
            {
                const CallbackScope scope(owner);
                callback(std::move(status));
            });
        if (!registered.IsOk())
            return registered;
        auto source = registered.Value().GetSource();
        bool ready;
        {
            const std::lock_guard guard(peersMutex);
            if (!draining.load() && !stopping.load())
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
            if (drainWait.IsPending())
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::AlreadyExists));
            ready = peers.empty();
            if (!ready)
                drainWait = source;
        }
        registered.Value().BindCancellation(cancellation);
        if (ready)
            (void)source.Complete();
        return registered;
    }
    catch (...)
    {
        return Result::FromStatus(Status::AllocationFailure());
    }
}

void HttpServer::State::Shutdown()
{
    stopping.store(true);
    running.store(false);
    handlers.RequestStop();
    acceptor.Stop();
    sweepChanged.notify_all();
    if (sweepThread.joinable())
        sweepThread.join();
    {
        const std::lock_guard guard(progressMutex);
        progressChanged.notify_all();
    }
    if (progressThread.joinable())
        progressThread.join();
    {
        // 남은 진행 요청은 버린다. 아래에서 모든 피어를 닫으므로 이어 갈 입력이 없다.
        const std::lock_guard guard(progressMutex);
        progressQueue.clear();
    }
    // 진행 스레드가 끝났으니 넘겨받은 끊김 통지는 여기서 마무리한다.
    while (RunDeferredDisconnect())
    {
    }
    // Avoid allocating a shutdown snapshot: close one retained peer at a time,
    // then wait for its observer to release the registry entry and callback.
    while (true)
    {
        std::shared_ptr<Peer> peer;
        std::uint64_t id = 0;
        {
            const std::lock_guard<std::mutex> guard(peersMutex);
            if (peers.empty())
                break;
            id = peers.begin()->first;
            peer = peers.begin()->second;
        }
        peer->ForceClose();
        std::unique_lock<std::mutex> lock(peersMutex);
        while (true)
        {
            peersChanged.wait(lock, [this, id]
                { return peers.find(id) == peers.end() || !deferredDisconnects.empty(); });
            if (peers.find(id) == peers.end())
                break;
            lock.unlock();
            while (RunDeferredDisconnect())
            {
            }
            lock.lock();
        }
    }
    (void)handlers.Stop();
    io.Stop();
    observation->Stop();
    stopComplete.store(true);
}

void HttpServer::State::Accepted(std::shared_ptr<Net::Connection> connection) noexcept
{
    std::shared_ptr<Peer> peer;
    try
    {
        {
            const std::lock_guard<std::mutex> guard(peersMutex);
            if (!stopping.load() && !draining.load() && peers.size() < options.maxConnections &&
                nextId != 0)
            {
                const auto id = nextId++;
                peer = std::make_shared<Peer>(weak_from_this(), id, connection, options);
                peers.emplace(id, peer);
                Observability::Detail::Add(observation->acceptedConnections);
            }
        }
        if (!peer)
        {
            Observability::Detail::Add(observation->rejectedConnections);
            connection->Close();
            return;
        }
        connection->SetObserver(peer);
    }
    catch (...)
    {
        connection->Close();
        if (peer)
            Disconnected(peer->Id());
    }
}

void HttpServer::State::Disconnected(std::uint64_t id) noexcept
{
    Core::CompletionSource complete;
    {
        const std::lock_guard<std::mutex> guard(peersMutex);
        if (peers.erase(id))
            Observability::Detail::Add(observation->closedConnections);
        if (peers.empty())
        {
            complete = drainWait;
            drainWait = {};
        }
    }
    peersChanged.notify_all();
    const CallbackScope callback(this);
    (void)complete.Complete();
}

void HttpServer::State::DeferDisconnect(std::shared_ptr<Peer> peer) noexcept
{
    try
    {
        const std::lock_guard guard(peersMutex);
        deferredDisconnects.push_back(peer);
    }
    catch (...)
    {
        // Start가 maxConnections만큼 잡아 두었으므로 오지 않는 경로다. 오면 예전처럼 이 스레드에서 마무리한다.
        peer->CompleteDeferredDisconnect();
        return;
    }
    {
        const std::lock_guard guard(progressMutex);
        deferredPending = true;
    }
    progressChanged.notify_all();
    peersChanged.notify_all();
}

void HttpServer::State::QueueProgress(std::shared_ptr<Peer> peer) noexcept
{
    {
        const std::lock_guard guard(progressMutex);
        try
        {
            // 멈춘 뒤에는 줄을 세우지 않는다. 피어마다 한 번만 서므로 대개 Start가 잡은 용량 안에 든다.
            if (!stopping.load())
            {
                progressQueue.push_back(std::move(peer));
                peer = nullptr;
            }
        }
        catch (...)
        {
        }
    }
    if (peer)
    {
        peer->ClearProgressRequest();
        return;
    }
    progressChanged.notify_all();
}

void HttpServer::State::Progress() noexcept
{
    std::unique_lock lock(progressMutex);
    while (true)
    {
        progressChanged.wait(
            lock, [this] { return stopping.load() || deferredPending || !progressQueue.empty(); });
        if (stopping.load())
            break;
        deferredPending = false;
        progressBatch.swap(progressQueue);
        lock.unlock();
        while (RunDeferredDisconnect())
        {
        }
        for (const auto& peer : progressBatch)
            peer->RunProgress();
        progressBatch.clear();
        lock.lock();
    }
}

bool HttpServer::State::RunDeferredDisconnect() noexcept
{
    std::shared_ptr<Peer> peer;
    {
        const std::lock_guard guard(peersMutex);
        if (deferredDisconnects.empty())
            return false;
        peer = std::move(deferredDisconnects.back());
        deferredDisconnects.pop_back();
    }
    peer->CompleteDeferredDisconnect();
    return true;
}

Observability::HttpServerMetricsSnapshot HttpServer::State::GetMetrics() const noexcept
{
    auto result = observation->Snapshot();
    {
        const std::lock_guard guard(peersMutex);
        result.activeConnections = peers.size();
        for (const auto& [id, peer] : peers)
        {
            (void)id;
            result.retainedSendBytes += peer->RetainedSendBytes();
        }
    }
    {
        const std::lock_guard guard(requestBudget->mutex);
        result.activeRequests = requestBudget->active;
        result.retainedRequestBytes = requestBudget->retained;
    }
    result.handlers = handlers.GetMetrics();
    result.lifecycle = stopComplete.load()                    ? Observability::Lifecycle::Stopped
                       : (draining.load() || stopping.load()) ? Observability::Lifecycle::Draining
                       : running.load()                       ? Observability::Lifecycle::Running
                                                              : Observability::Lifecycle::Created;
    return result;
}

void HttpServer::State::Sweep() noexcept
{
    onMaintenanceThread = true;
    std::unique_lock<std::mutex> waitLock(sweepMutex);
    while (!stopping.load())
    {
        sweepChanged.wait_for(
            waitLock, std::chrono::milliseconds(25), [this] { return stopping.load(); });
        if (stopping.load())
            break;
        waitLock.unlock();
        try
        {
            std::vector<std::shared_ptr<Peer>> snapshot;
            {
                const std::lock_guard<std::mutex> guard(peersMutex);
                snapshot.reserve(peers.size());
                for (const auto& [id, peer] : peers)
                {
                    (void)id;
                    snapshot.push_back(peer);
                }
            }
            const auto now = Now();
            for (const auto& peer : snapshot)
            {
                if (draining.load())
                    peer->BeginDrain();
                peer->CheckTimeout(now);
            }
        }
        catch (...)
        { /* A later sweep retries if a temporary snapshot cannot be allocated. */
        }
        waitLock.lock();
    }
}

HttpServer::HttpServer()
    : mState(std::make_shared<State>())
{
}

HttpServer::~HttpServer()
{
    const auto result = mState->Stop();
    SERVERCORE_ASSERT(
        result.IsOk(), "HttpServer must be destroyed outside its callbacks and I/O threads");
}

Status HttpServer::RegisterRoute(std::string_view method, std::string_view path, Handler handler)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!handler)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        return RegisterAsyncRoute(method, path, AdaptHandler(std::move(handler)));
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

Status HttpServer::RegisterAsyncRoute(
    std::string_view method, std::string_view path, AsyncHandler handler)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!Detail::IsToken(method) || !ValidPath(path) || !handler)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        // Keep one persistent callable identity. Retain this local until after
        // the lifecycle lock is released, including duplicate-route failures.
        std::shared_ptr<const AsyncHandler> stored;
        const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        stored = std::make_shared<const AsyncHandler>(std::move(handler));
        return mState->routes.AddExact(method, path, stored);
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

Status HttpServer::RegisterStreamingRoute(
    std::string_view method, std::string_view path, AsyncHandler handler)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!Detail::IsToken(method) || !ValidPath(path) || !handler)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        auto stored = std::make_shared<const AsyncHandler>(std::move(handler));
        const std::lock_guard guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        return mState->routes.AddExact(method, path, HttpRoute{ std::move(stored), true });
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

Status HttpServer::RegisterWebSocket(std::string_view path, WebSocketCallbacks callbacks)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!ValidPath(path) || !ValidWebSocketCallbacks(callbacks))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        return mState->sockets.AddExact(
            "GET", path, std::make_shared<const WebSocketCallbacks>(std::move(callbacks)));
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

Status HttpServer::RegisterRoutePattern(
    std::string_view method, std::string_view pattern, Handler handler)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!handler)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        return RegisterAsyncRoutePattern(method, pattern, AdaptHandler(std::move(handler)));
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

Status HttpServer::RegisterAsyncRoutePattern(
    std::string_view method, std::string_view pattern, AsyncHandler handler)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!Detail::IsToken(method) || !handler)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        Detail::RoutePattern compiled;
        if (!Detail::CompileRoutePattern(pattern, compiled))
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        std::shared_ptr<const AsyncHandler> stored;
        const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        stored = std::make_shared<const AsyncHandler>(std::move(handler));
        return mState->routes.AddPattern(method, std::move(compiled), stored);
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

Status HttpServer::RegisterStreamingRoutePattern(
    std::string_view method, std::string_view pattern, AsyncHandler handler)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!Detail::IsToken(method) || !handler)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        Detail::RoutePattern compiled;
        if (!Detail::CompileRoutePattern(pattern, compiled))
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        auto stored = std::make_shared<const AsyncHandler>(std::move(handler));
        const std::lock_guard guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        return mState->routes.AddPattern(
            method, std::move(compiled), HttpRoute{ std::move(stored), true });
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

Status HttpServer::RegisterWebSocketPattern(std::string_view pattern, WebSocketCallbacks callbacks)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!ValidWebSocketCallbacks(callbacks))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        Detail::RoutePattern compiled;
        if (!Detail::CompileRoutePattern(pattern, compiled))
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        const std::lock_guard<std::mutex> guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        return mState->sockets.AddPattern("GET", std::move(compiled),
            std::make_shared<const WebSocketCallbacks>(std::move(callbacks)));
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
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
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    return mState->Start(options);
}
Status HttpServer::Stop()
{
    return mState->Stop();
}
Status HttpServer::BeginDrain()
{
    return mState->BeginDrain();
}
Status HttpServer::DrainStatus() const noexcept
{
    return mState->DrainStatus();
}
Core::Result<Core::CompletionSubscription> HttpServer::WaitForDrain(
    std::function<void(Status)> callback, std::stop_token cancellation)
{
    return mState->WaitForDrain(std::move(callback), cancellation);
}
Status HttpServer::StopGracefully(Clock::time_point deadline)
{
    return mState->StopGracefully(deadline);
}
Status HttpServer::SetRequestPolicy(RequestPolicy policy)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!policy)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    try
    {
        auto stored = std::make_shared<const RequestPolicy>(std::move(policy));
        std::shared_ptr<const RequestPolicy> previous;
        {
            const std::lock_guard guard(mState->lifecycleMutex);
            if (mState->used)
                return Status::FailWithoutMessage(ErrorCode::Closed);
            previous = std::exchange(mState->policy, std::move(stored));
        }
        return Status::Ok();
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}
Status HttpServer::SetLogger(std::shared_ptr<Core::ILogger> logger)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    std::shared_ptr<Core::ILogger> previous;
    {
        const std::lock_guard guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        mState->acceptor.SetLogger(logger);
        previous = std::exchange(mState->logger, std::move(logger));
    }
    return Status::Ok();
}
bool HttpServer::IsRunning() const noexcept
{
    return mState->running.load();
}
std::uint16_t HttpServer::Port() const noexcept
{
    return mState->acceptor.Port();
}
Observability::HttpServerMetricsSnapshot HttpServer::GetMetrics() const noexcept
{
    return mState->GetMetrics();
}
Status HttpServer::SetRequestTraceHandler(
    Observability::RequestTraceHandler handler, std::size_t maximum)
{
    if (CallbackScope::Contains(mState.get()))
        return Status::FailWithoutMessage(ErrorCode::Closed);
    if (!handler || maximum == 0 || maximum > 65536)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    {
        const std::lock_guard guard(mState->lifecycleMutex);
        if (mState->used)
            return Status::FailWithoutMessage(ErrorCode::Closed);
    }
    try
    {
        return mState->observation->Configure(std::move(handler), maximum);
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}
std::shared_ptr<WebSocketFlowControl> GetWebSocketFlowControl(
    const std::shared_ptr<WebSocketConnection>& connection) noexcept
{
    return std::dynamic_pointer_cast<WebSocketFlowControl>(connection);
}
std::shared_ptr<WebSocketMessageControl> GetWebSocketMessageControl(
    const std::shared_ptr<WebSocketConnection>& connection) noexcept
{
    return std::dynamic_pointer_cast<WebSocketMessageControl>(connection);
}
}
