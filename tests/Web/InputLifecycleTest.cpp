#include "SocketTestSupport.h"
#include "TestHarness.h"
#include "ServerCore/Web/HttpServer.h"
#include "ServerCore/Web/HttpStreaming.h"
#include "Web/FileResponseInternal.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
namespace Web = ServerCore::Web;
using ServerCore::Core::ErrorCode;
using ServerCoreTest::ExpectTrue;
using ServerCoreTest::ExpectEqual;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class Client
{
public:
    ~Client() { ServerCoreTest::CloseSocket(socket); }
    bool Open(std::uint16_t port)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == ServerCoreTest::InvalidSocket || !ServerCoreTest::SetSocketTimeouts(socket, 2000)) return false;
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    bool Send(std::string_view text)
    {
        while (!text.empty())
        {
            const auto count = ServerCoreTest::Send(socket, text.data(), static_cast<int>(text.size()));
            if (count <= 0) return false;
            text.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }
    std::string Read(bool headersOnly = false)
    {
        std::string output;
        std::array<char, 2048> bytes{};
        while (output.size() < 65536)
        {
            const auto count = ServerCoreTest::Receive(socket, bytes.data(), static_cast<int>(bytes.size()));
            if (count <= 0) break;
            output.append(bytes.data(), static_cast<std::size_t>(count));
            if (headersOnly && output.find("\r\n\r\n") != std::string::npos) break;
        }
        return output;
    }
private:
    ServerCoreTest::Socket socket = ServerCoreTest::InvalidSocket;
};
std::uint16_t Port()
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ServerCoreTest::SocketLength length = sizeof(address);
    const bool valid = ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ServerCoreTest::CloseSocket(socket);
    return valid ? ntohs(address.sin_port) : 0;
}
template<class T> class Inbox
{
public:
    void Put(std::shared_ptr<const T> value)
    { const std::lock_guard guard(mutex); item = std::move(value); changed.notify_all(); }
    std::shared_ptr<const T> Take()
    {
        std::unique_lock guard(mutex);
        if (!changed.wait_for(guard, 2s, [&] { return item != nullptr; })) return {};
        return std::exchange(item, {});
    }
private:
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const T> item;
};
auto Next(Web::HttpRequestBody& body)
{
    const auto deadline = Clock::now() + 2s;
    for (;;)
    {
        auto result = body.Read();
        if (result.IsOk() || result.GetStatus().Code() != ErrorCode::WouldBlock || Clock::now() >= deadline) return result;
        std::this_thread::sleep_for(2ms);
    }
}
std::string UploadHead(std::string_view framing)
{ return "POST /upload/value HTTP/1.1\r\nHost: local\r\nConnection: close\r\n" + std::string(framing) + "\r\n\r\n"; }

