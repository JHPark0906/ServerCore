#include "ServerCore/C/Web.h"
#include "C/EndpointInternal.h"
#include "C/Internal.h"
#include "C/ObservationInternal.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Observability/Prometheus.h"
#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Web/HttpServer.h"
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <vector>

namespace C = ServerCore::CDetail;
namespace W = ServerCore::Web;
namespace ServerCore::CDetail
{
struct WebState;
struct SocketState;
struct ResponseOwner;
struct SocketOwner;
struct BodyOwner;
struct DecisionOwner;
struct EventStorageCharge
{
    std::shared_ptr<std::atomic<std::uint64_t>> counter;
    std::uint64_t bytes = 0;
    void Set(
        const std::shared_ptr<std::atomic<std::uint64_t>>& target, std::uint64_t amount) noexcept
    {
        counter = target;
        bytes = amount;
        counter->fetch_add(bytes, std::memory_order_relaxed);
    }
    ~EventStorageCharge()
    {
        if (counter)
            counter->fetch_sub(bytes, std::memory_order_relaxed);
    }
};
}
struct sc_http_response
{
    std::shared_ptr<C::ResponseOwner> owner;
};
struct sc_websocket
{
    std::shared_ptr<C::SocketOwner> owner;
};
struct sc_websocket_message
{
    std::shared_ptr<C::SocketOwner> owner;
    std::shared_ptr<W::WebSocketMessageWriter> writer;
};
struct sc_http_body
{
    std::shared_ptr<C::BodyOwner> owner;
};
struct sc_request_decision
{
    std::shared_ptr<C::DecisionOwner> owner;
};
struct sc_body_chunk
{
    std::shared_ptr<const std::vector<std::byte>> value;
};
struct sc_websocket_event
{
    // Leases are released after the storage they account for.
    std::shared_ptr<C::Budget::Lease> own, charge, slot;
    uint32_t kind = SC_WS_CLOSED;
    uint16_t code = 1006;
    std::string bytes;
};
struct sc_web_event
{
    // First member retires last, after all owned strings/views/context storage.
    C::EventStorageCharge additionalStorage;
    std::shared_ptr<C::Budget::Lease> charge;
    uint32_t kind = SC_WEB_REQUEST;
    std::shared_ptr<const W::HttpRequestContext> context;
    std::shared_ptr<const W::HttpPolicyContext> policy;
    W::HttpRequest handshake;
    std::shared_ptr<C::SocketState> socket;
    std::vector<sc_header> headers, parameters, attributes;
    std::mutex mutex;
    bool claimed = false;
    bool bodyClaimed = false;
    std::weak_ptr<C::ResponseOwner> responseOwner;
    std::weak_ptr<C::SocketOwner> socketOwner;
    std::weak_ptr<C::BodyOwner> bodyOwner;
    std::weak_ptr<C::DecisionOwner> decisionOwner;
    const W::HttpRequest& Request() const
    {
        return context ? context->request : policy ? policy->request : handshake;
    }
    void Prepare()
    {
        for (const auto& [name, value] : Request().headers)
            headers.push_back({ C::View(name), C::View(value) });
        for (const auto& [name, value] : Request().pathParameters)
            parameters.push_back({ C::View(name), C::View(value) });
        if (context)
            for (const auto& [name, value] : context->attributes)
                attributes.push_back({ C::View(name), C::View(value) });
    }
    size_t WrapperBytes() const noexcept
    {
        return sizeof(sc_web_event) +
               (headers.capacity() + parameters.capacity() + attributes.capacity()) *
                   sizeof(sc_header);
    }
    ~sc_web_event();
};
struct sc_web_server
{
    std::shared_ptr<C::WebState> state;
};

