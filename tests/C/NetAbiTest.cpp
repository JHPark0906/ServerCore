#include "AbiSocketTestSupport.h"
#include "ServerCore/C/Net.h"

namespace
{
using namespace AbiTest;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using Server = Handle<sc_tcp_server, sc_tcp_server_destroy>;
using Connection = Handle<sc_tcp_connection, sc_tcp_connection_destroy>;
using Event = Handle<sc_tcp_event, sc_tcp_event_destroy>;
using Wait = Handle<sc_wait, sc_wait_destroy>;
Server Start(std::size_t eventCount = 64)
{
    sc_tcp_options options{};
    ExpectTrue(sc_tcp_options_init(&options, sizeof(options)) == SC_OK, "C TCP options initialize");
    options.port = FreePort(); options.max_event_count = eventCount;
    std::string address = "127.0.0.1";
    options.listen_address = Bytes(address);
    sc_tcp_server* output = nullptr;
    auto invalid = options; invalid.abi_version = SC_ABI_VERSION + 1;
    ExpectTrue(sc_tcp_server_create(&invalid, &output) == SC_INVALID_ARGUMENT && !output,
        "C TCP create rejects incompatible descriptor versions");
    if (!Check(sc_tcp_server_create(&options, &output) == SC_OK && output, "C TCP server is created"))
        return Server(nullptr, sc_tcp_server_destroy);
    Server server(output, sc_tcp_server_destroy);
    address.assign(address.size(), 'x');
    if (!Check(sc_tcp_server_start(server.get()) == SC_OK, "TCP server copied its borrowed listen address"))
        return Server(nullptr, sc_tcp_server_destroy);
    return server;
}
Connection Accept(sc_tcp_server* server)
{
    sc_tcp_connection* output = nullptr;
    Check(sc_tcp_server_accept(server, 5000, &output) == SC_OK && output, "C TCP accepts an owning connection handle");
    return Connection(output, sc_tcp_connection_destroy);
}
Event Next(sc_tcp_connection* connection)
{
    sc_tcp_event* output = nullptr;
    Check(sc_tcp_connection_next(connection, 5000, &output) == SC_OK && output, "C TCP delivers an owning event");
    return Event(output, sc_tcp_event_destroy);
}
sc_tcp_event_view EventView(const sc_tcp_event* event)
{
    auto result = View<sc_tcp_event_view>();
    ExpectTrue(sc_tcp_event_get_view(event, &result) == SC_OK, "C TCP event exposes a versioned borrowed view");
    return result;
}
void TcpOwnershipAndTerminal()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "C TCP socket runtime starts")) return;
    ExpectTrue(sc_abi_version() == SC_ABI_VERSION && (sc_capabilities() & SC_CAP_TCP) != 0,
        "C TCP ABI advertises its version and capability");
    auto server = Start(); if (!server) return;
    sc_tcp_connection* noConnection = nullptr;
    ExpectTrue(sc_tcp_server_accept(server.get(), 0, &noConnection) == SC_WOULD_BLOCK && !noConnection,
        "empty C TCP accept is nonblocking");
    Peer peer;
    if (!peer.Connect(sc_tcp_server_port(server.get()))) return;
    auto connection = Accept(server.get()); if (!connection) return;
    sc_tcp_connection* duplicate = nullptr;
    if (!Check(sc_tcp_connection_retain(connection.get(), &duplicate) == SC_OK && duplicate,
        "C TCP connection can be independently retained")) return;
    Connection retained(duplicate, sc_tcp_connection_destroy);
    const auto id = sc_tcp_connection_id(connection.get());
    ExpectTrue(id != 0 && id == sc_tcp_connection_id(retained.get()), "retained TCP handle preserves identity");
    connection.reset();
    ExpectTrue(sc_tcp_connection_pause(retained.get()) == SC_OK && sc_tcp_connection_resume(retained.get()) == SC_OK,
        "retained TCP handle exposes receive flow control");
    ExpectTrue(sc_tcp_connection_send(retained.get(), {nullptr, 1}) == SC_INVALID_ARGUMENT,
        "C TCP rejects invalid nonempty null slices before reading them");
    const std::string payload("a\0b\xffz", 5);
    if (!peer.Send(payload)) return;
    Event held(nullptr, sc_tcp_event_destroy);
    std::string received;
    std::string heldBytes;
    while (received.size() < payload.size())
    {
        auto event = Next(retained.get()); if (!event) return;
        const auto view = EventView(event.get());
        if (!Check(view.kind == SC_TCP_BYTES && view.bytes.len != 0, "C TCP delivers raw bytes without framing")) return;
        received += Text(view.bytes);
        if (!held) { heldBytes = Text(view.bytes); held = std::move(event); }
    }
    ExpectEqual(payload, received, "raw TCP events preserve binary data across arbitrary receive segmentation");
    sc_wait* rawWait = nullptr;
    ExpectTrue(sc_tcp_connection_wait_capacity(retained.get(), payload.size(), &rawWait) == SC_OK && rawWait,
        "C TCP exposes an owning capacity wait handle");
    Wait wait(rawWait, sc_wait_destroy);
    if (wait) ExpectTrue(sc_wait_wait(wait.get(), 5000) == SC_OK, "C TCP capacity wait completes without polling bytes");
    auto echo = payload;
    ExpectTrue(sc_tcp_connection_send(retained.get(), Bytes(echo)) == SC_OK, "retained TCP connection remains usable after original handle destruction");
    echo.assign(echo.size(), 'x');
    ExpectEqual(payload, peer.Read(payload.size()), "C TCP send copies borrowed input before returning");
    server.reset();
    auto terminal = Next(retained.get()); if (!terminal) return;
    const auto closed = EventView(terminal.get());
    ExpectTrue(closed.kind == SC_TCP_CLOSED && closed.status == SC_CLOSED, "server destruction yields a terminal event on surviving connection handles");
    sc_tcp_event* none = nullptr;
    ExpectTrue(sc_tcp_connection_next(retained.get(), 0, &none) == SC_CLOSED && !none, "TCP terminal event is delivered exactly once");
    ExpectTrue(sc_tcp_connection_send(retained.get(), Bytes("late")) == SC_CLOSED, "surviving TCP handle rejects late sends");
    retained.reset();
    ExpectEqual(heldBytes, Text(EventView(held.get()).bytes), "data event remains readable after both server and connection handles are destroyed");
}
void TcpRetainedEventOverflow()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "TCP overflow socket runtime starts")) return;
    auto server = Start(1); if (!server) return;
    Peer peer;
    if (!peer.Connect(sc_tcp_server_port(server.get()))) return;
    auto connection = Accept(server.get()); if (!connection) return;
    if (!peer.Send("a")) return;
    auto held = Next(connection.get()); if (!held) return;
    const auto first = EventView(held.get());
    ExpectTrue(first.kind == SC_TCP_BYTES && Text(first.bytes) == "a", "single-byte payload occupies the retained event slot");
    if (!peer.Send("b")) return;
    auto terminal = Next(connection.get()); if (!terminal) return;
    const auto view = EventView(terminal.get());
    ExpectTrue(view.kind == SC_TCP_CLOSED && view.status == SC_TOO_LARGE,
        "popped but retained event prevents unbounded receive admission and reserves a terminal event");
    sc_tcp_event* none = nullptr;
    ExpectTrue(sc_tcp_connection_next(connection.get(), 0, &none) == SC_CLOSED && !none,
        "overflow cannot publish duplicate terminal events");
    server.reset(); connection.reset();
    ExpectEqual(std::string("a"), Text(EventView(held.get()).bytes), "overflow and shutdown preserve already delivered event ownership");
}
const ServerCoreTest::CheckRegistration ownership("CAbi.TcpOwnershipAndTerminal", TcpOwnershipAndTerminal);
const ServerCoreTest::CheckRegistration overflow("CAbi.TcpRetainedEventOverflow", TcpRetainedEventOverflow);
}