void StreamingUploadBackpressure()
{
    ServerCoreTest::SocketRuntime runtime;
    ExpectTrue(runtime.IsReady(), "streaming socket runtime starts");
    Inbox<Web::HttpRequestContext> inbox;
    Web::HttpServer server;
    ExpectTrue(server.RegisterStreamingRoutePattern("POST", "/upload/{id}", [&](auto context) { inbox.Put(std::move(context)); }).IsOk(),
        "streaming route registers");
    Web::HttpServerOptions options; options.port = Port(); options.maxBodyBytes = 8; options.maxStreamedBodyBytes = 1024;
    options.maxRequestBodyBufferBytes = 4; options.maxStreamChunkBytes = 4;
    ExpectTrue(server.Start(options).IsOk(), "bounded upload server starts");
    Client client;
    ExpectTrue(client.Open(server.Port()) && client.Send(UploadHead("Content-Length: 12") + "abcdefghijkl"), "large buffered-limit upload sends");
    auto context = inbox.Take();
    if (!context || !context->body) { ExpectTrue(false, "streaming request arrives with reader"); return; }
    ExpectTrue(context->request.body.empty() && context->request.PathParameter("id") == "value", "head dispatch retains route metadata without buffering body");
    std::string received;
    auto held = Next(*context->body);
    if (!held.IsOk() || !held.Value()) { ExpectTrue(false, "first owned chunk arrives"); return; }
    received.append(reinterpret_cast<const char*>(held.Value()->data()), held.Value()->size());
    ExpectEqual(std::size_t{4}, context->body->RetainedBytes(), "popped chunk remains charged");
    std::this_thread::sleep_for(50ms);
    ExpectTrue(context->body->Read().GetStatus().Code() == ErrorCode::WouldBlock, "retained chunk prevents further body admission");
    held.Value().reset();
    for (unsigned index = 0; index < 8; ++index)
    {
        auto next = Next(*context->body);
        if (!next.IsOk()) { ExpectTrue(false, "upload resumes after chunk release"); break; }
        if (!next.Value()) break;
        received.append(reinterpret_cast<const char*>(next.Value()->data()), next.Value()->size());
    }
    ExpectEqual(std::string("abcdefghijkl"), received, "fixed body streams exactly once beyond buffered body limit");
    context->body->Cancel();
    ExpectTrue(!context->body->GetCancellationToken().stop_requested(), "Cancel after clean EOF does not abort successful response");
    ExpectTrue(context->response->Complete({200, {}, "done"}).IsOk(), "uploaded request can finish");
    ExpectTrue(client.Read().ends_with("done"), "response follows fully consumed upload");

    Client chunked;
    ExpectTrue(chunked.Open(server.Port()) && chunked.Send(UploadHead("Transfer-Encoding: chunked") + "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"), "chunked upload sends");
    auto chunks = inbox.Take();
    if (chunks && chunks->body)
    {
        std::string text;
        for (unsigned index = 0; index < 8; ++index)
        {
            auto next = Next(*chunks->body);
            if (!next.IsOk() || !next.Value()) break;
            text.append(reinterpret_cast<const char*>(next.Value()->data()), next.Value()->size());
        }
        ExpectEqual(std::string("abcde"), text, "chunk framing is removed incrementally");
        ExpectTrue(chunks->response->Complete({200, {}, "chunks"}).IsOk(), "chunked upload response completes");
        ExpectTrue(chunked.Read().ends_with("chunks"), "chunked upload keeps response framing intact");
    }
    else ExpectTrue(false, "chunked body reader arrives");

    Client cancelled;
    ExpectTrue(cancelled.Open(server.Port()) && cancelled.Send(UploadHead("Content-Length: 100") + "x"), "partial upload sends");
    auto partial = inbox.Take();
    if (partial && partial->body)
    {
        partial->body->Cancel();
        ExpectTrue(partial->body->Read().GetStatus().Code() == ErrorCode::Cancelled, "cancelled upload has stable terminal error");
    }
    ExpectTrue(server.Stop().IsOk(), "upload shutdown joins readers and responses");
    if (partial) ExpectTrue(partial->response->GetCancellationToken().stop_requested(), "upload cancellation reaches response writer");
}

