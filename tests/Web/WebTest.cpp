#include "../../src/Web/WebProtocol.h"
#include "ServerCore/Web/HttpPolicy.h"
#include "ServerCore/Web/HttpServer.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
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
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;

std::span<const std::byte> Bytes(std::string_view value)
{
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::string Text(std::span<const std::byte> value)
{
    return value.empty() ? std::string{}
                         : std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string MaskedFrame(std::uint8_t opcode, std::string_view payload, bool final = true)
{
    auto wire = Detail::EncodeServerFrame(opcode, Bytes(payload));
    if (!final)
        wire[0] &= std::byte{ 0x7f };
    wire[1] |= std::byte{ 0x80 };
    const std::size_t header = payload.size() < 126 ? 2 : payload.size() <= 65535 ? 4 : 10;
    constexpr std::array mask{ std::byte{ 0x37 }, std::byte{ 0xfa }, std::byte{ 0x21 },
        std::byte{ 0x3d } };
    wire.insert(wire.begin() + static_cast<std::ptrdiff_t>(header), mask.begin(), mask.end());
    for (std::size_t index = 0; index < payload.size(); ++index)
        wire[header + 4 + index] ^= mask[index % 4];
    return Text(wire);
}

void HttpParserConformance()
{
    Detail::HttpParser parser(1024, 1024);
    const std::string wire =
        "POST /echo?q=1 HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nExpect: "
        "100-continue\r\n\r\n"
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
        ExpectTrue(
            result.kind != Detail::HttpParseKind::Error, "split chunked request remains valid");
        if (result.kind == Detail::HttpParseKind::Complete)
            complete = true;
    }
    ExpectTrue(continued && complete, "100-continue and complete request are reported");
    ExpectEqual(std::string("hello"), request.body, "chunks are decoded into one body");
    ExpectEqual(
        std::string("/echo"), std::string(request.Path()), "routing excludes the query string");
    ExpectTrue(request.Header("x-checksum").empty(), "trailers do not overwrite request headers");
    pending = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\n";
    pending.append("a\0b", 3);
    pending += "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    ExpectTrue(parser.Parse(pending, request).kind == Detail::HttpParseKind::Complete,
        "binary body completes");
    ExpectEqual(std::size_t{ 3 }, request.body.size(), "NUL remains part of the HTTP body");
    ExpectTrue(parser.Parse(pending, request).kind == Detail::HttpParseKind::Complete,
        "pipelined request completes");
    ExpectEqual(std::string("/next"), request.target, "pipeline preserves request boundaries");
    Web::HttpResponse response;
    response.body = "hello";
    std::string encoded;
    ExpectTrue(Detail::SerializeResponse(response, true, false, 1024, 1024, encoded),
        "HEAD response serializes");
    ExpectTrue(
        encoded.find("Content-Length: 5\r\n") != std::string::npos && encoded.ends_with("\r\n\r\n"),
        "HEAD reports representation length without a payload");
}

/// <summary>
/// 파서가 호출자의 버퍼를 고치지 않고, 처리한 바이트 수만 돌려주는지 본다(WEB-1).
/// </summary>
/// <remarks>
/// 이 시험은 소비 바이트 수의 계약을 고정할 뿐, 복사량이 입력에 비례한다는 것을 재지 않는다.
/// 그 성질을 지키는 것은 커서 오버로드의 입력 타입(string_view)이다. 벽시계 시간으로는 판정하지 않는다.
/// </remarks>
void HttpParserReportsConsumedBytes()
{
    std::string first = "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (std::size_t index = 0; index < 2730; ++index)
        first += "1\r\nx\r\n";
    first += "0\r\n\r\n";
    const std::string second = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string wire = first + second;
    {
        Detail::HttpParser parser(64 * 1024, 64 * 1024);
        Web::HttpRequest request;
        std::size_t consumed = 0;
        auto result = parser.Parse(std::string_view(wire), consumed, request);
        ExpectTrue(result.kind == Detail::HttpParseKind::Complete &&
                       request.body == std::string(2730, 'x'),
            "one call decodes every one-byte chunk");
        ExpectEqual(first.size(), consumed, "consumed stops at the first request boundary");
        result = parser.Parse(std::string_view(wire).substr(consumed), consumed, request);
        ExpectTrue(result.kind == Detail::HttpParseKind::Complete && request.target == "/next",
            "the unconsumed remainder parses as the pipelined request");
        ExpectEqual(second.size(), consumed, "second call consumes exactly the pipelined request");
    }
    {
        // Bytes arrive one at a time; the caller keeps one buffer and only advances its offset.
        Detail::HttpParser parser(64 * 1024, 64 * 1024);
        Web::HttpRequest request;
        std::size_t offset = 0;
        std::vector<std::string> targets;
        for (std::size_t end = 1; end <= wire.size(); ++end)
        {
            std::size_t consumed = 0;
            const auto result = parser.Parse(
                std::string_view(wire).substr(offset, end - offset), consumed, request);
            offset += consumed;
            ExpectTrue(
                result.kind != Detail::HttpParseKind::Error, "byte-by-byte arrival stays valid");
            if (result.kind == Detail::HttpParseKind::Complete)
                targets.push_back(request.target);
        }
        ExpectEqual(wire.size(), offset, "every arrived byte is eventually consumed");
        ExpectTrue(targets == std::vector<std::string>{ "/upload", "/next" },
            "both requests complete in order");
    }
    {
        // Streaming bodies report the same offsets through Headers and Body results.
        Detail::HttpParser parser(1024, 64 * 1024, true);
        Web::HttpRequest request;
        std::size_t offset = 0;
        std::string body;
        bool complete = false;
        while (!complete && offset <= first.size())
        {
            std::size_t consumed = 0;
            const auto result =
                parser.Parse(std::string_view(first).substr(offset), consumed, request);
            offset += consumed;
            if (result.kind == Detail::HttpParseKind::Headers)
                ExpectTrue(parser.ConfigureBody(true, 64 * 1024, 7),
                    "streaming body is configured after headers");
            else if (result.kind == Detail::HttpParseKind::Body)
                body += request.body;
            else if (result.kind == Detail::HttpParseKind::Complete)
                complete = true;
            else
            {
                ExpectTrue(false, "streaming parse only reports headers, body and completion");
                break;
            }
        }
        ExpectTrue(
            complete && offset == first.size(), "streaming parse consumes the whole request");
        ExpectEqual(std::string(2730, 'x'), body, "streamed pieces reassemble the body");
    }
}

/// <summary>
/// 흔한 클라이언트·프록시가 보내는 요청 줄과 목록 형태를 받아들이는지 본다(WEB-2).
/// </summary>
void HttpParserInteroperability()
{
    const auto parse = [](std::string wire, Web::HttpRequest& request)
    {
        Detail::HttpParser parser(1024, 1024);
        return parser.Parse(wire, request);
    };
    Web::HttpRequest request;
    auto result = parse("GET /old HTTP/1.0\r\n\r\n", request);
    ExpectTrue(result.kind == Detail::HttpParseKind::Complete && request.target == "/old",
        "HTTP/1.0 request without Host completes");
    ExpectEqual(505u, parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n", request).status,
        "other versions remain 505");
    ExpectTrue(parse("GET / HTTP/1.1\r\n\r\n", request).kind == Detail::HttpParseKind::Error,
        "HTTP/1.1 still requires Host");
    result =
        parse("POST / HTTP/1.0\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", request);
    ExpectTrue(result.kind == Detail::HttpParseKind::Error && result.status == 400,
        "HTTP/1.0 transfer coding is faulty framing");
    result =
        parse("POST / HTTP/1.0\r\nHost: x\r\nContent-Length: 2\r\nExpect: 100-continue\r\n\r\nhi",
            request);
    ExpectTrue(result.kind == Detail::HttpParseKind::Complete && request.body == "hi",
        "HTTP/1.0 100-continue expectation is ignored");

    result = parse("\r\n\r\nGET /after HTTP/1.1\r\nHost: x\r\n\r\n", request);
    ExpectTrue(result.kind == Detail::HttpParseKind::Complete && request.target == "/after",
        "empty lines before the request line are ignored");
    {
        Detail::HttpParser parser(1024, 1024);
        std::string pending = "\r";
        ExpectTrue(parser.Parse(pending, request).kind == Detail::HttpParseKind::NeedMore,
            "a lone CR waits for its LF");
        pending += "\nGET /split HTTP/1.1\r\nHost: x\r\n\r\n";
        result = parser.Parse(pending, request);
        ExpectTrue(result.kind == Detail::HttpParseKind::Complete && request.target == "/split",
            "an empty line split across reads is still ignored");
    }
    for (const auto* wire : { "GET / HTTP/1.1\nHost: x\n\n", "GET / HTTP/1.1\r\nHost: x\n",
             "GET / HTTP/1.1\rHost: x\r\n\r\n" })
    {
        result = parse(wire, request);
        ExpectTrue(result.kind == Detail::HttpParseKind::Error && result.status == 400,
            "bare LF or CR in the header section is rejected without waiting for more input");
    }

    result = parse("GET http://Example.com:8080/a/b?q=1 HTTP/1.1\r\nHost: other\r\n\r\n", request);
    ExpectTrue(
        result.kind == Detail::HttpParseKind::Complete, "absolute-form request target is accepted");
    ExpectEqual(
        std::string("/a/b?q=1"), request.target, "absolute-form target is reduced to origin-form");
    ExpectEqual(std::string("Example.com:8080"), std::string(request.Header("host")),
        "absolute-form authority replaces the Host field");
    result = parse("GET HTTPS://h?x HTTP/1.1\r\nHost: h\r\n\r\n", request);
    ExpectTrue(result.kind == Detail::HttpParseKind::Complete && request.target == "/?x" &&
                   request.Path() == "/",
        "absolute-form without a path selects the root");
    for (const auto* wire : { "GET ftp://h/ HTTP/1.1\r\nHost: h\r\n\r\n",
             "GET http://user@h/ HTTP/1.1\r\nHost: h\r\n\r\n",
             "GET http:///x HTTP/1.1\r\nHost: h\r\n\r\n",
             "GET http:/x HTTP/1.1\r\nHost: h\r\n\r\n" })
        ExpectTrue(parse(wire, request).kind == Detail::HttpParseKind::Error,
            "unsupported or malformed absolute-form is rejected");

    for (const auto* field :
        { "Connection: close,\r\n", "Connection: , keep-alive\r\n", "Connection: a,,b\r\n" })
    {
        result = parse(std::string("GET / HTTP/1.1\r\nHost: x\r\n") + field + "\r\n", request);
        ExpectTrue(result.kind == Detail::HttpParseKind::Complete,
            "empty Connection list elements are ignored");
    }
    result = parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close,\r\n\r\n", request);
    ExpectTrue(Detail::HasToken(request, "connection", "close"),
        "Connection option survives an empty element");
    ExpectTrue(
        parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked,\r\n\r\n0\r\n\r\n", request)
                .kind == Detail::HttpParseKind::Error,
        "Transfer-Encoding framing stays strict");
}

void HttpParserRejectsAmbiguousRequests()
{
    const std::array<std::string_view, 9> invalid{
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nTransfer-Encoding: "
        "chunked\r\n\r\n0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n", "GET / HTTP/1.1\r\nHost : x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n folded: value\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!\n0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: "
        "2\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
        "chunked\r\n\r\n1;bad=\"unterminated\r\nx\r\n0\r\n\r\n"
    };
    for (const auto wire : invalid)
    {
        Detail::HttpParser parser(1024, 1024);
        std::string pending(wire);
        Web::HttpRequest request;
        ExpectTrue(parser.Parse(pending, request).kind == Detail::HttpParseKind::Error,
            "ambiguous or malformed framing is rejected");
    }
    Detail::HttpParser bounded(256, 4);
    Web::HttpRequest request;
    std::string pending =
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    ExpectEqual(
        413u, bounded.Parse(pending, request).status, "decoded chunked body has an enforced limit");
    Detail::HttpParser headers(256, 4);
    pending = "GET / HTTP/1.1\r\nHost: x\r\nX: " + std::string(256, 'a');
    ExpectEqual(
        431u, headers.Parse(pending, request).status, "unfinished headers have an enforced limit");
    Detail::HttpParser coding(1024, 1024);
    pending = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    ExpectEqual(
        501u, coding.Parse(pending, request).status, "unsupported transfer coding is explicit");
    Web::HttpResponse response;
    response.headers.emplace_back("X-Test", "value\r\nInjected: yes");
    std::string encoded;
    ExpectTrue(!Detail::SerializeResponse(response, false, false, 1024, 1024, encoded),
        "response splitting is rejected");
    response.headers = { { "Content-Length", "999" } };
    ExpectTrue(!Detail::SerializeResponse(response, false, false, 1024, 1024, encoded),
        "application cannot override response framing");
}

