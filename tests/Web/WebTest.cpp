#include "TestHarness.h"
#include "SocketTestSupport.h"
#include "ServerCore/Web/HttpServer.h"
#include "../../src/Web/WebProtocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace Web = ServerCore::Web;
namespace Detail = ServerCore::Web::Detail;
using ServerCoreTest::ExpectTrue;
using ServerCoreTest::ExpectEqual;

std::span<const std::byte> Bytes(std::string_view value)
{
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::string Text(std::span<const std::byte> value)
{
    return value.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string MaskedFrame(std::uint8_t opcode, std::string_view payload, bool final = true)
{
    auto wire = Detail::EncodeServerFrame(opcode, Bytes(payload));
    if (!final) wire[0] &= std::byte{0x7f};
    wire[1] |= std::byte{0x80};
    const std::size_t header = payload.size() < 126 ? 2 : payload.size() <= 65535 ? 4 : 10;
    constexpr std::array mask{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};
    wire.insert(wire.begin() + static_cast<std::ptrdiff_t>(header), mask.begin(), mask.end());
    for (std::size_t index = 0; index < payload.size(); ++index) wire[header + 4 + index] ^= mask[index % 4];
    return Text(wire);
}

void HttpParserConformance()
{
    Detail::HttpParser parser(1024, 1024);
    const std::string wire = "POST /echo?q=1 HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n"
        "2;name=\"a\\\"b\"\r\nhe\r\n3\r\nllo\r\n0\r\nX-Checksum: value\r\n\r\n";
    std::string pending;
    Web::HttpRequest request;
    bool continued = false;
    bool complete = false;
    for (char byte : wire)
    {
        pending.push_back(byte);
        auto result = parser.Parse(pending, request);
        if (result.kind == Detail::HttpParseKind::Continue)
        {
            continued = true;
            result = parser.Parse(pending, request);
        }
        ExpectTrue(result.kind != Detail::HttpParseKind::Error, "split chunked request remains valid");
        if (result.kind == Detail::HttpParseKind::Complete) complete = true;
    }
    ExpectTrue(continued && complete, "100-continue and complete request are reported");
    ExpectEqual(std::string("hello"), request.body, "chunks are decoded into one body");
    ExpectEqual(std::string("/echo"), std::string(request.Path()), "routing excludes the query string");
    ExpectTrue(request.Header("x-checksum").empty(), "trailers do not overwrite request headers");
    pending = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\n";
    pending.append("a\0b", 3);
    pending += "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    ExpectTrue(parser.Parse(pending, request).kind == Detail::HttpParseKind::Complete, "binary body completes");
    ExpectEqual(std::size_t{3}, request.body.size(), "NUL remains part of the HTTP body");
    ExpectTrue(parser.Parse(pending, request).kind == Detail::HttpParseKind::Complete, "pipelined request completes");
    ExpectEqual(std::string("/next"), request.target, "pipeline preserves request boundaries");
    Web::HttpResponse response;
    response.body = "hello";
    std::string encoded;
    ExpectTrue(Detail::SerializeResponse(response, true, false, 1024, 1024, encoded), "HEAD response serializes");
    ExpectTrue(encoded.find("Content-Length: 5\r\n") != std::string::npos && encoded.ends_with("\r\n\r\n"),
        "HEAD reports representation length without a payload");
}

void HttpParserRejectsAmbiguousRequests()
{
    const std::array<std::string_view, 9> invalid{
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n folded: value\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!\n0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 2\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1;bad=\"unterminated\r\nx\r\n0\r\n\r\n"
    };
    for (const auto wire : invalid)
    {
        Detail::HttpParser parser(1024, 1024);
        std::string pending(wire);
        Web::HttpRequest request;
        ExpectTrue(parser.Parse(pending, request).kind == Detail::HttpParseKind::Error, "ambiguous or malformed framing is rejected");
    }
    Detail::HttpParser bounded(256, 4);
    Web::HttpRequest request;
    std::string pending = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    ExpectEqual(413u, bounded.Parse(pending, request).status, "decoded chunked body has an enforced limit");
    Detail::HttpParser headers(256, 4);
    pending = "GET / HTTP/1.1\r\nHost: x\r\nX: " + std::string(256, 'a');
    ExpectEqual(431u, headers.Parse(pending, request).status, "unfinished headers have an enforced limit");
    Detail::HttpParser coding(1024, 1024);
    pending = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    ExpectEqual(501u, coding.Parse(pending, request).status, "unsupported transfer coding is explicit");
    Web::HttpResponse response;
    response.headers.emplace_back("X-Test", "value\r\nInjected: yes");
    std::string encoded;
    ExpectTrue(!Detail::SerializeResponse(response, false, false, 1024, 1024, encoded), "response splitting is rejected");
    response.headers = {{"Content-Length", "999"}};
    ExpectTrue(!Detail::SerializeResponse(response, false, false, 1024, 1024, encoded), "application cannot override response framing");
}

void WebSocketProtocolConformance()
{
    std::string accept;
    ExpectTrue(Detail::WebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==", accept), "RFC key is accepted");
    ExpectEqual(std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), accept, "SHA-1 and base64 match RFC 6455 example");
    ExpectTrue(!Detail::WebSocketAccept("dGhlIHNhbXBsZSBub25jZR==", accept), "noncanonical key padding is rejected");
    const std::string wire = MaskedFrame(1, "Hello");
    Detail::WebSocketFrame frame;
    for (std::size_t count = 0; count < wire.size(); ++count)
        ExpectTrue(Detail::ParseClientFrame(Bytes(wire).first(count), 1024, frame).kind == Detail::FrameParseKind::NeedMore,
            "partial client frame needs more data");
    const auto result = Detail::ParseClientFrame(Bytes(wire), 1024, frame);
    ExpectTrue(result.kind == Detail::FrameParseKind::Complete && frame.final && frame.opcode == 1, "masked text frame decodes");
    ExpectEqual(std::string("Hello"), Text(frame.payload), "mask is removed");
    auto unmasked = Detail::EncodeServerFrame(1, Bytes("Hello"));
    ExpectEqual(std::uint16_t{1002}, Detail::ParseClientFrame(unmasked, 1024, frame).closeCode, "unmasked client frame is a protocol error");
    auto fragmentedPing = MaskedFrame(9, "ping", false);
    ExpectTrue(Detail::ParseClientFrame(Bytes(fragmentedPing), 1024, frame).kind == Detail::FrameParseKind::Error,
        "control frames cannot be fragmented");
    const auto large = MaskedFrame(2, std::string(126, 'x'));
    ExpectEqual(std::uint16_t{1009}, Detail::ParseClientFrame(Bytes(large).first(4), 125, frame).closeCode,
        "oversize length is rejected before waiting for payload");
    std::string noncanonical{"\x81\xfe\x00\x01", 4};
    ExpectTrue(Detail::ParseClientFrame(Bytes(noncanonical), 1024, frame).kind == Detail::FrameParseKind::Error,
        "extended frame lengths must use the shortest representation");
    ExpectTrue(!Detail::ValidCloseCode(1006) && !Detail::ValidCloseCode(2000) && Detail::ValidCloseCode(1000),
        "close status validation excludes reserved codes");
}

std::uint16_t FreePort()
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ExpectTrue(socket != ServerCoreTest::InvalidSocket, "port probe socket opens");
    if (socket == ServerCoreTest::InvalidSocket) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool bound = ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ServerCoreTest::SocketLength size = static_cast<ServerCoreTest::SocketLength>(sizeof(address));
    const bool named = bound && ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    ServerCoreTest::CloseSocket(socket);
    ExpectTrue(named, "ephemeral test port is discovered");
    return named ? ntohs(address.sin_port) : 0;
}

class Client
{
public:
    explicit Client(std::uint16_t port)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == ServerCoreTest::InvalidSocket) { ExpectTrue(false, "client socket opens"); return; }
        ExpectTrue(ServerCoreTest::SetSocketTimeouts(socket, 3000), "client socket timeouts are configured");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        ExpectTrue(::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "client connects");
    }
    ~Client() { if (socket != ServerCoreTest::InvalidSocket) ServerCoreTest::CloseSocket(socket); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Send(std::string_view value)
    {
        while (!value.empty())
        {
            const auto count = static_cast<int>((std::min)(value.size(), std::size_t{4096}));
            const int sent = ServerCoreTest::Send(socket, value.data(), count);
            if (sent <= 0) { ExpectTrue(false, "client sends complete request"); return false; }
            value.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    std::string Read(std::size_t count)
    {
        std::string value(count, '\0');
        std::size_t offset = 0;
        while (offset < count)
        {
            const auto wanted = static_cast<int>((std::min)(count - offset, std::size_t{4096}));
            const auto read = ServerCoreTest::Receive(socket, value.data() + offset, wanted);
            if (read <= 0) { ExpectTrue(false, "client reads the expected bytes"); value.resize(offset); return value; }
            offset += static_cast<std::size_t>(read);
        }
        return value;
    }

    std::string Response(bool body = true)
    {
        std::string wire;
        while (!wire.ends_with("\r\n\r\n") && wire.size() < 65536)
        {
            const auto byte = Read(1);
            if (byte.empty()) return wire;
            wire += byte;
        }
        const auto at = wire.find("Content-Length: ");
        if (body && at != std::string::npos)
        {
            const auto first = at + std::string_view("Content-Length: ").size();
            const auto last = wire.find("\r\n", first);
            std::size_t size = 0;
            const auto parsed = std::from_chars(wire.data() + first, wire.data() + last, size);
            ExpectTrue(parsed.ec == std::errc{} && size <= 1024 * 1024, "response length is bounded");
            if (size <= 1024 * 1024) wire += Read(size);
        }
        return wire;
    }

    std::pair<unsigned int, std::string> Frame(bool final = true)
    {
        const auto head = Read(2);
        if (head.size() != 2) return {};
        const auto first = static_cast<unsigned char>(head[0]);
        const auto second = static_cast<unsigned char>(head[1]);
        ExpectTrue((second & 128u) == 0 && ((first & 128u) != 0) == final, "server frame FIN and mask bits match");
        std::uint64_t length = second & 127u;
        if (length >= 126)
        {
            const auto extended = Read(length == 126 ? 2 : 8);
            length = 0;
            for (char byte : extended) length = (length << 8) | static_cast<unsigned char>(byte);
        }
        ExpectTrue(length <= 1024 * 1024, "server frame length is bounded");
        if (length > 1024 * 1024) return {};
        return {first & 15u, Read(static_cast<std::size_t>(length))};
    }

    bool Closed()
    {
        char byte = 0;
        const auto result = ServerCoreTest::Receive(socket, &byte, 1);
        if (result == 0) return true;
        if (result >= 0) return false;
        const int error = ServerCoreTest::LastSocketError();
#ifdef _WIN32
        return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN;
#else
        return error == ECONNRESET || error == ENOTCONN;
#endif
    }
    ServerCoreTest::Socket socket = ServerCoreTest::InvalidSocket;
};

const std::string handshake = "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

void HttpSocketIntegration()
{
    ServerCoreTest::SocketRuntime runtime;
    ExpectTrue(runtime.IsReady(), "socket runtime initializes");
    if (!runtime.IsReady()) return;
    Web::HttpServer server;
    ExpectTrue(server.RegisterRoute("GET", "/health", [](const Web::HttpRequest&) { return Web::HttpResponse{200, {{"Content-Type", "application/json"}}, "{\"ok\":true}"}; }).IsOk(), "health route registers");
    ExpectTrue(server.RegisterRoute("POST", "/echo", [](const Web::HttpRequest& request) { return Web::HttpResponse{200, {}, request.body}; }).IsOk(), "echo route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "HTTP server starts");
    if (!server.IsRunning()) return;
    Client client(server.Port());
    client.Send("GET /health HTTP/1.1\r\nHost: local");
    client.Send("host\r\n\r\nHEAD /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto health = client.Response();
    ExpectTrue(health.starts_with("HTTP/1.1 200 ") && health.ends_with("{\"ok\":true}"), "fragmented request receives route response");
    const auto head = client.Response(false);
    ExpectTrue(head.find("Content-Length: 11\r\n") != std::string::npos && head.ends_with("\r\n\r\n"), "pipelined HEAD has no body");
    client.Send("POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n");
    ExpectTrue(client.Response().starts_with("HTTP/1.1 100 Continue"), "server admits expected request body");
    client.Send("2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n");
    ExpectTrue(client.Response().ends_with("hello"), "real socket chunked request is decoded");
    client.Send("GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    ExpectTrue(client.Response().starts_with("HTTP/1.1 404 ") && client.Closed(), "close response drains before transport shutdown");
    ExpectTrue(server.Stop().IsOk(), "HTTP server stops after transport completion");
}

void WebSocketSocketIntegration()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server;
    std::atomic<unsigned int> closes{0};
    std::atomic<std::uint16_t> closeCode{0};
    Web::WebSocketCallbacks callbacks;
    callbacks.onMessage = [](const std::shared_ptr<Web::WebSocketConnection>& connection, const Web::WebSocketMessage& message)
    {
        const auto result = message.type == Web::WebSocketMessageType::Text ?
            connection->SendText(Text(message.bytes)) : connection->SendBinary(message.bytes);
        ExpectTrue(result.IsOk(), "WebSocket echoes through public send API");
    };
    callbacks.onClose = [&closes, &closeCode](std::uint64_t, std::uint16_t code, std::string_view)
    { closeCode.store(code); closes.fetch_add(1); };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "WebSocket route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "WebSocket server starts");
    if (!server.IsRunning()) return;
    Client client(server.Port());
    client.Send(handshake + MaskedFrame(1, "hel", false) + MaskedFrame(9, "p") + MaskedFrame(0, "lo"));
    const auto response = client.Response();
    ExpectTrue(response.starts_with("HTTP/1.1 101 ") && response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
        "upgrade and an immediately following frame share one stream");
    const auto pong = client.Frame();
    ExpectTrue(pong.first == 10 && pong.second == "p", "ping interleaves with fragmented messages");
    const auto echo = client.Frame();
    ExpectTrue(echo.first == 1 && echo.second == "hello", "continuations form one text callback");
    client.Send(MaskedFrame(2, std::string("\0\xff", 2)));
    const auto binary = client.Frame();
    ExpectTrue(binary.first == 2 && binary.second == std::string("\0\xff", 2), "binary messages preserve all bytes");
    client.Send(MaskedFrame(8, std::string("\x03\xe8", 2)));
    const auto closed = client.Frame();
    ExpectTrue(closed.first == 8 && closed.second == std::string("\x03\xe8", 2), "received close frame is acknowledged");
    ExpectTrue(client.Closed(), "closing handshake ends transport");
    ExpectTrue(server.Stop().IsOk(), "WebSocket server stops");
    ExpectTrue(closes.load() == 1 && closeCode.load() == 1000, "onClose runs once with peer close status");
}

void HttpResponseSemantics()
{
    using namespace std::chrono;
    ExpectEqual(std::string("Thu, 01 Jan 1970 00:00:00 GMT"), Detail::HttpDate(system_clock::time_point{}),
        "HTTP dates use UTC with fixed English names");
    ExpectEqual(std::string("Tue, 29 Feb 2000 23:59:58 GMT"),
        Detail::HttpDate(sys_days{year{2000}/2/29} + hours{23} + minutes{59} + seconds{58}),
        "HTTP date formatting handles calendar boundaries without locale or shared tm state");
    for (unsigned int status : {204u, 205u, 304u})
    {
        Web::HttpResponse response{status, {}, "must not be sent"};
        std::string encoded;
        ExpectTrue(Detail::SerializeResponse(response, false, false, 1024, 1024, encoded), "content-free status serializes");
        ExpectTrue(encoded.ends_with("\r\n\r\n"), "content-free status never writes payload bytes");
        if (status == 205)
            ExpectTrue(encoded.find("Content-Length: 0\r\n") != std::string::npos, "205 has empty persistent-connection framing");
        else
            ExpectTrue(encoded.find("Content-Length:") == std::string::npos, "204 and unknown-length 304 omit content length");
        ExpectTrue(encoded.find("\r\nDate: ") != std::string::npos, "ordinary responses get a Date header");
    }
    Web::HttpResponse suppliedDate{200, {{"Date", "Thu, 01 Jan 1970 00:00:00 GMT"}}, "ok"};
    std::string dated;
    ExpectTrue(Detail::SerializeResponse(suppliedDate, false, false, 1024, 1024, dated) &&
        dated.find("Date:") == dated.rfind("Date:"), "application Date is preserved without a duplicate");
    suppliedDate.headers.emplace_back("date", "Thu, 01 Jan 1970 00:00:00 GMT");
    ExpectTrue(!Detail::SerializeResponse(suppliedDate, false, false, 1024, 1024, dated), "duplicate Date fields are rejected");
    Web::HttpResponse upgradeRequired{426, {}, {}};
    ExpectTrue(!Detail::SerializeResponse(upgradeRequired, false, true, 1024, 1024, dated),
        "426 requires an Upgrade field identifying acceptable protocols");
    upgradeRequired.headers.emplace_back("Upgrade", "websocket");
    ExpectTrue(Detail::SerializeResponse(upgradeRequired, false, true, 1024, 1024, dated) &&
        dated.find("Connection: close, Upgrade\r\n") != std::string::npos,
        "Upgrade advertisements generate their required connection option");
    upgradeRequired.headers.front().second = "websocket/";
    ExpectTrue(!Detail::SerializeResponse(upgradeRequired, false, true, 1024, 1024, dated),
        "malformed upgrade protocol versions are rejected on output");
    Detail::HttpParser upgradeParser(1024, 1024);
    Web::HttpRequest upgradeRequest;
    std::string badUpgrade = "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket/\r\n\r\n";
    ExpectTrue(upgradeParser.Parse(badUpgrade, upgradeRequest).kind == Detail::HttpParseKind::Error,
        "input and output share Upgrade protocol validation");

    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server;
    ExpectTrue(server.RegisterRoute("GET", "/throw", [](const Web::HttpRequest&) -> Web::HttpResponse
    { throw std::runtime_error("test handler failure"); }).IsOk(), "throwing route registers");
    ExpectTrue(server.RegisterRoute("GET", "/reset", [](const Web::HttpRequest&)
    { return Web::HttpResponse{205, {}, "ignored"}; }).IsOk(), "205 route registers");
    ExpectTrue(server.RegisterRoute("GET", "/cached", [](const Web::HttpRequest&)
    { return Web::HttpResponse{304, {}, {}}; }).IsOk(), "304 route registers");
    ExpectTrue(server.RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
    { return Web::HttpResponse{200, {}, "ready"}; }).IsOk(), "semantic probe route registers");
    ExpectTrue(server.RegisterWebSocket("/ws", {}).IsOk(), "HEAD upgrade-error route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxHeaderBytes = 256;
    options.maxBodyBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "response-semantics server starts");
    if (!server.IsRunning()) return;
    const std::array<std::pair<std::string, unsigned int>, 6> failures{{
        {"HEAD /throw HTTP/1.1\r\nHost: x\r\n\r\n", 500u},
        {"HEAD /ws HTTP/1.1\r\nHost: x\r\n\r\n", 400u},
        {"HEAD / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", 413u},
        {"HEAD / HTTP/1.1\r\nHost: x\r\nX: " + std::string(256, 'a'), 431u},
        {"HEAD / HTTP/1.0\r\nHost: x\r\n\r\n", 505u},
        {"HEAD /unknown HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n", 404u}
    }};
    for (const auto& [wire, status] : failures)
    {
        Client client(server.Port());
        client.Send(wire);
        const auto response = client.Response(false);
        ExpectTrue(response.starts_with("HTTP/1.1 " + std::to_string(status) + " "), "HEAD error preserves its status");
        ExpectTrue(client.Closed(), "HEAD errors end at the header terminator, without payload bytes");
    }
    {
        Client client(server.Port());
        std::string oldVersion = handshake;
        const auto version = oldVersion.find("Version: 13");
        oldVersion.replace(version, std::string_view("Version: 13").size(), "Version: 12");
        client.Send(oldVersion);
        const auto response = client.Response();
        ExpectTrue(response.starts_with("HTTP/1.1 426 ") &&
            response.find("\r\nUpgrade: websocket\r\n") != std::string::npos &&
            response.find("\r\nSec-WebSocket-Version: 13\r\n") != std::string::npos &&
            response.find("Connection: close, Upgrade\r\n") != std::string::npos,
            "unsupported WebSocket version gets a complete HTTP upgrade-required response");
    }
    Client pipeline(server.Port());
    pipeline.Send("HEAD /health HTTP/1.1\r\nHost: x\r\n\r\nGET /reset HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /cached HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    ExpectTrue(pipeline.Response(false).starts_with("HTTP/1.1 200 "), "HEAD response completes in the pipeline");
    ExpectTrue(pipeline.Response().starts_with("HTTP/1.1 205 "), "205 keeps the next response boundary intact");
    ExpectTrue(pipeline.Response().starts_with("HTTP/1.1 304 "), "304 keeps the next response boundary intact");
    ExpectTrue(pipeline.Response().ends_with("ready"), "HEAD state resets before a subsequent GET");
    Client errorPipeline(server.Port());
    errorPipeline.Send("HEAD /health HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
    ExpectTrue(errorPipeline.Response(false).starts_with("HTTP/1.1 200 "), "HEAD finishes before a malformed pipelined GET");
    const auto getError = errorPipeline.Response();
    ExpectTrue(getError.starts_with("HTTP/1.1 413 ") && getError.ends_with("Content Too Large\n"),
        "a prior HEAD does not suppress a subsequent GET parser-error body");
    ExpectTrue(server.Stop().IsOk(), "response-semantics server stops");
}

void WebSocketSendValidationSymmetry()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server;
    std::shared_ptr<Web::WebSocketConnection> retained;
    std::atomic<bool> opened{false};
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&retained, &opened](const std::shared_ptr<Web::WebSocketConnection>& connection)
    {
        using ServerCore::Core::ErrorCode;
        const std::string oversized(5, '\xff');
        ExpectTrue(connection->SendText(oversized).Code() == ErrorCode::TooLarge,
            "outgoing text checks its size before UTF-8");
        ExpectTrue(connection->SendBinary(Bytes(oversized)).Code() == ErrorCode::TooLarge,
            "binary and text share the same data-frame bound");
        ExpectTrue(connection->SendText(std::string("\xc0\xaf", 2)).Code() == ErrorCode::InvalidArgument,
            "bounded malformed caller text reports InvalidArgument");
        ExpectTrue(connection->Ping(Bytes(std::string(126, 'x'))).Code() == ErrorCode::TooLarge,
            "outgoing controls enforce their independent 125-byte bound");
        ExpectTrue(connection->Close(1000, std::string(124, '\xff')).Code() == ErrorCode::TooLarge,
            "close reason length is checked before UTF-8");
        ExpectTrue(connection->Close(1000, std::string("\xc0\xaf", 2)).Code() == ErrorCode::InvalidArgument,
            "malformed caller text and close reasons share InvalidArgument semantics");
        ExpectTrue(connection->Close(1010, {}).Code() == ErrorCode::InvalidArgument,
            "server cannot send a client-only close code");
        ExpectTrue(connection->IsOpen() && connection->SendText("ok").IsOk(),
            "rejected application payloads do not close or corrupt the connection");
        retained = connection;
        opened.store(true);
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "send-validation route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxWebSocketFrameBytes = 4;
    options.maxWebSocketMessageBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "send-validation server starts");
    if (!server.IsRunning()) return;
    Client client(server.Port());
    client.Send(handshake);
    ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "send-validation client upgrades");
    ExpectTrue(client.Frame().second == "ok", "only the accepted application frame reaches the peer");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!opened.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ExpectTrue(opened.load(), "onOpen publishes its retained connection");
    ExpectTrue(server.Stop().IsOk(), "send-validation server stops");
    if (retained)
    {
        using ServerCore::Core::ErrorCode;
        const std::string oversized(126, '\xff');
        ExpectTrue(retained->SendText(oversized).Code() == ErrorCode::Closed &&
            retained->SendBinary(Bytes(oversized)).Code() == ErrorCode::Closed &&
            retained->Ping(Bytes(oversized)).Code() == ErrorCode::Closed &&
            retained->Close(999, oversized).Code() == ErrorCode::Closed,
            "all WebSocket send operations report closed state before payload errors");
    }
}

void GlobalSendBudget()
{
    using ServerCore::Core::ErrorCode;
    const std::string payload(256, 'b');
    for (const std::size_t capacity : {std::size_t{64}, std::size_t{1024}})
    {
        std::atomic<unsigned int> requests{0};
        std::atomic<unsigned int> upgrades{0};
        std::atomic<unsigned int> opens{0};
        std::atomic<unsigned int> closes{0};
        std::atomic<ErrorCode> sent{ErrorCode::WouldBlock};
        Web::HttpServer server;
        ExpectTrue(server.RegisterRoute("GET", "/budget", [&](const Web::HttpRequest&)
        {
            requests.fetch_add(1);
            return Web::HttpResponse{200, {}, payload, true};
        }).IsOk(), "send-budget HTTP route registers");
        Web::WebSocketCallbacks callbacks;
        callbacks.accept = [&upgrades](const Web::HttpRequest&)
        {
            upgrades.fetch_add(1);
            return true;
        };
        callbacks.onOpen = [&](const std::shared_ptr<Web::WebSocketConnection>& connection)
        {
            opens.fetch_add(1);
            sent.store(connection->SendText(payload).Code());
        };
        callbacks.onClose = [&closes](std::uint64_t, std::uint16_t, std::string_view) { closes.fetch_add(1); };
        ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "send-budget WebSocket route registers");
        Web::HttpServerOptions options;
        options.port = FreePort();
        options.maxTotalSendQueueCapacityBytes = 0;
        ExpectTrue(server.Start(options).Code() == ErrorCode::InvalidArgument,
            "zero total send budget is invalid");
        options.maxTotalSendQueueCapacityBytes = std::size_t{512} * 1024 * 1024 + 1;
        ExpectTrue(server.Start(options).Code() == ErrorCode::InvalidArgument,
            "total send budget above 512 MiB is invalid");
        options.maxTotalSendQueueCapacityBytes = capacity;
        ExpectTrue(server.Start(options).IsOk(), "valid send budget starts after rejected configurations");
        if (!server.IsRunning()) continue;
        {
            Client client(server.Port());
            client.Send("GET /budget HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            if (capacity == 64)
                ExpectTrue(client.Closed(), "HTTP wire larger than the total budget is rejected before any bytes are sent");
            else
            {
                const auto response = client.Response();
                ExpectTrue(response.starts_with("HTTP/1.1 200 ") && response.ends_with(payload),
                    "HTTP response within the configured total budget is delivered");
                ExpectTrue(client.Closed(), "successful close response drains normally");
            }
        }
        {
            Client client(server.Port());
            client.Send(handshake);
            if (capacity == 64)
                ExpectTrue(client.Closed(), "WebSocket handshake also honors the configured total send budget");
            else
            {
                ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "WebSocket handshake fits the larger total budget");
                const auto frame = client.Frame();
                ExpectTrue(frame.first == 1 && frame.second == payload,
                    "WebSocket application frame fits the larger total budget");
            }
        }
        ExpectTrue(server.Stop().IsOk(), "send-budget server stops");
        ExpectTrue(requests.load() == 1 && upgrades.load() == 1,
            "budget checks run after valid HTTP routing and WebSocket acceptance");
        ExpectTrue(opens.load() == (capacity == 64 ? 0u : 1u) && closes.load() == opens.load(),
            "rejected upgrade has no open or close callback, accepted upgrade closes once");
        ExpectTrue(sent.load() == (capacity == 64 ? ErrorCode::WouldBlock : ErrorCode::Ok),
            "application WebSocket send only runs after a successful handshake");
    }
}

void LimitsAndShutdown()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server;
    std::atomic<bool> selfStopRejected{false};
    std::atomic<unsigned int> closes{0};
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&server, &selfStopRejected](const std::shared_ptr<Web::WebSocketConnection>& connection)
    {
        selfStopRejected.store(server.Stop().Code() == ServerCore::Core::ErrorCode::InvalidArgument);
        ExpectTrue(connection->Close(1000, "done").IsOk(), "server can initiate a closing handshake");
    };
    callbacks.onClose = [&closes](std::uint64_t, std::uint16_t, std::string_view) { closes.fetch_add(1); };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "bounded WebSocket route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.requestTimeout = std::chrono::milliseconds(100);
    options.webSocketCloseTimeout = std::chrono::milliseconds(100);
    options.idleTimeout = std::chrono::milliseconds(2000);
    options.maxHeaderBytes = 256;
    options.maxBodyBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "bounded server starts");
    if (!server.IsRunning()) return;
    {
        Client slow(server.Port());
        slow.Send("GET / HTTP/1.1\r\n");
        ExpectTrue(slow.Closed(), "incomplete HTTP request expires");
    }
    {
        Client bad(server.Port());
        bad.Send("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
        ExpectTrue(bad.Response().starts_with("HTTP/1.1 413 "), "declared oversize body is rejected immediately");
    }
    {
        Client socket(server.Port());
        socket.Send(handshake);
        ExpectTrue(socket.Response().starts_with("HTTP/1.1 101 "), "close-timeout test upgrades");
        ExpectTrue(socket.Frame().first == 8, "server close frame is sent");
        ExpectTrue(socket.Closed(), "unanswered closing handshake expires");
    }
    ExpectTrue(server.Stop().IsOk(), "bounded server shuts down");
    ExpectTrue(selfStopRejected.load() && closes.load() == 1, "callback self-join is rejected and close is delivered once");
    ExpectTrue(server.Stop().IsOk() && !server.Start(options).IsOk(), "stop is idempotent and host is single-use");
    GlobalSendBudget();
}

