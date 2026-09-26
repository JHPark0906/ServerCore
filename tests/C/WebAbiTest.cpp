#include "AbiSocketTestSupport.h"
#include "ServerCore/C/Web.h"
#include "Web/RequestInputInternal.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace
{
using namespace AbiTest;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using Server = Handle<sc_web_server, sc_web_server_destroy>;
using Event = Handle<sc_web_event, sc_web_event_destroy>;
using Response = Handle<sc_http_response, sc_http_response_destroy>;
using Socket = Handle<sc_websocket, sc_websocket_destroy>;
using SocketEvent = Handle<sc_websocket_event, sc_websocket_event_destroy>;
using Wait = Handle<sc_wait, sc_wait_destroy>;
Event Next(sc_web_server* server)
{
    sc_web_event* event = nullptr;
    Check(sc_web_server_next(server, 5000, &event) == SC_OK && event,
        "C Web server delivers an owning event");
    return Event(event, sc_web_event_destroy);
}
Response Claim(sc_web_event* event)
{
    sc_http_response* response = nullptr;
    Check(sc_web_event_response(event, &response) == SC_OK && response,
        "C HTTP event yields an independent response handle");
    return Response(response, sc_http_response_destroy);
}
Server Start(bool websocket = false)
{
    sc_web_options options{};
    ExpectTrue(sc_web_options_init(&options, sizeof(options)) == SC_OK, "C Web options initialize");
    options.port = FreePort();
    options.max_event_count = 1;
    options.max_ws_event_count = 1;
    std::string address = "127.0.0.1";
    options.listen_address = Bytes(address);
    sc_web_server* server = nullptr;
    if (!Check(
            sc_web_server_create(&options, &server) == SC_OK && server, "C Web server is created"))
        return Server(nullptr, sc_web_server_destroy);
    Server owned(server, sc_web_server_destroy);
    address.assign(address.size(), 'x');
    std::string method = "POST", pattern = "/files/{id}";
    ExpectTrue(sc_web_server_route(server, Bytes(method), Bytes(pattern), 1) == SC_OK,
        "C HTTP pattern registers");
    method.assign(method.size(), 'x');
    pattern.assign(pattern.size(), 'x');
    if (websocket)
        ExpectTrue(sc_web_server_websocket(server, Bytes("/ws/{id}"), 1) == SC_OK,
            "C WebSocket pattern registers");
    if (!Check(sc_web_server_start(server) == SC_OK,
            "C Web server copies listen and route inputs before Start"))
        return Server(nullptr, sc_web_server_destroy);
    return owned;
}
sc_response_head Head(unsigned flags = SC_RESPONSE_CLOSE)
{
    sc_response_head head{};
    ExpectTrue(
        sc_response_head_init(&head, sizeof(head)) == SC_OK, "C response descriptor initializes");
    head.flags = flags;
    return head;
}
void WebHttpOwnershipAndStreaming()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "C HTTP socket runtime starts"))
        return;
    auto server = Start();
    if (!server)
        return;
    const std::string input("a\0b", 3);
    Peer peer;
    if (!peer.Connect(sc_web_server_port(server.get())) ||
        !peer.Send(Http("POST", "/files/a%20b?q=one", input)))
        return;
    auto event = Next(server.get());
    if (!event)
        return;
    ExpectTrue(sc_web_event_kind(event.get()) == SC_WEB_REQUEST,
        "HTTP route emits the request event kind");
    auto view = View<sc_request_view>();
    ExpectTrue(
        sc_web_event_request(event.get(), &view) == SC_OK, "C request exposes a versioned view");
    ExpectEqual(input, Text(view.body), "C request keeps binary body bytes");
    ExpectEqual(
        std::string("/files/a%20b?q=one"), Text(view.target), "C request owns its original target");
    ExpectTrue(view.parameter_count == 1 && Text(view.parameters[0].name) == "id" &&
                   Text(view.parameters[0].value) == "a b",
        "C pattern event exposes decoded owned parameters");
    auto response = Claim(event.get());
    if (!response)
        return;
    sc_http_response* duplicate = nullptr;
    ExpectTrue(sc_http_response_retain(response.get(), &duplicate) == SC_OK && duplicate,
        "C response retain creates independent handle lifetime");
    Response retained(duplicate, sc_http_response_destroy);
    response.reset();
    ExpectTrue(sc_http_response_cancelled(retained.get()) == 0,
        "dropping one response handle does not abort another");
    auto head = Head(SC_RESPONSE_CLOSE | SC_RESPONSE_HAS_LENGTH);
    head.content_length = 2;
    ExpectTrue(
        sc_http_response_complete(retained.get(), &head, Bytes(input)) == SC_INVALID_ARGUMENT,
        "buffered response rejects inconsistent declared length without consuming the writer");
    head.content_length = 3;
    std::string headerName = "X-Owned", headerValue = "before";
    sc_header header{ Bytes(headerName), Bytes(headerValue) };
    head.headers = &header;
    head.header_count = 1;
    ExpectTrue(sc_http_response_start(retained.get(), &head) == SC_OK,
        "C streaming response starts with exact length");
    headerName.assign(headerName.size(), 'x');
    headerValue.assign(headerValue.size(), 'x');
    sc_wait* capacity = nullptr;
    ExpectTrue(sc_http_response_wait_capacity(retained.get(), 3, &capacity) == SC_OK && capacity,
        "C response exposes capacity wait handle");
    Wait ready(capacity, sc_wait_destroy);
    if (ready)
        ExpectTrue(sc_wait_wait(ready.get(), 5000) == SC_OK, "C response capacity wait completes");
    std::string output("x\0y", 3);
    const auto expected = output;
    ExpectTrue(sc_http_response_write(retained.get(), Bytes(output)) == SC_OK,
        "C streaming write accepts binary bytes");
    output.assign(output.size(), 'z');
    ExpectTrue(sc_http_response_finish(retained.get()) == SC_OK, "C streaming response finishes");
    const auto wireHead = peer.Head();
    ExpectTrue(wireHead.find("Content-Length: 3\r\n") != std::string::npos &&
                   wireHead.find("X-Owned: before\r\n") != std::string::npos,
        "streaming headers are copied before caller mutations");
    ExpectEqual(expected, peer.Read(3), "streaming payload is copied before caller mutations");
    Peer rejected;
    if (!rejected.Connect(sc_web_server_port(server.get())) ||
        !rejected.Send(Http("POST", "/files/rejected")))
        return;
    ExpectTrue(rejected.UntilClosed().starts_with("HTTP/1.1 503"),
        "popped but retained request event still consumes event-count capacity");
    event.reset();
    Peer dropped;
    if (!dropped.Connect(sc_web_server_port(server.get())) ||
        !dropped.Send(Http("POST", "/files/drop")))
        return;
    auto droppedEvent = Next(server.get());
    if (!droppedEvent)
        return;
    auto droppedResponse = Claim(droppedEvent.get());
    droppedEvent.reset();
    droppedResponse.reset();
    bool closed = false;
    ExpectTrue(dropped.UntilClosed(&closed).empty() && closed,
        "last unfinished response handle aborts the transport");
    Peer unclaimed;
    if (!unclaimed.Connect(sc_web_server_port(server.get())) ||
        !unclaimed.Send(Http("POST", "/files/unclaimed")))
        return;
    auto unclaimedEvent = Next(server.get());
    if (!unclaimedEvent)
        return;
    unclaimedEvent.reset();
    closed = false;
    ExpectTrue(unclaimed.UntilClosed(&closed).empty() && closed,
        "dropping an unclaimed event aborts its response");
    Peer chunked;
    if (!chunked.Connect(sc_web_server_port(server.get())) ||
        !chunked.Send(Http("POST", "/files/chunked")))
        return;
    auto chunkEvent = Next(server.get());
    if (!chunkEvent)
        return;
    auto chunkResponse = Claim(chunkEvent.get());
    chunkEvent.reset();
    head = Head();
    ExpectTrue(sc_http_response_start(chunkResponse.get(), &head) == SC_OK &&
                   sc_http_response_write(chunkResponse.get(), Bytes("chunk")) == SC_OK &&
                   sc_http_response_finish(chunkResponse.get()) == SC_OK,
        "C unknown-length response emits a finished stream");
    const auto chunks = chunked.UntilClosed();
    ExpectTrue(chunks.find("Transfer-Encoding: chunked\r\n") != std::string::npos &&
                   chunks.ends_with("5\r\nchunk\r\n0\r\n\r\n"),
        "C streaming uses complete HTTP chunk framing");
    Peer surviving;
    if (!surviving.Connect(sc_web_server_port(server.get())) ||
        !surviving.Send(Http("POST", "/files/survives", input)))
        return;
    auto survivingEvent = Next(server.get());
    if (!survivingEvent)
        return;
    auto survivingResponse = Claim(survivingEvent.get());
    server.reset();
    view = View<sc_request_view>();
    ExpectTrue(
        sc_web_event_request(survivingEvent.get(), &view) == SC_OK && Text(view.body) == input,
        "request event views remain valid after server destruction");
    ExpectTrue(sc_http_response_cancelled(survivingResponse.get()) != 0 &&
                   sc_http_response_write(survivingResponse.get(), Bytes("late")) == SC_CLOSED,
        "response handle survives server destruction in a cancelled closed state");
}