void WebSocketProtocolConformance()
{
    std::string accept;
    ExpectTrue(Detail::WebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==", accept), "RFC key is accepted");
    ExpectEqual(std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), accept,
        "SHA-1 and base64 match RFC 6455 example");
    ExpectTrue(!Detail::WebSocketAccept("dGhlIHNhbXBsZSBub25jZR==", accept),
        "noncanonical key padding is rejected");
    const std::string wire = MaskedFrame(1, "Hello");
    Detail::WebSocketFrame frame;
    for (std::size_t count = 0; count < wire.size(); ++count)
        ExpectTrue(Detail::ParseClientFrame(Bytes(wire).first(count), 1024, frame).kind ==
                       Detail::FrameParseKind::NeedMore,
            "partial client frame needs more data");
    const auto result = Detail::ParseClientFrame(Bytes(wire), 1024, frame);
    ExpectTrue(result.kind == Detail::FrameParseKind::Complete && frame.final && frame.opcode == 1,
        "masked text frame decodes");
    ExpectEqual(std::string("Hello"), Text(frame.payload), "mask is removed");
    auto unmasked = Detail::EncodeServerFrame(1, Bytes("Hello"));
    ExpectEqual(std::uint16_t{ 1002 }, Detail::ParseClientFrame(unmasked, 1024, frame).closeCode,
        "unmasked client frame is a protocol error");
    auto fragmentedPing = MaskedFrame(9, "ping", false);
    ExpectTrue(Detail::ParseClientFrame(Bytes(fragmentedPing), 1024, frame).kind ==
                   Detail::FrameParseKind::Error,
        "control frames cannot be fragmented");
    const auto large = MaskedFrame(2, std::string(126, 'x'));
    ExpectEqual(std::uint16_t{ 1009 },
        Detail::ParseClientFrame(Bytes(large).first(4), 125, frame).closeCode,
        "oversize length is rejected before waiting for payload");
    std::string noncanonical{ "\x81\xfe\x00\x01", 4 };
    ExpectTrue(Detail::ParseClientFrame(Bytes(noncanonical), 1024, frame).kind ==
                   Detail::FrameParseKind::Error,
        "extended frame lengths must use the shortest representation");
    ExpectTrue(!Detail::ValidCloseCode(1006) && !Detail::ValidCloseCode(2000) &&
                   Detail::ValidCloseCode(1000),
        "close status validation excludes reserved codes");
}

std::uint16_t FreePort()
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ExpectTrue(socket != ServerCoreTest::InvalidSocket, "port probe socket opens");
    if (socket == ServerCoreTest::InvalidSocket)
        return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool bound =
        ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ServerCoreTest::SocketLength size = static_cast<ServerCoreTest::SocketLength>(sizeof(address));
    const bool named =
        bound && ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0;
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
        if (socket == ServerCoreTest::InvalidSocket)
        {
            ExpectTrue(false, "client socket opens");
            return;
        }
        ExpectTrue(ServerCoreTest::SetSocketTimeouts(socket, 3000),
            "client socket timeouts are configured");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        ExpectTrue(
            ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "client connects");
    }
    ~Client()
    {
        if (socket != ServerCoreTest::InvalidSocket)
            ServerCoreTest::CloseSocket(socket);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Send(std::string_view value)
    {
        while (!value.empty())
        {
            const auto count = static_cast<int>((std::min)(value.size(), std::size_t{ 4096 }));
            const int sent = ServerCoreTest::Send(socket, value.data(), count);
            if (sent <= 0)
            {
                ExpectTrue(false, "client sends complete request");
                return false;
            }
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
            const auto wanted = static_cast<int>((std::min)(count - offset, std::size_t{ 4096 }));
            const auto read = ServerCoreTest::Receive(socket, value.data() + offset, wanted);
            if (read <= 0)
            {
                ExpectTrue(false, "client reads the expected bytes");
                value.resize(offset);
                return value;
            }
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
            if (byte.empty())
                return wire;
            wire += byte;
        }
        const auto at = wire.find("Content-Length: ");
        if (body && at != std::string::npos)
        {
            const auto first = at + std::string_view("Content-Length: ").size();
            const auto last = wire.find("\r\n", first);
            std::size_t size = 0;
            const auto parsed = std::from_chars(wire.data() + first, wire.data() + last, size);
            ExpectTrue(
                parsed.ec == std::errc{} && size <= 1024 * 1024, "response length is bounded");
            if (size <= 1024 * 1024)
                wire += Read(size);
        }
        return wire;
    }

    std::pair<unsigned int, std::string> Frame(bool final = true)
    {
        const auto head = Read(2);
        if (head.size() != 2)
            return {};
        const auto first = static_cast<unsigned char>(head[0]);
        const auto second = static_cast<unsigned char>(head[1]);
        ExpectTrue((second & 128u) == 0 && ((first & 128u) != 0) == final,
            "server frame FIN and mask bits match");
        std::uint64_t length = second & 127u;
        if (length >= 126)
        {
            const auto extended = Read(length == 126 ? 2 : 8);
            length = 0;
            for (char byte : extended)
                length = (length << 8) | static_cast<unsigned char>(byte);
        }
        ExpectTrue(length <= 1024 * 1024, "server frame length is bounded");
        if (length > 1024 * 1024)
            return {};
        return { first & 15u, Read(static_cast<std::size_t>(length)) };
    }

    /// <summary>상대가 연결을 정상적으로 닫았는지(FIN) 본다.</summary>
    /// <remarks>
    /// 연결 재설정(RST)은 닫힘으로 치지 않는다. RST는 큐에 든 응답을 잘라 낼 수 있어, 그것을 정상 종료로
    /// 받으면 잘린 응답이 가려진다(리뷰 NET-2). 읽을 바이트가 남았거나 수신 대기가 끝나도 거짓이다.
    /// </remarks>
    bool Closed()
    {
        char byte = 0;
        return ServerCoreTest::Receive(socket, &byte, 1) == 0;
    }
    ServerCoreTest::Socket socket = ServerCoreTest::InvalidSocket;
};

const std::string handshake =
    "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: keep-alive, "
    "Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";

void HttpSocketIntegration()
{
    ServerCoreTest::SocketRuntime runtime;
    ExpectTrue(runtime.IsReady(), "socket runtime initializes");
    if (!runtime.IsReady())
        return;
    Web::HttpServer server;
    ExpectTrue(server
                   .RegisterRoute("GET", "/health",
                       [](const Web::HttpRequest&)
                       {
                           return Web::HttpResponse{ 200,
                               { { "Content-Type", "application/json" } }, "{\"ok\":true}" };
                       })
                   .IsOk(),
        "health route registers");
    ExpectTrue(server
                   .RegisterRoute("POST", "/echo", [](const Web::HttpRequest& request)
                       { return Web::HttpResponse{ 200, {}, request.body }; })
                   .IsOk(),
        "echo route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "HTTP server starts");
    if (!server.IsRunning())
        return;
    Client client(server.Port());
    client.Send("GET /health HTTP/1.1\r\nHost: local");
    client.Send("host\r\n\r\nHEAD /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto health = client.Response();
    ExpectTrue(health.starts_with("HTTP/1.1 200 ") && health.ends_with("{\"ok\":true}"),
        "fragmented request receives route response");
    const auto head = client.Response(false);
    ExpectTrue(
        head.find("Content-Length: 11\r\n") != std::string::npos && head.ends_with("\r\n\r\n"),
        "pipelined HEAD has no body");
    client.Send("POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nExpect: "
                "100-continue\r\n\r\n");
    ExpectTrue(client.Response().starts_with("HTTP/1.1 100 Continue"),
        "server admits expected request body");
    client.Send("2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n");
    ExpectTrue(client.Response().ends_with("hello"), "real socket chunked request is decoded");
    client.Send("GET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    ExpectTrue(client.Response().starts_with("HTTP/1.1 404 ") && client.Closed(),
        "close response drains before transport shutdown");
    ExpectTrue(server.Stop().IsOk(), "HTTP server stops after transport completion");
}

/// <summary>
/// HTTP/1.0 요청과 관대한 요청 줄이 실제 소켓에서 응답을 받는지 본다(WEB-2).
/// </summary>
/// <remarks>
/// requestTimeout을 기본값(30초)으로 두고 클라이언트 수신 대기는 3초다. 그래서 400 응답이 오면
/// 그것은 타임아웃 경로가 아니라 즉시 거절 경로에서 온 것이다(타임아웃은 응답 없이 끊는다).
/// </remarks>
void Http10AndLenientRequestLines()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    ExpectTrue(server
                   .RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "ready" }; })
                   .IsOk(),
        "buffered route registers");
    ExpectTrue(server
                   .RegisterRoute("POST", "/echo", [](const Web::HttpRequest& request)
                       { return Web::HttpResponse{ 200, {}, request.body }; })
                   .IsOk(),
        "echo route registers");
    ExpectTrue(server
                   .RegisterAsyncRoute("GET", "/stream",
                       [](std::shared_ptr<const Web::HttpRequestContext> context)
                       {
                           Web::HttpResponseHead head;
                           const bool started = context->response->Start(head).IsOk();
                           ExpectTrue(started && context->response->Write(Bytes("part1-")).IsOk() &&
                                          context->response->Write(Bytes("part2")).IsOk() &&
                                          context->response->Finish().IsOk(),
                               "unknown-length stream is admitted");
                       })
                   .IsOk(),
        "streaming route registers");
    std::atomic<unsigned int> opens{ 0 };
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&opens](const std::shared_ptr<Web::WebSocketConnection>&)
    { opens.fetch_add(1); };
    ExpectTrue(
        server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "WebSocket route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "interoperability server starts");
    if (!server.IsRunning())
        return;
    {
        Client client(server.Port());
        client.Send("GET /health HTTP/1.0\r\n\r\n");
        const auto response = client.Response();
        ExpectTrue(response.starts_with("HTTP/1.1 200 ") && response.ends_with("ready"),
            "HTTP/1.0 request receives its route response");
        ExpectTrue(response.find("Connection: close\r\n") != std::string::npos,
            "HTTP/1.0 response does not keep the connection alive");
        ExpectTrue(client.Closed(), "HTTP/1.0 connection closes after the response");
    }
    {
        Client client(server.Port());
        client.Send("GET /stream HTTP/1.0\r\nHost: x\r\n\r\n");
        const auto head = client.Response(false);
        ExpectTrue(head.starts_with("HTTP/1.1 200 ") &&
                       head.find("Transfer-Encoding") == std::string::npos &&
                       head.find("Content-Length") == std::string::npos &&
                       head.find("Connection: close\r\n") != std::string::npos,
            "HTTP/1.0 unknown-length stream is close-delimited, never chunked");
        ExpectEqual(
            std::string("part1-part2"), client.Read(11), "close-delimited body carries raw bytes");
        ExpectTrue(client.Closed(), "close-delimited body ends with the connection");
    }
    {
        Client client(server.Port());
        client.Send("POST /echo HTTP/1.0\r\nContent-Length: 2\r\nExpect: 100-continue\r\n\r\nhi");
        const auto response = client.Response();
        ExpectTrue(response.starts_with("HTTP/1.1 200 ") && response.ends_with("hi"),
            "HTTP/1.0 client never receives 100 Continue");
    }
    {
        Client client(server.Port());
        std::string oldHandshake = handshake;
        oldHandshake.replace(oldHandshake.find("HTTP/1.1"), 8, "HTTP/1.0");
        client.Send(oldHandshake);
        ExpectTrue(
            client.Response().starts_with("HTTP/1.1 400 "), "HTTP/1.0 cannot upgrade to WebSocket");
    }
    {
        Client client(server.Port());
        client.Send("GET /health HTTP/1.1\r\nHost: x\r\n\r\n\r\nGET http://x/health "
                    "HTTP/1.1\r\nHost: x\r\nConnection: close,\r\n\r\n");
        ExpectTrue(client.Response().ends_with("ready"), "first pipelined request completes");
        const auto second = client.Response();
        ExpectTrue(second.starts_with("HTTP/1.1 200 ") && second.ends_with("ready"),
            "stray CRLF, absolute-form and an empty list element do not break the next request");
        ExpectTrue(second.find("Connection: close\r\n") != std::string::npos && client.Closed(),
            "Connection: close, still closes");
    }
    {
        Client client(server.Port());
        client.Send("GET /health HTTP/1.1\nHost: x\n\n");
        ExpectTrue(client.Response().starts_with("HTTP/1.1 400 "),
            "bare-LF request gets an immediate 400");
    }
    ExpectTrue(server.Stop().IsOk(), "interoperability server stops");
    ExpectEqual(0u, opens.load(), "no WebSocket opened from an HTTP/1.0 handshake");
}