void WebSocketRejectsProtocolErrors()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server;
    ExpectTrue(server.RegisterWebSocket("/ws", {}).IsOk(), "protocol-validation route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxWebSocketFrameBytes = 4;
    options.maxWebSocketMessageBytes = 6;
    ExpectTrue(server.Start(options).IsOk(), "protocol-validation server starts");
    if (!server.IsRunning()) return;
    const std::array<std::pair<std::string, std::uint16_t>, 6> malformed{{
        {Text(Detail::EncodeServerFrame(1, Bytes("x"))), std::uint16_t{1002}},
        {MaskedFrame(1, std::string("\xc0\xaf", 2)), std::uint16_t{1007}},
        {MaskedFrame(0, "x"), std::uint16_t{1002}},
        {MaskedFrame(2, "12345"), std::uint16_t{1009}},
        {MaskedFrame(1, "abcd", false) + MaskedFrame(0, "efg"), std::uint16_t{1009}},
        {MaskedFrame(8, std::string("\x03\xed", 2)), std::uint16_t{1002}}
    }};
    for (const auto& [wire, expectedCode] : malformed)
    {
        Client client(server.Port());
        client.Send(handshake);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "invalid-frame test upgrades");
        client.Send(wire);
        const auto frame = client.Frame();
        ExpectTrue(frame.first == 8 && frame.second.size() >= 2, "invalid frame receives a close frame");
        if (frame.second.size() >= 2)
        {
            const auto code = static_cast<std::uint16_t>((static_cast<unsigned char>(frame.second[0]) << 8) |
                static_cast<unsigned char>(frame.second[1]));
            ExpectEqual(expectedCode, code, "close frame explains invalid client data");
        }
        ExpectTrue(client.Closed(), "protocol failure drains and closes the connection");
    }
    ExpectTrue(server.Stop().IsOk(), "protocol-validation server stops");
}