namespace ServerCore::CDetail
{
struct BodyOwner
{
    explicit BodyOwner(std::shared_ptr<W::HttpRequestBody> value)
        : body(std::move(value))
    {
    }
    ~BodyOwner() { body->Cancel(); }
    std::shared_ptr<W::HttpRequestBody> body;
};
struct DecisionOwner
{
    explicit DecisionOwner(std::shared_ptr<W::HttpRequestDecision> value)
        : decision(std::move(value))
    {
    }
    ~DecisionOwner() { decision->Abort(); }
    std::shared_ptr<W::HttpRequestDecision> decision;
};
struct ResponseOwner
{
    explicit ResponseOwner(std::shared_ptr<const W::HttpRequestContext> value)
        : context(std::move(value))
    {
    }
    ~ResponseOwner() { context->response->Abort(); }
    std::shared_ptr<const W::HttpRequestContext> context;
};
/// <summary>
/// WebSocket 하나의 수신 사건과 흐름 제어(CABI-2). 메시지는 소켓별 몫(own)과 서버 전역 WebSocket
/// 몫(budget)에 함께 과금한다. 받은 메시지는 거절하지 않고, 둘 중 하나가 차면 이 소켓의 수신만
/// 멈춘다(WebSocketFlowControl::PauseReceive). 소비자가 사건을 버려 몫이 돌아오면 다시 연다. 그래서
/// 사건을 소비하지 않는 소켓이 다른 소켓을 끊지 못한다(CAbi.WebSocketOwnershipAndOverflow).
/// </summary>
/// <remarks>
/// Message는 onMessage 안에서 불리므로 여기서 멈추면 재개 전까지 다음 onMessage가 오지 않는다. 판단과
/// 네이티브 호출은 flowMutex 아래에서 한다. 네이티브는 피어 잠금을 잡지 않고, 재개한 뒤의 메시지는
/// 진행 스레드가 내므로 이 잠금으로 되돌아오지 않는다. 전역 몫 때문에 멈춘 소켓은 서버의 대기 목록에
/// 등록한 뒤 한 번 더 확인해 깨움을 놓치지 않는다(TCP의 TcpPeer와 같은 규칙).
/// </remarks>
struct SocketState : std::enable_shared_from_this<SocketState>
{
    std::shared_ptr<Budget::Lease> slot;
    std::shared_ptr<W::WebSocketConnection> connection;
    std::shared_ptr<W::WebSocketFlowControl> flow;
    std::weak_ptr<WebState> owner;
    std::shared_ptr<Budget> budget, own;
    PullQueue<sc_websocket_event> events;
    std::unique_ptr<sc_websocket_event> terminal;
    std::mutex flowMutex;
    bool paused = false, waitingGlobal = false;
    void Message(const W::WebSocketMessage& message) noexcept
    {
        try
        {
            const auto size = sizeof(sc_websocket_event) + message.bytes.size();
            auto event = std::make_unique<sc_websocket_event>();
            event->kind = message.type == W::WebSocketMessageType::Text ? SC_WS_TEXT : SC_WS_BINARY;
            event->own = own->Charge(size);
            event->charge = budget->Charge(size);
            if (!message.bytes.empty())
                event->bytes.assign(
                    reinterpret_cast<const char*>(message.bytes.data()), message.bytes.size());
            if (!events.Push(std::move(event)))
            {
                (void)connection->Close();
                return;
            }
        }
        catch (...)
        {
            (void)connection->Close(1011);
            return;
        }
        UpdateFlow();
    }
    void Closed(uint16_t code) noexcept
    {
        if (terminal)
        {
            terminal->code = code;
            events.Finish(std::move(terminal));
        }
    }
    // 멈춤 이유(소켓별 몫, 전역 몫)를 다시 보고 네이티브 수신을 맞춘다.
    void UpdateFlow() noexcept;
};
struct SocketOwner
{
    explicit SocketOwner(std::shared_ptr<SocketState> value)
        : socket(std::move(value))
    {
    }
    ~SocketOwner()
    {
        (void)socket->connection->Close();
        socket->events.Close();
    }
    std::shared_ptr<SocketState> socket;
};
inline size_t RequestBytes(const W::HttpRequest& request)
{
    size_t bytes =
        sizeof(sc_web_event) + request.method.size() + request.target.size() + request.body.size();
    for (const auto& [name, value] : request.headers)
        bytes +=
            sizeof(sc_header) + sizeof(W::HttpHeaders::value_type) + name.size() + value.size();
    for (const auto& [name, value] : request.pathParameters)
        bytes +=
            sizeof(sc_header) + sizeof(W::HttpHeaders::value_type) + name.size() + value.size();
    return bytes;
}
struct WebState : std::enable_shared_from_this<WebState>
{
    explicit WebState(const sc_web_options& value)
        : budget(std::make_shared<Budget>(value.max_event_count, value.max_event_bytes))
        , socketBudget(std::make_shared<Budget>(value.max_ws_event_count, value.max_ws_event_bytes))
        , socketSlots(
              std::make_shared<Budget>(value.max_connections, (std::numeric_limits<size_t>::max)()))
        , socketEventCount(value.max_ws_connection_event_count)
        , socketEventBytes(value.max_ws_connection_event_bytes)
    {
        options.listenAddress = std::string(Text(value.listen_address));
        options.port = value.port;
        options.ioWorkerThreadCount = static_cast<int>(value.io_threads);
        options.handlerWorkerThreadCount = value.handler_threads;
        options.maxConnections = value.max_connections;
        options.maxHeaderBytes = value.max_header_bytes;
        options.maxBodyBytes = value.max_body_bytes;
        options.maxResponseBodyBytes = value.max_buffered_response_bytes;
        options.maxActiveHttpRequests = value.max_active_requests;
        options.maxTotalRequestBytes = value.max_request_bytes;
        options.maxTotalSendQueueCapacityBytes = value.max_send_bytes;
        options.maxStreamChunkBytes = value.max_chunk_bytes;
        options.handlerTimeout = std::chrono::milliseconds(value.handler_timeout_ms);
        options.streamIdleTimeout = std::chrono::milliseconds(value.stream_idle_timeout_ms);
        options.sendStallTimeout = std::chrono::milliseconds(value.send_stall_timeout_ms);
    }
    void Request(std::shared_ptr<const W::HttpRequestContext> context) noexcept
    {
        try
        {
            auto bytes = RequestBytes(context->request);
            for (const auto& [name, value] : context->attributes)
                bytes += sizeof(sc_header) + sizeof(W::HttpHeaders::value_type) + name.size() +
                         value.size();
            auto charge = budget->Acquire(bytes);
            if (!charge || stopped.load())
            {
                W::HttpResponse rejected;
                rejected.status = 503;
                rejected.close = true;
                if (!context->response->Complete(rejected).IsOk())
                    context->response->Abort();
                return;
            }
            auto event = std::make_unique<sc_web_event>();
            event->context = context;
            event->charge = std::move(charge);
            event->Prepare();
            event->additionalStorage.Set(eventStorage, event->WrapperBytes());
            (void)events.Push(std::move(event));
        }
        catch (...)
        {
            context->response->Abort();
        }
    }
    void Policy(std::shared_ptr<const W::HttpPolicyContext> context) noexcept
    {
        try
        {
            auto charge = budget->Acquire(RequestBytes(context->request));
            if (!charge || stopped.load())
            {
                W::HttpResponse rejected;
                rejected.status = 503;
                rejected.close = true;
                if (!context->decision->Reject(rejected).IsOk())
                    context->decision->Abort();
                return;
            }
            auto event = std::make_unique<sc_web_event>();
            event->kind = SC_WEB_POLICY;
            event->policy = context;
            event->charge = std::move(charge);
            event->Prepare();
            event->additionalStorage.Set(eventStorage, event->WrapperBytes());
            (void)events.Push(std::move(event));
        }
        catch (...)
        {
            context->decision->Abort();
        }
    }
    W::RequestPolicy PolicyHandler()
    {
        return [weak = weak_from_this()](auto context)
        {
            if (auto owner = weak.lock())
                owner->Policy(std::move(context));
            else
                context->decision->Abort();
        };
    }
    W::WebSocketCallbacks SocketCallbacks(bool authorize)
    {
        W::WebSocketCallbacks callbacks;
        callbacks.onOpenWithRequest = [weak = weak_from_this()](
                                          const auto& connection, const auto& request)
        {
            if (auto owner = weak.lock())
                owner->OpenSocket(connection, request);
            else
                (void)connection->Close();
        };
        callbacks.onMessage = [weak = weak_from_this()](const auto& connection, const auto& message)
        {
            if (auto owner = weak.lock())
                if (auto socket = owner->Find(connection->Id()))
                    socket->Message(message);
        };
        callbacks.onClose = [weak = weak_from_this()](auto id, auto code, std::string_view)
        {
            if (auto owner = weak.lock())
                owner->Closed(id, code);
        };
        if (authorize)
            callbacks.authorize = PolicyHandler();
        return callbacks;
    }
    void OpenSocket(const std::shared_ptr<W::WebSocketConnection>& connection,
        const W::HttpRequest& request) noexcept
    {
        try
        {
            auto slot = socketSlots->Acquire(1);
            auto charge = budget->Acquire(RequestBytes(request));
            if (!slot || !charge || stopped.load())
            {
                (void)connection->Close(1013);
                return;
            }
            auto socket = std::make_shared<SocketState>();
            socket->connection = connection;
            socket->budget = socketBudget;
            socket->slot = std::move(slot);
            socket->flow = W::GetWebSocketFlowControl(connection);
            socket->owner = weak_from_this();
            socket->own = std::make_shared<Budget>(socketEventCount, socketEventBytes);
            socket->own->released = [weak = std::weak_ptr<SocketState>(socket)]
            {
                if (auto self = weak.lock())
                    self->UpdateFlow();
            };
            socket->terminal = std::make_unique<sc_websocket_event>();
            socket->terminal->slot = socket->slot;
            auto event = std::make_unique<sc_web_event>();
            event->kind = SC_WEB_WEBSOCKET;
            event->handshake = request;
            event->socket = socket;
            event->charge = std::move(charge);
            event->Prepare();
            // Upgrade metadata is copied, unlike HTTP/policy shared contexts.
            event->additionalStorage.Set(eventStorage, RequestBytes(event->handshake));
            {
                std::lock_guard lock(socketMutex);
                sockets.emplace(connection->Id(), socket);
            }
            (void)events.Push(std::move(event));
        }
        catch (...)
        {
            (void)connection->Close(1011);
        }
    }
    std::shared_ptr<SocketState> Find(uint64_t id)
    {
        std::lock_guard lock(socketMutex);
        const auto found = sockets.find(id);
        return found == sockets.end() ? nullptr : found->second;
    }
    void Closed(uint64_t id, uint16_t code)
    {
        std::shared_ptr<SocketState> socket;
        {
            std::lock_guard lock(socketMutex);
            const auto it = sockets.find(id);
            if (it == sockets.end())
                return;
            socket = std::move(it->second);
            sockets.erase(it);
        }
        socket->Closed(code);
    }
    void AddSocketWaiter(std::weak_ptr<SocketState> socket)
    {
        std::lock_guard guard(waitMutex);
        socketWaiters.push_back(std::move(socket));
    }
    // 전역 WebSocket 몫이 돌아올 때마다 부른다. 전역 몫 때문에 멈춘 소켓만 다시 보게 한다.
    void WakeSocketWaiters() noexcept
    {
        std::vector<std::weak_ptr<SocketState>> woken;
        {
            std::lock_guard guard(waitMutex);
            woken.swap(socketWaiters);
        }
        for (const auto& weak : woken)
            if (auto socket = weak.lock())
            {
                {
                    std::lock_guard guard(socket->flowMutex);
                    socket->waitingGlobal = false;
                }
                socket->UpdateFlow();
            }
    }
    sc_status Stop()
    {
        std::lock_guard lock(lifecycle);
        stopped.store(true);
        events.Close();
        const auto status = server.Stop();
        if (status.IsOk())
            stopFinished.store(true);
        return Code(status);
    }
    std::mutex lifecycle, socketMutex;
    std::atomic<bool> stopped{ false };
    std::atomic<bool> stopFinished{ false };
    bool started = false;
    W::HttpServer server;
    W::HttpServerOptions options;
    std::shared_ptr<Budget> budget, socketBudget, socketSlots;
    const size_t socketEventCount, socketEventBytes;
    std::mutex waitMutex;
    std::vector<std::weak_ptr<SocketState>> socketWaiters;
    const std::shared_ptr<std::atomic<std::uint64_t>> eventStorage =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    PullQueue<sc_web_event> events;
    std::unordered_map<uint64_t, std::shared_ptr<SocketState>> sockets;
};
void SocketState::UpdateFlow() noexcept
{
    const auto state = owner.lock();
    try
    {
        std::lock_guard guard(flowMutex);
        for (;;)
        {
            const bool global = budget->Full();
            const bool want = own->Full() || global;
            if (want != paused && flow)
            {
                (void)(want ? flow->PauseReceive() : flow->ResumeReceive());
                paused = want;
            }
            if (!global || !state || waitingGlobal)
                return;
            state->AddSocketWaiter(weak_from_this());
            waitingGlobal = true;
            if (budget->Full())
                return;
        }
    }
    catch (...)
    {
        // 대기 등록에 실패하면 깨울 수단 없이 멈춰 있게 되므로 끊는다.
        (void)connection->Close(1011);
    }
}

sc_status DecodeHead(const sc_response_head* head, W::HttpResponseHead& result)
{
    if (!Version(head) || (head->flags & ~(SC_RESPONSE_CLOSE | SC_RESPONSE_HAS_LENGTH)) != 0 ||
        (head->header_count && !head->headers) || head->header_count > 65536)
        return SC_INVALID_ARGUMENT;
    result.status = head->status;
    result.close = (head->flags & SC_RESPONSE_CLOSE) != 0;
    if (head->flags & SC_RESPONSE_HAS_LENGTH)
        result.contentLength = head->content_length;
    size_t bytes = 0;
    for (size_t i = 0; i < head->header_count; ++i)
    {
        const auto& field = head->headers[i];
        if (!Valid(field.name) || !Valid(field.value))
            return SC_INVALID_ARGUMENT;
        if (!Add(bytes, field.name.len) || !Add(bytes, field.value.len) || bytes > 65536)
            return SC_TOO_LARGE;
        result.headers.emplace_back(Text(field.name), Text(field.value));
    }
    return SC_OK;
}
sc_task_metrics TaskMetrics(const ServerCore::Observability::TaskExecutorMetricsSnapshot& source)
{
    sc_task_metrics value{};
    value.pending_tasks = source.pendingTasks;
    value.running_tasks = source.runningTasks;
    value.retained_bytes = source.retainedBytes;
    value.accepted_tasks = source.acceptedTasks;
    value.completed_tasks = source.completedTasks;
    value.failed_tasks = source.failedTasks;
    value.cancelled_tasks = source.cancelledTasks;
    value.timed_out_tasks = source.timedOutTasks;
    value.rejected_tasks = source.rejectedTasks;
    value.total_latency_nanoseconds = source.totalLatencyNanoseconds;
    value.max_latency_nanoseconds = source.maxLatencyNanoseconds;
    std::copy(source.latencyHistogram.buckets.begin(), source.latencyHistogram.buckets.end(),
        value.latency_buckets);
    return value;
}
}
sc_web_event::~sc_web_event()
{
    if (context && context->body && !bodyClaimed)
        context->body->Cancel();
    if (!claimed)
    {
        if (context)
            context->response->Abort();
        if (socket)
            (void)socket->connection->Close();
        if (policy)
            policy->decision->Abort();
    }
}