/// <summary>
/// 정책이 준 Vary·Set-Cookie가 핸들러의 같은 이름 헤더와 함께 나가는지 본다(WEB-7).
/// </summary>
/// <remarks>
/// 정책 헤더는 기본값이라 같은 이름이 응답에 있으면 빠졌다. 목록인 Vary가 빠지면 CORS의 Vary: Origin이
/// 사라져 공유 캐시가 다른 origin에 같은 응답을 주고, Set-Cookie가 빠지면 정책의 쿠키가 사라진다.
/// </remarks>
void PolicyHeadersKeepListFields()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    ExpectTrue(server
                   .SetRequestPolicy(
                       [](std::shared_ptr<const Web::HttpPolicyContext> context)
                       {
                           (void)context->decision->Allow({ { "Vary", "Origin" },
                               { "Set-Cookie", "policy=1" }, { "X-Default", "policy" } });
                       })
                   .IsOk(),
        "header policy registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/buffered",
                       [](const Web::HttpRequest&)
                       {
                           return Web::HttpResponse{ 200,
                               { { "Vary", "Accept-Encoding" }, { "Set-Cookie", "handler=1" },
                                   { "X-Default", "handler" } },
                               "ok" };
                       })
                   .IsOk(),
        "buffered route registers");
    ExpectTrue(server
                   .RegisterAsyncRoute("GET", "/streamed",
                       [](std::shared_ptr<const Web::HttpRequestContext> context)
                       {
                           Web::HttpResponseHead head;
                           head.headers = { { "Vary", "Accept-Encoding" },
                               { "Set-Cookie", "handler=1" }, { "X-Default", "handler" } };
                           head.contentLength = 2;
                           ExpectTrue(context->response->Start(head).IsOk() &&
                                          context->response->Write(Bytes("ok")).IsOk() &&
                                          context->response->Finish().IsOk(),
                               "streamed response is admitted");
                       })
                   .IsOk(),
        "streamed route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "header policy server starts");
    if (!server.IsRunning())
        return;
    for (const auto* path : { "/buffered", "/streamed" })
    {
        Client client(server.Port());
        client.Send(std::string("GET ") + path + " HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto response = client.Response();
        ExpectTrue(response.find("\r\nVary: Accept-Encoding\r\n") != std::string::npos &&
                       response.find("\r\nVary: Origin\r\n") != std::string::npos,
            "handler and policy Vary values are both sent");
        ExpectTrue(response.find("\r\nSet-Cookie: handler=1\r\n") != std::string::npos &&
                       response.find("\r\nSet-Cookie: policy=1\r\n") != std::string::npos,
            "handler and policy cookies are both sent");
        ExpectTrue(response.find("\r\nX-Default: handler\r\n") != std::string::npos &&
                       response.find("X-Default: policy") == std::string::npos,
            "other policy headers remain defaults the handler overrides");
    }
    ExpectTrue(server.Stop().IsOk(), "header policy server stops");
}

void WebSocketSocketIntegration()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    std::atomic<unsigned int> closes{ 0 };
    std::atomic<std::uint16_t> closeCode{ 0 };
    Web::WebSocketCallbacks callbacks;
    callbacks.onMessage = [](const std::shared_ptr<Web::WebSocketConnection>& connection,
                              const Web::WebSocketMessage& message)
    {
        const auto result = message.type == Web::WebSocketMessageType::Text
                                ? connection->SendText(Text(message.bytes))
                                : connection->SendBinary(message.bytes);
        ExpectTrue(result.IsOk(), "WebSocket echoes through public send API");
    };
    callbacks.onClose = [&closes, &closeCode](std::uint64_t, std::uint16_t code, std::string_view)
    {
        closeCode.store(code);
        closes.fetch_add(1);
    };
    ExpectTrue(
        server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "WebSocket route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "WebSocket server starts");
    if (!server.IsRunning())
        return;
    Client client(server.Port());
    client.Send(
        handshake + MaskedFrame(1, "hel", false) + MaskedFrame(9, "p") + MaskedFrame(0, "lo"));
    const auto response = client.Response();
    ExpectTrue(response.starts_with("HTTP/1.1 101 ") &&
                   response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
        "upgrade and an immediately following frame share one stream");
    const auto pong = client.Frame();
    ExpectTrue(pong.first == 10 && pong.second == "p", "ping interleaves with fragmented messages");
    const auto echo = client.Frame();
    ExpectTrue(echo.first == 1 && echo.second == "hello", "continuations form one text callback");
    client.Send(MaskedFrame(2, std::string("\0\xff", 2)));
    const auto binary = client.Frame();
    ExpectTrue(binary.first == 2 && binary.second == std::string("\0\xff", 2),
        "binary messages preserve all bytes");
    client.Send(MaskedFrame(8, std::string("\x03\xe8", 2)));
    const auto closed = client.Frame();
    ExpectTrue(closed.first == 8 && closed.second == std::string("\x03\xe8", 2),
        "received close frame is acknowledged");
    ExpectTrue(client.Closed(), "closing handshake ends transport");
    ExpectTrue(server.Stop().IsOk(), "WebSocket server stops");
    ExpectTrue(
        closes.load() == 1 && closeCode.load() == 1000, "onClose runs once with peer close status");
}

void HttpResponseSemantics()
{
    using namespace std::chrono;
    ExpectEqual(std::string("Thu, 01 Jan 1970 00:00:00 GMT"),
        Detail::HttpDate(system_clock::time_point{}),
        "HTTP dates use UTC with fixed English names");
    ExpectEqual(std::string("Tue, 29 Feb 2000 23:59:58 GMT"),
        Detail::HttpDate(
            sys_days{ year{ 2000 } / 2 / 29 } + hours{ 23 } + minutes{ 59 } + seconds{ 58 }),
        "HTTP date formatting handles calendar boundaries without locale or shared tm state");
    for (unsigned int status : { 204u, 205u, 304u })
    {
        Web::HttpResponse response{ status, {}, "must not be sent" };
        std::string encoded;
        ExpectTrue(Detail::SerializeResponse(response, false, false, 1024, 1024, encoded),
            "content-free status serializes");
        ExpectTrue(encoded.ends_with("\r\n\r\n"), "content-free status never writes payload bytes");
        if (status == 205)
            ExpectTrue(encoded.find("Content-Length: 0\r\n") != std::string::npos,
                "205 has empty persistent-connection framing");
        else
            ExpectTrue(encoded.find("Content-Length:") == std::string::npos,
                "204 and unknown-length 304 omit content length");
        ExpectTrue(encoded.find("\r\nDate: ") != std::string::npos,
            "ordinary responses get a Date header");
    }
    Web::HttpResponse suppliedDate{ 200, { { "Date", "Thu, 01 Jan 1970 00:00:00 GMT" } }, "ok" };
    std::string dated;
    ExpectTrue(Detail::SerializeResponse(suppliedDate, false, false, 1024, 1024, dated) &&
                   dated.find("Date:") == dated.rfind("Date:"),
        "application Date is preserved without a duplicate");
    suppliedDate.headers.emplace_back("date", "Thu, 01 Jan 1970 00:00:00 GMT");
    ExpectTrue(!Detail::SerializeResponse(suppliedDate, false, false, 1024, 1024, dated),
        "duplicate Date fields are rejected");
    Web::HttpResponse upgradeRequired{ 426, {}, {} };
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
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    ExpectTrue(server
                   .RegisterRoute("GET", "/throw", [](const Web::HttpRequest&) -> Web::HttpResponse
                       { throw std::runtime_error("test handler failure"); })
                   .IsOk(),
        "throwing route registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/reset", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 205, {}, "ignored" }; })
                   .IsOk(),
        "205 route registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/cached",
                       [](const Web::HttpRequest&) { return Web::HttpResponse{ 304, {}, {} }; })
                   .IsOk(),
        "304 route registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "ready" }; })
                   .IsOk(),
        "semantic probe route registers");
    ExpectTrue(server.RegisterWebSocket("/ws", {}).IsOk(), "HEAD upgrade-error route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxHeaderBytes = 256;
    options.maxBodyBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "response-semantics server starts");
    if (!server.IsRunning())
        return;
    const std::array<std::pair<std::string, unsigned int>, 6> failures{
        { { "HEAD /throw HTTP/1.1\r\nHost: x\r\n\r\n", 500u },
            { "HEAD /ws HTTP/1.1\r\nHost: x\r\n\r\n", 400u },
            { "HEAD / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", 413u },
            { "HEAD / HTTP/1.1\r\nHost: x\r\nX: " + std::string(256, 'a'), 431u },
            { "HEAD / HTTP/2.0\r\nHost: x\r\n\r\n", 505u },
            { "HEAD /unknown HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n", 404u } }
    };
    for (const auto& [wire, status] : failures)
    {
        Client client(server.Port());
        client.Send(wire);
        const auto response = client.Response(false);
        ExpectTrue(response.starts_with("HTTP/1.1 " + std::to_string(status) + " "),
            "HEAD error preserves its status");
        ExpectTrue(
            client.Closed(), "HEAD errors end at the header terminator, without payload bytes");
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
                  "GET /cached HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: "
                  "x\r\nConnection: close\r\n\r\n");
    ExpectTrue(pipeline.Response(false).starts_with("HTTP/1.1 200 "),
        "HEAD response completes in the pipeline");
    ExpectTrue(pipeline.Response().starts_with("HTTP/1.1 205 "),
        "205 keeps the next response boundary intact");
    ExpectTrue(pipeline.Response().starts_with("HTTP/1.1 304 "),
        "304 keeps the next response boundary intact");
    ExpectTrue(pipeline.Response().ends_with("ready"), "HEAD state resets before a subsequent GET");
    Client errorPipeline(server.Port());
    errorPipeline.Send("HEAD /health HTTP/1.1\r\nHost: x\r\n\r\n"
                       "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
    ExpectTrue(errorPipeline.Response(false).starts_with("HTTP/1.1 200 "),
        "HEAD finishes before a malformed pipelined GET");
    const auto getError = errorPipeline.Response();
    ExpectTrue(getError.starts_with("HTTP/1.1 413 ") && getError.ends_with("Content Too Large\n"),
        "a prior HEAD does not suppress a subsequent GET parser-error body");
    ExpectTrue(server.Stop().IsOk(), "response-semantics server stops");
}

