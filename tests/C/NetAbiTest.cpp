#include "AbiSocketTestSupport.h"
#include "ServerCore/C/Net.h"
#include "ServerCore/C/Observability.h"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>
#include <thread>

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
    options.port = FreePort();
    options.max_event_count = eventCount;
    std::string address = "127.0.0.1";
    options.listen_address = Bytes(address);
    sc_tcp_server* output = nullptr;
    auto invalid = options;
    invalid.abi_version = SC_ABI_VERSION + 1;
    ExpectTrue(sc_tcp_server_create(&invalid, &output) == SC_INVALID_ARGUMENT && !output,
        "C TCP create rejects incompatible descriptor versions");
    if (!Check(
            sc_tcp_server_create(&options, &output) == SC_OK && output, "C TCP server is created"))
        return Server(nullptr, sc_tcp_server_destroy);
    Server server(output, sc_tcp_server_destroy);
    address.assign(address.size(), 'x');
    if (!Check(sc_tcp_server_start(server.get()) == SC_OK,
            "TCP server copied its borrowed listen address"))
        return Server(nullptr, sc_tcp_server_destroy);
    return server;
}
Connection Accept(sc_tcp_server* server)
{
    sc_tcp_connection* output = nullptr;
    Check(sc_tcp_server_accept(server, 5000, &output) == SC_OK && output,
        "C TCP accepts an owning connection handle");
    return Connection(output, sc_tcp_connection_destroy);
}
Event Next(sc_tcp_connection* connection)
{
    sc_tcp_event* output = nullptr;
    Check(sc_tcp_connection_next(connection, 5000, &output) == SC_OK && output,
        "C TCP delivers an owning event");
    return Event(output, sc_tcp_event_destroy);
}
sc_tcp_event_view EventView(const sc_tcp_event* event)
{
    auto result = View<sc_tcp_event_view>();
    ExpectTrue(sc_tcp_event_get_view(event, &result) == SC_OK,
        "C TCP event exposes a versioned borrowed view");
    return result;
}
/// <summary>
/// 입력 서술자의 struct_size 계약과 기본 포트 계약을 본다(CABI-5). 이 라이브러리보다 큰 입력
/// 서술자는 모르는 뒤쪽이 모두 0일 때만 받고, 크기 상한을 넘으면 뒤쪽을 읽기 전에 거절한다.
/// 출력 구조체는 뒤쪽을 보지 않는다. 옵션 초기화의 기본 포트 0은 Start에서 거절된다.
/// </summary>
void DescriptorContract()
{
    struct ExtendedOptions
    {
        sc_tcp_options options;
        unsigned char tail[8];
    };
    ExtendedOptions extended{};
    ExpectTrue(sc_tcp_options_init(&extended.options, sizeof(extended.options)) == SC_OK,
        "extended TCP options initialize");
    extended.options.struct_size = static_cast<uint32_t>(sizeof(extended));
    sc_tcp_server* raw = nullptr;
    // ABI 2에서 구조체 배치가 바뀌었으므로 ABI 1이라고 적힌 서술자는 받지 않는다.
    auto previous = extended.options;
    previous.abi_version = 1;
    ExpectTrue(sc_abi_version() == 2 &&
                   sc_tcp_server_create(&previous, &raw) == SC_INVALID_ARGUMENT && !raw,
        "an ABI 1 descriptor is rejected by ABI 2");
    if (!Check(sc_tcp_server_create(&extended.options, &raw) == SC_OK && raw,
            "a larger input descriptor with a zero tail is accepted"))
        return;
    Server server(raw, sc_tcp_server_destroy);
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT }, sc_tcp_server_start(server.get()),
        "the default port 0 is rejected at Start");
    struct ExtendedObservation
    {
        sc_observation value;
        unsigned char tail[8];
    };
    ExtendedObservation observation{};
    (void)sc_observation_init(&observation.value, sizeof(observation.value));
    observation.value.struct_size = static_cast<uint32_t>(sizeof(observation));
    std::fill(
        std::begin(observation.tail), std::end(observation.tail), static_cast<unsigned char>(0xAB));
    ExpectEqual(sc_status{ SC_OK }, sc_tcp_server_get_observation(server.get(), &observation.value),
        "a larger output structure ignores its tail");
    extended.tail[3] = 1;
    raw = nullptr;
    ExpectTrue(sc_tcp_server_create(&extended.options, &raw) == SC_INVALID_ARGUMENT && !raw,
        "a larger input descriptor with unknown nonzero fields is rejected");
    sc_tcp_server_destroy(raw);
    extended.tail[3] = 0;
    extended.options.struct_size = 1u << 20;
    raw = nullptr;
    ExpectTrue(sc_tcp_server_create(&extended.options, &raw) == SC_INVALID_ARGUMENT && !raw,
        "an input descriptor beyond the size limit is rejected");
    sc_tcp_server_destroy(raw);
}
void TcpOwnershipAndTerminal()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "C TCP socket runtime starts"))
        return;
    DescriptorContract();
    ExpectTrue(sc_abi_version() == SC_ABI_VERSION && (sc_capabilities() & SC_CAP_TCP) != 0,
        "C TCP ABI advertises its version and capability");
    auto server = Start();
    if (!server)
        return;
    sc_tcp_connection* noConnection = nullptr;
    ExpectTrue(
        sc_tcp_server_accept(server.get(), 0, &noConnection) == SC_WOULD_BLOCK && !noConnection,
        "empty C TCP accept is nonblocking");
    Peer peer;
    if (!peer.Connect(sc_tcp_server_port(server.get())))
        return;
    auto connection = Accept(server.get());
    if (!connection)
        return;
    sc_tcp_connection* duplicate = nullptr;
    if (!Check(sc_tcp_connection_retain(connection.get(), &duplicate) == SC_OK && duplicate,
            "C TCP connection can be independently retained"))
        return;
    Connection retained(duplicate, sc_tcp_connection_destroy);
    const auto id = sc_tcp_connection_id(connection.get());
    ExpectTrue(id != 0 && id == sc_tcp_connection_id(retained.get()),
        "retained TCP handle preserves identity");
    connection.reset();
    ExpectTrue(sc_tcp_connection_pause(retained.get()) == SC_OK &&
                   sc_tcp_connection_resume(retained.get()) == SC_OK,
        "retained TCP handle exposes receive flow control");
    ExpectTrue(sc_tcp_connection_send(retained.get(), { nullptr, 1 }) == SC_INVALID_ARGUMENT,
        "C TCP rejects invalid nonempty null slices before reading them");
    const std::string payload("a\0b\xffz", 5);
    if (!peer.Send(payload))
        return;
    Event held(nullptr, sc_tcp_event_destroy);
    std::string received;
    std::string heldBytes;
    while (received.size() < payload.size())
    {
        auto event = Next(retained.get());
        if (!event)
            return;
        const auto view = EventView(event.get());
        if (!Check(view.kind == SC_TCP_BYTES && view.bytes.len != 0,
                "C TCP delivers raw bytes without framing"))
            return;
        received += Text(view.bytes);
        if (!held)
        {
            heldBytes = Text(view.bytes);
            held = std::move(event);
        }
    }
    ExpectEqual(payload, received,
        "raw TCP events preserve binary data across arbitrary receive segmentation");
    sc_wait* rawWait = nullptr;
    ExpectTrue(sc_tcp_connection_wait_capacity(retained.get(), payload.size(), &rawWait) == SC_OK &&
                   rawWait,
        "C TCP exposes an owning capacity wait handle");
    Wait wait(rawWait, sc_wait_destroy);
    if (wait)
        ExpectTrue(sc_wait_wait(wait.get(), 5000) == SC_OK,
            "C TCP capacity wait completes without polling bytes");
    auto echo = payload;
    ExpectTrue(sc_tcp_connection_send(retained.get(), Bytes(echo)) == SC_OK,
        "retained TCP connection remains usable after original handle destruction");
    echo.assign(echo.size(), 'x');
    ExpectEqual(
        payload, peer.Read(payload.size()), "C TCP send copies borrowed input before returning");
    server.reset();
    auto terminal = Next(retained.get());
    if (!terminal)
        return;
    const auto closed = EventView(terminal.get());
    ExpectTrue(closed.kind == SC_TCP_CLOSED && closed.status == SC_CLOSED,
        "server destruction yields a terminal event on surviving connection handles");
    sc_tcp_event* none = nullptr;
    ExpectTrue(sc_tcp_connection_next(retained.get(), 0, &none) == SC_CLOSED && !none,
        "TCP terminal event is delivered exactly once");
    ExpectTrue(sc_tcp_connection_send(retained.get(), Bytes("late")) == SC_CLOSED,
        "surviving TCP handle rejects late sends");
    retained.reset();
    ExpectEqual(heldBytes, Text(EventView(held.get()).bytes),
        "data event remains readable after both server and connection handles are destroyed");
}
std::uint64_t PendingEvents(sc_tcp_server* server)
{
    auto value = View<sc_observation>();
    return sc_tcp_server_get_observation(server, &value) == SC_OK ? value.pending_work : 0;
}
/// <summary>A connection's events, destroying each, until expected bytes arrived or 5 s passed.</summary>
std::string Drain(sc_tcp_connection* connection, std::size_t expected)
{
    std::string bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bytes.size() < expected && std::chrono::steady_clock::now() < deadline)
    {
        sc_tcp_event* raw = nullptr;
        if (sc_tcp_connection_next(connection, 100, &raw) != SC_OK || !raw)
            continue;
        Event event(raw, sc_tcp_event_destroy);
        const auto view = EventView(event.get());
        if (view.kind != SC_TCP_BYTES)
            break;
        bytes += Text(view.bytes);
    }
    return bytes;
}
/// <summary>
/// CABI-2: 소비되지 않은 A의 이벤트가 서버 전역 몫(max_event_count 4)을 채운 뒤 B가 보내도 B는
/// 끊기지 않고 자기 데이터를 받는다. A도 끊기지 않고, 몫이 찬 동안 보낸 바이트는 A의 이벤트를
/// 소비하면 이어서 도착한다. 몫이 실제로 찼는지(pending 4)를 먼저 단언한다.
/// </summary>
void ServerWideBudgetBackpressure()
{
    auto server = Start(4);
    if (!server)
        return;
    const auto port = sc_tcp_server_port(server.get());
    Peer first, second;
    if (!first.Connect(port))
        return;
    auto a = Accept(server.get());
    if (!a)
        return;
    if (!second.Connect(port))
        return;
    auto b = Accept(server.get());
    if (!b)
        return;
    for (std::uint64_t index = 1; index <= 4; ++index)
    {
        if (!first.Send(std::to_string(index)))
            return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (PendingEvents(server.get()) < index && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!Check(
            PendingEvents(server.get()) == 4, "unconsumed events of A fill the server-wide budget"))
        return;
    if (!second.Send("y"))
        return;
    auto event = Next(b.get());
    if (!event)
        return;
    const auto view = EventView(event.get());
    ExpectTrue(view.kind == SC_TCP_BYTES && Text(view.bytes) == "y",
        "B receives its data while A holds the whole server-wide budget");
    event.reset();
    if (!first.Send("5"))
        return;
    ExpectEqual(std::string("12345"), Drain(a.get(), 5), "A resumes after its events are consumed");
    sc_tcp_event* none = nullptr;
    ExpectTrue(sc_tcp_connection_next(a.get(), 0, &none) == SC_WOULD_BLOCK && !none &&
                   sc_tcp_connection_next(b.get(), 0, &none) == SC_WOULD_BLOCK && !none,
        "neither connection was closed by the budget");
}
/// <summary>
/// 연결별 몫(max_connection_event_count 2)이 차면 전역 몫이 남아 있어도 그 연결만 멈춘다. 멈추기
/// 전에 이미 진행 중이던 수신 하나까지만 더 들어오므로 쌓인 사건은 셋을 넘지 않는다. 다른 연결은
/// 곧바로 받고, 멈춘 연결은 사건을 소비하면 남은 바이트를 이어서 받는다.
/// </summary>
void ConnectionBudgetBackpressure()
{
    sc_tcp_options options{};
    (void)sc_tcp_options_init(&options, sizeof(options));
    ExpectTrue(options.max_connection_event_count == 64 &&
                   options.max_connection_event_bytes == 1024 * 1024,
        "per-connection budget defaults to 64 events and 1 MiB");
    auto invalid = options;
    invalid.max_connection_event_count = 0;
    sc_tcp_server* raw = nullptr;
    ExpectTrue(sc_tcp_server_create(&invalid, &raw) == SC_INVALID_ARGUMENT && !raw,
        "a zero per-connection budget is rejected");
    options.port = FreePort();
    options.max_connection_event_count = 2;
    if (!Check(sc_tcp_server_create(&options, &raw) == SC_OK && raw,
            "per-connection budget server creates"))
        return;
    Server server(raw, sc_tcp_server_destroy);
    if (!Check(sc_tcp_server_start(raw) == SC_OK, "per-connection budget server starts"))
        return;
    const auto port = sc_tcp_server_port(raw);
    Peer first, second;
    if (!first.Connect(port))
        return;
    auto a = Accept(raw);
    if (!a)
        return;
    if (!second.Connect(port))
        return;
    auto b = Accept(raw);
    if (!b)
        return;
    for (const char* text : { "1", "2", "3", "4" })
        if (!first.Send(text))
            return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (PendingEvents(raw) < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto held = PendingEvents(raw);
    ExpectTrue(held >= 1 && held <= 3,
        "a full per-connection budget pauses receiving within one extra receive");
    if (!second.Send("y"))
        return;
    ExpectEqual(std::string("y"), Drain(b.get(), 1),
        "another connection receives while the first is paused");
    ExpectEqual(std::string("1234"), Drain(a.get(), 4),
        "the paused connection resumes as its events are consumed");
    sc_tcp_event* none = nullptr;
    ExpectTrue(sc_tcp_connection_next(a.get(), 0, &none) == SC_WOULD_BLOCK && !none,
        "the paused connection was not closed");
    ExpectTrue(
        sc_tcp_connection_pause(a.get()) == SC_OK && sc_tcp_connection_resume(a.get()) == SC_OK,
        "the caller's pause and resume compose with budget backpressure");
}
void TcpRetainedEventOverflow()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "TCP overflow socket runtime starts"))
        return;
    ServerWideBudgetBackpressure();
    ConnectionBudgetBackpressure();
    auto server = Start(1);
    if (!server)
        return;
    Peer peer;
    if (!peer.Connect(sc_tcp_server_port(server.get())))
        return;
    auto connection = Accept(server.get());
    if (!connection)
        return;
    if (!peer.Send("a"))
        return;
    auto held = Next(connection.get());
    if (!held)
        return;
    const auto first = EventView(held.get());
    ExpectTrue(first.kind == SC_TCP_BYTES && Text(first.bytes) == "a",
        "single-byte payload occupies the retained event slot");
    if (!peer.Send("b"))
        return;
    // 몫이 찬 연결은 닫히지 않고 멈춘다. 멈추기 전에 진행 중이던 수신 하나는 들어올 수 있다.
    sc_tcp_event* raw = nullptr;
    const auto early = sc_tcp_connection_next(connection.get(), 200, &raw);
    Event second(raw, sc_tcp_event_destroy);
    if (early == SC_OK)
        ExpectTrue(EventView(second.get()).kind == SC_TCP_BYTES,
            "a full budget never publishes a terminal event");
    else
        ExpectEqual(
            sc_status{ SC_TIMEOUT }, early, "a full budget pauses receiving instead of closing");
    const auto text = std::string("a");
    if (!second)
    {
        held.reset();
        second = Next(connection.get());
        if (!second)
            return;
    }
    ExpectEqual(std::string("b"), Text(EventView(second.get()).bytes),
        "the paused bytes arrive once credit returns");
    server.reset();
    connection.reset();
    ExpectEqual(std::string("b"), Text(EventView(second.get()).bytes),
        "shutdown preserves already delivered event ownership");
    if (held)
        ExpectEqual(
            text, Text(EventView(held.get()).bytes), "the held event also survives shutdown");
}
const ServerCoreTest::CheckRegistration ownership(
    "CAbi.TcpOwnershipAndTerminal", TcpOwnershipAndTerminal);
const ServerCoreTest::CheckRegistration overflow(
    "CAbi.TcpRetainedEventOverflow", TcpRetainedEventOverflow);
}