void ConnectionLimitAndActiveShutdown()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server;
    std::atomic<unsigned int> closes{0};
    Web::WebSocketCallbacks callbacks;
    callbacks.onClose = [&closes](std::uint64_t, std::uint16_t code, std::string_view)
    {
        ExpectEqual(std::uint16_t{1006}, code, "forced server shutdown reports an abnormal peer closure");
        closes.fetch_add(1);
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "limited route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxConnections = 1;
    ExpectTrue(server.Start(options).IsOk(), "one-connection server starts");
    if (!server.IsRunning()) return;
    Client active(server.Port());
    active.Send(handshake);
    ExpectTrue(active.Response().starts_with("HTTP/1.1 101 "), "first connection is admitted");
    Client excess(server.Port());
    ExpectTrue(excess.Closed(), "connection limit rejects additional sockets");
    ExpectTrue(server.Stop().IsOk(), "server stops with an active WebSocket");
    ExpectTrue(closes.load() == 1 && active.Closed(), "Stop waits for onClose and socket completion");

    // A peer must not retain the only slot by sending bytes while refusing to
    // read a large closing response. An absolute drain deadline wins over idle.
    Web::HttpServer draining;
    std::atomic<bool> largeRequest{false};
    ExpectTrue(draining.RegisterRoute("GET", "/large", [&largeRequest](const Web::HttpRequest&)
    {
        largeRequest.store(true);
        return Web::HttpResponse{200, {}, std::string(900 * 1024, 'x'), true};
    }).IsOk(), "large close-response route registers");
    ExpectTrue(draining.RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
    { return Web::HttpResponse{200, {}, "ready", true}; }).IsOk(), "drain probe route registers");
    options.port = FreePort();
    options.maxResponseBodyBytes = 900 * 1024;
    options.responseDrainTimeout = std::chrono::milliseconds(100);
    options.idleTimeout = std::chrono::milliseconds(1000);
    ExpectTrue(draining.Start(options).IsOk(), "drain-deadline server starts");
    if (!draining.IsRunning()) return;
    Client unread(draining.Port());
    const int receiveBuffer = 1024;
    ExpectTrue(::setsockopt(unread.socket, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer)) == 0,
        "small receive window bounds transport progress");
    unread.Send("GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    const auto requestDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!largeRequest.load() && std::chrono::steady_clock::now() < requestDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ExpectTrue(largeRequest.load(), "large response starts before the deadline probe");
    const auto trickleUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    while (std::chrono::steady_clock::now() < trickleUntil)
    {
        (void)ServerCoreTest::Send(unread.socket, "x", 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Client probe(draining.Port());
    probe.Send("GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    ExpectTrue(probe.Response().ends_with("ready"), "absolute response drain deadline releases the connection slot despite trickle input");
    ExpectTrue(draining.Stop().IsOk(), "drain-deadline server stops");
}

void WebSocketNegotiationAndFragments()
{
    using ServerCore::Core::ErrorCode;
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server;
    Web::WebSocketCallbacks invalid; invalid.subprotocols = {"same", "same"};
    ExpectTrue(server.RegisterWebSocket("/bad", invalid).Code() == ErrorCode::InvalidArgument, "duplicate supported protocol rejects registration");
    invalid.subprotocols = {"not a token"};
    ExpectTrue(server.RegisterWebSocket("/bad", invalid).Code() == ErrorCode::InvalidArgument, "invalid supported protocol rejects registration");
    Web::WebSocketCallbacks callbacks;
    callbacks.subprotocols = {"v2", "v1"};
    callbacks.onOpen = [](const auto& socket) {
        ExpectTrue(socket->SendText(Web::GetWebSocketMessageControl(socket)->Subprotocol()).IsOk(), "negotiated protocol is exposed");
    };
    ExpectTrue(server.RegisterWebSocket("/ws", callbacks).IsOk(), "subprotocol route registers");
    Web::WebSocketCallbacks fragmented;
    fragmented.onOpen = [](const auto& socket) {
        auto control = Web::GetWebSocketMessageControl(socket);
        auto result = control->BeginMessage(Web::WebSocketMessageType::Text);
        ExpectTrue(result.IsOk(), "text writer reserves the message lane"); if (!result.IsOk()) return;
        auto writer = std::move(result).Value();
        ExpectTrue(writer->Write(Bytes(std::string("A\xf0", 2)), false).IsOk(), "UTF8 prefix can span fragments");
        ExpectTrue(socket->SendText("x").Code() == ErrorCode::AlreadyExists, "ordinary data cannot interleave");
        ExpectTrue(control->BeginMessage(Web::WebSocketMessageType::Binary).GetStatus().Code() == ErrorCode::AlreadyExists,
            "another writer cannot interleave");
        ExpectTrue(socket->Ping(Bytes("p")).IsOk(), "control frame may interleave");
        ExpectTrue(writer->Write(Bytes("x"), true).Code() == ErrorCode::InvalidArgument, "invalid continuation changes no state");
        ExpectTrue(writer->Write(Bytes(std::string("\x9f\x98\x80" "B", 4)), true).IsOk(), "valid UTF8 continuation completes");
        ExpectTrue(writer->Write({}, true).Code() == ErrorCode::Closed, "finished writer cannot send twice");
        writer = std::move(control->BeginMessage(Web::WebSocketMessageType::Binary)).Value();
        ExpectTrue(writer->Write(Bytes("1234"), false).IsOk(), "binary writer starts");
        ExpectTrue(writer->Write(Bytes("567"), true).Code() == ErrorCode::TooLarge, "total message size is bounded");
        ExpectTrue(writer->Write(Bytes("56"), true).IsOk(), "oversized attempt did not mutate message size");
        ExpectTrue(socket->SendText("end").IsOk(), "finished writer releases data lane");
    };
    ExpectTrue(server.RegisterWebSocket("/fragments", std::move(fragmented)).IsOk(), "fragment route registers");
    Web::WebSocketCallbacks aborted;
    aborted.onMessage = [](const auto& socket, const auto&) {
        auto writer = std::move(Web::GetWebSocketMessageControl(socket)->BeginMessage(Web::WebSocketMessageType::Binary)).Value();
        ExpectTrue(writer->Write(Bytes("a"), false).IsOk(), "abandoned writer admits initial fragment");
    };
    ExpectTrue(server.RegisterWebSocket("/abandon", std::move(aborted)).IsOk(), "abandon route registers");
    Web::HttpServerOptions options; options.port = FreePort(); options.maxWebSocketFrameBytes = 4; options.maxWebSocketMessageBytes = 6;
    ExpectTrue(server.Start(options).IsOk(), "WebSocket feature server starts"); if (!server.IsRunning()) return;
    for (const auto offered : {std::string("Sec-WebSocket-Protocol: v1, v2\r\n"),
        std::string("Sec-WebSocket-Protocol: v1\r\nSec-WebSocket-Protocol: v2\r\n"), std::string("Sec-WebSocket-Protocol: other\r\n"), std::string{}})
    {
        Client client(server.Port()); auto request = handshake; request.insert(request.size() - 2, offered); client.Send(request);
        const auto response = client.Response(); const bool matched = offered.find("v1") != std::string::npos;
        ExpectTrue(response.starts_with("HTTP/1.1 101 "), "valid offered protocols upgrade");
        ExpectTrue((response.find("Sec-WebSocket-Protocol: v2\r\n") != std::string::npos) == matched, "server preference wins and no match omits header");
        ExpectEqual(matched ? std::string("v2") : std::string{}, client.Frame().second, "connection reports selected protocol");
    }
    for (const auto offered : {"Sec-WebSocket-Protocol: v1, v1\r\n", "Sec-WebSocket-Protocol: v1\r\nSec-WebSocket-Protocol: v1\r\n",
        "Sec-WebSocket-Protocol: bad token\r\n", "Sec-WebSocket-Protocol: v1,\r\n"})
    {
        Client client(server.Port()); auto request = handshake; request.insert(request.size() - 2, offered); client.Send(request);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 400 "), "invalid/duplicate offered protocol rejects handshake");
    }
    {
        Client client(server.Port()); auto request = handshake; request.replace(4, 3, "/fragments"); client.Send(request);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "fragment connection upgrades");
        auto frame = client.Frame(false); ExpectTrue(frame.first == 1 && frame.second == std::string("A\xf0", 2), "first frame has Text opcode and FIN clear");
        frame = client.Frame(); ExpectTrue(frame.first == 9 && frame.second == "p", "Ping interleaves between data fragments");
        frame = client.Frame(); ExpectTrue(frame.first == 0 && frame.second == std::string("\x9f\x98\x80" "B", 4), "last frame is final continuation");
        frame = client.Frame(false); ExpectTrue(frame.first == 2 && frame.second == "1234", "binary message starts with Binary opcode");
        frame = client.Frame(); ExpectTrue(frame.first == 0 && frame.second == "56", "message byte limit includes previous frames");
        ExpectEqual(std::string("end"), client.Frame().second, "ordinary sends resume after final fragment");
    }
    {
        Client client(server.Port()); auto request = handshake; request.replace(4, 3, "/abandon"); client.Send(request);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "abandon connection upgrades before abort trigger");
        client.Send(MaskedFrame(1, "go"));
        char buffer[64]; while (ServerCoreTest::Receive(client.socket, buffer, sizeof(buffer)) > 0) {}
        ExpectTrue(client.Closed(), "dropping a partially written message closes transport");
    }
    ExpectTrue(server.Stop().IsOk(), "feature server stops");
    for (const auto invalidText : {std::string("\xe0\x80", 2), std::string("\xed\xa0", 2), std::string("\xf4\x90", 2), std::string("\xff", 1)})
    {
        Detail::Utf8FragmentState state;
        ExpectTrue(!state.Append(Bytes(invalidText), false), "invalid partial scalar is rejected before wire admission");
    }
}

void WebSocketHeartbeatCorrelation()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    Web::HttpServer server; ExpectTrue(server.RegisterWebSocket("/ws", {}).IsOk(), "heartbeat route registers");
    Web::HttpServerOptions options; options.port = FreePort(); options.webSocketPingInterval = std::chrono::milliseconds(30);
    options.webSocketPongTimeout = std::chrono::milliseconds(120); options.webSocketCloseTimeout = std::chrono::milliseconds(80);
    ExpectTrue(server.Start(options).IsOk(), "heartbeat server starts"); if (!server.IsRunning()) return;
    Client client(server.Port()); client.Send(handshake); ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "heartbeat connection upgrades");
    const auto first = client.Frame(); ExpectTrue(first.first == 9 && first.second.size() == 16, "automatic ping carries correlation payload");
    client.Send(MaskedFrame(10, first.second));
    const auto second = client.Frame(); ExpectTrue(second.first == 9 && second.second != first.second, "matching pong permits next distinct heartbeat");
    client.Send(MaskedFrame(10, first.second));
    const auto close = client.Frame(); ExpectTrue(close.first == 8 && close.second.size() >= 2 && close.second.substr(0, 2) == std::string("\x03\xe9", 2),
        "stale pong does not postpone heartbeat timeout");
    ExpectTrue(client.Closed(), "unresponsive peer closes within close deadline");
    ExpectTrue(server.Stop().IsOk(), "heartbeat server stops");
}