void WebSocketSendValidationSymmetry()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    std::shared_ptr<Web::WebSocketConnection> retained;
    std::atomic<bool> opened{ false };
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&retained, &opened](
                           const std::shared_ptr<Web::WebSocketConnection>& connection)
    {
        using ServerCore::Core::ErrorCode;
        const std::string oversized(5, '\xff');
        ExpectTrue(connection->SendText(oversized).Code() == ErrorCode::TooLarge,
            "outgoing text checks its size before UTF-8");
        ExpectTrue(connection->SendBinary(Bytes(oversized)).Code() == ErrorCode::TooLarge,
            "binary and text share the same data-frame bound");
        ExpectTrue(
            connection->SendText(std::string("\xc0\xaf", 2)).Code() == ErrorCode::InvalidArgument,
            "bounded malformed caller text reports InvalidArgument");
        ExpectTrue(connection->Ping(Bytes(std::string(126, 'x'))).Code() == ErrorCode::TooLarge,
            "outgoing controls enforce their independent 125-byte bound");
        ExpectTrue(connection->Close(1000, std::string(124, '\xff')).Code() == ErrorCode::TooLarge,
            "close reason length is checked before UTF-8");
        ExpectTrue(connection->Close(1000, std::string("\xc0\xaf", 2)).Code() ==
                       ErrorCode::InvalidArgument,
            "malformed caller text and close reasons share InvalidArgument semantics");
        ExpectTrue(connection->Close(1010, {}).Code() == ErrorCode::InvalidArgument,
            "server cannot send a client-only close code");
        ExpectTrue(connection->IsOpen() && connection->SendText("ok").IsOk(),
            "rejected application payloads do not close or corrupt the connection");
        retained = connection;
        opened.store(true);
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(),
        "send-validation route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxWebSocketFrameBytes = 4;
    options.maxWebSocketMessageBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "send-validation server starts");
    if (!server.IsRunning())
        return;
    Client client(server.Port());
    client.Send(handshake);
    ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "send-validation client upgrades");
    ExpectTrue(
        client.Frame().second == "ok", "only the accepted application frame reaches the peer");
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
    for (const std::size_t capacity : { std::size_t{ 64 }, std::size_t{ 1024 } })
    {
        std::atomic<unsigned int> requests{ 0 };
        std::atomic<unsigned int> upgrades{ 0 };
        std::atomic<unsigned int> opens{ 0 };
        std::atomic<unsigned int> closes{ 0 };
        std::atomic<ErrorCode> sent{ ErrorCode::WouldBlock };
        Web::HttpServer server;
        ExpectTrue(server
                       .RegisterRoute("GET", "/budget",
                           [&](const Web::HttpRequest&)
                           {
                               requests.fetch_add(1);
                               return Web::HttpResponse{ 200, {}, payload, true };
                           })
                       .IsOk(),
            "send-budget HTTP route registers");
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
        callbacks.onClose = [&closes](std::uint64_t, std::uint16_t, std::string_view)
        { closes.fetch_add(1); };
        ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(),
            "send-budget WebSocket route registers");
        Web::HttpServerOptions options;
        options.port = FreePort();
        options.maxTotalSendQueueCapacityBytes = 0;
        ExpectTrue(server.Start(options).Code() == ErrorCode::InvalidArgument,
            "zero total send budget is invalid");
        options.maxTotalSendQueueCapacityBytes = std::size_t{ 512 } * 1024 * 1024 + 1;
        ExpectTrue(server.Start(options).Code() == ErrorCode::InvalidArgument,
            "total send budget above 512 MiB is invalid");
        options.maxTotalSendQueueCapacityBytes = capacity;
        ExpectTrue(
            server.Start(options).IsOk(), "valid send budget starts after rejected configurations");
        if (!server.IsRunning())
            continue;
        {
            Client client(server.Port());
            client.Send("GET /budget HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            if (capacity == 64)
                ExpectTrue(client.Closed(),
                    "HTTP wire larger than the total budget is rejected before any bytes are sent");
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
                ExpectTrue(client.Closed(),
                    "WebSocket handshake also honors the configured total send budget");
            else
            {
                ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "),
                    "WebSocket handshake fits the larger total budget");
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
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    std::atomic<bool> selfStopRejected{ false };
    std::atomic<unsigned int> closes{ 0 };
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&server, &selfStopRejected](
                           const std::shared_ptr<Web::WebSocketConnection>& connection)
    {
        selfStopRejected.store(
            server.Stop().Code() == ServerCore::Core::ErrorCode::InvalidArgument);
        ExpectTrue(
            connection->Close(1000, "done").IsOk(), "server can initiate a closing handshake");
    };
    callbacks.onClose = [&closes](std::uint64_t, std::uint16_t, std::string_view)
    { closes.fetch_add(1); };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(),
        "bounded WebSocket route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.requestTimeout = std::chrono::milliseconds(100);
    options.webSocketCloseTimeout = std::chrono::milliseconds(100);
    options.idleTimeout = std::chrono::milliseconds(2000);
    options.maxHeaderBytes = 256;
    options.maxBodyBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "bounded server starts");
    if (!server.IsRunning())
        return;
    {
        Client slow(server.Port());
        slow.Send("GET / HTTP/1.1\r\n");
        ExpectTrue(slow.Closed(), "incomplete HTTP request expires");
    }
    {
        Client bad(server.Port());
        bad.Send("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n");
        ExpectTrue(bad.Response().starts_with("HTTP/1.1 413 "),
            "declared oversize body is rejected immediately");
    }
    {
        Client socket(server.Port());
        socket.Send(handshake);
        ExpectTrue(socket.Response().starts_with("HTTP/1.1 101 "), "close-timeout test upgrades");
        ExpectTrue(socket.Frame().first == 8, "server close frame is sent");
        ExpectTrue(socket.Closed(), "unanswered closing handshake expires");
    }
    ExpectTrue(server.Stop().IsOk(), "bounded server shuts down");
    ExpectTrue(selfStopRejected.load() && closes.load() == 1,
        "callback self-join is rejected and close is delivered once");
    ExpectTrue(server.Stop().IsOk() && !server.Start(options).IsOk(),
        "stop is idempotent and host is single-use");
    GlobalSendBudget();
}

/// <summary>
/// 첫 바이트를 보내지 않는 연결이 요청 기한을 피하지 못하는지 본다(WEB-8).
/// </summary>
/// <remarks>
/// 고치기 전에는 요청 기한이 첫 바이트에서 시작해, 아무것도 보내지 않는 연결은 유휴 기한(기본 120초)까지
/// 연결 자리를 쥐었다. 유휴 기한을 10초로 두고 클라이언트는 3초만 기다리므로, 닫힘이 보이면 그것은
/// 요청 기한이나 첫 바이트 기한에서 온 것이다.
/// </remarks>
void SilentConnectionsExpire()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    {
        Web::HttpServer server;
        Web::HttpServerOptions options;
        options.port = FreePort();
        options.requestTimeout = std::chrono::milliseconds(200);
        options.idleTimeout = std::chrono::milliseconds(10000);
        ExpectTrue(server.Start(options).IsOk(), "request-deadline server starts");
        if (!server.IsRunning())
            return;
        Client silent(server.Port());
        ExpectTrue(
            silent.Closed(), "a connection that never sends is bounded by the request deadline");
        ExpectTrue(server.Stop().IsOk(), "request-deadline server stops");
    }
    {
        Web::HttpServer server;
        ExpectTrue(server
                       .RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
                           { return Web::HttpResponse{ 200, {}, "ready" }; })
                       .IsOk(),
            "first-byte route registers");
        Web::HttpServerOptions options;
        options.port = FreePort();
        options.firstByteTimeout = std::chrono::milliseconds(0);
        ExpectTrue(server.Start(options).Code() == ServerCore::Core::ErrorCode::InvalidArgument,
            "a zero first-byte deadline is invalid");
        options.firstByteTimeout = std::chrono::milliseconds(200);
        options.idleTimeout = std::chrono::milliseconds(10000);
        ExpectTrue(server.Start(options).IsOk(), "first-byte server starts");
        if (!server.IsRunning())
            return;
        {
            Client silent(server.Port());
            ExpectTrue(silent.Closed(),
                "the first-byte deadline closes a silent connection before the request deadline");
        }
        {
            Client active(server.Port());
            active.Send("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
            ExpectTrue(active.Response().ends_with("ready"),
                "a connection that sends is not bound by the first-byte deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            active.Send("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
            ExpectTrue(active.Response().ends_with("ready"),
                "keep-alive waits between requests use the idle deadline");
        }
        ExpectTrue(server.Stop().IsOk(), "first-byte server stops");
    }
}

/// <summary>
/// drain이 닫은 쉬던 keep-alive 연결도 responseDrainTimeout 안에 연결 자리를 내놓는지 본다.
/// </summary>
/// <remarks>
/// NET-2 뒤로 CloseAfterSend는 상대가 닫을 때까지 소켓을 쥔다. 고치기 전에는 drain이 쉬던 HTTP 연결을
/// HttpClosing으로 바꾸지 않아, 닫지 않는 클라이언트는 idleTimeout(여기서는 10초)까지 남았다.
/// responseDrainTimeout을 200ms로 두고 3초 안에 drain이 끝나는지 본다. 클라이언트는 끝까지 닫지 않는다.
/// </remarks>
void DrainBoundsIdleKeepAliveConnections()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    ExpectTrue(server
                   .RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "ready" }; })
                   .IsOk(),
        "drain route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.responseDrainTimeout = std::chrono::milliseconds(200);
    options.idleTimeout = std::chrono::milliseconds(10000);
    ExpectTrue(server.Start(options).IsOk(), "drain server starts");
    if (!server.IsRunning())
        return;
    Client idle(server.Port());
    idle.Send("GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    ExpectTrue(
        idle.Response().ends_with("ready"), "the keep-alive connection is idle after its response");
    ExpectTrue(server.BeginDrain().IsOk(), "drain begins");
    ExpectTrue(server.DrainStatus().Code() == ServerCore::Core::ErrorCode::WouldBlock,
        "the idle connection is still admitted when drain begins");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!server.DrainStatus().IsOk() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ExpectTrue(server.DrainStatus().IsOk(),
        "a drained idle connection that never closes is released by responseDrainTimeout");
    ExpectTrue(server.Stop().IsOk(), "drain server stops");
}

void WebSocketRejectsProtocolErrors()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    ExpectTrue(server.RegisterWebSocket("/ws", {}).IsOk(), "protocol-validation route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxWebSocketFrameBytes = 4;
    options.maxWebSocketMessageBytes = 6;
    ExpectTrue(server.Start(options).IsOk(), "protocol-validation server starts");
    if (!server.IsRunning())
        return;
    const std::array<std::pair<std::string, std::uint16_t>, 6> malformed{
        { { Text(Detail::EncodeServerFrame(1, Bytes("x"))), std::uint16_t{ 1002 } },
            { MaskedFrame(1, std::string("\xc0\xaf", 2)), std::uint16_t{ 1007 } },
            { MaskedFrame(0, "x"), std::uint16_t{ 1002 } },
            { MaskedFrame(2, "12345"), std::uint16_t{ 1009 } },
            { MaskedFrame(1, "abcd", false) + MaskedFrame(0, "efg"), std::uint16_t{ 1009 } },
            { MaskedFrame(8, std::string("\x03\xed", 2)), std::uint16_t{ 1002 } } }
    };
    for (const auto& [wire, expectedCode] : malformed)
    {
        Client client(server.Port());
        client.Send(handshake);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "invalid-frame test upgrades");
        client.Send(wire);
        const auto frame = client.Frame();
        ExpectTrue(
            frame.first == 8 && frame.second.size() >= 2, "invalid frame receives a close frame");
        if (frame.second.size() >= 2)
        {
            const auto code =
                static_cast<std::uint16_t>((static_cast<unsigned char>(frame.second[0]) << 8) |
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
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    std::atomic<unsigned int> closes{ 0 };
    Web::WebSocketCallbacks callbacks;
    callbacks.onClose = [&closes](std::uint64_t, std::uint16_t code, std::string_view)
    {
        ExpectEqual(
            std::uint16_t{ 1006 }, code, "forced server shutdown reports an abnormal peer closure");
        closes.fetch_add(1);
    };
    ExpectTrue(
        server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "limited route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxConnections = 1;
    ExpectTrue(server.Start(options).IsOk(), "one-connection server starts");
    if (!server.IsRunning())
        return;
    Client active(server.Port());
    active.Send(handshake);
    ExpectTrue(active.Response().starts_with("HTTP/1.1 101 "), "first connection is admitted");
    Client excess(server.Port());
    ExpectTrue(excess.Closed(), "connection limit rejects additional sockets");
    ExpectTrue(server.Stop().IsOk(), "server stops with an active WebSocket");
    ExpectTrue(
        closes.load() == 1 && active.Closed(), "Stop waits for onClose and socket completion");

    // A peer must not retain the only slot by sending bytes while refusing to
    // read a large closing response. An absolute drain deadline wins over idle.
    Web::HttpServer draining;
    std::atomic<bool> largeRequest{ false };
    ExpectTrue(draining
                   .RegisterRoute("GET", "/large",
                       [&largeRequest](const Web::HttpRequest&)
                       {
                           largeRequest.store(true);
                           return Web::HttpResponse{ 200, {}, std::string(900 * 1024, 'x'), true };
                       })
                   .IsOk(),
        "large close-response route registers");
    ExpectTrue(draining
                   .RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "ready", true }; })
                   .IsOk(),
        "drain probe route registers");
    options.port = FreePort();
    options.maxResponseBodyBytes = 900 * 1024;
    options.responseDrainTimeout = std::chrono::milliseconds(100);
    options.idleTimeout = std::chrono::milliseconds(1000);
    ExpectTrue(draining.Start(options).IsOk(), "drain-deadline server starts");
    if (!draining.IsRunning())
        return;
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
    ExpectTrue(probe.Response().ends_with("ready"),
        "absolute response drain deadline releases the connection slot despite trickle input");
    ExpectTrue(draining.Stop().IsOk(), "drain-deadline server stops");
}