void RequestPolicyAndWebSocketGate()
{
    ServerCoreTest::SocketRuntime runtime;
    ExpectTrue(runtime.IsReady(), "policy socket runtime starts");
    Inbox<Web::HttpPolicyContext> policies;
    Inbox<Web::HttpRequestContext> requests;
    std::atomic<unsigned> opened{0}, messages{0};
    std::atomic<ErrorCode> drainFromOpen{ErrorCode::Ok}, drainFromMessage{ErrorCode::Ok};
    Web::HttpServer server;
    ExpectTrue(server.SetRequestPolicy([&](auto context) { policies.Put(std::move(context)); }).IsOk(), "global asynchronous policy registers");
    ExpectTrue(server.RegisterAsyncRoute("GET", "/secure", [&](auto context) { requests.Put(std::move(context)); }).IsOk(), "policy route registers");
    Web::WebSocketCallbacks callbacks;
    callbacks.authorize = [&](auto context) { policies.Put(std::move(context)); };
    callbacks.onOpen = [&](const auto&) { drainFromOpen.store(server.BeginDrain().Code()); opened.fetch_add(1); };
    callbacks.onMessage = [&](const auto&, const auto&) { drainFromMessage.store(server.BeginDrain().Code()); messages.fetch_add(1); };
    ExpectTrue(server.RegisterWebSocket("/socket", std::move(callbacks)).IsOk(), "deferred WebSocket route registers");
    Web::HttpServerOptions options; options.port = Port(); options.handlerTimeout = 250ms;
    ExpectTrue(server.Start(options).IsOk(), "policy server starts");
    Client client;
    ExpectTrue(client.Open(server.Port()) && client.Send("GET /secure HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n"), "policy request sends");
    auto decision = policies.Take();
    if (!decision) { ExpectTrue(false, "owned policy decision arrives"); return; }
    ExpectTrue(decision->decision->Allow({{"Access-Control-Allow-Origin", "https://example.test"}}, {{"principal", "alice"}}).IsOk(), "policy carries CORS defaults and owned principal metadata");
    ExpectTrue(decision->decision->Reject({403, {}, "late"}).Code() == ErrorCode::Closed, "policy resolves once");
    auto request = requests.Take();
    if (!request) { ExpectTrue(false, "authorized request is dispatched"); return; }
    ExpectTrue(request->attributes.size() == 1 && request->attributes.front().second == "alice", "authorized request sees bounded attributes");
    ExpectTrue(request->response->Complete({200, {}, "allowed"}).IsOk(), "authorized response completes");
    const auto wire = client.Read();
    ExpectTrue(wire.find("Access-Control-Allow-Origin: https://example.test") != std::string::npos && wire.ends_with("allowed"), "policy response defaults reach wire");

    Client rejected;
    ExpectTrue(rejected.Open(server.Port()) && rejected.Send("GET /secure HTTP/1.1\r\nHost: local\r\n\r\n"), "rejected request sends");
    auto denied = policies.Take();
    if (denied) ExpectTrue(denied->decision->Reject({401, {{"Content-Type", "application/json"}}, "{\"error\":\"unauthorized\"}"}).IsOk(), "standard error response resolves policy");
    ExpectTrue(rejected.Read().starts_with("HTTP/1.1 401"), "rejected policy never runs route handler");

    Client socket;
    const std::string handshake = "GET /socket HTTP/1.1\r\nHost: local\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    ExpectTrue(socket.Open(server.Port()) && socket.Send(handshake), "pending upgrade sends");
    auto global = policies.Take();
    if (global) ExpectTrue(global->webSocketUpgrade && global->decision->Allow().IsOk(), "global policy precedes route authorization");
    auto upgrade = policies.Take();
    if (!upgrade) { ExpectTrue(false, "per-route upgrade policy arrives before 101"); return; }
    ExpectTrue(upgrade->decision->Reject({403, {}, "denied"}).IsOk(), "WebSocket authorization rejects before upgrade");
    ExpectTrue(socket.Read().starts_with("HTTP/1.1 403"), "denied upgrade emits HTTP error rather than 101");

    Client accepted;
    std::string earlyFrame("\x82\x81\x01\x02\x03\x04", 6); earlyFrame += 'y';
    ExpectTrue(accepted.Open(server.Port()) && accepted.Send(handshake + earlyFrame), "upgrade can retain an already received first frame while policy waits");
    auto firstGate = policies.Take();
    if (firstGate) ExpectTrue(firstGate->decision->Allow().IsOk(), "global upgrade approval succeeds");
    auto secondGate = policies.Take();
    ExpectEqual(0u, opened.load(), "connection cannot open before route authorization");
    if (secondGate) ExpectTrue(secondGate->decision->Allow({{"X-Policy", "accepted"}}).IsOk(), "route upgrade approval succeeds");
    const auto upgradeWire = accepted.Read(true);
    ExpectTrue(upgradeWire.starts_with("HTTP/1.1 101") && upgradeWire.find("X-Policy: accepted\r\n") != std::string::npos,
        "101 and approved response headers are sent after both decisions");

    Client timeout;
    ExpectTrue(timeout.Open(server.Port()) && timeout.Send("GET /secure HTTP/1.1\r\nHost: local\r\n\r\n"), "deferred policy timeout request sends");
    auto expired = policies.Take();
    std::atomic<ErrorCode> reentrantStop{ErrorCode::Ok};
    std::stop_callback callback(expired ? expired->decision->GetCancellationToken() : std::stop_token{}, [&] {
        reentrantStop.store(server.Stop().Code());
    });
    std::this_thread::sleep_for(350ms);
    ExpectTrue(expired && expired->decision->GetCancellationToken().stop_requested(), "absolute policy deadline cancels deferred decision without retaining a worker");
    ExpectTrue(server.Stop().IsOk(), "policy server stops");
    ExpectTrue(reentrantStop.load() == ErrorCode::InvalidArgument, "policy cancellation callback cannot join its maintenance thread");
    ExpectTrue(opened.load() == 1 && messages.load() == 1, "deferred approved upgrade preserves initial frame and callback order");
    ExpectTrue(drainFromOpen.load() == ErrorCode::InvalidArgument && drainFromMessage.load() == ErrorCode::InvalidArgument,
        "WebSocket I/O callbacks reject BeginDrain without stopping or asserting the listener");
}