void WebSocketFragmentBackpressure()
{
    using ServerCore::Core::ErrorCode;
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady()) { ExpectTrue(false, "socket runtime initializes"); return; }
    std::mutex mutex; std::condition_variable changed;
    std::shared_ptr<Web::WebSocketMessageWriter> writer;
    std::size_t admitted = 0; bool blocked = false, finished = false;
    Web::HttpServer server;
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&](const auto& socket) {
        auto created = Web::GetWebSocketMessageControl(socket)->BeginMessage(Web::WebSocketMessageType::Binary);
        if (!created.IsOk()) { std::lock_guard lock(mutex); finished = true; changed.notify_all(); return; }
        auto current = std::move(created).Value(); const std::string payload(64 * 1024, 'x');
        std::size_t count = 0; bool pressure = false;
        for (; count < 128; ++count) {
            const auto result = current->Write(Bytes(payload), false);
            if (!result.IsOk()) { pressure = result.Code() == ErrorCode::WouldBlock; break; }
        }
        { std::lock_guard lock(mutex); writer = std::move(current); admitted = count; blocked = pressure; finished = true; }
        changed.notify_all();
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "backpressure route registers");
    Web::HttpServerOptions options; options.port = FreePort(); options.maxWebSocketMessageBytes = 16 * 1024 * 1024;
    ExpectTrue(server.Start(options).IsOk(), "fragment pressure server starts"); if (!server.IsRunning()) return;
    Client client(server.Port()); const int receiveBytes = 64 * 1024;
    ExpectTrue(::setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBytes), sizeof(receiveBytes)) == 0,
        "small receive window stops unbounded transport progress");
    client.Send(handshake); ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "pressure connection upgrades");
    bool completed = false;
    {
        std::unique_lock lock(mutex);
        completed = changed.wait_for(lock, std::chrono::seconds(3), [&] { return finished; });
        ExpectTrue(completed, "bounded producer reports capacity pressure");
    }
    if (!completed) { (void)server.Stop(); return; }
    ExpectTrue(blocked && admitted != 0 && writer, "unread peer yields WouldBlock before message limit");
    if (!writer) { (void)server.Stop(); return; }
    std::atomic<bool> ready{false};
    auto wait = writer->WaitForWriteCapacity(64 * 1024, [&](auto status) { ready.store(status.IsOk()); });
    ExpectTrue(wait.IsOk(), "fragment capacity notification registers");
    for (std::size_t index = 0; index < admitted; ++index) {
        const auto frame = client.Frame(false);
        ExpectTrue(frame.first == (index == 0 ? 2u : 0u) && frame.second == std::string(64 * 1024, 'x'),
            "each successful write appears exactly once");
        if (frame.second.empty()) break;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ExpectTrue(ready.load(), "draining frames signals send capacity");
    if (wait.IsOk()) wait.Value().Reset();
    ExpectTrue(writer->Write(Bytes("done"), true).IsOk(), "retry after pressure completes same message");
    const auto last = client.Frame(); ExpectTrue(last.first == 0 && last.second == "done", "rejected frame added no hidden bytes");
    writer.reset();
    ExpectTrue(server.Stop().IsOk(), "pressure server stops");
}

