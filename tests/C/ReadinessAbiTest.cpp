#include "AbiSocketTestSupport.h"
#include "ServerCore/C/Net.h"
#include "ServerCore/C/Web.h"
#include <atomic>
#include <thread>

namespace {
using namespace AbiTest;
using ServerCoreTest::ExpectTrue;
using Notifier = Handle<sc_notifier, sc_notifier_destroy>;
using Subscription = Handle<sc_subscription, sc_subscription_destroy>;
using Tcp = Handle<sc_tcp_server, sc_tcp_server_destroy>;
using Connection = Handle<sc_tcp_connection, sc_tcp_connection_destroy>;
using TcpEvent = Handle<sc_tcp_event, sc_tcp_event_destroy>;
using Web = Handle<sc_web_server, sc_web_server_destroy>;
using WebEvent = Handle<sc_web_event, sc_web_event_destroy>;
using Body = Handle<sc_http_body, sc_http_body_destroy>;
using Chunk = Handle<sc_body_chunk, sc_body_chunk_destroy>;
using Response = Handle<sc_http_response, sc_http_response_destroy>;
using Wait = Handle<sc_wait, sc_wait_destroy>;
Notifier Mailbox(size_t capacity) {
    sc_notifier* value = nullptr;
    Check(sc_notifier_create(capacity, &value) == SC_OK && value, "readiness mailbox creates");
    return Notifier(value, sc_notifier_destroy);
}
Tcp TcpServer(bool start = false) {
    sc_tcp_options options{};
    (void)sc_tcp_options_init(&options, sizeof(options)); options.port = FreePort();
    sc_tcp_server* value = nullptr;
    if (!Check(sc_tcp_server_create(&options, &value) == SC_OK, "readiness TCP owner creates"))
        return Tcp(nullptr, sc_tcp_server_destroy);
    Tcp server(value, sc_tcp_server_destroy);
    if (start) Check(sc_tcp_server_start(value) == SC_OK, "readiness TCP owner starts");
    return server;
}
uint64_t Notified(sc_notifier* value) {
    uint64_t key = 0;
    Check(sc_notifier_next(value, 5000, &key) == SC_OK, "native readiness arrives without polling");
    return key;
}
void ReadinessLifetimeAndRaces() {
    ServerCoreTest::SocketRuntime sockets;
    if (!Check(sockets.IsReady(), "readiness socket runtime starts")) return;
    auto mailbox = Mailbox(2); auto server = TcpServer();
    if (!mailbox || !server) return;
    sc_subscription *first = nullptr, *second = nullptr, *extra = nullptr;
    ExpectTrue(sc_tcp_server_subscribe(server.get(), mailbox.get(), 1, &first) == SC_OK,
        "first persistent source registers");
    Subscription one(first, sc_subscription_destroy);
    ExpectTrue(sc_tcp_server_subscribe(server.get(), mailbox.get(), 1, &extra) == SC_ALREADY_EXISTS && !extra,
        "live notification keys are unique");
    ExpectTrue(sc_tcp_server_subscribe(server.get(), mailbox.get(), 2, &second) == SC_OK,
        "second independent waiter registers");
    Subscription two(second, sc_subscription_destroy);
    ExpectTrue(sc_tcp_server_subscribe(server.get(), mailbox.get(), 3, &extra) == SC_WOULD_BLOCK && !extra,
        "native registration capacity is bounded");
    uint64_t key = 99;
    ExpectTrue(sc_notifier_next(mailbox.get(), 10, &key) == SC_TIMEOUT && key == 0,
        "idle native source produces no periodic readiness");
    ExpectTrue(sc_tcp_server_stop(server.get()) == SC_OK, "closing event queue wakes registered keys");
    one.reset();
    ExpectTrue(Notified(mailbox.get()) == 2, "dropping a registration removes its queued key");
    ExpectTrue(sc_notifier_next(mailbox.get(), 0, &key) == SC_WOULD_BLOCK,
        "repeated source readiness is coalesced");
    ExpectTrue(sc_tcp_server_subscribe(server.get(), mailbox.get(), 3, &extra) == SC_OK,
        "released registration capacity is immediately reusable");
    Subscription three(extra, sc_subscription_destroy);
    ExpectTrue(Notified(mailbox.get()) == 3, "registering an already closed source rechecks readiness");
    server.reset();
    two.reset(); three.reset();
    sc_status blocked = SC_PLATFORM_ERROR;
    std::thread waiter([&] { uint64_t ignored = 0; blocked = sc_notifier_next(mailbox.get(), UINT32_MAX, &ignored); });
    sc_notifier_close(mailbox.get()); waiter.join();
    ExpectTrue(blocked == SC_CLOSED, "mailbox close releases a blocking bridge wait");
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        auto raceMailbox = Mailbox(1); auto raceServer = TcpServer();
        if (!raceMailbox || !raceServer) return;
        std::atomic<bool> start{false};
        std::thread closer([&] { while (!start.load()) std::this_thread::yield(); (void)sc_tcp_server_stop(raceServer.get()); });
        start.store(true);
        sc_subscription* raw = nullptr;
        const auto status = sc_tcp_server_subscribe(raceServer.get(), raceMailbox.get(), 7, &raw);
        Subscription registration(raw, sc_subscription_destroy);
        closer.join();
        ExpectTrue(status == SC_OK && Notified(raceMailbox.get()) == 7,
            "register-versus-close race never loses readiness");
        raceMailbox.reset(); // Subscription teardown must tolerate its mailbox gone.
        registration.reset();
    }
}
void ReadinessWebAndTcp() {
    ServerCoreTest::SocketRuntime sockets;
    if (!Check(sockets.IsReady(), "readiness integration socket runtime starts")) return;
    auto mailbox = Mailbox(8); if (!mailbox) return;
    sc_web_options options{}; (void)sc_web_options_init(&options, sizeof(options)); options.port = FreePort();
    sc_web_server* rawWeb = nullptr;
    if (!Check(sc_web_server_create(&options, &rawWeb) == SC_OK, "readiness web owner creates")) return;
    Web web(rawWeb, sc_web_server_destroy);
    if (!Check(sc_web_server_stream_route(web.get(), Bytes("POST"), Bytes("/upload"), 0) == SC_OK &&
        sc_web_server_start(web.get()) == SC_OK, "readiness streaming route starts")) return;
    sc_subscription* raw = nullptr;
    ExpectTrue(sc_web_server_subscribe(web.get(), mailbox.get(), 10, &raw) == SC_OK, "web event readiness registers");
    Subscription requestReady(raw, sc_subscription_destroy);
    Peer http;
    if (!http.Connect(sc_web_server_port(web.get())) ||
        !http.Send("POST /upload HTTP/1.1\r\nHost: test\r\nContent-Length: 3\r\n\r\n")) return;
    ExpectTrue(Notified(mailbox.get()) == 10, "request headers notify the event consumer");
    sc_web_event* rawEvent = nullptr;
    if (!Check(sc_web_server_next(web.get(), 0, &rawEvent) == SC_OK, "notified request is immediately available")) return;
    WebEvent event(rawEvent, sc_web_event_destroy); requestReady.reset();
    sc_http_body* rawBody = nullptr; sc_http_response* rawResponse = nullptr;
    if (!Check(sc_web_event_body(event.get(), &rawBody) == SC_OK &&
        sc_web_event_response(event.get(), &rawResponse) == SC_OK, "request yields independent body and response owners")) return;
    Body body(rawBody, sc_http_body_destroy); Response response(rawResponse, sc_http_response_destroy);
    raw = nullptr;
    ExpectTrue(sc_http_body_subscribe(body.get(), mailbox.get(), 11, &raw) == SC_OK, "pending upload readiness registers");
    Subscription bodyReady(raw, sc_subscription_destroy);
    sc_body_chunk* rawChunk = nullptr;
    ExpectTrue(sc_http_body_read(body.get(), &rawChunk) == SC_WOULD_BLOCK, "header-only upload is pending");
    if (!http.Send("abc")) return;
    ExpectTrue(Notified(mailbox.get()) == 11, "body arrival notifies its pending reader");
    if (!Check(sc_http_body_read(body.get(), &rawChunk) == SC_OK && rawChunk, "notified body returns an owned chunk")) return;
    Chunk chunk(rawChunk, sc_body_chunk_destroy);
    ExpectTrue(Text(sc_body_chunk_view(chunk.get())) == "abc", "notified body preserves its bytes");
    bodyReady.reset(); raw = nullptr;
    ExpectTrue(sc_http_body_subscribe(body.get(), mailbox.get(), 12, &raw) == SC_OK, "clean EOF readiness rearms");
    Subscription eofReady(raw, sc_subscription_destroy);
    ExpectTrue(Notified(mailbox.get()) == 12, "clean EOF is observable without waiting for another byte");
    rawChunk = nullptr;
    ExpectTrue(sc_http_body_read(body.get(), &rawChunk) == SC_OK && !rawChunk, "notified EOF is clean");
    eofReady.reset(); raw = nullptr;
    ExpectTrue(sc_http_response_subscribe_cancelled(response.get(), mailbox.get(), 13, &raw) == SC_OK,
        "response cancellation readiness registers");
    Subscription cancelled(raw, sc_subscription_destroy);
    ExpectTrue(sc_web_server_begin_drain(web.get()) == SC_OK, "drain begins while response remains active");
    raw = nullptr;
    ExpectTrue(sc_web_server_subscribe_drain(web.get(), mailbox.get(), 14, &raw) == SC_OK, "pending drain readiness registers");
    Subscription drained(raw, sc_subscription_destroy);
    sc_http_response_abort(response.get());
    const auto left = Notified(mailbox.get()), right = Notified(mailbox.get());
    ExpectTrue((left == 13 && right == 14) || (left == 14 && right == 13),
        "abort and final peer retirement each deliver their own readiness");
    ExpectTrue(sc_web_server_drain_status(web.get()) == SC_OK, "drain signal reflects terminal peer state");
    cancelled.reset(); drained.reset(); web.reset();

    auto tcp = TcpServer(true); if (!tcp) return;
    raw = nullptr;
    ExpectTrue(sc_tcp_server_subscribe(tcp.get(), mailbox.get(), 20, &raw) == SC_OK, "TCP accept readiness registers");
    Subscription accepted(raw, sc_subscription_destroy);
    Peer peer; if (!peer.Connect(sc_tcp_server_port(tcp.get()))) return;
    ExpectTrue(Notified(mailbox.get()) == 20, "incoming TCP connection notifies accept");
    sc_tcp_connection* rawConnection = nullptr;
    if (!Check(sc_tcp_server_accept(tcp.get(), 0, &rawConnection) == SC_OK, "notified accept is nonblocking")) return;
    Connection connection(rawConnection, sc_tcp_connection_destroy); accepted.reset(); raw = nullptr;
    ExpectTrue(sc_tcp_connection_subscribe(connection.get(), mailbox.get(), 21, &raw) == SC_OK, "TCP receive readiness registers");
    Subscription received(raw, sc_subscription_destroy);
    if (!peer.Send("data")) return;
    ExpectTrue(Notified(mailbox.get()) == 21, "TCP byte arrival notifies receiver");
    sc_tcp_event* rawTcpEvent = nullptr;
    if (!Check(sc_tcp_connection_next(connection.get(), 0, &rawTcpEvent) == SC_OK, "notified TCP bytes are available")) return;
    TcpEvent tcpEvent(rawTcpEvent, sc_tcp_event_destroy);
    sc_wait* rawWait = nullptr;
    if (!Check(sc_tcp_connection_wait_capacity(connection.get(), 1, &rawWait) == SC_OK, "TCP capacity wait creates")) return;
    Wait capacity(rawWait, sc_wait_destroy); raw = nullptr;
    ExpectTrue(sc_wait_subscribe(capacity.get(), mailbox.get(), 22, &raw) == SC_OK, "completed capacity readiness registers");
    Subscription writable(raw, sc_subscription_destroy);
    ExpectTrue(Notified(mailbox.get()) == 22 && sc_wait_result(capacity.get()) == SC_OK,
        "send capacity uses the same native notification bridge");
    writable.reset(); peer.Close();
    ExpectTrue(Notified(mailbox.get()) == 21, "TCP terminal event notifies the existing receive subscription");
}
const ServerCoreTest::CheckRegistration lifetime("CAbi.ReadinessLifetimeAndRaces", ReadinessLifetimeAndRaces);
const ServerCoreTest::CheckRegistration integration("CAbi.ReadinessWebAndTcp", ReadinessWebAndTcp);
}