void GracefulDrain()
{
    ServerCoreTest::SocketRuntime runtime;
    ExpectTrue(runtime.IsReady(), "drain socket runtime starts");
    Inbox<Web::HttpRequestContext> requests;
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/work", [&](auto value) { requests.Put(std::move(value)); }).IsOk(), "drain route registers");
    Web::HttpServerOptions options; options.port = Port();
    ExpectTrue(server.Start(options).IsOk(), "drain server starts");
    Client client;
    ExpectTrue(client.Open(server.Port()) && client.Send("GET /work HTTP/1.1\r\nHost: local\r\n\r\n"), "in-flight drain request sends");
    auto context = requests.Take();
    if (!context) { ExpectTrue(false, "drain request is admitted"); return; }
    ExpectTrue(server.BeginDrain().IsOk() && server.DrainStatus().Code() == ErrorCode::WouldBlock, "drain preserves admitted response");
    ExpectTrue(context->response->Complete({200, {}, "finished"}).IsOk(), "admitted request finishes during drain");
    ExpectTrue(client.Read().ends_with("finished"), "drain flushes response before closing keep-alive connection");
    ExpectTrue(server.StopGracefully(Clock::now() + 2s).IsOk(), "successful drain stops cleanly");
    ExpectTrue(server.BeginDrain().IsOk() && server.DrainStatus().IsOk(), "stopped drain is idempotent");

    Web::HttpServer forced;
    ExpectTrue(forced.RegisterAsyncRoute("GET", "/work", [&](auto value) { requests.Put(std::move(value)); }).IsOk(), "forced drain route registers");
    options.port = Port();
    ExpectTrue(forced.Start(options).IsOk(), "forced drain server starts");
    Client waiting;
    ExpectTrue(waiting.Open(forced.Port()) && waiting.Send("GET /work HTTP/1.1\r\nHost: local\r\n\r\n"), "unfinished drain request sends");
    auto pending = requests.Take();
    ExpectTrue(forced.StopGracefully(Clock::now() + 50ms).Code() == ErrorCode::Timeout, "deadline forces incomplete response cancellation");
    ExpectTrue(pending && pending->response->GetCancellationToken().stop_requested(), "forced drain signals retained request handles");
}

