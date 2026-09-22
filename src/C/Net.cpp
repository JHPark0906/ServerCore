#include "ServerCore/C/Net.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/IoContext.h"
#include "C/Internal.h"
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
struct TcpState : std::enable_shared_from_this<TcpState> {
    explicit TcpState(const sc_tcp_options& value) : address(Text(value.listen_address)), options(value),
        budget(std::make_shared<Budget>(value.max_event_count, value.max_event_bytes)),
        slots(std::make_shared<Budget>(value.max_connections, (std::numeric_limits<size_t>::max)())) {}
    void Accepted(std::shared_ptr<N::Connection> connection) noexcept {
        try {
            auto slot = slots->Acquire(1);
            if (!slot || stopping.load()) { connection->Close(); return; }
            auto peer = std::make_shared<TcpPeer>(); peer->owner = weak_from_this(); peer->connection = connection;
            peer->flow = N::GetConnectionFlowControl(connection); peer->budget = budget; peer->slot = std::move(slot);
            peer->terminal = std::make_unique<sc_tcp_event>(); peer->terminal->slot = peer->slot;
            auto event = std::make_unique<sc_tcp_connection>(); event->owner = std::make_shared<TcpOwner>(peer);
            {
                std::lock_guard lock(mutex);
                if (nextId == 0) { connection->Close(); return; }
                peer->id = nextId++; peers.emplace(peer->id, peer);
            }
            connection->SetObserver(peer);
            (void)accepted.Push(std::move(event));
        } catch (...) { connection->Close(); }
    }
    void Disconnected(uint64_t id) noexcept {
        { std::lock_guard lock(mutex); peers.erase(id); } changed.notify_all();
    }
    sc_status Start() {
        std::lock_guard lock(lifecycle);
        if (used || stopping.load()) return SC_CLOSED;
        used = true;
        auto status = acceptor.SetSendQueueLimits({options.connection_send_bytes, options.total_send_bytes});
        if (!status.IsOk()) return Code(status);
        acceptor.SetConnectionHandler([weak = weak_from_this()](auto connection) {
            if (auto self = weak.lock()) self->Accepted(std::move(connection)); else connection->Close(); });
        status = acceptor.Listen(address, options.port, 64);
        if (!status.IsOk()) return Code(status);
        status = io.Start(static_cast<int>(options.io_threads));
        if (!status.IsOk()) { acceptor.Stop(); return Code(status); }
        status = acceptor.Start(io);
        if (!status.IsOk()) { acceptor.Stop(); io.Stop(); }
        return Code(status);
    }
    sc_status Stop() {
        std::lock_guard lock(lifecycle);
        stopping.store(true); acceptor.Stop(); accepted.Close();
        for (;;) {
            std::shared_ptr<TcpPeer> peer;
            { std::lock_guard peersLock(mutex); if (peers.empty()) break; peer = peers.begin()->second; }
            peer->connection->Close();
            std::unique_lock peersLock(mutex);
            changed.wait(peersLock, [&] { return peers.find(peer->id) == peers.end(); });
        }
        io.Stop(); return SC_OK;
    }
    const std::string address;
    const sc_tcp_options options;
    std::mutex lifecycle, mutex;
    std::condition_variable changed;
    bool used = false;
    std::atomic<bool> stopping{false};
    uint64_t nextId = 1;
    N::IoContext io;
    N::Acceptor acceptor;
    std::shared_ptr<Budget> budget, slots;
    std::unordered_map<uint64_t, std::shared_ptr<TcpPeer>> peers;
    PullQueue<sc_tcp_connection> accepted;
};
void TcpPeer::OnDisconnected(Core::Status reason) {
    if (terminal) {
        terminal->status = failure.load() != SC_OK ? failure.load() : Code(reason);
        events.Finish(std::move(terminal));
    }
    if (auto state = owner.lock()) state->Disconnected(id);
}
}
extern "C" {
sc_status sc_tcp_options_init(sc_tcp_options* options, size_t size) {
    if (!options || size < sizeof(*options)) return SC_INVALID_ARGUMENT;
    *options = {}; options->abi_version = SC_ABI_VERSION; options->struct_size = sizeof(*options);
    options->listen_address = C::View("127.0.0.1"); options->io_threads = 2; options->max_connections = 256;
    options->max_event_count = 1024; options->max_event_bytes = 16 * 1024 * 1024;
    options->connection_send_bytes = 1024 * 1024; options->total_send_bytes = 256 * 1024 * 1024; return SC_OK;
}
sc_status sc_tcp_server_create(const sc_tcp_options* options, sc_tcp_server** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    if (!C::Version(options) || !C::Valid(options->listen_address) || options->listen_address.len > 64 || options->reserved ||
        !options->io_threads || options->io_threads > 64 || !options->max_connections || options->max_connections > 65536 ||
        !options->max_event_count || options->max_event_count > 65536 || !options->max_event_bytes) return SC_INVALID_ARGUMENT;
    return C::Protect([&]() -> sc_status { auto result = std::make_unique<sc_tcp_server>();
        result->state = std::make_shared<C::TcpState>(*options); *out = result.release(); return SC_OK; });
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
    return C::Protect([&] { return C::Code(connection->owner->peer->connection->Send(C::Bytes(bytes))); });
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
}