void WebSocketNegotiationAndFragments()
{
    using ServerCore::Core::ErrorCode;
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    Web::WebSocketCallbacks invalid;
    invalid.subprotocols = { "same", "same" };
    ExpectTrue(server.RegisterWebSocket("/bad", invalid).Code() == ErrorCode::InvalidArgument,
        "duplicate supported protocol rejects registration");
    invalid.subprotocols = { "not a token" };
    ExpectTrue(server.RegisterWebSocket("/bad", invalid).Code() == ErrorCode::InvalidArgument,
        "invalid supported protocol rejects registration");
    Web::WebSocketCallbacks callbacks;
    callbacks.subprotocols = { "v2", "v1" };
    callbacks.onOpen = [](const auto& socket)
    {
        ExpectTrue(socket->SendText(Web::GetWebSocketMessageControl(socket)->Subprotocol()).IsOk(),
            "negotiated protocol is exposed");
    };
    ExpectTrue(server.RegisterWebSocket("/ws", callbacks).IsOk(), "subprotocol route registers");
    Web::WebSocketCallbacks fragmented;
    fragmented.onOpen = [](const auto& socket)
    {
        auto control = Web::GetWebSocketMessageControl(socket);
        auto result = control->BeginMessage(Web::WebSocketMessageType::Text);
        ExpectTrue(result.IsOk(), "text writer reserves the message lane");
        if (!result.IsOk())
            return;
        auto writer = std::move(result).Value();
        ExpectTrue(writer->Write(Bytes(std::string("A\xf0", 2)), false).IsOk(),
            "UTF8 prefix can span fragments");
        ExpectTrue(socket->SendText("x").Code() == ErrorCode::AlreadyExists,
            "ordinary data cannot interleave");
        ExpectTrue(control->BeginMessage(Web::WebSocketMessageType::Binary).GetStatus().Code() ==
                       ErrorCode::AlreadyExists,
            "another writer cannot interleave");
        ExpectTrue(socket->Ping(Bytes("p")).IsOk(), "control frame may interleave");
        ExpectTrue(writer->Write(Bytes("x"), true).Code() == ErrorCode::InvalidArgument,
            "invalid continuation changes no state");
        ExpectTrue(writer
                       ->Write(Bytes(std::string("\x9f\x98\x80"
                                                 "B",
                                   4)),
                           true)
                       .IsOk(),
            "valid UTF8 continuation completes");
        ExpectTrue(writer->Write({}, true).Code() == ErrorCode::Closed,
            "finished writer cannot send twice");
        writer = std::move(control->BeginMessage(Web::WebSocketMessageType::Binary)).Value();
        ExpectTrue(writer->Write(Bytes("1234"), false).IsOk(), "binary writer starts");
        ExpectTrue(writer->Write(Bytes("567"), true).Code() == ErrorCode::TooLarge,
            "total message size is bounded");
        ExpectTrue(writer->Write(Bytes("56"), true).IsOk(),
            "oversized attempt did not mutate message size");
        ExpectTrue(socket->SendText("end").IsOk(), "finished writer releases data lane");
    };
    ExpectTrue(server.RegisterWebSocket("/fragments", std::move(fragmented)).IsOk(),
        "fragment route registers");
    Web::WebSocketCallbacks aborted;
    aborted.onMessage = [](const auto& socket, const auto&)
    {
        auto writer = std::move(Web::GetWebSocketMessageControl(socket)->BeginMessage(
                                    Web::WebSocketMessageType::Binary))
                          .Value();
        ExpectTrue(
            writer->Write(Bytes("a"), false).IsOk(), "abandoned writer admits initial fragment");
    };
    ExpectTrue(
        server.RegisterWebSocket("/abandon", std::move(aborted)).IsOk(), "abandon route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxWebSocketFrameBytes = 4;
    options.maxWebSocketMessageBytes = 6;
    ExpectTrue(server.Start(options).IsOk(), "WebSocket feature server starts");
    if (!server.IsRunning())
        return;
    for (const auto offered : { std::string("Sec-WebSocket-Protocol: v1, v2\r\n"),
             std::string("Sec-WebSocket-Protocol: v1\r\nSec-WebSocket-Protocol: v2\r\n"),
             std::string("Sec-WebSocket-Protocol: other\r\n"), std::string{} })
    {
        Client client(server.Port());
        auto request = handshake;
        request.insert(request.size() - 2, offered);
        client.Send(request);
        const auto response = client.Response();
        const bool matched = offered.find("v1") != std::string::npos;
        ExpectTrue(response.starts_with("HTTP/1.1 101 "), "valid offered protocols upgrade");
        ExpectTrue(
            (response.find("Sec-WebSocket-Protocol: v2\r\n") != std::string::npos) == matched,
            "server preference wins and no match omits header");
        ExpectEqual(matched ? std::string("v2") : std::string{}, client.Frame().second,
            "connection reports selected protocol");
    }
    for (const auto offered : { "Sec-WebSocket-Protocol: v1, v1\r\n",
             "Sec-WebSocket-Protocol: v1\r\nSec-WebSocket-Protocol: v1\r\n",
             "Sec-WebSocket-Protocol: bad token\r\n", "Sec-WebSocket-Protocol: v1,\r\n" })
    {
        Client client(server.Port());
        auto request = handshake;
        request.insert(request.size() - 2, offered);
        client.Send(request);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 400 "),
            "invalid/duplicate offered protocol rejects handshake");
    }
    {
        Client client(server.Port());
        auto request = handshake;
        request.replace(4, 3, "/fragments");
        client.Send(request);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "fragment connection upgrades");
        auto frame = client.Frame(false);
        ExpectTrue(frame.first == 1 && frame.second == std::string("A\xf0", 2),
            "first frame has Text opcode and FIN clear");
        frame = client.Frame();
        ExpectTrue(
            frame.first == 9 && frame.second == "p", "Ping interleaves between data fragments");
        frame = client.Frame();
        ExpectTrue(frame.first == 0 && frame.second == std::string("\x9f\x98\x80"
                                                                   "B",
                                                           4),
            "last frame is final continuation");
        frame = client.Frame(false);
        ExpectTrue(
            frame.first == 2 && frame.second == "1234", "binary message starts with Binary opcode");
        frame = client.Frame();
        ExpectTrue(frame.first == 0 && frame.second == "56",
            "message byte limit includes previous frames");
        ExpectEqual(std::string("end"), client.Frame().second,
            "ordinary sends resume after final fragment");
    }
    {
        Client client(server.Port());
        auto request = handshake;
        request.replace(4, 3, "/abandon");
        client.Send(request);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "),
            "abandon connection upgrades before abort trigger");
        client.Send(MaskedFrame(1, "go"));
        char buffer[64];
        while (ServerCoreTest::Receive(client.socket, buffer, sizeof(buffer)) > 0)
        {
        }
        ExpectTrue(client.Closed(), "dropping a partially written message closes transport");
    }
    ExpectTrue(server.Stop().IsOk(), "feature server stops");
    for (const auto invalidText : { std::string("\xe0\x80", 2), std::string("\xed\xa0", 2),
             std::string("\xf4\x90", 2), std::string("\xff", 1) })
    {
        Detail::Utf8FragmentState state;
        ExpectTrue(!state.Append(Bytes(invalidText), false),
            "invalid partial scalar is rejected before wire admission");
    }
}

/// <summary>
/// 응용 스레드에서 전송을 닫게 만든 호출 안에서 onClose가 불리지 않는지 본다(WEB-3).
/// </summary>
/// <remarks>
/// 시작한 메시지 작성기를 버리면 AbortMessage가 전송을 바로 닫는다. Linux epoll 백엔드는 진행
/// 중인 I/O 콜백이 없으면 끊김 통지를 그 호출 스레드에서 동기로 보낸다. 고치기 전에는 onClose가
/// 이 시험 스레드에서, 피어 잠금을 쥔 채 불렸다. IOCP는 걸린 수신이 있으면 통지를 I/O 스레드로
/// 미루므로 Windows에서는 고치기 전에도 이 시험이 붉어지지 않는다. Windows에서 그 경로는 수신이
/// 멈춘 WebSocket에서만 탄다.
/// </remarks>
void WebSocketCloseFromApplicationThread()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Web::WebSocketConnection> opened;
    std::atomic<std::thread::id> closingThread{};
    std::atomic<unsigned int> closes{ 0 };
    std::atomic<bool> closedOnCaller{ false };
    Web::HttpServer server;
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&](const std::shared_ptr<Web::WebSocketConnection>& connection)
    {
        {
            const std::lock_guard lock(mutex);
            opened = connection;
        }
        changed.notify_all();
    };
    callbacks.onClose = [&](std::uint64_t, std::uint16_t, std::string_view)
    {
        if (closingThread.load() == std::this_thread::get_id())
            closedOnCaller.store(true);
        closes.fetch_add(1);
        changed.notify_all();
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(),
        "application-close route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "application-close server starts");
    if (!server.IsRunning())
        return;
    Client client(server.Port());
    client.Send(handshake);
    ExpectTrue(
        client.Response().starts_with("HTTP/1.1 101 "), "application-close connection upgrades");
    std::shared_ptr<Web::WebSocketConnection> connection;
    {
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, std::chrono::seconds(3), [&] { return opened != nullptr; });
        connection = opened;
    }
    ExpectTrue(connection != nullptr, "onOpen publishes the connection");
    if (!connection)
    {
        (void)server.Stop();
        return;
    }
    auto created = Web::GetWebSocketMessageControl(connection)
                       ->BeginMessage(Web::WebSocketMessageType::Binary);
    ExpectTrue(created.IsOk(), "application thread reserves the message lane");
    if (!created.IsOk())
    {
        (void)server.Stop();
        return;
    }
    auto writer = std::move(created).Value();
    closingThread.store(std::this_thread::get_id());
    ExpectTrue(writer->Write(Bytes("partial"), false).IsOk(),
        "a fragment is admitted before abandoning the message");
    writer->Abort();
    const bool closedByAbort = !connection->IsOpen();
    closingThread.store(std::thread::id{});
    ExpectTrue(closedByAbort,
        "abandoning a started message closes the transport from the application thread");
    {
        std::unique_lock lock(mutex);
        ExpectTrue(
            changed.wait_for(lock, std::chrono::seconds(3), [&] { return closes.load() == 1; }),
            "onClose follows the application-initiated transport close");
    }
    ExpectTrue(!closedOnCaller.load(),
        "onClose never runs inside the application call that closed the transport");
    writer.reset();
    ExpectTrue(server.Stop().IsOk(), "application-close server stops");
    ExpectEqual(1u, closes.load(), "onClose runs exactly once");
}

/// <summary>
/// 정책 뒤의 accept 콜백이 오래 걸려도 다른 연결의 기한이 집행되는지 본다(WEB-5).
/// </summary>
/// <remarks>
/// 고치기 전에는 정책을 통과한 업그레이드의 accept가 유지보수 스레드에서 돌았다. 그 콜백이 붙잡혀 있는
/// 동안에는 어떤 연결의 기한도 확인되지 않으므로, 두 번째 연결의 미완성 요청은 닫히지 않았다.
/// </remarks>
void PolicyCallbacksDoNotStallDeadlines()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    Web::HttpServer server;
    ExpectTrue(server
                   .SetRequestPolicy([](std::shared_ptr<const Web::HttpPolicyContext> context)
                       { (void)context->decision->Allow(); })
                   .IsOk(),
        "immediate policy registers");
    Web::WebSocketCallbacks callbacks;
    callbacks.accept = [&](const Web::HttpRequest&)
    {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        (void)changed.wait_for(lock, std::chrono::seconds(10), [&] { return release; });
        return true;
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(),
        "slow-accept route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.requestTimeout = std::chrono::milliseconds(200);
    ExpectTrue(server.Start(options).IsOk(), "slow-accept server starts");
    if (!server.IsRunning())
        return;
    Client blocked(server.Port());
    blocked.Send(handshake);
    {
        std::unique_lock lock(mutex);
        ExpectTrue(changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }),
            "post-policy accept callback starts");
    }
    {
        Client slow(server.Port());
        slow.Send("GET / HTTP/1.1\r\n");
        ExpectTrue(slow.Closed(),
            "request deadline fires while a post-policy accept callback is still running");
    }
    {
        const std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    ExpectTrue(
        blocked.Response().starts_with("HTTP/1.1 101 "), "released accept completes the upgrade");
    ExpectTrue(server.Stop().IsOk(), "slow-accept server stops");
}

