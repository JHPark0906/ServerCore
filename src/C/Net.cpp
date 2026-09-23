#include "ServerCore/C/Net.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/IoContext.h"
#include "C/Internal.h"
#include "C/EndpointInternal.h"
#include "C/ObservationInternal.h"
#include "Observability/MetricsInternal.h"
#include "C/GameExecutionInternal.h"
#include <atomic>
#include <unordered_map>

namespace C = ServerCore::CDetail;
namespace N = ServerCore::Net;
namespace ServerCore::CDetail { struct TcpState; struct TcpPeer; struct TcpOwner; }
struct sc_tcp_connection { std::shared_ptr<C::TcpOwner> owner; };
struct sc_tcp_event {
    // Leases are released after the storage they account for.
    std::shared_ptr<C::Budget::Lease> charge, slot;
    uint32_t kind = SC_TCP_CLOSED;
    sc_status status = SC_OK;
    std::string bytes;
};
struct sc_tcp_server { std::shared_ptr<C::TcpState> state; };

namespace ServerCore::CDetail {
struct TcpPeer final : N::IConnectionObserver {
    std::shared_ptr<Budget::Lease> slot;
    std::weak_ptr<TcpState> owner;
    uint64_t id = 0;
    std::shared_ptr<N::Connection> connection;
    std::shared_ptr<N::ConnectionFlowControl> flow;
    std::shared_ptr<Budget> budget;
    PullQueue<sc_tcp_event> events;
    std::unique_ptr<sc_tcp_event> terminal;
    std::atomic<sc_status> failure{SC_OK};
    void OnBytesReceived(std::span<const std::byte> bytes) override {
        try {
            auto charge = budget->Acquire(sizeof(sc_tcp_event) + bytes.size());
            if (!charge) { failure.store(SC_TOO_LARGE); connection->Close(); return; }
            auto event = std::make_unique<sc_tcp_event>(); event->kind = SC_TCP_BYTES; event->charge = std::move(charge);
            if (!bytes.empty()) event->bytes.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!events.Push(std::move(event))) connection->Close();
        } catch (...) { failure.store(SC_PLATFORM_ERROR); connection->Close(); }
    }
    void OnDisconnected(Core::Status reason) override;
};
struct TcpOwner {
    explicit TcpOwner(std::shared_ptr<TcpPeer> value) : peer(std::move(value)) {}
    ~TcpOwner() { peer->connection->Close(); peer->events.Close(); }
    std::shared_ptr<TcpPeer> peer;
};
TcpConnectionLease RetainTcpConnection(const sc_tcp_connection* value) noexcept
{
    return value ? TcpConnectionLease{value->owner, value->owner->peer->connection} : TcpConnectionLease{};
}
struct TcpState : std::enable_shared_from_this<TcpState> {
    explicit TcpState(const sc_tcp_options& value,bool only) : address(Text(value.listen_address)), options(value),ipv6Only(only),
        budget(std::make_shared<Budget>(value.max_event_count, value.max_event_bytes)),
        slots(std::make_shared<Budget>(value.max_connections, (std::numeric_limits<size_t>::max)())) {}
    void Accepted(std::shared_ptr<N::Connection> connection) noexcept {
        try {
            auto slot = slots->Acquire(1);
            if (!slot || stopping.load()) {
                Rejected(stopping.load() ? Core::ErrorCode::Cancelled : Core::ErrorCode::WouldBlock);
                connection->Close(); return;
            }
            auto peer = std::make_shared<TcpPeer>(); peer->owner = weak_from_this(); peer->connection = connection;
            peer->flow = N::GetConnectionFlowControl(connection); peer->budget = budget; peer->slot = std::move(slot);
            peer->terminal = std::make_unique<sc_tcp_event>(); peer->terminal->slot = peer->slot;
            auto event = std::make_unique<sc_tcp_connection>(); event->owner = std::make_shared<TcpOwner>(peer);
            {
                std::lock_guard lock(mutex);
                if (nextId == 0) { Rejected(Core::ErrorCode::TooLarge); connection->Close(); return; }
                peer->id = nextId++; peers.emplace(peer->id, peer);
            }
            connection->SetObserver(peer);
            (void)accepted.Push(std::move(event));
        } catch (...) { Rejected(Core::ErrorCode::PlatformError); connection->Close(); }
    }
    void Rejected(Core::ErrorCode reason) noexcept {
        Observability::Detail::Add(rejected[static_cast<size_t>(Observability::ClassifyReason(reason))]);
    }
    void Disconnected(uint64_t id, Core::ErrorCode reason) noexcept {
        Observability::Detail::Add(closed[static_cast<size_t>(Observability::ClassifyReason(reason))]);
        { std::lock_guard lock(mutex); peers.erase(id); } changed.notify_all();
    }
    Observability::ObservationSnapshot Observe() {
        using namespace Observability;
        ObservationSnapshot value; value.protocol = Observability::Protocol::Tcp;
        value.lifecycle = stopped.load() ? Lifecycle::Stopped : stopping.load() ? Lifecycle::Draining :
            running.load() ? Lifecycle::Running : Lifecycle::Created;
        value.available = Connections | PendingWork | ReceiveBytes | SendBytes | RetainedBytes | ClosedEvents | RejectedEvents | TimedOutEvents;
        { std::lock_guard guard(mutex); value.connections = peers.size();
          for (const auto& [id, peer] : peers) { (void)id; Detail::Add(value.sendBytes, peer->flow->RetainedSendBytes()); } }
        { std::lock_guard guard(budget->mutex); value.pendingWork = budget->count; value.receiveBytes = budget->bytes; }
        value.retainedBytes = value.receiveBytes; Detail::Add(value.retainedBytes, value.sendBytes);
        for (size_t i = 0; i < EventReasonCount; ++i) { value.closed[i] = closed[i].load(); value.rejected[i] = rejected[i].load(); }
        value.timedOut = value.closed[static_cast<size_t>(EventReason::Timeout)];
        if (value.lifecycle == Lifecycle::Draining || value.lifecycle == Lifecycle::Stopped) {
            value.available |= DrainRemaining; value.drainRemaining = value.connections;
        }
        return value;
    }
    sc_status Start() {
        std::lock_guard lock(lifecycle);
        if (used || stopping.load()) return SC_CLOSED;
        used = true;
        auto status = acceptor.SetSendQueueLimits({options.connection_send_bytes, options.total_send_bytes});
        if (!status.IsOk()) return Code(status);
        acceptor.SetConnectionHandler([weak = weak_from_this()](auto connection) {
            if (auto self = weak.lock()) self->Accepted(std::move(connection)); else connection->Close(); });
        auto endpoint=Core::IpEndpoint::Parse(address,options.port);
        status = endpoint.IsOk() ? acceptor.Listen(endpoint.Value(),64,ipv6Only) : std::move(endpoint).TakeStatus();
        if (!status.IsOk()) return Code(status);
        status = io.Start(static_cast<int>(options.io_threads));
        if (!status.IsOk()) { acceptor.Stop(); return Code(status); }
        status = acceptor.Start(io);
        if (!status.IsOk()) { acceptor.Stop(); io.Stop(); }
        else running.store(true);
        return Code(status);
    }
    sc_status Stop() {
        std::lock_guard lock(lifecycle);
        stopping.store(true); running.store(false); acceptor.Stop(); accepted.Close();
        for (;;) {
            std::shared_ptr<TcpPeer> peer;
            { std::lock_guard peersLock(mutex); if (peers.empty()) break; peer = peers.begin()->second; }
            peer->connection->Close();
            std::unique_lock peersLock(mutex);
            changed.wait(peersLock, [&] { return peers.find(peer->id) == peers.end(); });
        }
        io.Stop(); stopped.store(true); return SC_OK;
    }
    const std::string address;
    const sc_tcp_options options;
    const bool ipv6Only;
    std::mutex lifecycle, mutex;
    std::condition_variable changed;
    bool used = false;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false}, stopped{false};
    std::array<std::atomic<uint64_t>, Observability::EventReasonCount> closed{}, rejected{};
    uint64_t nextId = 1;
    N::IoContext io;
    N::Acceptor acceptor;
    std::shared_ptr<Budget> budget, slots;
    std::unordered_map<uint64_t, std::shared_ptr<TcpPeer>> peers;
    PullQueue<sc_tcp_connection> accepted;
};
void TcpPeer::OnDisconnected(Core::Status reason) {
    const auto outcome = failure.load() != SC_OK ? failure.load() : Code(reason);
    if (terminal) {
        terminal->status = outcome;
        events.Finish(std::move(terminal));
    }
    if (auto state = owner.lock()) state->Disconnected(id, static_cast<Core::ErrorCode>(outcome));
}
}
extern "C" {
sc_status sc_tcp_server_get_observation(const sc_tcp_server* server, sc_observation* out) {
    if (!server || !C::Version(out)) return SC_INVALID_ARGUMENT;
    return C::Protect([&] { return C::CopyObservation(server->state->Observe(), out); });
}
sc_status sc_tcp_options_init(sc_tcp_options* options, size_t size) {
    if (!options || size < sizeof(*options)) return SC_INVALID_ARGUMENT;
    *options = {}; options->abi_version = SC_ABI_VERSION; options->struct_size = sizeof(*options);
    options->listen_address = C::View("127.0.0.1"); options->io_threads = 2; options->max_connections = 256;
    options->max_event_count = 1024; options->max_event_bytes = 16 * 1024 * 1024;
    options->connection_send_bytes = 1024 * 1024; options->total_send_bytes = 256 * 1024 * 1024; return SC_OK;
}
sc_status sc_tcp_server_create(const sc_tcp_options* options, sc_tcp_server** out) {
    return sc_tcp_server_create_ex(options,1,out);
}
sc_status sc_tcp_server_create_ex(const sc_tcp_options* options,uint32_t ipv6Only,sc_tcp_server** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    if (ipv6Only>1 || !C::Version(options) || !C::Valid(options->listen_address) || options->listen_address.len > 64 || options->reserved ||
        !options->io_threads || options->io_threads > 64 || !options->max_connections || options->max_connections > 65536 ||
        !options->max_event_count || options->max_event_count > 65536 || !options->max_event_bytes) return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status { auto result = std::make_unique<sc_tcp_server>();
        result->state = std::make_shared<C::TcpState>(*options,ipv6Only!=0); *out = result.release(); return SC_OK; });
}
sc_status sc_tcp_server_local_endpoint(const sc_tcp_server* server,sc_ip_endpoint* out) {
    if (!server||!out) return SC_INVALID_ARGUMENT;
    const auto endpoint=server->state->acceptor.LocalEndpoint();
    if (!endpoint.IsValid()) return SC_CLOSED;
    *out=C::FromEndpoint(endpoint); return SC_OK;
}
sc_status sc_tcp_connection_endpoints(const sc_tcp_connection* connection,sc_ip_endpoint* local,sc_ip_endpoint* remote) {
    if (!connection||!local||!remote) return SC_INVALID_ARGUMENT;
    const auto& native=connection->owner->peer->connection;
    *local=C::FromEndpoint(native->LocalEndpoint());
    *remote=C::FromEndpoint(native->RemoteEndpoint());
    return SC_OK;
}
sc_status sc_tcp_server_start(sc_tcp_server* server) {
    if (!server) return SC_INVALID_ARGUMENT;
    return C::Protect([&] { return server->state->Start(); });
}
uint16_t sc_tcp_server_port(const sc_tcp_server* server) { return server ? server->state->acceptor.Port() : 0; }
sc_status sc_tcp_server_accept(sc_tcp_server* server, uint32_t timeout, sc_tcp_connection** out) {
    if (!server) { if (out) *out = nullptr; return SC_INVALID_ARGUMENT; }
    return C::Protect([&] { return server->state->accepted.Next(timeout, out); });
}
sc_status sc_tcp_server_stop(sc_tcp_server* server) {
    if (!server) return SC_INVALID_ARGUMENT;
    return C::Protect([&] { return server->state->Stop(); });
}
void sc_tcp_server_destroy(sc_tcp_server* server) { if (server) { (void)sc_tcp_server_stop(server); delete server; } }
sc_status sc_tcp_connection_retain(const sc_tcp_connection* connection, sc_tcp_connection** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    if (!connection) return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status { *out = new sc_tcp_connection{connection->owner}; return SC_OK; });
}
uint64_t sc_tcp_connection_id(const sc_tcp_connection* connection) { return connection ? connection->owner->peer->id : 0; }
sc_status sc_tcp_connection_send(sc_tcp_connection* connection, sc_bytes bytes) {
    if (!connection || !C::Valid(bytes)) return SC_INVALID_ARGUMENT;
    return C::Protect([&] {
        const auto result = connection->owner->peer->connection->Send(C::Bytes(bytes));
        if (!result.IsOk()) if (auto owner = connection->owner->peer->owner.lock()) owner->Rejected(result.Code());
        return C::Code(result);
    });
}
sc_status sc_tcp_connection_next(sc_tcp_connection* connection, uint32_t timeout, sc_tcp_event** out) {
    if (!connection) { if (out) *out = nullptr; return SC_INVALID_ARGUMENT; }
    return C::Protect([&] { return connection->owner->peer->events.Next(timeout, out); });
}
sc_status sc_tcp_connection_pause(sc_tcp_connection* connection) {
    if (!connection) return SC_INVALID_ARGUMENT;
    return C::Protect([&] { return C::Code(connection->owner->peer->flow->PauseReceive()); });
}
sc_status sc_tcp_connection_resume(sc_tcp_connection* connection) {
    if (!connection) return SC_INVALID_ARGUMENT;
    return C::Protect([&] { return C::Code(connection->owner->peer->flow->ResumeReceive()); });
}
sc_status sc_tcp_connection_wait_capacity(sc_tcp_connection* connection, size_t bytes, sc_wait** out) {
    if (!connection) { if (out) *out = nullptr; return SC_INVALID_ARGUMENT; }
    return C::Protect([&] { return C::MakeWait([&](auto callback) {
        return connection->owner->peer->flow->WaitForSendCapacity(bytes, std::move(callback)); }, out); });
}
void sc_tcp_connection_close(sc_tcp_connection* connection) { if (connection) connection->owner->peer->connection->Close(); }
sc_status sc_tcp_event_get_view(const sc_tcp_event* event, sc_tcp_event_view* view) {
    if (!event || !C::Version(view)) return SC_INVALID_ARGUMENT;
    view->kind = event->kind; view->status = event->status; view->bytes = C::View(event->bytes); return SC_OK;
}
void sc_tcp_event_destroy(sc_tcp_event* event) { delete event; }
void sc_tcp_connection_destroy(sc_tcp_connection* connection) { delete connection; }
sc_status sc_tcp_server_subscribe(sc_tcp_server* server, sc_notifier* notifier, uint64_t key, sc_subscription** out) {
    if (!server) { if (out) *out = nullptr; return SC_INVALID_ARGUMENT; }
    return C::Protect([&] { return server->state->accepted.Subscribe(notifier, key, out); });
}
sc_status sc_tcp_connection_subscribe(sc_tcp_connection* connection, sc_notifier* notifier, uint64_t key, sc_subscription** out) {
    if (!connection) { if (out) *out = nullptr; return SC_INVALID_ARGUMENT; }
    return C::Protect([&] { return connection->owner->peer->events.Subscribe(notifier, key, out); });
}
}