void FileRangesAndConditionals()
{
    const auto modified = std::chrono::system_clock::from_time_t(784111777);
    const auto plan = [&](std::string_view method, Web::HttpHeaders fields, std::string_view tag = "\"v1\"", std::uint64_t size = 10) {
        return Web::Detail::PlanFileResponse(Web::HttpRequest{std::string(method), "/file", std::move(fields), {}}, size, tag, modified);
    };
    const auto middle = plan("GET", {{"range", "bytes=2-5"}});
    ExpectTrue(middle.status == 206 && middle.offset == 2 && middle.length == 4 && middle.contentRange == "bytes 2-5/10", "single bounded range has correct offset and representation metadata");
    const auto suffix = plan("GET", {{"range", "bytes=-3"}});
    ExpectTrue(suffix.status == 206 && suffix.offset == 7 && suffix.length == 3, "suffix range selects final bytes");
    ExpectTrue(plan("GET", {{"range", "Bytes=1-2"}}).status == 206, "range unit names are case-insensitive");
    ExpectTrue(plan("GET", {{"range", "bytes=10-"}}).contentRange == "bytes */10", "unsatisfiable range includes complete representation size");
    ExpectTrue(plan("HEAD", {{"range", "bytes=2-5"}}).status == 200 && plan("HEAD", {{"range", "bytes=2-5"}}).length == 10, "HEAD ignores Range and describes full GET representation");
    ExpectTrue(plan("GET", {{"range", "bytes=1-2,4-5"}}).status == 200, "unsupported multipart ranges safely select full representation");
    ExpectTrue(plan("GET", {{"if-none-match", "W/\"v1\""}, {"range", "bytes=1-2"}}).status == 304, "weak If-None-Match takes precedence over Range");
    ExpectTrue(plan("GET", {{"if-match", "W/\"v1\""}, {"if-none-match", "\"v1\""}}).status == 412, "strong If-Match failure precedes cache condition");
    ExpectTrue(plan("GET", {{"if-range", "\"v1\""}, {"range", "bytes=1-2"}}).status == 206, "matching strong If-Range permits partial transfer");
    ExpectTrue(plan("GET", {{"if-range", "W/\"v1\""}, {"range", "bytes=1-2"}}, "W/\"v1\"").status == 200, "weak If-Range cannot validate byte ranges");
    const auto date = Web::Detail::HttpDate(modified);
    ExpectTrue(plan("GET", {{"if-modified-since", date}}).status == 304, "unmodified date produces bodyless cache response");
    ExpectTrue(plan("GET", {{"if-none-match", "\"other\""}, {"if-modified-since", date}}).status == 200, "entity tag condition overrides date condition");
    ExpectTrue(plan("GET", {{"range", "bytes=0-0"}}, "\"v1\"", 0).status == 416, "empty representation has no satisfiable byte interval");

    ServerCoreTest::SocketRuntime runtime;
    ExpectTrue(runtime.IsReady(), "file-range socket runtime starts");
    struct TemporaryFile
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("servercore-file-range-" + std::to_string(Clock::now().time_since_epoch().count()));
        ~TemporaryFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } file;
    { std::ofstream output(file.path, std::ios::binary); output << "0123456789"; }
    ServerCore::Runtime::TaskExecutor executor;
    ExpectTrue(executor.Start({1, 4, 1024 * 1024}).IsOk(), "bounded range executor starts");
    Web::HttpServer server;
    ExpectTrue(server.RegisterAsyncRoute("GET", "/file", [&](auto context) {
        Web::HttpResponseHead head; head.headers.emplace_back("ETag", "\"v1\"");
        if (!Web::SendFile(executor, context, file.path, std::move(head)).IsOk()) context->response->Abort();
    }).IsOk(), "range file route registers");
    Web::HttpServerOptions options; options.port = Port();
    ExpectTrue(server.Start(options).IsOk(), "range file server starts");
    const auto get = [&](std::string_view method, std::string_view fields) {
        Client client;
        ExpectTrue(client.Open(server.Port()) && client.Send(std::string(method) +
            " /file HTTP/1.1\r\nHost: local\r\nConnection: close\r\n" + std::string(fields) + "\r\n"), "file request sends");
        return client.Read();
    };
    const auto partial = get("GET", "Range: bytes=2-5\r\n");
    ExpectTrue(partial.starts_with("HTTP/1.1 206") && partial.find("Content-Length: 4\r\n") != std::string::npos && partial.ends_with("2345"),
        "file helper seeks and transfers only the selected interval");
    const auto head = get("HEAD", "Range: bytes=2-5\r\n");
    ExpectTrue(head.starts_with("HTTP/1.1 200") && head.find("Content-Length: 10\r\n") != std::string::npos && head.ends_with("\r\n\r\n"),
        "file HEAD emits full metadata and no content");
    const auto cached = get("GET", "If-None-Match: W/\"v1\"\r\n");
    ExpectTrue(cached.starts_with("HTTP/1.1 304") && cached.ends_with("\r\n\r\n"), "file conditional cache hit emits no payload");
    const auto missing = get("GET", "Range: bytes=99-\r\n");
    ExpectTrue(missing.starts_with("HTTP/1.1 416") && missing.find("Content-Range: bytes */10\r\n") != std::string::npos &&
        missing.find("Content-Length: 0\r\n") != std::string::npos, "file unsatisfiable interval has valid empty framing");
    ExpectTrue(server.Stop().IsOk() && executor.Stop().IsOk(), "file-range resources stop");
}

const ServerCoreTest::CheckRegistration uploads("Web.StreamingUploadBackpressure", StreamingUploadBackpressure);
const ServerCoreTest::CheckRegistration policies("Web.RequestPolicyAndWebSocketGate", RequestPolicyAndWebSocketGate);
const ServerCoreTest::CheckRegistration drain("Web.GracefulDrain", GracefulDrain);
const ServerCoreTest::CheckRegistration files("Web.FileRangesAndConditionals", FileRangesAndConditionals);
}