/// <summary>WebSocket 경로 하나를 연 서버. 전역 WebSocket 사건 몫만 정한다.</summary>
Server SocketServer(std::size_t socketEvents)
{
    sc_web_options options{};
    (void)sc_web_options_init(&options, sizeof(options));
    options.port = FreePort();
    options.max_ws_event_count = socketEvents;
    sc_web_server* raw = nullptr;
    if (!Check(sc_web_server_create(&options, &raw) == SC_OK && raw,
            "WebSocket budget server creates"))
        return Server(nullptr, sc_web_server_destroy);
    Server server(raw, sc_web_server_destroy);
    if (!Check(sc_web_server_websocket(raw, Bytes("/ws/{id}"), 1) == SC_OK &&
                   sc_web_server_start(raw) == SC_OK,
            "WebSocket budget server starts"))
        return Server(nullptr, sc_web_server_destroy);
    return server;
}
/// <summary>업그레이드를 마치고 그 연결의 소유 핸들을 돌려준다.</summary>
Socket OpenSocket(sc_web_server* server, Peer& peer, std::string_view path)
{
    if (!peer.Connect(sc_web_server_port(server)) || !peer.Send(Upgrade(path)) ||
        !Check(peer.Head().starts_with("HTTP/1.1 101"), "budget WebSocket upgrades"))
        return Socket(nullptr, sc_websocket_destroy);
    auto event = Next(server);
    if (!event)
        return Socket(nullptr, sc_websocket_destroy);
    sc_websocket* raw = nullptr;
    Check(sc_web_event_socket(event.get(), &raw) == SC_OK && raw, "budget WebSocket handle");
    return Socket(raw, sc_websocket_destroy);
}
/// <summary>사건 하나를 기다려 받는다. 없으면 null이다.</summary>
SocketEvent NextMessage(sc_websocket* socket, uint32_t timeout)
{
    sc_websocket_event* raw = nullptr;
    (void)sc_websocket_next(socket, timeout, &raw);
    return SocketEvent(raw, sc_websocket_event_destroy);
}
std::string MessageText(const SocketEvent& event, uint32_t* kind = nullptr)
{
    auto view = View<sc_websocket_event_view>();
    if (!event || sc_websocket_event_get_view(event.get(), &view) != SC_OK)
        return {};
    if (kind)
        *kind = view.kind;
    return Text(view.bytes);
}
/// <summary>
/// CABI-2(WebSocket): 소비되지 않은 A의 메시지가 서버 전역 WebSocket 몫(max_ws_event_count 4)을
/// 채운 뒤 B가 보내도 B는 끊기지 않고 받는다. A도 끊기지 않고, 몫이 찬 동안 보낸 메시지는 A의
/// 사건을 소비하면 이어서 도착한다. A의 사건 넷을 실제로 꺼내 쥐어 몫이 찼음을 먼저 단언한다.
/// </summary>
void SocketServerWideBudgetBackpressure()
{
    auto server = SocketServer(4);
    if (!server)
        return;
    Peer first, second;
    auto a = OpenSocket(server.get(), first, "/ws/a");
    if (!a)
        return;
    auto b = OpenSocket(server.get(), second, "/ws/b");
    if (!b)
        return;
    std::vector<SocketEvent> held;
    for (const char* text : { "1", "2", "3", "4" })
    {
        if (!first.Masked(1, text))
            return;
        auto event = NextMessage(a.get(), 5000);
        if (!Check(MessageText(event) == text,
                "A's unconsumed messages fill the server-wide WebSocket budget"))
            return;
        held.push_back(std::move(event));
    }
    if (!second.Masked(1, "y"))
        return;
    uint32_t kind = 0;
    const auto received = MessageText(NextMessage(b.get(), 5000), &kind);
    ExpectTrue(kind == SC_WS_TEXT && received == "y",
        "B receives its message while A holds the whole WebSocket budget");
    if (!first.Masked(1, "5"))
        return;
    held.clear();
    ExpectEqual(std::string("5"), MessageText(NextMessage(a.get(), 5000)),
        "A resumes after its messages are consumed");
    sc_websocket_event* none = nullptr;
    ExpectTrue(sc_websocket_next(a.get(), 0, &none) == SC_WOULD_BLOCK && !none &&
                   sc_websocket_next(b.get(), 0, &none) == SC_WOULD_BLOCK && !none,
        "neither WebSocket was closed by the budget");
}
/// <summary>
/// 소켓별 몫(max_ws_connection_event_count 2)이 차면 전역 몫이 남아 있어도 그 소켓만 멈춘다. 멈춤은
/// onMessage 안에서 일어나므로 셋째 메시지는 몫이 돌아올 때까지 오지 않는다. 다른 소켓은 곧바로
/// 받고, 멈춘 소켓은 사건을 소비하면 남은 메시지를 이어서 받는다.
/// </summary>
void SocketConnectionBudgetBackpressure()
{
    sc_web_options options{};
    (void)sc_web_options_init(&options, sizeof(options));
    ExpectTrue(options.max_ws_connection_event_count == 64 &&
                   options.max_ws_connection_event_bytes == 1024 * 1024 && options.reserved2 == 0,
        "per-socket budget defaults to 64 messages and 1 MiB");
    sc_web_server* raw = nullptr;
    auto invalid = options;
    invalid.max_ws_connection_event_count = 0;
    ExpectTrue(sc_web_server_create(&invalid, &raw) == SC_INVALID_ARGUMENT && !raw,
        "a zero per-socket budget is rejected");
    invalid = options;
    invalid.reserved2 = 1;
    ExpectTrue(sc_web_server_create(&invalid, &raw) == SC_INVALID_ARGUMENT && !raw,
        "a nonzero reserved2 is rejected");
    options.port = FreePort();
    options.max_ws_connection_event_count = 2;
    if (!Check(sc_web_server_create(&options, &raw) == SC_OK && raw,
            "per-socket budget server creates"))
        return;
    Server server(raw, sc_web_server_destroy);
    if (!Check(sc_web_server_websocket(raw, Bytes("/ws/{id}"), 1) == SC_OK &&
                   sc_web_server_start(raw) == SC_OK,
            "per-socket budget server starts"))
        return;
    Peer first, second;
    auto a = OpenSocket(raw, first, "/ws/a");
    if (!a)
        return;
    auto b = OpenSocket(raw, second, "/ws/b");
    if (!b)
        return;
    for (const char* text : { "1", "2", "3" })
        if (!first.Masked(1, text))
            return;
    std::vector<SocketEvent> held;
    for (const char* text : { "1", "2" })
    {
        auto event = NextMessage(a.get(), 5000);
        if (!Check(MessageText(event) == text, "A's first messages fill its own budget"))
            return;
        held.push_back(std::move(event));
    }
    sc_websocket_event* early = nullptr;
    ExpectTrue(sc_websocket_next(a.get(), 200, &early) == SC_TIMEOUT && !early,
        "a full per-socket budget pauses that socket while the server-wide budget has room");
    if (!second.Masked(1, "y"))
        return;
    ExpectEqual(std::string("y"), MessageText(NextMessage(b.get(), 5000)),
        "another socket receives while the first is paused");
    held.clear();
    ExpectEqual(std::string("3"), MessageText(NextMessage(a.get(), 5000)),
        "the paused socket resumes as its events are consumed");
    sc_websocket_event* none = nullptr;
    ExpectTrue(sc_websocket_next(a.get(), 0, &none) == SC_WOULD_BLOCK && !none,
        "the paused socket was not closed");
}
void WebSocketOwnershipAndOverflow()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!Check(runtime.IsReady(), "C WebSocket socket runtime starts"))
        return;
    SocketServerWideBudgetBackpressure();
    SocketConnectionBudgetBackpressure();
    auto server = Start(true);
    if (!server)
        return;
    Peer peer;
    if (!peer.Connect(sc_web_server_port(server.get())) || !peer.Send(Upgrade("/ws/saved%20id")))
        return;
    ExpectTrue(peer.Head().starts_with("HTTP/1.1 101"), "C WebSocket upgrades");
    auto event = Next(server.get());
    if (!event)
        return;
    auto request = View<sc_request_view>();
    ExpectTrue(sc_web_event_kind(event.get()) == SC_WEB_WEBSOCKET &&
                   sc_web_event_request(event.get(), &request) == SC_OK &&
                   request.parameter_count == 1 && Text(request.parameters[0].value) == "saved id",
        "C WebSocket event retains handshake pattern metadata");
    sc_websocket* raw = nullptr;
    if (!Check(sc_web_event_socket(event.get(), &raw) == SC_OK && raw,
            "C WebSocket event yields an owning handle"))
        return;
    Socket socket(raw, sc_websocket_destroy);
    sc_websocket* clone = nullptr;
    ExpectTrue(sc_websocket_retain(socket.get(), &clone) == SC_OK && clone,
        "C WebSocket retain creates an independent owner");
    Socket retained(clone, sc_websocket_destroy);
    ExpectTrue(sc_websocket_id(socket.get()) != 0 &&
                   sc_websocket_id(socket.get()) == sc_websocket_id(retained.get()),
        "retained WebSocket keeps connection identity");
    socket.reset();
    event.reset();
    sc_websocket_event* incoming = nullptr;
    ExpectTrue(sc_websocket_next(retained.get(), 0, &incoming) == SC_WOULD_BLOCK && !incoming,
        "empty WebSocket poll is nonblocking");
    const std::string binary("a\0b", 3);
    if (!peer.Masked(2, binary))
        return;
    if (!Check(sc_websocket_next(retained.get(), 5000, &incoming) == SC_OK && incoming,
            "C WebSocket receives an owned binary event"))
        return;
    SocketEvent held(incoming, sc_websocket_event_destroy);
    auto view = View<sc_websocket_event_view>();
    ExpectTrue(sc_websocket_event_get_view(held.get(), &view) == SC_OK &&
                   view.kind == SC_WS_BINARY && Text(view.bytes) == binary,
        "C WebSocket event preserves arbitrary binary bytes");
    std::string output("x\0y", 3);
    const auto expected = output;
    ExpectTrue(sc_websocket_send(retained.get(), SC_WS_BINARY, Bytes(output)) == SC_OK,
        "C WebSocket sends binary bytes");
    output.assign(output.size(), 'z');
    const auto frame = peer.Frame();
    ExpectTrue(
        frame.first == 2 && frame.second == expected, "C WebSocket copies borrowed send input");
    ExpectTrue(
        sc_websocket_send(retained.get(), SC_WS_TEXT, Bytes("\xc0\xaf")) == SC_INVALID_ARGUMENT,
        "C WebSocket rejects malformed outgoing UTF-8");
    if (!peer.Masked(2, "overflow"))
        return;
    // 몫(1)이 찬 소켓은 닫히지 않고 멈춘다. 멈춘 소켓의 다음 메시지는 몫이 돌아올 때까지 오지 않는다.
    sc_websocket_event* early = nullptr;
    ExpectTrue(sc_websocket_next(retained.get(), 200, &early) == SC_TIMEOUT && !early,
        "a full budget pauses the socket instead of closing it");
    held.reset();
    if (!Check(sc_websocket_next(retained.get(), 5000, &incoming) == SC_OK && incoming,
            "the paused message arrives once credit returns"))
        return;
    SocketEvent resumed(incoming, sc_websocket_event_destroy);
    view = View<sc_websocket_event_view>();
    ExpectTrue(sc_websocket_event_get_view(resumed.get(), &view) == SC_OK &&
                   Text(view.bytes) == "overflow",
        "the paused message keeps its bytes");
    ExpectTrue(sc_websocket_close(retained.get(), 1000, {}) == SC_OK &&
                   sc_websocket_close(retained.get(), 1000, {}) == SC_OK,
        "C close is idempotent and also unpauses the closing handshake");
    const auto close = peer.Frame();
    ExpectTrue(close.first == 8 && close.second.size() >= 2 &&
                   static_cast<unsigned char>(close.second[0]) == 3 &&
                   static_cast<unsigned char>(close.second[1]) == 232,
        "server close sends code 1000");
    if (!peer.Masked(8, close.second))
        return;
    sc_websocket_event* terminal = nullptr;
    if (!Check(sc_websocket_next(retained.get(), 5000, &terminal) == SC_OK && terminal,
            "WebSocket terminal event has reserved capacity"))
        return;
    SocketEvent ended(terminal, sc_websocket_event_destroy);
    auto terminalView = View<sc_websocket_event_view>();
    ExpectTrue(sc_websocket_event_get_view(ended.get(), &terminalView) == SC_OK &&
                   terminalView.kind == SC_WS_CLOSED,
        "WebSocket poll returns a distinct terminal event");
    terminal = nullptr;
    ExpectTrue(sc_websocket_next(retained.get(), 0, &terminal) == SC_CLOSED && !terminal,
        "WebSocket terminal event is delivered once");
    server.reset();
    view = View<sc_websocket_event_view>();
    ExpectTrue(sc_websocket_event_get_view(resumed.get(), &view) == SC_OK &&
                   Text(view.bytes) == "overflow",
        "held WebSocket data remains readable after server destruction");
    ExpectTrue(sc_websocket_send(retained.get(), SC_WS_BINARY, Bytes("late")) == SC_CLOSED,
        "surviving WebSocket handle rejects sends after shutdown");
}
Server ExtensionServer(std::size_t chunk = 4096)
{
    sc_web_options options{};
    sc_web_options_init(&options, sizeof(options));
    options.port = FreePort();
    options.max_chunk_bytes = chunk;
    options.max_body_bytes = 64;
    sc_web_server* raw = nullptr;
    Check(sc_web_server_create(&options, &raw) == SC_OK && raw, "extension server creates");
    return Server(raw, sc_web_server_destroy);
}
using Decision = Handle<sc_request_decision, sc_request_decision_destroy>;
Decision Decide(sc_web_event* event)
{
    sc_request_decision* raw = nullptr;
    Check(sc_web_event_decision(event, &raw) == SC_OK && raw,
        "policy decision is independently owned");
    return Decision(raw, sc_request_decision_destroy);
}
void WebPolicyBeforeUpgrade()
{
    ServerCoreTest::SocketRuntime runtime;
    auto server = ExtensionServer();
    if (!runtime.IsReady() || !server)
        return;
    ExpectTrue(sc_web_server_enable_policy(server.get()) == SC_OK &&
                   sc_web_server_route(server.get(), Bytes("GET"), Bytes("/plain"), 0) == SC_OK &&
                   sc_web_server_websocket_policy(server.get(), Bytes("/ws/{id}"), 1) == SC_OK &&
                   sc_web_server_start(server.get()) == SC_OK,
        "global and route-specific policy register before start");
    Peer denied;
    if (!denied.Connect(sc_web_server_port(server.get())) || !denied.Send(Upgrade("/ws/private")))
        return;
    auto gate = Next(server.get());
    if (!gate)
        return;
    ExpectTrue(sc_web_event_kind(gate.get()) == SC_WEB_POLICY &&
                   sc_web_event_policy_is_websocket(gate.get()),
        "upgrade arrives as a pre-101 policy event");
    auto decision = Decide(gate.get());
    if (!decision)
        return;
    gate.reset();
    auto allow = View<sc_policy_allow>();
    ExpectTrue(sc_request_decision_allow(decision.get(), &allow) == SC_OK,
        "global gate allows route authorization");
    auto routeGate = Next(server.get());
    if (!routeGate)
        return;
    auto routeDecision = Decide(routeGate.get());
    if (!routeDecision)
        return;
    routeGate.reset();
    auto deniedHead = Head();
    deniedHead.status = 403;
    ExpectTrue(
        sc_request_decision_reject(routeDecision.get(), &deniedHead, Bytes("denied")) == SC_OK &&
            sc_request_decision_allow(routeDecision.get(), &allow) == SC_CLOSED,
        "route gate rejects exactly once");
    auto wire = denied.UntilClosed();
    ExpectTrue(wire.starts_with("HTTP/1.1 403") &&
                   wire.find("101 Switching") == std::string::npos && wire.ends_with("denied"),
        "authorization rejects before any switching-protocol response");
    Peer plain;
    if (!plain.Connect(sc_web_server_port(server.get())) || !plain.Send(Http("GET", "/plain")))
        return;
    gate = Next(server.get());
    if (!gate)
        return;
    decision = Decide(gate.get());
    if (!decision)
        return;
    gate.reset();
    sc_header attribute{ Bytes("UserId"), Bytes("42") },
        header{ Bytes("X-Policy"), Bytes("allowed") };
    allow.attributes = &attribute;
    allow.attribute_count = 1;
    allow.response_headers = &header;
    allow.response_header_count = 1;
    ExpectTrue(sc_request_decision_allow(decision.get(), &allow) == SC_OK,
        "middleware transfers bounded metadata");
    auto request = Next(server.get());
    if (!request)
        return;
    auto attributes = View<sc_headers_view>();
    ExpectTrue(sc_web_event_attributes(request.get(), &attributes) == SC_OK &&
                   attributes.count == 1 && Text(attributes.headers[0].name) == "UserId" &&
                   Text(attributes.headers[0].value) == "42",
        "request event owns case-sensitive policy attributes");
    auto response = Claim(request.get());
    if (!response)
        return;
    auto head = Head();
    ExpectTrue(sc_http_response_complete(response.get(), &head, Bytes("ok")) == SC_OK,
        "allowed request completes");
    wire = plain.UntilClosed();
    ExpectTrue(wire.find("X-Policy: allowed\r\n") != std::string::npos,
        "policy response metadata reaches the wire");
}
void WebStreamingBodyOwnership()
{
    ServerCoreTest::SocketRuntime runtime;
    sc_web_options defaults{};
    sc_web_options_init(&defaults, sizeof(defaults));
    sc_web_server* unported = nullptr;
    ExpectTrue(sc_web_server_create(&defaults, &unported) == SC_OK &&
                   sc_web_server_start(unported) == SC_INVALID_ARGUMENT,
        "the default Web port 0 is rejected at Start");
    sc_web_server_destroy(unported);
    auto server = ExtensionServer(4);
    if (!runtime.IsReady() || !server)
        return;
    sc_body_options options{};
    sc_body_options_init(&options, sizeof(options));
    options.max_buffered_bytes = 8;
    options.max_body_bytes = 32; // Below max_body_bytes 64.
    ExpectTrue(sc_web_server_set_body_limits(server.get(), &options) == SC_OK &&
                   sc_web_server_start(server.get()) == SC_INVALID_ARGUMENT,
        "Start rejects body limits below max_body_bytes");
    options.max_body_bytes = 64;
    // 쥐고 있는 조각마다 장부 비용이 몫에 더해진다(WEB-4). 8 + 2×비용이면 4바이트 조각 둘째까지
    // 받고(8 + 2c - 4 - 2c = 4), 둘을 쥔 동안 셋째는 막힌다(3c ≥ 8 + 2c - 8).
    options.max_buffered_bytes =
        8 + 2 * ServerCore::Web::Detail::RequestBodyState::PieceOverheadBytes;
    ExpectEqual(sc_status{ SC_OK }, sc_web_server_set_body_limits(server.get(), &options),
        "body limits can be corrected after Start rejected them");
    ExpectTrue(
        sc_web_server_set_body_limits(server.get(), &options) == SC_OK &&
            sc_web_server_stream_route(server.get(), Bytes("POST"), Bytes("/upload"), 0) == SC_OK &&
            sc_web_server_start(server.get()) == SC_OK,
        "streaming request limits register");
    Peer peer;
    if (!peer.Connect(sc_web_server_port(server.get())) ||
        !peer.Send(Http("POST", "/upload", "abcdefghijklmnop")))
        return;
    auto event = Next(server.get());
    if (!event)
        return;
    auto response = Claim(event.get());
    if (!response)
        return;
    sc_http_body* raw = nullptr;
    if (!Check(sc_web_event_body(event.get(), &raw) == SC_OK && raw,
            "stream event yields owned body reader"))
        return;
    Handle<sc_http_body, sc_http_body_destroy> body(raw, sc_http_body_destroy);
    event.reset();
    using Chunk = Handle<sc_body_chunk, sc_body_chunk_destroy>;
    auto read = [&]()
    {
        sc_body_chunk* chunk = nullptr;
        sc_status status = SC_WOULD_BLOCK;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (status == SC_WOULD_BLOCK && std::chrono::steady_clock::now() < deadline)
        {
            status = sc_http_body_read(body.get(), &chunk);
            if (status == SC_WOULD_BLOCK)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(status == SC_OK, "upload reader reaches data or clean EOF");
        return Chunk(chunk, sc_body_chunk_destroy);
    };
    auto first = read(), second = read();
    if (!first || !second)
        return;
    ExpectTrue(Text(sc_body_chunk_view(first.get())) == "abcd" &&
                   Text(sc_body_chunk_view(second.get())) == "efgh" &&
                   sc_http_body_retained_bytes(body.get()) == 8,
        "owned chunks remain charged after leaving the queue");
    sc_body_chunk* blocked = nullptr;
    ExpectTrue(sc_http_body_read(body.get(), &blocked) == SC_WOULD_BLOCK && !blocked,
        "held chunks backpressure further upload reads");
    first.reset();
    second.reset();
    std::string remaining;
    Chunk held(nullptr, sc_body_chunk_destroy);
    while (auto chunk = read())
    {
        remaining += Text(sc_body_chunk_view(chunk.get()));
        held = std::move(chunk);
        if (remaining.size() < 8)
            held.reset();
    }
    ExpectTrue(remaining == "ijklmnop", "releasing chunks resumes upload through clean EOF");
    auto head = Head();
    ExpectTrue(sc_http_response_complete(response.get(), &head, Bytes("ok")) == SC_OK,
        "consumed upload can complete its response");
    ExpectTrue(peer.UntilClosed().ends_with("ok"), "stream request gets response");
    body.reset();
    server.reset();
    ExpectTrue(held && Text(sc_body_chunk_view(held.get())) == "mnop",
        "last body chunk survives reader and server destruction");
}
void WebDrainMetricsAndLogger()
{
    ServerCoreTest::SocketRuntime runtime;
    auto server = ExtensionServer();
    if (!runtime.IsReady() || !server)
        return;
    sc_logger_options loggerOptions{};
    sc_logger_options_init(&loggerOptions, sizeof(loggerOptions));
    loggerOptions.max_message_bytes = 16;
    sc_logger* raw = nullptr;
    if (!Check(sc_logger_create(&loggerOptions, &raw) == SC_OK && raw,
            "C logger creates bounded instance"))
        return;
    Handle<sc_logger, sc_logger_destroy> logger(raw, sc_logger_destroy);
    ExpectTrue(sc_logger_try_write(logger.get(), SC_LOG_DEBUG, Bytes("filtered")) == SC_OK &&
                   sc_logger_try_write(logger.get(), SC_LOG_INFO, Bytes("C ABI logger")) == SC_OK &&
                   sc_logger_try_write(logger.get(), SC_LOG_INFO, Bytes("01234567890123456")) ==
                       SC_TOO_LARGE,
        "C logger filters and bounds caller messages");
    ExpectTrue(sc_logger_stop(logger.get()) == SC_OK, "logger stop drains output");
    auto logMetrics = View<sc_logger_metrics>();
    ExpectTrue(sc_logger_get_metrics(logger.get(), &logMetrics) == SC_OK &&
                   logMetrics.filtered_messages == 1 && logMetrics.written_messages == 1 &&
                   logMetrics.pending_messages == 0 && logMetrics.retained_bytes == 0,
        "C logger exposes drained instance accounting");
    ExpectTrue(sc_web_server_set_logger(server.get(), logger.get()) == SC_OK &&
                   sc_web_server_route(server.get(), Bytes("GET"), Bytes("/drain"), 0) == SC_OK &&
                   sc_web_server_start(server.get()) == SC_OK,
        "instance logger and route register");
    logger.reset();
    Peer peer;
    if (!peer.Connect(sc_web_server_port(server.get())) || !peer.Send(Http("GET", "/drain")))
        return;
    auto event = Next(server.get());
    if (!event)
        return;
    auto response = Claim(event.get());
    if (!response)
        return;
    event.reset();
    ExpectTrue(sc_web_server_begin_drain(server.get()) == SC_OK &&
                   sc_web_server_drain_status(server.get()) == SC_WOULD_BLOCK,
        "drain retains an admitted unresolved request");
    auto head = Head();
    ExpectTrue(sc_http_response_complete(response.get(), &head, Bytes("finished")) == SC_OK,
        "admitted handler completes during drain");
    // 서버는 보내기를 닫은 뒤 상대가 닫을 때까지 입력을 읽는다(NET-2). EOF를 본 클라이언트가 닫아야
    // 그 연결이 끝나고 graceful 정지가 기한 전에 돌아온다.
    const auto flushed = peer.UntilClosed();
    peer.Close();
    ExpectTrue(
        flushed.ends_with("finished") && sc_web_server_stop_gracefully(server.get(), 5000) == SC_OK,
        "graceful shutdown flushes the admitted response");
    auto metrics = View<sc_web_metrics>();
    ExpectTrue(sc_web_server_get_metrics(server.get(), &metrics) == SC_OK &&
                   metrics.accepted_requests == 1 && metrics.completed_requests == 1 &&
                   metrics.active_requests == 0,
        "C metrics preserve terminal counters after shutdown");
    std::uint64_t samples = 0;
    for (const auto count : metrics.latency_buckets)
        samples += count;
    ExpectTrue(samples == 1, "request terminal records exactly one latency bin");
    sc_owned_text* text = nullptr;
    if (!Check(sc_web_server_metrics_prometheus(server.get(), &text) == SC_OK && text,
            "Prometheus text has independent ownership"))
        return;
    Handle<sc_owned_text, sc_owned_text_destroy> exported(text, sc_owned_text_destroy);
    server.reset();
    const auto exposition = Text(sc_owned_text_view(exported.get()));
    ExpectTrue(exposition.find("servercore_http_") != std::string::npos &&
                   exposition.find("_bucket{") != std::string::npos,
        "metrics export survives server destruction and contains fixed-label histogram");

    server = ExtensionServer();
    if (!server)
        return;
    ExpectTrue(sc_web_server_route(server.get(), Bytes("GET"), Bytes("/pending"), 0) == SC_OK &&
                   sc_web_server_start(server.get()) == SC_OK,
        "interruptible drain server starts");
    Peer pending;
    if (!pending.Connect(sc_web_server_port(server.get())) ||
        !pending.Send(Http("GET", "/pending")))
        return;
    event = Next(server.get());
    if (!event)
        return;
    response = Claim(event.get());
    if (!response)
        return;
    event.reset();
    ExpectTrue(
        sc_web_server_begin_drain(server.get()) == SC_OK, "long graceful wait starts draining");
    std::atomic<bool> entered{ false };
    sc_status graceful = SC_PLATFORM_ERROR;
    std::thread waiter(
        [&]
        {
            entered.store(true);
            graceful = sc_web_server_stop_gracefully(server.get(), 5000);
        });
    while (!entered.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto before = std::chrono::steady_clock::now();
    const auto immediate = sc_web_server_stop(server.get());
    const auto elapsed = std::chrono::steady_clock::now() - before;
    waiter.join();
    ExpectTrue(immediate == SC_OK && graceful == SC_OK && elapsed < std::chrono::seconds(2),
        "immediate stop interrupts a concurrent graceful wait instead of waiting for its deadline");
    ExpectTrue(sc_http_response_cancelled(response.get()) != 0,
        "interrupted drain cancels pending response");
}
const ServerCoreTest::CheckRegistration policy(
    "CAbi.WebPolicyBeforeUpgrade", WebPolicyBeforeUpgrade);
const ServerCoreTest::CheckRegistration requestBody(
    "CAbi.WebStreamingBodyOwnership", WebStreamingBodyOwnership);
const ServerCoreTest::CheckRegistration drainMetrics(
    "CAbi.WebDrainMetricsAndLogger", WebDrainMetricsAndLogger);
const ServerCoreTest::CheckRegistration http(
    "CAbi.WebHttpOwnershipAndStreaming", WebHttpOwnershipAndStreaming);
const ServerCoreTest::CheckRegistration websocket(
    "CAbi.WebSocketOwnershipAndOverflow", WebSocketOwnershipAndOverflow);
}