ServerCoreTest::CheckRegistration wsFeatures("Web.WebSocketNegotiationAndFragments", &WebSocketNegotiationAndFragments);
ServerCoreTest::CheckRegistration wsHeartbeat("Web.WebSocketHeartbeatCorrelation", &WebSocketHeartbeatCorrelation);
ServerCoreTest::CheckRegistration wsPressure("Web.WebSocketFragmentBackpressure", &WebSocketFragmentBackpressure);
ServerCoreTest::CheckRegistration httpParser("Web.HttpParserConformance", &HttpParserConformance);
ServerCoreTest::CheckRegistration httpRejected("Web.HttpParserRejectsAmbiguousRequests", &HttpParserRejectsAmbiguousRequests);
ServerCoreTest::CheckRegistration wsProtocol("Web.WebSocketProtocolConformance", &WebSocketProtocolConformance);
ServerCoreTest::CheckRegistration httpSocket("Web.HttpSocketIntegration", &HttpSocketIntegration);
ServerCoreTest::CheckRegistration wsSocket("Web.WebSocketSocketIntegration", &WebSocketSocketIntegration);
ServerCoreTest::CheckRegistration limits("Web.LimitsAndShutdown", &LimitsAndShutdown);
ServerCoreTest::CheckRegistration rejectedFrames("Web.WebSocketRejectsProtocolErrors", &WebSocketRejectsProtocolErrors);
ServerCoreTest::CheckRegistration connectionLimit("Web.ConnectionLimitAndActiveShutdown", &ConnectionLimitAndActiveShutdown);
ServerCoreTest::CheckRegistration responseSemantics("Web.HttpResponseSemantics", &HttpResponseSemantics);
ServerCoreTest::CheckRegistration sendValidation("Web.WebSocketSendValidationSymmetry", &WebSocketSendValidationSymmetry);
}