/// <summary>
/// 정책 결정, 응답 완료, 업로드 몫 반환이 유지보수 주기 없이 연결을 다시 진행시키는지 본다(WEB-5).
/// </summary>
/// <remarks>
/// WebSocket 하나의 onMessage를 붙잡아 그 피어의 잠금을 쥐고 있게 한다. 고치기 전의 유지보수 스레드는
/// 한 주기(25ms) 안에 그 피어의 잠금에서 멈추고, 진행은 그 스레드의 폴링에만 기대었으므로 아래 세
/// 요청 모두 멈췄다. 200ms를 기다리는 것은 고치기 전의 실패를 확실히 보이기 위한 것이고, 고친 뒤의
/// 통과는 이 기다림에 기대지 않는다.
/// </remarks>
void ProgressEventsWakeWithoutMaintenance()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    std::shared_ptr<const Web::HttpRequestContext> asyncContext, uploadContext;
    Web::HttpServer server;
    Web::WebSocketCallbacks gated;
    gated.authorize = [](std::shared_ptr<const Web::HttpPolicyContext> context)
    { (void)context->decision->Allow(); };
    ExpectTrue(server.RegisterWebSocket("/gated", std::move(gated)).IsOk(),
        "authorized WebSocket route registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "ready" }; })
                   .IsOk(),
        "buffered route registers");
    ExpectTrue(server
                   .RegisterAsyncRoute("GET", "/async",
                       [&](std::shared_ptr<const Web::HttpRequestContext> context)
                       {
                           {
                               const std::lock_guard lock(mutex);
                               asyncContext = std::move(context);
                           }
                           changed.notify_all();
                       })
                   .IsOk(),
        "async route registers");
    ExpectTrue(server
                   .RegisterStreamingRoute("POST", "/upload",
                       [&](std::shared_ptr<const Web::HttpRequestContext> context)
                       {
                           {
                               const std::lock_guard lock(mutex);
                               uploadContext = std::move(context);
                           }
                           changed.notify_all();
                       })
                   .IsOk(),
        "upload route registers");
    Web::WebSocketCallbacks callbacks;
    callbacks.onMessage =
        [&](const std::shared_ptr<Web::WebSocketConnection>&, const Web::WebSocketMessage&)
    {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        (void)changed.wait_for(lock, std::chrono::seconds(20), [&] { return release; });
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(),
        "holding WebSocket route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.ioWorkerThreadCount = 4;
    options.maxRequestBodyBufferBytes = 4;
    options.maxStreamChunkBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "progress server starts");
    if (!server.IsRunning())
        return;
    Client holder(server.Port());
    holder.Send(handshake);
    ExpectTrue(holder.Response().starts_with("HTTP/1.1 101 "), "holding connection upgrades");
    holder.Send(MaskedFrame(1, "hold"));
    {
        std::unique_lock lock(mutex);
        ExpectTrue(changed.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }),
            "holding onMessage starts");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto take = [&](std::shared_ptr<const Web::HttpRequestContext>& slot)
    {
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, std::chrono::seconds(3), [&] { return slot != nullptr; });
        return std::exchange(slot, nullptr);
    };
    {
        Client client(server.Port());
        std::string request = handshake;
        request.replace(4, 3, "/gated");
        client.Send(request);
        ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "),
            "an allowed policy decision upgrades without the maintenance sweep");
    }
    {
        Client client(server.Port());
        client.Send(
            "GET /async HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto context = take(asyncContext);
        ExpectTrue(context != nullptr, "async request is dispatched");
        if (context)
        {
            ExpectTrue(context->response->Complete({ 200, {}, "async" }).IsOk(),
                "async response completes");
            ExpectTrue(client.Response().ends_with("async"), "async response arrives");
            ExpectTrue(client.Response().ends_with("ready"),
                "a finished async response releases the pipelined request without the maintenance "
                "sweep");
        }
    }
    {
        Client client(server.Port());
        client.Send("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 12\r\n\r\nabcdefghijkl");
        const auto context = take(uploadContext);
        ExpectTrue(context != nullptr && context->body != nullptr,
            "upload request is dispatched with a body reader");
        if (context && context->body)
        {
            std::string received;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (received.size() < 12 && std::chrono::steady_clock::now() < deadline)
            {
                const auto next = context->body->Read();
                if (next.IsOk() && next.Value())
                {
                    received.append(
                        reinterpret_cast<const char*>(next.Value()->data()), next.Value()->size());
                    continue;
                }
                if (next.IsOk() ||
                    next.GetStatus().Code() != ServerCore::Core::ErrorCode::WouldBlock)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ExpectEqual(std::string("abcdefghijkl"), received,
                "released body credit resumes a paused upload without the maintenance sweep");
            ExpectTrue(context->response->Complete({ 200, {}, "stored" }).IsOk(),
                "upload response completes");
            ExpectTrue(client.Response().ends_with("stored"), "upload response arrives");
        }
    }
    {
        const std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    ExpectTrue(server.Stop().IsOk(), "progress server stops");
}

/// <summary>
/// 정책을 기다리는 동안 많이 받아 둔 프레임 뒤에도 업그레이드된 연결이 계속 읽는지 본다(WEB-10).
/// </summary>
/// <remarks>
/// 정책 대기 중 입력이 48 KiB를 넘으면 수신을 멈춘다. 고치기 전에는 101 전환 뒤에 다시 풀지 않아, 받아 둔
/// 프레임만 처리되고 그 뒤의 프레임은 영영 읽히지 않았다. 마지막 "tail" 프레임의 응답이 그것을 판정한다.
/// </remarks>
void EarlyWebSocketDataResumesAfterPolicy()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const Web::HttpPolicyContext> pending;
    Web::HttpServer server;
    ExpectTrue(server
                   .SetRequestPolicy(
                       [&](std::shared_ptr<const Web::HttpPolicyContext> context)
                       {
                           {
                               const std::lock_guard lock(mutex);
                               pending = std::move(context);
                           }
                           changed.notify_all();
                       })
                   .IsOk(),
        "deferred policy registers");
    Web::WebSocketCallbacks callbacks;
    callbacks.onMessage = [](const std::shared_ptr<Web::WebSocketConnection>& connection,
                              const Web::WebSocketMessage& message)
    {
        (void)connection->SendText(
            message.bytes.size() == 4 ? std::string("tail") : std::to_string(message.bytes.size()));
    };
    ExpectTrue(
        server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "early-data route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "early-data server starts");
    if (!server.IsRunning())
        return;
    Client client(server.Port());
    std::string early;
    for (int index = 0; index < 15; ++index)
        early += MaskedFrame(2, std::string(4000, 'e'));
    client.Send(handshake + early);
    std::shared_ptr<const Web::HttpPolicyContext> context;
    {
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, std::chrono::seconds(3), [&] { return pending != nullptr; });
        context = std::move(pending);
    }
    ExpectTrue(context != nullptr, "policy receives the upgrade");
    if (!context)
    {
        (void)server.Stop();
        return;
    }
    // 서버가 정책을 기다리며 48 KiB를 넘게 받아 수신을 멈출 시간을 준다.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ExpectTrue(context->decision->Allow().IsOk(), "policy allows the upgrade");
    ExpectTrue(
        client.Response().starts_with("HTTP/1.1 101 "), "upgrade completes after the policy");
    unsigned echoed = 0;
    for (; echoed < 15; ++echoed)
        if (client.Frame().second != "4000")
            break;
    ExpectEqual(15u, echoed, "every early frame is delivered after the upgrade");
    client.Send(MaskedFrame(2, "tail"));
    ExpectEqual(std::string("tail"), client.Frame().second,
        "the upgraded connection keeps reading after its early data");
    ExpectTrue(server.Stop().IsOk(), "early-data server stops");
}

/// <summary>
/// 정책이 돌려준 성공 응답(예: CORS preflight 204)이 연결을 닫지 않는지 본다(WEB-10).
/// </summary>
/// <summary>
/// 정책이 있는 서버에서 CORS preflight가 경로의 메서드 판정(405)보다 먼저 정책에 닿는지 본다.
/// </summary>
/// <remarks>
/// 고치기 전에는 GET만 등록한 경로로 오는 preflight가 정책을 거치지 않고 405가 되어, CORS 파이프라인을
/// 쓰려면 OPTIONS 경로를 따로 등록해야 했다. 정책이 응답하지 않고 허용만 하면 원래의 405를 그대로 보낸다.
/// 정책이 없는 서버와 preflight가 아닌 OPTIONS는 예전처럼 405다.
/// </remarks>
void CorsPreflightReachesPolicyBeforeMethodCheck()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    const std::string preflight =
        "OPTIONS /api/data HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n"
        "Access-Control-Request-Method: GET\r\n\r\n";
    const auto routes = [](Web::HttpServer& server)
    {
        ExpectTrue(server
                       .RegisterRoute("GET", "/api/data", [](const Web::HttpRequest&)
                           { return Web::HttpResponse{ 200, {}, "data" }; })
                       .IsOk(),
            "GET-only API route registers");
        ExpectTrue(server
                       .RegisterRoute("GET", "/other", [](const Web::HttpRequest&)
                           { return Web::HttpResponse{ 200, {}, "other" }; })
                       .IsOk(),
            "GET-only route outside the CORS prefix registers");
    };
    {
        Web::CorsOptions cors;
        cors.allowedOrigins = { "https://app.example" };
        auto step = Web::CorsPolicy(cors);
        ExpectTrue(step.IsOk(), "CORS step is valid");
        if (!step.IsOk())
            return;
        auto pipeline = Web::HttpPolicyPipeline::Create({ { "/api", std::move(step).Value() } });
        ExpectTrue(pipeline.IsOk(), "CORS pipeline is valid");
        if (!pipeline.IsOk())
            return;
        Web::HttpServer server;
        routes(server);
        ExpectTrue(server.SetRequestPolicy(pipeline.Value().AsRequestPolicy()).IsOk(),
            "CORS pipeline policy registers");
        Web::HttpServerOptions options;
        options.port = FreePort();
        ExpectTrue(server.Start(options).IsOk(), "CORS server starts");
        if (!server.IsRunning())
            return;
        {
            Client client(server.Port());
            client.Send(preflight +
                        "GET /api/data HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
            const auto response = client.Response();
            ExpectTrue(response.starts_with("HTTP/1.1 204 "),
                "a preflight to a GET-only route reaches the CORS policy");
            ExpectTrue(
                response.find("\r\naccess-control-allow-origin: https://app.example\r\n") !=
                        std::string::npos &&
                    response.find("\r\naccess-control-allow-methods: GET, HEAD, POST\r\n") !=
                        std::string::npos &&
                    response.find("\r\naccess-control-max-age: 600\r\n") != std::string::npos,
                "the preflight response carries the CORS allow fields");
            ExpectTrue(response.find("Allow:") == std::string::npos,
                "the preflight response is not a 405");
            const auto actual = client.Response();
            ExpectTrue(
                actual.starts_with("HTTP/1.1 200 ") && actual.ends_with("data") &&
                    actual.find("\r\naccess-control-allow-origin: https://app.example\r\n") !=
                        std::string::npos,
                "the actual request after the preflight is served on the same connection");
        }
        {
            Client client(server.Port());
            std::string denied = preflight;
            denied.replace(denied.find("app.example"), 11, "evil.example");
            client.Send(denied);
            ExpectTrue(client.Response().starts_with("HTTP/1.1 403 "),
                "a preflight from an unapproved origin is rejected by the policy");
        }
        {
            Client client(server.Port());
            client.Send(
                "OPTIONS /api/data HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\n\r\n");
            const auto response = client.Response();
            ExpectTrue(response.starts_with("HTTP/1.1 405 ") &&
                           response.find("\r\nAllow: GET, HEAD\r\n") != std::string::npos,
                "an OPTIONS request that is not a preflight keeps the method check");
        }
        {
            Client client(server.Port());
            std::string outside = preflight;
            outside.replace(outside.find("/api/data"), 9, "/other");
            client.Send(outside);
            const auto response = client.Response();
            ExpectTrue(response.starts_with("HTTP/1.1 405 ") &&
                           response.find("\r\nAllow: GET, HEAD\r\n") != std::string::npos,
                "a preflight the policy only allows still gets the route's 405");
        }
        ExpectTrue(server.Stop().IsOk(), "CORS server stops");
    }
    {
        Web::HttpServer server;
        routes(server);
        Web::HttpServerOptions options;
        options.port = FreePort();
        ExpectTrue(server.Start(options).IsOk(), "policy-free server starts");
        if (!server.IsRunning())
            return;
        Client client(server.Port());
        client.Send(preflight);
        const auto response = client.Response();
        ExpectTrue(response.starts_with("HTTP/1.1 405 ") &&
                       response.find("\r\nAllow: GET, HEAD\r\n") != std::string::npos,
            "without a policy a preflight keeps the method check");
        ExpectTrue(server.Stop().IsOk(), "policy-free server stops");
    }
}

void PolicyResponsesKeepConnectionAlive()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    ExpectTrue(server
                   .SetRequestPolicy(
                       [](std::shared_ptr<const Web::HttpPolicyContext> context)
                       {
                           if (context->request.method == "OPTIONS")
                               (void)context->decision->Reject(
                                   { 204, { { "Access-Control-Allow-Origin", "*" } }, {} });
                           else if (context->request.Path() == "/closing")
                               (void)context->decision->Reject({ 403, {}, "no", true });
                           else
                               (void)context->decision->Allow();
                       })
                   .IsOk(),
        "preflight policy registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/health", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "ready" }; })
                   .IsOk(),
        "preflight route registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/closing", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "unreachable" }; })
                   .IsOk(),
        "closing route registers");
    // 정책은 경로와 메서드가 맞은 요청에서만 돈다. preflight가 정책에 닿도록 OPTIONS 경로를 둔다.
    ExpectTrue(server
                   .RegisterRoute("OPTIONS", "/health", [](const Web::HttpRequest&)
                       { return Web::HttpResponse{ 200, {}, "unreachable" }; })
                   .IsOk(),
        "preflight OPTIONS route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "preflight server starts");
    if (!server.IsRunning())
        return;
    {
        Client client(server.Port());
        client.Send("OPTIONS /health HTTP/1.1\r\nHost: x\r\nOrigin: "
                    "https://a.example\r\nAccess-Control-Request-Method: GET\r\n\r\n"
                    "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto preflight = client.Response();
        ExpectTrue(preflight.starts_with("HTTP/1.1 204 ") &&
                       preflight.find("Connection: keep-alive\r\n") != std::string::npos,
            "a policy preflight response keeps the connection alive");
        ExpectTrue(client.Response().ends_with("ready"),
            "the pipelined request after the preflight is served");
    }
    {
        Client client(server.Port());
        client.Send("GET /closing HTTP/1.1\r\nHost: x\r\n\r\n");
        const auto rejected = client.Response();
        ExpectTrue(rejected.starts_with("HTTP/1.1 403 ") &&
                       rejected.find("Connection: close\r\n") != std::string::npos &&
                       client.Closed(),
            "a policy response that asks to close still closes");
    }
    ExpectTrue(server.Stop().IsOk(), "preflight server stops");
}

/// <summary>
/// 진행 중인 스트리밍 업로드가 요청·핸들러 기한에 끊기지 않고, 멈춘 업로드는 여전히 닫히는지 본다(WEB-10).
/// </summary>
/// <remarks>
/// 고치기 전에는 요청 기한과 핸들러 기한이 둘 다 디스패치 무렵부터 절대 시간으로 흘러, 기본 30초 안에 끝나지
/// 않는 업로드는 maxStreamedBodyBytes(기본 1 GiB)와 상관없이 끊겼다. 여기서는 두 기한을 500ms로 두고
/// 100ms 간격으로 1.2초 동안 보낸다.
/// </remarks>
void StreamingUploadOutlivesRequestDeadline()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const Web::HttpRequestContext> upload;
    Web::HttpServer server;
    ExpectTrue(server
                   .RegisterStreamingRoute("POST", "/upload",
                       [&](std::shared_ptr<const Web::HttpRequestContext> context)
                       {
                           {
                               const std::lock_guard lock(mutex);
                               upload = std::move(context);
                           }
                           changed.notify_all();
                       })
                   .IsOk(),
        "slow-upload route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.requestTimeout = std::chrono::milliseconds(500);
    options.handlerTimeout = std::chrono::milliseconds(500);
    ExpectTrue(server.Start(options).IsOk(), "slow-upload server starts");
    if (!server.IsRunning())
        return;
    const auto take = [&]
    {
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, std::chrono::seconds(3), [&] { return upload != nullptr; });
        return std::exchange(upload, nullptr);
    };
    {
        Client client(server.Port());
        client.Send("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 120\r\n\r\n");
        const auto context = take();
        ExpectTrue(context != nullptr && context->body != nullptr,
            "slow upload is dispatched with a body reader");
        if (context && context->body)
        {
            std::thread sender(
                [&client]
                {
                    for (int index = 0; index < 12; ++index)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        (void)ServerCoreTest::Send(client.socket, "0123456789", 10);
                    }
                });
            std::size_t received = 0;
            bool ended = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ended && std::chrono::steady_clock::now() < deadline)
            {
                const auto next = context->body->Read();
                if (next.IsOk() && next.Value())
                {
                    received += next.Value()->size();
                    continue;
                }
                if (next.IsOk())
                {
                    ended = true;
                    break;
                }
                if (next.GetStatus().Code() != ServerCore::Core::ErrorCode::WouldBlock)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            sender.join();
            ExpectTrue(ended && received == 120,
                "a progressing upload is not cut by the request or handler deadline");
            ExpectTrue(context->response->Complete({ 200, {}, "stored" }).IsOk(),
                "the handler deadline starts when the body ends");
            ExpectTrue(client.Response().ends_with("stored"), "slow upload response arrives");
        }
    }
    {
        Client stalled(server.Port());
        stalled.Send("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n0123456789");
        const auto context = take();
        ExpectTrue(context != nullptr, "stalled upload is dispatched");
        ExpectTrue(stalled.Closed(),
            "an upload that stops making progress is closed by the request deadline");
    }
    ExpectTrue(server.Stop().IsOk(), "slow-upload server stops");
}