extern "C"
{
    sc_status sc_web_server_subscribe(
        sc_web_server* server, sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!server)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect([&] { return server->state->events.Subscribe(notifier, key, out); });
    }
    sc_status sc_web_server_subscribe_drain(
        sc_web_server* server, sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!server)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&]
            {
                return C::SubscribeCompletion([&](auto callback)
                    { return server->state->server.WaitForDrain(std::move(callback)); }, notifier,
                    key, out);
            });
    }
    sc_status sc_http_body_subscribe(
        sc_http_body* body, sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!body)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&]
            {
                return C::SubscribeCompletion([&](auto callback)
                    { return body->owner->body->WaitForReadReady(std::move(callback)); }, notifier,
                    key, out);
            });
    }
    sc_status sc_http_response_subscribe_cancelled(sc_http_response* response,
        sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!response)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&]
            {
                return C::SubscribeCancellation(
                    response->owner->context->response->GetCancellationToken(), notifier, key, out);
            });
    }
    sc_status sc_request_decision_subscribe_cancelled(sc_request_decision* decision,
        sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!decision)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&]
            {
                return C::SubscribeCancellation(
                    decision->owner->decision->GetCancellationToken(), notifier, key, out);
            });
    }
    sc_status sc_websocket_subscribe(
        sc_websocket* socket, sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!socket)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&] { return socket->owner->socket->events.Subscribe(notifier, key, out); });
    }
    sc_status sc_web_options_init(sc_web_options* value, size_t size) noexcept
    {
        if (!value || size < sizeof(*value))
            return SC_INVALID_ARGUMENT;
        *value = {};
        value->abi_version = SC_ABI_VERSION;
        value->struct_size = sizeof(*value);
        value->listen_address = C::View("127.0.0.1");
        value->io_threads = 2;
        value->handler_threads = 2;
        value->max_connections = 256;
        value->max_header_bytes = 16384;
        value->max_body_bytes = 65536;
        value->max_buffered_response_bytes = 256 * 1024;
        value->max_active_requests = 256;
        value->max_request_bytes = 16 * 1024 * 1024;
        value->max_event_count = 256;
        value->max_event_bytes = 16 * 1024 * 1024;
        value->max_ws_event_count = 1024;
        value->max_ws_event_bytes = 16 * 1024 * 1024;
        value->max_ws_connection_event_count = 64;
        value->max_ws_connection_event_bytes = 1024 * 1024;
        value->max_send_bytes = 256 * 1024 * 1024;
        value->max_chunk_bytes = 65536;
        value->handler_timeout_ms = 30000;
        value->stream_idle_timeout_ms = 120000;
        value->send_stall_timeout_ms = 30000;
        return SC_OK;
    }
    sc_status sc_response_head_init(sc_response_head* head, size_t size) noexcept
    {
        if (!head || size < sizeof(*head))
            return SC_INVALID_ARGUMENT;
        *head = {};
        head->abi_version = SC_ABI_VERSION;
        head->struct_size = sizeof(*head);
        head->status = 200;
        return SC_OK;
    }
    sc_status sc_web_server_create(const sc_web_options* options, sc_web_server** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Version(options) || !C::Valid(options->listen_address) ||
            options->listen_address.len > 64 || !options->max_event_count ||
            options->max_event_count > 65536 || !options->max_event_bytes ||
            !options->max_ws_event_count || options->max_ws_event_count > 65536 ||
            !options->max_ws_event_bytes || options->io_threads > 64 ||
            options->handler_threads > 64 || !options->max_connections ||
            options->max_connections > 65536 || options->reserved != 0 || options->reserved2 != 0 ||
            !options->max_ws_connection_event_count ||
            options->max_ws_connection_event_count > 65536 ||
            !options->max_ws_connection_event_bytes)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto result = std::make_unique<sc_web_server>();
                result->state = std::make_shared<C::WebState>(*options);
                result->state->socketBudget->released =
                    [weak = std::weak_ptr<C::WebState>(result->state)]
                {
                    if (auto state = weak.lock())
                        state->WakeSocketWaiters();
                };
                *out = result.release();
                return SC_OK;
            });
    }
    sc_status sc_web_server_route(
        sc_web_server* server, sc_bytes method, sc_bytes path, uint32_t pattern) noexcept
    {
        if (!server || !C::Valid(method) || !C::Valid(path) || method.len > 65536 ||
            path.len > 65536 || pattern > 1)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                if (state->stopped.load())
                    return SC_CLOSED;
                W::HttpServer::AsyncHandler handler = [weak = std::weak_ptr(state)](auto context)
                {
                    if (auto owner = weak.lock())
                        owner->Request(std::move(context));
                    else
                        context->response->Abort();
                };
                return C::Code(pattern ? state->server.RegisterAsyncRoutePattern(
                                             C::Text(method), C::Text(path), std::move(handler))
                                       : state->server.RegisterAsyncRoute(
                                             C::Text(method), C::Text(path), std::move(handler)));
            });
    }
    sc_status sc_web_server_set_ipv6_only(sc_web_server* server, uint32_t ipv6Only) noexcept
    {
        if (!server || ipv6Only > 1)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                if (state->started || state->stopped.load())
                    return SC_CLOSED;
                state->options.ipv6Only = ipv6Only != 0;
                return SC_OK;
            });
    }
    sc_status sc_web_server_websocket(
        sc_web_server* server, sc_bytes path, uint32_t pattern) noexcept
    {
        sc_websocket_route_options options{};
        (void)sc_websocket_route_options_init(&options, sizeof(options));
        return sc_web_server_websocket_ex(server, path, pattern, &options);
    }
    sc_status sc_websocket_options_init(sc_websocket_options* options, size_t size) noexcept
    {
        if (!options || size < sizeof(*options))
            return SC_INVALID_ARGUMENT;
        *options = {};
        options->abi_version = SC_ABI_VERSION;
        options->struct_size = sizeof(*options);
        options->max_frame_bytes = 64 * 1024;
        options->max_message_bytes = 256 * 1024;
        options->pong_timeout_ms = 10000;
        return SC_OK;
    }
    sc_status sc_websocket_route_options_init(
        sc_websocket_route_options* options, size_t size) noexcept
    {
        if (!options || size < sizeof(*options))
            return SC_INVALID_ARGUMENT;
        *options = {};
        options->abi_version = SC_ABI_VERSION;
        options->struct_size = sizeof(*options);
        return SC_OK;
    }
    sc_status sc_web_server_set_websocket_options(
        sc_web_server* server, const sc_websocket_options* options) noexcept
    {
        if (!server || !C::Version(options) || !options->max_frame_bytes ||
            options->max_frame_bytes > ServerCore::Net::SendQueueLimitBytes - 14 ||
            options->max_message_bytes < options->max_frame_bytes ||
            options->max_message_bytes > 16 * 1024 * 1024 || !options->pong_timeout_ms)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                if (state->started || state->stopped.load())
                    return SC_CLOSED;
                state->options.maxWebSocketFrameBytes = options->max_frame_bytes;
                state->options.maxWebSocketMessageBytes = options->max_message_bytes;
                state->options.webSocketPingInterval =
                    std::chrono::milliseconds(options->ping_interval_ms);
                state->options.webSocketPongTimeout =
                    std::chrono::milliseconds(options->pong_timeout_ms);
                return SC_OK;
            });
    }
    sc_status sc_web_server_websocket_ex(sc_web_server* server, sc_bytes path, uint32_t pattern,
        const sc_websocket_route_options* options) noexcept
    {
        if (!server || !C::Valid(path) || path.len > 65536 || pattern > 1)
            return SC_INVALID_ARGUMENT;
        if (!C::Version(options) || (options->flags & ~SC_WS_AUTHORIZE) != 0 ||
            options->subprotocol_count > 64 ||
            (options->subprotocol_count != 0 && !options->subprotocols))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                if (state->stopped.load())
                    return SC_CLOSED;
                auto callbacks = state->SocketCallbacks((options->flags & SC_WS_AUTHORIZE) != 0);
                size_t bytes = 0;
                for (size_t index = 0; index < options->subprotocol_count; ++index)
                {
                    const auto token = options->subprotocols[index];
                    if (!C::Valid(token) || token.len > 4096 - bytes)
                        return SC_INVALID_ARGUMENT;
                    bytes += token.len;
                    callbacks.subprotocols.emplace_back(C::Text(token));
                }
                return C::Code(
                    pattern ? state->server.RegisterWebSocketPattern(
                                  C::Text(path), std::move(callbacks))
                            : state->server.RegisterWebSocket(C::Text(path), std::move(callbacks)));
            });
    }
    sc_status sc_web_server_start(sc_web_server* server) noexcept
    {
        if (!server)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                if (state->stopped.load())
                    return sc_status{ SC_CLOSED };
                // 네이티브 Start는 옵션 검증 실패(InvalidArgument)를 서버를 쓴 것으로 치기 전에 돌려준다.
                // 그때는 설정 함수를 열어 두어 옵션을 고쳐 다시 Start할 수 있게 한다(CAbi.WebStreamingBodyOwnership).
                const auto status = state->server.Start(state->options);
                state->started =
                    status.IsOk() || status.Code() != ServerCore::Core::ErrorCode::InvalidArgument;
                return C::Code(status);
            });
    }
    sc_status sc_body_options_init(sc_body_options* options, size_t size) noexcept
    {
        if (!options || size < sizeof(*options))
            return SC_INVALID_ARGUMENT;
        *options = {};
        options->abi_version = SC_ABI_VERSION;
        options->struct_size = sizeof(*options);
        options->max_buffered_bytes = 64 * 1024;
        options->max_body_bytes = 1024 * 1024 * 1024;
        return SC_OK;
    }
    sc_status sc_web_server_set_body_limits(
        sc_web_server* server, const sc_body_options* options) noexcept
    {
        if (!server || !C::Version(options) || !options->max_buffered_bytes ||
            !options->max_body_bytes)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                if (state->started || state->stopped.load())
                    return SC_CLOSED;
                state->options.maxRequestBodyBufferBytes = options->max_buffered_bytes;
                state->options.maxStreamedBodyBytes = options->max_body_bytes;
                return SC_OK;
            });
    }
    sc_status sc_web_server_stream_route(
        sc_web_server* server, sc_bytes method, sc_bytes path, uint32_t pattern) noexcept
    {
        if (!server || !C::Valid(method) || !C::Valid(path) || method.len > 65536 ||
            path.len > 65536 || pattern > 1)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                if (state->stopped.load())
                    return SC_CLOSED;
                W::HttpServer::AsyncHandler handler = [weak = std::weak_ptr(state)](auto context)
                {
                    if (auto owner = weak.lock())
                        owner->Request(std::move(context));
                    else
                        context->response->Abort();
                };
                return C::Code(pattern ? state->server.RegisterStreamingRoutePattern(
                                             C::Text(method), C::Text(path), std::move(handler))
                                       : state->server.RegisterStreamingRoute(
                                             C::Text(method), C::Text(path), std::move(handler)));
            });
    }
    sc_status sc_web_server_enable_policy(sc_web_server* server) noexcept
    {
        if (!server)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                return C::Code(state->server.SetRequestPolicy(state->PolicyHandler()));
            });
    }
    sc_status sc_web_server_websocket_policy(
        sc_web_server* server, sc_bytes path, uint32_t pattern) noexcept
    {
        sc_websocket_route_options options{};
        (void)sc_websocket_route_options_init(&options, sizeof(options));
        options.flags = SC_WS_AUTHORIZE;
        return sc_web_server_websocket_ex(server, path, pattern, &options);
    }
    sc_status sc_web_server_begin_drain(sc_web_server* server) noexcept
    {
        if (!server)
            return SC_INVALID_ARGUMENT;
        return C::Protect([&] { return C::Code(server->state->server.BeginDrain()); });
    }
    sc_status sc_web_server_drain_status(const sc_web_server* server) noexcept
    {
        return !server ? SC_INVALID_ARGUMENT : C::Code(server->state->server.DrainStatus());
    }
    sc_status sc_web_server_stop_gracefully(sc_web_server* server, uint32_t timeout) noexcept
    {
        if (!server)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto state = server->state;
                const auto deadline =
                    timeout == UINT32_MAX
                        ? std::chrono::steady_clock::time_point::max()
                        : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
                {
                    std::lock_guard lock(state->lifecycle);
                    if (state->stopped.load())
                        return SC_OK;
                    const auto begun = state->server.BeginDrain();
                    if (!begun.IsOk())
                        return C::Code(begun);
                }
                // A concurrent immediate Stop must remain able to interrupt this wait.
                // Native lifecycle synchronization serializes the final worker joins.
                const auto status = state->server.StopGracefully(deadline);
                if (status.IsOk() || status.Code() == ServerCore::Core::ErrorCode::Timeout)
                {
                    std::lock_guard lock(state->lifecycle);
                    state->stopped.store(true);
                    state->events.Close();
                    state->stopFinished.store(true);
                }
                return C::Code(status);
            });
    }
    uint16_t sc_web_server_port(const sc_web_server* server) noexcept
    {
        return server ? server->state->server.Port() : 0;
    }
    sc_status sc_web_server_next(
        sc_web_server* server, uint32_t timeout, sc_web_event** out) noexcept
    {
        if (!server)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect([&] { return server->state->events.Next(timeout, out); });
    }
    sc_status sc_web_server_stop(sc_web_server* server) noexcept
    {
        if (!server)
            return SC_INVALID_ARGUMENT;
        return C::Protect([&] { return server->state->Stop(); });
    }
    sc_status sc_web_server_set_logger(sc_web_server* server, const sc_logger* logger) noexcept
    {
        if (!server || !logger)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                auto state = server->state;
                std::lock_guard lock(state->lifecycle);
                return C::Code(state->server.SetLogger(C::LoggerInstance(logger)));
            });
    }
    sc_status sc_web_server_get_metrics(const sc_web_server* server, sc_web_metrics* out) noexcept
    {
        if (!server || !C::OutputVersion(out))
            return SC_INVALID_ARGUMENT;
        const auto value = server->state->server.GetMetrics();
        out->active_connections = value.activeConnections;
        out->active_requests = value.activeRequests;
        out->retained_request_bytes = value.retainedRequestBytes;
        out->retained_send_bytes = value.retainedSendBytes;
        out->pending_trace_events = value.pendingTraceEvents;
        out->accepted_connections = value.acceptedConnections;
        out->closed_connections = value.closedConnections;
        out->rejected_connections = value.rejectedConnections;
        out->accepted_requests = value.acceptedRequests;
        out->completed_requests = value.completedRequests;
        out->failed_requests = value.failedRequests;
        out->cancelled_requests = value.cancelledRequests;
        out->timed_out_requests = value.timedOutRequests;
        out->rejected_requests = value.rejectedRequests;
        out->protocol_errors = value.protocolErrors;
        out->dropped_trace_events = value.droppedTraceEvents;
        out->trace_callback_errors = value.traceCallbackErrors;
        out->total_latency_nanoseconds = value.totalLatencyNanoseconds;
        out->max_latency_nanoseconds = value.maxLatencyNanoseconds;
        std::copy(value.latencyHistogram.buckets.begin(), value.latencyHistogram.buckets.end(),
            out->latency_buckets);
        out->handlers = C::TaskMetrics(value.handlers);
        return SC_OK;
    }
    sc_status sc_web_server_get_observation(
        const sc_web_server* server, sc_observation* out) noexcept
    {
        if (!server || !C::OutputVersion(out))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                const auto state = server->state;
                auto value = ServerCore::Observability::Observe(state->server);
                if (state->stopFinished.load())
                {
                    value.lifecycle = ServerCore::Observability::Lifecycle::Stopped;
                    value.available |= ServerCore::Observability::DrainRemaining;
                }
                else if (state->stopped.load())
                    value.lifecycle = ServerCore::Observability::Lifecycle::Draining;
                // HTTP/policy contexts are already counted by native RequestBudget.
                // Only independent wrapper/view storage, copied WS upgrade metadata,
                // and copied WS message events add new retained bytes here.
                auto additional = state->eventStorage->load(std::memory_order_relaxed);
                {
                    std::lock_guard guard(state->socketBudget->mutex);
                    C::AddObservation(additional, state->socketBudget->bytes);
                }
                C::AddObservation(value.receiveBytes, additional);
                C::AddObservation(value.retainedBytes, additional);
                return C::CopyObservation(value, out);
            });
    }
    sc_status sc_web_server_metrics_prometheus(
        const sc_web_server* server, sc_owned_text** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!server)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto rendered =
                    ServerCore::Observability::RenderPrometheus(server->state->server.GetMetrics());
                if (!rendered.IsOk())
                    return C::Code(rendered.GetStatus());
                return C::MakeOwnedText(std::move(rendered.Value()), out);
            });
    }
    void sc_web_server_destroy(sc_web_server* server) noexcept
    {
        if (server)
        {
            (void)sc_web_server_stop(server);
            delete server;
        }
    }
    uint32_t sc_web_event_kind(const sc_web_event* event) noexcept
    {
        return event ? event->kind : 0;
    }
    sc_status sc_web_event_request(const sc_web_event* event, sc_request_view* view) noexcept
    {
        if (!event || !C::OutputVersion(view))
            return SC_INVALID_ARGUMENT;
        view->method = C::View(event->Request().method);
        view->target = C::View(event->Request().target);
        view->body = C::View(event->Request().body);
        view->headers = event->headers.data();
        view->header_count = event->headers.size();
        view->parameters = event->parameters.data();
        view->parameter_count = event->parameters.size();
        return SC_OK;
    }
    sc_status sc_web_event_endpoints(
        const sc_web_event* event, sc_request_endpoints* endpoints) noexcept
    {
        if (!event || !C::OutputVersion(endpoints))
            return SC_INVALID_ARGUMENT;
        endpoints->local_endpoint = C::FromEndpoint(event->Request().localEndpoint);
        endpoints->remote_endpoint = C::FromEndpoint(event->Request().remoteEndpoint);
        return SC_OK;
    }
    sc_status sc_web_event_response(sc_web_event* event, sc_http_response** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!event || event->kind != SC_WEB_REQUEST)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                std::lock_guard lock(event->mutex);
                auto owner = event->responseOwner.lock();
                if (!owner && event->claimed)
                    return SC_CLOSED;
                if (!owner)
                    owner = std::make_shared<C::ResponseOwner>(event->context);
                auto result = std::make_unique<sc_http_response>();
                result->owner = owner;
                event->responseOwner = owner;
                event->claimed = true;
                *out = result.release();
                return SC_OK;
            });
    }
    sc_status sc_web_event_socket(sc_web_event* event, sc_websocket** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!event || event->kind != SC_WEB_WEBSOCKET)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                std::lock_guard lock(event->mutex);
                auto owner = event->socketOwner.lock();
                if (!owner && event->claimed)
                    return SC_CLOSED;
                if (!owner)
                    owner = std::make_shared<C::SocketOwner>(event->socket);
                auto result = std::make_unique<sc_websocket>();
                result->owner = owner;
                event->socketOwner = owner;
                event->claimed = true;
                *out = result.release();
                return SC_OK;
            });
    }
    sc_status sc_web_event_attributes(const sc_web_event* event, sc_headers_view* view) noexcept
    {
        if (!event || !C::OutputVersion(view))
            return SC_INVALID_ARGUMENT;
        view->headers = event->attributes.data();
        view->count = event->attributes.size();
        return SC_OK;
    }
    sc_status sc_web_event_body(sc_web_event* event, sc_http_body** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!event || event->kind != SC_WEB_REQUEST)
            return SC_INVALID_ARGUMENT;
        if (!event->context->body)
            return SC_NOT_FOUND;
        return C::Protect(
            [&]() -> sc_status
            {
                std::lock_guard lock(event->mutex);
                auto owner = event->bodyOwner.lock();
                if (!owner && event->bodyClaimed)
                    return SC_CLOSED;
                if (!owner)
                    owner = std::make_shared<C::BodyOwner>(event->context->body);
                auto result = std::make_unique<sc_http_body>();
                result->owner = owner;
                event->bodyOwner = owner;
                event->bodyClaimed = true;
                *out = result.release();
                return SC_OK;
            });
    }
    sc_status sc_web_event_decision(sc_web_event* event, sc_request_decision** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!event || event->kind != SC_WEB_POLICY)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                std::lock_guard lock(event->mutex);
                auto owner = event->decisionOwner.lock();
                if (!owner && event->claimed)
                    return SC_CLOSED;
                if (!owner)
                    owner = std::make_shared<C::DecisionOwner>(event->policy->decision);
                auto result = std::make_unique<sc_request_decision>();
                result->owner = owner;
                event->decisionOwner = owner;
                event->claimed = true;
                *out = result.release();
                return SC_OK;
            });
    }
    uint32_t sc_web_event_policy_is_websocket(const sc_web_event* event) noexcept
    {
        return event && event->policy && event->policy->webSocketUpgrade;
    }
    sc_status sc_request_decision_allow(
        sc_request_decision* decision, const sc_policy_allow* options) noexcept
    {
        if (!decision || !C::Version(options))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                sc_response_head descriptor{};
                descriptor.abi_version = SC_ABI_VERSION;
                descriptor.struct_size = sizeof(descriptor);
                descriptor.status = 200;
                descriptor.headers = options->response_headers;
                descriptor.header_count = options->response_header_count;
                W::HttpResponseHead headers, attributes;
                auto status = C::DecodeHead(&descriptor, headers);
                if (status != SC_OK)
                    return status;
                descriptor.headers = options->attributes;
                descriptor.header_count = options->attribute_count;
                status = C::DecodeHead(&descriptor, attributes);
                if (status != SC_OK)
                    return status;
                return C::Code(decision->owner->decision->Allow(
                    std::move(headers.headers), std::move(attributes.headers)));
            });
    }
    sc_status sc_request_decision_reject(
        sc_request_decision* decision, const sc_response_head* head, sc_bytes body) noexcept
    {
        if (!decision || !C::Valid(body))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                if (body.len > ServerCore::Net::SendQueueLimitBytes)
                    return SC_TOO_LARGE;
                W::HttpResponseHead decoded;
                auto status = C::DecodeHead(head, decoded);
                if (status != SC_OK)
                    return status;
                if (decoded.contentLength && *decoded.contentLength != body.len)
                    return SC_INVALID_ARGUMENT;
                return C::Code(decision->owner->decision->Reject({ decoded.status,
                    std::move(decoded.headers), std::string(C::Text(body)), decoded.close }));
            });
    }
    uint32_t sc_request_decision_cancelled(const sc_request_decision* decision) noexcept
    {
        return !decision || decision->owner->decision->GetCancellationToken().stop_requested();
    }
    void sc_request_decision_abort(sc_request_decision* decision) noexcept
    {
        if (decision)
            decision->owner->decision->Abort();
    }
    void sc_request_decision_destroy(sc_request_decision* decision) noexcept
    {
        delete decision;
    }
    sc_status sc_http_body_read(sc_http_body* body, sc_body_chunk** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!body)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto chunk = std::make_unique<sc_body_chunk>();
                auto read = body->owner->body->Read();
                if (!read.IsOk())
                    return C::Code(read.GetStatus());
                if (!read.Value())
                    return SC_OK;
                chunk->value = std::move(read.Value());
                *out = chunk.release();
                return SC_OK;
            });
    }
    size_t sc_http_body_retained_bytes(const sc_http_body* body) noexcept
    {
        return body ? body->owner->body->RetainedBytes() : 0;
    }
    uint32_t sc_http_body_cancelled(const sc_http_body* body) noexcept
    {
        return !body || body->owner->body->GetCancellationToken().stop_requested();
    }
    void sc_http_body_cancel(sc_http_body* body) noexcept
    {
        if (body)
            body->owner->body->Cancel();
    }
    void sc_http_body_destroy(sc_http_body* body) noexcept
    {
        delete body;
    }
    sc_bytes sc_body_chunk_view(const sc_body_chunk* chunk) noexcept
    {
        return chunk ? sc_bytes{ reinterpret_cast<const uint8_t*>(chunk->value->data()),
            chunk->value->size() }
                     : sc_bytes{};
    }
    void sc_body_chunk_destroy(sc_body_chunk* chunk) noexcept
    {
        delete chunk;
    }
    void sc_web_event_destroy(sc_web_event* event) noexcept
    {
        delete event;
    }
    sc_status sc_http_response_retain(
        const sc_http_response* response, sc_http_response** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!response)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                *out = new sc_http_response{ response->owner };
                return SC_OK;
            });
    }
    sc_status sc_http_response_start(
        sc_http_response* response, const sc_response_head* head) noexcept
    {
        if (!response)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                W::HttpResponseHead decoded;
                auto status = C::DecodeHead(head, decoded);
                return status == SC_OK ? C::Code(response->owner->context->response->Start(decoded))
                                       : status;
            });
    }
    sc_status sc_http_response_write(sc_http_response* response, sc_bytes bytes) noexcept
    {
        if (!response || !C::Valid(bytes))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&] { return C::Code(response->owner->context->response->Write(C::Bytes(bytes))); });
    }
    sc_status sc_http_response_finish(sc_http_response* response) noexcept
    {
        if (!response)
            return SC_INVALID_ARGUMENT;
        return C::Protect([&] { return C::Code(response->owner->context->response->Finish()); });
    }
    sc_status sc_http_response_complete(
        sc_http_response* response, const sc_response_head* head, sc_bytes body) noexcept
    {
        if (!response || !C::Valid(body))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                if (body.len > ServerCore::Net::SendQueueLimitBytes)
                    return SC_TOO_LARGE;
                W::HttpResponseHead decoded;
                auto status = C::DecodeHead(head, decoded);
                if (status != SC_OK)
                    return status;
                if (decoded.contentLength && *decoded.contentLength != body.len)
                    return SC_INVALID_ARGUMENT;
                return C::Code(response->owner->context->response->Complete({ decoded.status,
                    std::move(decoded.headers), std::string(C::Text(body)), decoded.close }));
            });
    }
    sc_status sc_http_response_wait_capacity(
        sc_http_response* response, size_t bytes, sc_wait** out) noexcept
    {
        if (!response)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&]
            {
                return C::MakeWait(
                    [&](auto callback)
                    {
                        return response->owner->context->response->WaitForWriteCapacity(
                            bytes, std::move(callback));
                    },
                    out);
            });
    }
    uint32_t sc_http_response_cancelled(const sc_http_response* response) noexcept
    {
        return !response ||
               response->owner->context->response->GetCancellationToken().stop_requested();
    }
    uint32_t sc_http_response_is_head(const sc_http_response* response) noexcept
    {
        return response && response->owner->context->response->IsHeadRequest();
    }
    size_t sc_http_response_max_write(const sc_http_response* response) noexcept
    {
        return response ? response->owner->context->response->MaxWriteBytes() : 0;
    }
    void sc_http_response_abort(sc_http_response* response) noexcept
    {
        if (response)
            response->owner->context->response->Abort();
    }
    void sc_http_response_destroy(sc_http_response* response) noexcept
    {
        delete response;
    }
    sc_status sc_websocket_retain(const sc_websocket* socket, sc_websocket** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!socket)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                *out = new sc_websocket{ socket->owner };
                return SC_OK;
            });
    }
    uint64_t sc_websocket_id(const sc_websocket* socket) noexcept
    {
        return socket ? socket->owner->socket->connection->Id() : 0;
    }
    sc_status sc_websocket_send(sc_websocket* socket, uint32_t kind, sc_bytes bytes) noexcept
    {
        if (!socket || !C::Valid(bytes) || (kind != SC_WS_TEXT && kind != SC_WS_BINARY))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                auto& connection = socket->owner->socket->connection;
                return C::Code(kind == SC_WS_TEXT ? connection->SendText(C::Text(bytes))
                                                  : connection->SendBinary(C::Bytes(bytes)));
            });
    }
    sc_bytes sc_websocket_subprotocol(const sc_websocket* socket) noexcept
    {
        if (!socket)
            return {};
        const auto control = W::GetWebSocketMessageControl(socket->owner->socket->connection);
        return control ? C::View(control->Subprotocol()) : sc_bytes{};
    }
    sc_status sc_websocket_ping(sc_websocket* socket, sc_bytes bytes) noexcept
    {
        if (!socket || !C::Valid(bytes))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&] { return C::Code(socket->owner->socket->connection->Ping(C::Bytes(bytes))); });
    }
    sc_status sc_websocket_begin_message(
        sc_websocket* socket, uint32_t kind, sc_websocket_message** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!socket || (kind != SC_WS_TEXT && kind != SC_WS_BINARY))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                const auto control =
                    W::GetWebSocketMessageControl(socket->owner->socket->connection);
                if (!control)
                    return SC_UNIMPLEMENTED;
                auto result = std::make_unique<sc_websocket_message>();
                result->owner = socket->owner;
                auto writer =
                    control->BeginMessage(kind == SC_WS_TEXT ? W::WebSocketMessageType::Text
                                                             : W::WebSocketMessageType::Binary);
                if (!writer.IsOk())
                    return C::Code(writer.GetStatus());
                result->writer = std::move(writer).Value();
                *out = result.release();
                return SC_OK;
            });
    }
    sc_status sc_websocket_message_write(
        sc_websocket_message* message, sc_bytes bytes, uint32_t final) noexcept
    {
        if (!message || !C::Valid(bytes) || final > 1)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&] { return C::Code(message->writer->Write(C::Bytes(bytes), final != 0)); });
    }
    size_t sc_websocket_message_max_write(const sc_websocket_message* message) noexcept
    {
        return message ? message->writer->MaxWriteBytes() : 0;
    }
    sc_status sc_websocket_message_wait_capacity(
        sc_websocket_message* message, size_t bytes, sc_wait** out) noexcept
    {
        if (!message)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&]
            {
                return C::MakeWait([&](auto callback)
                    { return message->writer->WaitForWriteCapacity(bytes, std::move(callback)); },
                    out);
            });
    }
    void sc_websocket_message_abort(sc_websocket_message* message) noexcept
    {
        if (message)
            message->writer->Abort();
    }
    void sc_websocket_message_destroy(sc_websocket_message* message) noexcept
    {
        delete message;
    }
    sc_status sc_websocket_close(sc_websocket* socket, uint16_t code, sc_bytes reason) noexcept
    {
        if (!socket || !C::Valid(reason))
            return SC_INVALID_ARGUMENT;
        return C::Protect([&]
            { return C::Code(socket->owner->socket->connection->Close(code, C::Text(reason))); });
    }
    sc_status sc_websocket_next(
        sc_websocket* socket, uint32_t timeout, sc_websocket_event** out) noexcept
    {
        if (!socket)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect([&] { return socket->owner->socket->events.Next(timeout, out); });
    }
    sc_status sc_websocket_wait_capacity(sc_websocket* socket, size_t bytes, sc_wait** out) noexcept
    {
        if (!socket)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect(
            [&]
            {
                return C::MakeWait(
                    [&](auto callback)
                    {
                        return W::GetWebSocketFlowControl(socket->owner->socket->connection)
                            ->WaitForSendCapacity(bytes, std::move(callback));
                    },
                    out);
            });
    }
    sc_status sc_websocket_event_get_view(
        const sc_websocket_event* event, sc_websocket_event_view* view) noexcept
    {
        if (!event || !C::OutputVersion(view))
            return SC_INVALID_ARGUMENT;
        view->kind = event->kind;
        view->close_code = event->code;
        view->reserved = 0;
        view->bytes = C::View(event->bytes);
        return SC_OK;
    }
    void sc_websocket_event_destroy(sc_websocket_event* event) noexcept
    {
        delete event;
    }
    void sc_websocket_destroy(sc_websocket* socket) noexcept
    {
        delete socket;
    }
}