/// <summary>소켓의 블로킹 모드를 바꾼다. 수신 멈춤 시험에서 보낸 쪽이 막히는 것을 보는 데 쓴다.</summary>
bool SetBlocking(ServerCoreTest::Socket socket, bool blocking)
{
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags != -1 &&
           ::fcntl(socket, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

/// <summary>
/// WebSocketFlowControl::PauseReceive·ResumeReceive가 그 연결만 멈추고, 재개하면 쌓인 메시지를 순서대로
/// 내며, 멈춘 채 Close하면 closing handshake가 끝나는지 본다.
/// </summary>
/// <remarks>
/// 경로가 실제로 돌았다는 것은 시험이 스스로 단언한다. 멈춘 동안 받은 수가 0이고, 보낸 쪽이 TCP에서 막히며,
/// 재개 뒤 받은 수가 보낸 수와 같다. 멈춤은 첫 메시지의 onMessage 안에서 걸고, 같은 전송 묶음으로 뒤따른
/// 작은 메시지 20개가 버퍼에 남은 채 멈춰야 한다.
/// </remarks>
void WebSocketReceivePauseAndResume()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    constexpr unsigned Buffered = 20;
    constexpr unsigned Bulk = 2048;
    constexpr std::size_t BulkPayload = 16 * 1024;
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Web::WebSocketConnection> pausing;
    bool pausedInCallback = false;
    unsigned others = 0;
    std::vector<std::string> received;
    std::atomic<unsigned> closes{ 0 };
    std::atomic<std::uint16_t> closeCode{ 0 };
    Web::HttpServer server;
    Web::WebSocketCallbacks callbacks;
    callbacks.onMessage = [&](const std::shared_ptr<Web::WebSocketConnection>& connection,
                              const Web::WebSocketMessage& message)
    {
        const auto text = Text(message.bytes);
        if (text == "pause-now")
        {
            const auto flow = Web::GetWebSocketFlowControl(connection);
            const bool paused = flow && flow->PauseReceive().IsOk() && flow->IsReceivePaused();
            {
                const std::lock_guard lock(mutex);
                pausing = connection;
                pausedInCallback = paused;
            }
        }
        else if (text == "other")
        {
            const std::lock_guard lock(mutex);
            ++others;
        }
        else
        {
            const std::lock_guard lock(mutex);
            received.push_back(text.substr(0, 7));
        }
        changed.notify_all();
    };
    callbacks.onClose = [&](std::uint64_t, std::uint16_t code, std::string_view)
    {
        closeCode.store(code);
        closes.fetch_add(1);
        changed.notify_all();
    };
    ExpectTrue(
        server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(), "pause route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    ExpectTrue(server.Start(options).IsOk(), "pause server starts");
    if (!server.IsRunning())
        return;
    Client held(server.Port());
    held.Send(handshake);
    ExpectTrue(held.Response().starts_with("HTTP/1.1 101 "), "the paused connection upgrades");
    Client other(server.Port());
    other.Send(handshake);
    ExpectTrue(other.Response().starts_with("HTTP/1.1 101 "), "the other connection upgrades");
    const auto id = [](unsigned index)
    {
        std::array<char, 8> digits{};
        (void)std::snprintf(digits.data(), digits.size(), "m%06u", index);
        return std::string(digits.data());
    };
    std::string burst = MaskedFrame(1, "pause-now");
    for (unsigned index = 0; index < Buffered; ++index)
        burst += MaskedFrame(1, id(index));
    held.Send(burst);
    const auto wait = [&](auto predicate, std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, predicate);
    };
    ExpectTrue(wait([&] { return pausing != nullptr; }, std::chrono::seconds(3)),
        "the pause message arrives");
    std::shared_ptr<Web::WebSocketFlowControl> flow;
    {
        const std::lock_guard lock(mutex);
        ExpectTrue(pausedInCallback, "PauseReceive from onMessage succeeds and reports the pause");
        if (pausing)
            flow = Web::GetWebSocketFlowControl(pausing);
    }
    if (!flow)
    {
        (void)server.Stop();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        const std::lock_guard lock(mutex);
        ExpectEqual(std::size_t{ 0 }, received.size(),
            "frames buffered behind the pause are not delivered while paused");
    }
    other.Send(MaskedFrame(1, "other"));
    ExpectTrue(wait([&] { return others == 1; }, std::chrono::seconds(3)),
        "another connection keeps receiving while one is paused");

    std::string bulk;
    bulk.reserve(Bulk * (BulkPayload + 14));
    for (unsigned index = 0; index < Bulk; ++index)
    {
        auto payload = id(Buffered + index);
        payload.resize(BulkPayload, 'x');
        bulk += MaskedFrame(2, payload);
    }
    ExpectTrue(SetBlocking(held.socket, false), "the sender switches to nonblocking sends");
    std::size_t offset = 0;
    bool blocked = false;
    while (offset < bulk.size() && !blocked)
    {
        const auto count =
            static_cast<int>((std::min)(bulk.size() - offset, std::size_t{ 64 * 1024 }));
        const int sent = ServerCoreTest::Send(held.socket, bulk.data() + offset, count);
        if (sent > 0)
        {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && ServerCoreTest::SocketWouldBlock(ServerCoreTest::LastSocketError()))
        {
            // 한 번 막힌 것이 일시적이지 않은지 잠시 뒤 다시 본다. 서버가 읽지 않으면 계속 막혀 있다.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const int retry = ServerCoreTest::Send(held.socket, bulk.data() + offset, count);
            if (retry > 0)
            {
                offset += static_cast<std::size_t>(retry);
                continue;
            }
            blocked =
                retry < 0 && ServerCoreTest::SocketWouldBlock(ServerCoreTest::LastSocketError());
            if (!blocked)
                break;
            continue;
        }
        ExpectTrue(false, "nonblocking bulk send fails only with would-block");
        break;
    }
    ExpectTrue(blocked && offset < bulk.size(),
        "the sender blocks at the TCP level while receive is paused");
    other.Send(MaskedFrame(1, "other"));
    ExpectTrue(wait([&] { return others == 2; }, std::chrono::seconds(3)),
        "a stalled paused connection does not stall the other one");
    {
        const std::lock_guard lock(mutex);
        ExpectEqual(std::size_t{ 0 }, received.size(),
            "nothing from the paused connection is delivered before resume");
    }

    ExpectTrue(flow->ResumeReceive().IsOk() && !flow->IsReceivePaused(),
        "resume succeeds and clears the pause");
    ExpectTrue(SetBlocking(held.socket, true), "the sender switches back to blocking sends");
    ExpectTrue(held.Send(std::string_view(bulk).substr(offset)),
        "the rest of the bulk is sent after resume");
    ExpectTrue(wait([&] { return received.size() == Buffered + Bulk; }, std::chrono::seconds(20)),
        "every buffered and bulk message is delivered after resume");
    {
        const std::lock_guard lock(mutex);
        bool ordered = received.size() == Buffered + Bulk;
        for (unsigned index = 0; ordered && index < received.size(); ++index)
            ordered = received[index] == id(index);
        ExpectTrue(ordered, "messages arrive after resume in the order they were sent");
    }

    ExpectTrue(flow->PauseReceive().IsOk() && flow->IsReceivePaused(),
        "receive can be paused again from another thread");
    ExpectTrue(pausing->Close(1000, "done").IsOk(), "a paused connection can start a close");
    ExpectTrue(!flow->IsReceivePaused(), "Close lifts the receive pause");
    const auto frame = held.Frame();
    ExpectTrue(frame.first == 8, "the client receives the server close frame");
    held.Send(MaskedFrame(8, std::string("\x03\xe8", 2)));
    // 서버는 close 답을 읽은 뒤에야 보내기 쪽을 닫는다. 그 EOF를 본 뒤 클라이언트도 닫아 연결을 끝낸다.
    ExpectTrue(
        held.Closed(), "the server reads the close reply and ends its side after a paused Close");
    ServerCoreTest::CloseSocket(held.socket);
    held.socket = ServerCoreTest::InvalidSocket;
    ExpectTrue(wait([&] { return closes.load() == 1; }, std::chrono::seconds(3)),
        "onClose follows the completed closing handshake");
    ExpectEqual(std::uint16_t{ 1000 }, closeCode.load(),
        "the paused connection completes the closing handshake");
    ExpectTrue(server.Stop().IsOk(), "pause server stops");
}

/// <summary>
/// 수신을 멈춘 WebSocket에서 heartbeat는 쉬고 idle 기한은 그대로인지 본다.
/// </summary>
/// <remarks>
/// onOpen에서 바로 멈춘다. ping 간격 30ms·pong 기한 120ms로 두고 500ms 동안 아무 프레임도 오지 않아야 한다.
/// heartbeat가 멈춘 연결에도 돌면 ping을 보내고, 멈춘 채로는 pong을 읽지 못해 1001로 닫는다. 재개하면
/// ping이 다시 온다. idle 기한은 300ms로 둔 다른 서버에서 멈춘 연결이 닫히는 것으로 본다.
/// </remarks>
void WebSocketReceivePauseSuspendsHeartbeat()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Web::WebSocketFlowControl> flow;
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&](const std::shared_ptr<Web::WebSocketConnection>& connection)
    {
        auto control = Web::GetWebSocketFlowControl(connection);
        ExpectTrue(control && control->PauseReceive().IsOk(), "onOpen pauses receive");
        {
            const std::lock_guard lock(mutex);
            flow = std::move(control);
        }
        changed.notify_all();
    };
    const auto opened = [&]
    {
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, std::chrono::seconds(3), [&] { return flow != nullptr; });
        return std::exchange(flow, nullptr);
    };
    {
        Web::HttpServer server;
        ExpectTrue(
            server.RegisterWebSocket("/ws", callbacks).IsOk(), "heartbeat pause route registers");
        Web::HttpServerOptions options;
        options.port = FreePort();
        options.webSocketPingInterval = std::chrono::milliseconds(30);
        options.webSocketPongTimeout = std::chrono::milliseconds(120);
        ExpectTrue(server.Start(options).IsOk(), "heartbeat pause server starts");
        if (!server.IsRunning())
            return;
        Client client(server.Port());
        client.Send(handshake);
        ExpectTrue(
            client.Response().starts_with("HTTP/1.1 101 "), "heartbeat pause connection upgrades");
        const auto control = opened();
        ExpectTrue(control != nullptr, "the paused connection is published");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ExpectTrue(SetBlocking(client.socket, false), "the client peeks without blocking");
        char byte = 0;
        const int peeked = ServerCoreTest::Receive(client.socket, &byte, 1, MSG_PEEK);
        ExpectTrue(
            peeked < 0 && ServerCoreTest::SocketWouldBlock(ServerCoreTest::LastSocketError()),
            "no ping or close frame is sent while receive is paused");
        ExpectTrue(SetBlocking(client.socket, true), "the client blocks again");
        if (control)
            ExpectTrue(control->ResumeReceive().IsOk(), "receive resumes");
        const auto ping = client.Frame();
        ExpectTrue(
            ping.first == 9 && ping.second.size() == 16, "the heartbeat resumes with a ping");
        client.Send(MaskedFrame(10, ping.second));
        // pong을 읽었으면 다음 ping이 오고, 못 읽었으면 pong 기한 뒤 1001 close가 온다.
        ExpectTrue(client.Frame().first == 9,
            "the resumed connection reads the pong and keeps its heartbeat");
        ExpectTrue(server.Stop().IsOk(), "heartbeat pause server stops");
    }
    {
        Web::HttpServer server;
        ExpectTrue(server.RegisterWebSocket("/ws", callbacks).IsOk(), "idle pause route registers");
        Web::HttpServerOptions options;
        options.port = FreePort();
        options.idleTimeout = std::chrono::milliseconds(300);
        ExpectTrue(server.Start(options).IsOk(), "idle pause server starts");
        if (!server.IsRunning())
            return;
        Client client(server.Port());
        client.Send(handshake);
        ExpectTrue(
            client.Response().starts_with("HTTP/1.1 101 "), "idle pause connection upgrades");
        const auto control = opened();
        ExpectTrue(
            control != nullptr && control->IsReceivePaused(), "the idle connection is paused");
        ExpectTrue(client.Closed(), "idleTimeout still closes a paused connection");
        ExpectTrue(server.Stop().IsOk(), "idle pause server stops");
    }
}

void WebSocketHeartbeatCorrelation()
{
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    Web::HttpServer server;
    ExpectTrue(server.RegisterWebSocket("/ws", {}).IsOk(), "heartbeat route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.webSocketPingInterval = std::chrono::milliseconds(30);
    options.webSocketPongTimeout = std::chrono::milliseconds(120);
    options.webSocketCloseTimeout = std::chrono::milliseconds(80);
    ExpectTrue(server.Start(options).IsOk(), "heartbeat server starts");
    if (!server.IsRunning())
        return;
    Client client(server.Port());
    client.Send(handshake);
    ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "heartbeat connection upgrades");
    const auto first = client.Frame();
    ExpectTrue(first.first == 9 && first.second.size() == 16,
        "automatic ping carries correlation payload");
    client.Send(MaskedFrame(10, first.second));
    const auto second = client.Frame();
    ExpectTrue(second.first == 9 && second.second != first.second,
        "matching pong permits next distinct heartbeat");
    client.Send(MaskedFrame(10, first.second));
    const auto close = client.Frame();
    ExpectTrue(close.first == 8 && close.second.size() >= 2 &&
                   close.second.substr(0, 2) == std::string("\x03\xe9", 2),
        "stale pong does not postpone heartbeat timeout");
    ExpectTrue(client.Closed(), "unresponsive peer closes within close deadline");
    ExpectTrue(server.Stop().IsOk(), "heartbeat server stops");
}

void WebSocketFragmentBackpressure()
{
    using ServerCore::Core::ErrorCode;
    ServerCoreTest::SocketRuntime runtime;
    if (!runtime.IsReady())
    {
        ExpectTrue(false, "socket runtime initializes");
        return;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Web::WebSocketMessageWriter> writer;
    std::size_t admitted = 0;
    bool blocked = false, finished = false;
    Web::HttpServer server;
    Web::WebSocketCallbacks callbacks;
    callbacks.onOpen = [&](const auto& socket)
    {
        auto created = Web::GetWebSocketMessageControl(socket)->BeginMessage(
            Web::WebSocketMessageType::Binary);
        if (!created.IsOk())
        {
            std::lock_guard lock(mutex);
            finished = true;
            changed.notify_all();
            return;
        }
        auto current = std::move(created).Value();
        const std::string payload(64 * 1024, 'x');
        std::size_t count = 0;
        bool pressure = false;
        for (; count < 128; ++count)
        {
            const auto result = current->Write(Bytes(payload), false);
            if (!result.IsOk())
            {
                pressure = result.Code() == ErrorCode::WouldBlock;
                break;
            }
        }
        {
            std::lock_guard lock(mutex);
            writer = std::move(current);
            admitted = count;
            blocked = pressure;
            finished = true;
        }
        changed.notify_all();
    };
    ExpectTrue(server.RegisterWebSocket("/ws", std::move(callbacks)).IsOk(),
        "backpressure route registers");
    Web::HttpServerOptions options;
    options.port = FreePort();
    options.maxWebSocketMessageBytes = 16 * 1024 * 1024;
    ExpectTrue(server.Start(options).IsOk(), "fragment pressure server starts");
    if (!server.IsRunning())
        return;
    Client client(server.Port());
    const int receiveBytes = 64 * 1024;
    ExpectTrue(::setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&receiveBytes), sizeof(receiveBytes)) == 0,
        "small receive window stops unbounded transport progress");
    client.Send(handshake);
    ExpectTrue(client.Response().starts_with("HTTP/1.1 101 "), "pressure connection upgrades");
    bool completed = false;
    {
        std::unique_lock lock(mutex);
        completed = changed.wait_for(lock, std::chrono::seconds(3), [&] { return finished; });
        ExpectTrue(completed, "bounded producer reports capacity pressure");
    }
    if (!completed)
    {
        (void)server.Stop();
        return;
    }
    ExpectTrue(
        blocked && admitted != 0 && writer, "unread peer yields WouldBlock before message limit");
    if (!writer)
    {
        (void)server.Stop();
        return;
    }
    std::atomic<bool> ready{ false };
    auto wait =
        writer->WaitForWriteCapacity(64 * 1024, [&](auto status) { ready.store(status.IsOk()); });
    ExpectTrue(wait.IsOk(), "fragment capacity notification registers");
    for (std::size_t index = 0; index < admitted; ++index)
    {
        const auto frame = client.Frame(false);
        ExpectTrue(
            frame.first == (index == 0 ? 2u : 0u) && frame.second == std::string(64 * 1024, 'x'),
            "each successful write appears exactly once");
        if (frame.second.empty())
            break;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ExpectTrue(ready.load(), "draining frames signals send capacity");
    if (wait.IsOk())
        wait.Value().Reset();
    ExpectTrue(
        writer->Write(Bytes("done"), true).IsOk(), "retry after pressure completes same message");
    const auto last = client.Frame();
    ExpectTrue(last.first == 0 && last.second == "done", "rejected frame added no hidden bytes");
    writer.reset();
    ExpectTrue(server.Stop().IsOk(), "pressure server stops");
}

ServerCoreTest::CheckRegistration wsFeatures(
    "Web.WebSocketNegotiationAndFragments", &WebSocketNegotiationAndFragments);
ServerCoreTest::CheckRegistration wsHeartbeat(
    "Web.WebSocketHeartbeatCorrelation", &WebSocketHeartbeatCorrelation);
ServerCoreTest::CheckRegistration wsApplicationClose(
    "Web.WebSocketCloseFromApplicationThread", &WebSocketCloseFromApplicationThread);
ServerCoreTest::CheckRegistration wsReceivePause(
    "Web.WebSocketReceivePauseAndResume", &WebSocketReceivePauseAndResume);
ServerCoreTest::CheckRegistration wsPauseHeartbeat(
    "Web.WebSocketReceivePauseSuspendsHeartbeat", &WebSocketReceivePauseSuspendsHeartbeat);
ServerCoreTest::CheckRegistration policyDeadlines(
    "Web.PolicyCallbacksDoNotStallDeadlines", &PolicyCallbacksDoNotStallDeadlines);
ServerCoreTest::CheckRegistration progressEvents(
    "Web.ProgressEventsWakeWithoutMaintenance", &ProgressEventsWakeWithoutMaintenance);
ServerCoreTest::CheckRegistration earlyData(
    "Web.EarlyWebSocketDataResumesAfterPolicy", &EarlyWebSocketDataResumesAfterPolicy);
ServerCoreTest::CheckRegistration policyKeepAlive(
    "Web.PolicyResponsesKeepConnectionAlive", &PolicyResponsesKeepConnectionAlive);
ServerCoreTest::CheckRegistration corsPreflight("Web.CorsPreflightReachesPolicyBeforeMethodCheck",
    &CorsPreflightReachesPolicyBeforeMethodCheck);
ServerCoreTest::CheckRegistration slowUpload(
    "Web.StreamingUploadOutlivesRequestDeadline", &StreamingUploadOutlivesRequestDeadline);
ServerCoreTest::CheckRegistration wsPressure(
    "Web.WebSocketFragmentBackpressure", &WebSocketFragmentBackpressure);
ServerCoreTest::CheckRegistration httpParser("Web.HttpParserConformance", &HttpParserConformance);
ServerCoreTest::CheckRegistration httpRejected(
    "Web.HttpParserRejectsAmbiguousRequests", &HttpParserRejectsAmbiguousRequests);
ServerCoreTest::CheckRegistration httpConsumed(
    "Web.HttpParserReportsConsumedBytes", &HttpParserReportsConsumedBytes);
ServerCoreTest::CheckRegistration httpInterop(
    "Web.HttpParserInteroperability", &HttpParserInteroperability);
ServerCoreTest::CheckRegistration http10(
    "Web.Http10AndLenientRequestLines", &Http10AndLenientRequestLines);
ServerCoreTest::CheckRegistration policyLists(
    "Web.PolicyHeadersKeepListFields", &PolicyHeadersKeepListFields);
ServerCoreTest::CheckRegistration silentConnections(
    "Web.SilentConnectionsExpire", &SilentConnectionsExpire);
ServerCoreTest::CheckRegistration drainIdle(
    "Web.DrainBoundsIdleKeepAliveConnections", &DrainBoundsIdleKeepAliveConnections);
ServerCoreTest::CheckRegistration wsProtocol(
    "Web.WebSocketProtocolConformance", &WebSocketProtocolConformance);
ServerCoreTest::CheckRegistration httpSocket("Web.HttpSocketIntegration", &HttpSocketIntegration);
ServerCoreTest::CheckRegistration wsSocket(
    "Web.WebSocketSocketIntegration", &WebSocketSocketIntegration);
ServerCoreTest::CheckRegistration limits("Web.LimitsAndShutdown", &LimitsAndShutdown);
ServerCoreTest::CheckRegistration rejectedFrames(
    "Web.WebSocketRejectsProtocolErrors", &WebSocketRejectsProtocolErrors);
ServerCoreTest::CheckRegistration connectionLimit(
    "Web.ConnectionLimitAndActiveShutdown", &ConnectionLimitAndActiveShutdown);
ServerCoreTest::CheckRegistration responseSemantics(
    "Web.HttpResponseSemantics", &HttpResponseSemantics);
ServerCoreTest::CheckRegistration sendValidation(
    "Web.WebSocketSendValidationSymmetry", &WebSocketSendValidationSymmetry);
}
