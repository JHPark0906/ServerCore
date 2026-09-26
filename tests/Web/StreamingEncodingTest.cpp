#include "Net/SendQueueInternal.h"
#include "ServerCore/Web/HttpStreaming.h"
#include "TestHarness.h"
#include "Web/HttpResponseInternal.h"
#include "Web/ResponseEncoding.h"
#include "Web/WebProtocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace
{
namespace Web = ServerCore::Web;
namespace Detail = ServerCore::Web::Detail;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using namespace std::chrono_literals;

std::span<const std::byte> Bytes(std::string_view value)
{
    return std::as_bytes(std::span(value.data(), value.size()));
}

void ExpectCode(ErrorCode expected, const Status& actual, std::string_view description)
{
    ExpectTrue(expected == actual.Code(), description);
}

void StreamingResponseFraming()
{
    Web::HttpResponseHead head;
    head.headers.emplace_back("Date", "Sun, 06 Nov 1994 08:49:37 GMT");
    auto chunked = Detail::PrepareResponseHead(head, false, 1024);
    ExpectTrue(chunked.IsOk(), "unknown-length response head encodes");
    if (chunked.IsOk())
    {
        ExpectTrue(chunked.Value().mode == Detail::ResponseBodyMode::Chunked,
            "unknown length selects chunked framing");
        ExpectTrue(
            chunked.Value().wire.find("Transfer-Encoding: chunked\r\n") != std::string::npos &&
                chunked.Value().wire.find("Content-Length:") == std::string::npos,
            "chunked response has one framing mechanism");
        ExpectTrue(chunked.Value().wire.ends_with("\r\n\r\n"),
            "head encoder never appends a body or terminator");
    }
    head.contentLength = std::uint64_t{ 9 } * 1024 * 1024 * 1024;
    auto largeHead = Detail::PrepareResponseHead(head, true, 1024);
    ExpectTrue(
        largeHead.IsOk(), "HEAD metadata supports a body larger than memory and local send limits");
    if (largeHead.IsOk())
    {
        ExpectTrue(
            largeHead.Value().mode == Detail::ResponseBodyMode::None, "HEAD emits no content");
        ExpectEqual(*head.contentLength, largeHead.Value().length,
            "HEAD keeps full 64-bit representation length");
        ExpectTrue(
            largeHead.Value().wire.find("Content-Length: 9663676416\r\n") != std::string::npos,
            "large HEAD length is encoded without allocating the representation");
    }
    head.contentLength.reset();
    auto unknownHead = Detail::PrepareResponseHead(head, true, 1024);
    ExpectTrue(unknownHead.IsOk(), "HEAD may omit unknown representation length");
    if (unknownHead.IsOk())
        ExpectTrue(unknownHead.Value().mode == Detail::ResponseBodyMode::None &&
                       unknownHead.Value().wire.find("Transfer-Encoding:") == std::string::npos &&
                       unknownHead.Value().wire.find("Content-Length:") == std::string::npos,
            "unknown HEAD has no body framing bytes");
    for (const auto status : { 200u, 204u, 205u, 304u })
    {
        for (const bool isHead : { false, true })
        {
            Web::HttpResponse buffered{ status, head.headers, "abcdef" };
            auto prepared = Detail::PrepareResponseHead(
                status, buffered.headers, std::uint64_t{ 6 }, isHead, true, 1024);
            std::string legacy;
            ExpectTrue(prepared.IsOk() &&
                           Detail::SerializeResponse(buffered, isHead, true, 1024, 6, legacy),
                "buffered and streaming response heads share status validation");
            if (!prepared.IsOk())
                continue;
            auto expected = prepared.Value().wire;
            if (prepared.Value().mode != Detail::ResponseBodyMode::None)
                expected += buffered.body;
            ExpectEqual(expected, legacy, "legacy serializer shares identical framing policy");
            if (status == 204 || status == 304)
                ExpectTrue(legacy.find("Content-Length:") == std::string::npos &&
                               legacy.find("Transfer-Encoding:") == std::string::npos,
                    "204 and 304 omit body framing");
            if (status == 205)
                ExpectTrue(legacy.find("Content-Length: 0\r\n") != std::string::npos &&
                               legacy.ends_with("\r\n\r\n"),
                    "205 is explicitly empty on a persistent HTTP connection");
        }
    }
    std::string unchanged = "unchanged";
    ExpectTrue(!Detail::SerializeResponse(
                   Web::HttpResponse{ 200, {}, "abcdef" }, false, false, 1024, 5, unchanged),
        "buffered response limit remains enforced independently of streaming support");
    ExpectEqual(std::string("unchanged"), unchanged,
        "oversized buffered response is rejected before output");
}

void StreamingResponseRejectsInvalidHeaders()
{
    const std::array<std::pair<std::string_view, std::string_view>, 8> invalid{
        { { "Content-Length", "1" }, { "Transfer-Encoding", "chunked" }, { "Connection", "close" },
            { "Trailer", "X-Trailer" }, { "bad name", "value" },
            { "X-Test", "value\r\nInjected: yes" }, { "Upgrade", "bad protocol" },
            { "X-Test", "\x7f" } }
    };
    for (const auto& [name, value] : invalid)
    {
        Web::HttpResponseHead head;
        head.headers.emplace_back(name, value);
        ExpectCode(ErrorCode::InvalidArgument,
            Detail::PrepareResponseHead(head, false, 1024).GetStatus(),
            "caller cannot inject framing or malformed header fields");
    }
    Web::HttpResponseHead head;
    head.status = 101;
    ExpectCode(ErrorCode::InvalidArgument,
        Detail::PrepareResponseHead(head, false, 1024).GetStatus(),
        "ordinary response stream cannot perform an upgrade or informational response");
    head.status = 600;
    ExpectCode(ErrorCode::InvalidArgument,
        Detail::PrepareResponseHead(head, false, 1024).GetStatus(), "status is bounded");
    head.status = 426;
    ExpectCode(ErrorCode::InvalidArgument,
        Detail::PrepareResponseHead(head, false, 1024).GetStatus(), "426 requires Upgrade");
    head.headers.emplace_back("Upgrade", "websocket");
    auto upgrade = Detail::PrepareResponseHead(head, false, 1024);
    ExpectTrue(upgrade.IsOk(), "426 with valid Upgrade remains supported");
    if (upgrade.IsOk())
        ExpectTrue(
            upgrade.Value().wire.find("Connection: keep-alive, Upgrade\r\n") != std::string::npos,
            "framework adds the required Upgrade connection option");
    head.status = 200;
    head.headers = { { "Date", "Sun, 06 Nov 1994 08:49:37 GMT" }, { "date", "duplicate" } };
    ExpectCode(ErrorCode::InvalidArgument,
        Detail::PrepareResponseHead(head, false, 1024).GetStatus(), "duplicate Date is invalid");
    head.headers.pop_back();
    head.contentLength = 0;
    auto valid = Detail::PrepareResponseHead(head, false, 1024);
    ExpectTrue(valid.IsOk(), "fixed empty response encodes");
    if (valid.IsOk())
    {
        const auto size = valid.Value().wire.size();
        ExpectTrue(Detail::PrepareResponseHead(head, false, size).IsOk(),
            "exact header byte limit is accepted");
        ExpectCode(ErrorCode::TooLarge,
            Detail::PrepareResponseHead(head, false, size - 1).GetStatus(),
            "generated framing and final CRLF count against header limit");
    }
}

void HttpChunkEncoding()
{
    const std::string binary("a\0b", 3);
    auto result = Detail::EncodeChunk(Bytes(binary), 3);
    ExpectTrue(result.IsOk(), "binary payload encodes as a complete chunk");
    if (result.IsOk())
        ExpectEqual(std::string("3\r\n") + binary + "\r\n", result.Value(),
            "chunk framing preserves binary bytes");
    const std::string sixteen(16, 'x');
    auto hexadecimal = Detail::EncodeChunk(Bytes(sixteen), 16);
    ExpectTrue(hexadecimal.IsOk(), "multi-digit chunk length encodes");
    if (hexadecimal.IsOk())
        ExpectEqual(std::string("10\r\n") + sixteen + "\r\n", hexadecimal.Value(),
            "chunk length uses hexadecimal");
    ExpectCode(ErrorCode::TooLarge, Detail::EncodeChunk(Bytes(binary), 2).GetStatus(),
        "oversized chunk is rejected atomically");
    ExpectCode(ErrorCode::InvalidArgument, Detail::EncodeChunk({}, 3).GetStatus(),
        "empty write cannot accidentally terminate a stream");
    ExpectEqual(std::string("0\r\n\r\n"), std::string(Detail::FinalChunk),
        "Finish has a distinct complete final chunk");
}

class RecordingWriter final : public Web::HttpResponseWriter
{
public:
    Status Start(const Web::HttpResponseHead&) override { return Status::Ok(); }
    Status Write(std::span<const std::byte> bytes) override
    {
        ++writes;
        if (writeCode != ErrorCode::Ok)
            return Status::FailWithoutMessage(writeCode);
        payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return Status::Ok();
    }
    Status Finish() override { return Status::Ok(); }
    Status Complete(const Web::HttpResponse&) override { return Status::Ok(); }
    void Abort() noexcept override
    {
        ++aborts;
        (void)cancellation.request_stop();
    }
    std::stop_token GetCancellationToken() const noexcept override
    {
        return cancellation.get_token();
    }
    bool IsHeadRequest() const noexcept override { return false; }
    std::size_t MaxWriteBytes() const noexcept override { return 128; }
    std::size_t RetainedSendBytes() const noexcept override { return 0; }
    ServerCore::Core::Result<ServerCore::Net::SendCapacitySubscription> WaitForWriteCapacity(
        std::size_t, std::function<void(Status)>) override
    {
        return ServerCore::Core::Result<ServerCore::Net::SendCapacitySubscription>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::Unimplemented));
    }
    unsigned int writes = 0;
    ErrorCode writeCode = ErrorCode::Ok;
    std::string payload;
    std::atomic<unsigned int> aborts{ 0 };
    std::stop_source cancellation;
};

void SseEncoding()
{
    Web::SseEvent event;
    event.data = "a\r\nb\rc\n";
    event.event = "tick";
    event.id = "";
    event.retryMilliseconds = 0;
    const std::string expected =
        "event: tick\nid: \nretry: 0\ndata: a\ndata: b\ndata: c\ndata: \n\n";
    auto encoded = Web::EncodeSseEvent(event, expected.size());
    ExpectTrue(encoded.IsOk(), "SSE event encodes at exact byte limit");
    if (encoded.IsOk())
        ExpectEqual(expected, encoded.Value(),
            "SSE normalizes lines and preserves explicit empty id and trailing line");
    ExpectCode(ErrorCode::TooLarge, Web::EncodeSseEvent(event, expected.size() - 1).GetStatus(),
        "SSE limit includes all field prefixes and terminator");
    event = {};
    auto empty = Web::EncodeSseEvent(event, 8);
    ExpectTrue(empty.IsOk(), "empty data event is representable");
    if (empty.IsOk())
        ExpectEqual(std::string("data: \n\n"), empty.Value(),
            "empty event has a data field and dispatching blank line");
    event.data = "first\nid: injected";
    auto injection = Web::EncodeSseEvent(event, 128);
    ExpectTrue(injection.IsOk(), "multiline data accepts field-like text");
    if (injection.IsOk())
        ExpectEqual(std::string("data: first\ndata: id: injected\n\n"), injection.Value(),
            "data cannot inject metadata fields");
    event.data = "\xC0\xAF";
    ExpectCode(ErrorCode::InvalidArgument, Web::EncodeSseEvent(event, 128).GetStatus(),
        "SSE rejects invalid UTF-8");
    event.data = "valid";
    event.event = "tick\nid: injected";
    ExpectCode(ErrorCode::InvalidArgument, Web::EncodeSseEvent(event, 128).GetStatus(),
        "event name cannot inject lines");
    event.event.clear();
    event.id = std::string("a\0b", 3);
    ExpectCode(ErrorCode::InvalidArgument, Web::EncodeSseEvent(event, 128).GetStatus(),
        "SSE id cannot contain NUL");
    event.id = "id\rnext";
    ExpectCode(ErrorCode::InvalidArgument, Web::EncodeSseEvent(event, 128).GetStatus(),
        "SSE id cannot inject lines");
    event.id.reset();
    RecordingWriter writer;
    writer.writeCode = ErrorCode::WouldBlock;
    ExpectCode(ErrorCode::WouldBlock, Web::WriteSseEvent(writer, event),
        "SSE forwards bounded writer backpressure");
    ExpectTrue(writer.payload.empty(), "rejected SSE write does not partially emit an event");
    writer.writeCode = ErrorCode::Ok;
    ExpectTrue(Web::WriteSseEvent(writer, event).IsOk(), "caller may retry complete SSE event");
    ExpectEqual(2u, writer.writes, "SSE helper does not spin or retry internally");
    ExpectEqual(std::string("data: valid\n\n"), writer.payload,
        "successful retry emits exactly one complete event");
}

void FileTaskQueuedCancellation()
{
    ServerCore::Runtime::TaskExecutor executor;
    ExpectTrue(executor.Start({ 1, 1, 4096 }).IsOk(), "file cancellation executor starts");
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool release = false;
    auto blocking = executor.Submit(
        [&](std::stop_token)
        {
            std::unique_lock guard(mutex);
            entered = true;
            wake.notify_all();
            (void)wake.wait_for(guard, 5s, [&release] { return release; });
            return Status::Ok();
        });
    ExpectTrue(blocking.IsOk(), "blocking task is accepted");
    {
        std::unique_lock guard(mutex);
        ExpectTrue(wake.wait_for(guard, 2s, [&entered] { return entered; }),
            "worker is occupied before file admission");
    }
    const auto writer = std::make_shared<RecordingWriter>();
    auto context = std::make_shared<Web::HttpRequestContext>();
    context->response = writer;
    auto file = Web::SendFile(executor, context, "file-that-must-not-be-opened");
    ExpectTrue(file.IsOk(), "file transfer is queued behind occupied worker");
    if (file.IsOk())
    {
        ExpectTrue(file.Value().RequestCancel(), "queued file task can be cancelled");
        ExpectCode(ErrorCode::Cancelled,
            file.Value().WaitUntil(std::chrono::steady_clock::now() + 2s),
            "queued file cancellation finishes without reading its path");
        ExpectEqual(1u, writer->aborts.load(),
            "discarded queued file task aborts its HTTP response exactly once");
    }
    const auto beforeMetadataRejection = writer->aborts.load();
    Web::HttpResponseHead hugeMetadata;
    hugeMetadata.headers.emplace_back("X-Owned", std::string(5000, 'x'));
    ExpectCode(ErrorCode::TooLarge,
        Web::SendFile(executor, context, "unused", std::move(hugeMetadata)).GetStatus(),
        "queued file metadata is included in executor retained admission");
    ExpectEqual(beforeMetadataRejection, writer->aborts.load(),
        "metadata rejection leaves the response untouched");
    Web::HttpResponseHead spareCapacity;
    std::string compactable = "small";
    compactable.reserve(16384);
    spareCapacity.headers.emplace_back("X-Owned", std::move(compactable));
    auto compact = Web::SendFile(executor, context, "unused", std::move(spareCapacity));
    ExpectTrue(
        compact.IsOk(), "spare caller header capacity is removed before bounded file admission");
    if (compact.IsOk())
    {
        (void)compact.Value().RequestCancel();
        ExpectCode(ErrorCode::Cancelled,
            compact.Value().WaitUntil(std::chrono::steady_clock::now() + 2s),
            "compact metadata task cancels before opening its file");
    }
    {
        const std::lock_guard guard(mutex);
        release = true;
    }
    wake.notify_all();
    ExpectTrue(executor.Stop().IsOk(), "file cancellation executor stops");
    const auto before = writer->aborts.load();
    ExpectCode(ErrorCode::Closed,
        Web::SendFile(executor, context, "file-that-must-not-be-opened").GetStatus(),
        "stopped executor rejects file admission");
    ExpectEqual(before, writer->aborts.load(), "submission rejection leaves response untouched");
}

class ResponseTestConnection final : public ServerCore::Net::Connection,
                                     public ServerCore::Net::ConnectionFlowControl
{
public:
    explicit ResponseTestConnection(std::shared_ptr<ServerCore::Net::SendBudget> budget)
        : queue(std::move(budget))
    {
    }
    Status Send(std::span<const std::byte> bytes) override
    {
        if (!open)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        if (auto callback = std::exchange(onSend, {}))
            callback();
        return queue.Enqueue(bytes);
    }
    void Close() override
    {
        open = false;
        queue.CloseCapacityWaits();
        queue.Budget()->Notify();
    }
    void CloseAfterSend() override { Close(); }
    void SetObserver(std::weak_ptr<ServerCore::Net::IConnectionObserver>) override {}
    bool IsOpen() const noexcept override { return open; }
    std::size_t QueuedSendBytes() const noexcept override { return queue.QueuedBytes(); }
    Status PauseReceive() override { return Status::Ok(); }
    Status ResumeReceive() override { return Status::Ok(); }
    bool IsReceivePaused() const noexcept override { return false; }
    std::size_t RetainedSendBytes() const noexcept override { return queue.RetainedBytes(); }
    ServerCore::Core::Result<ServerCore::Net::SendCapacitySubscription> WaitForSendCapacity(
        std::size_t bytes, std::function<void(Status)> callback, std::stop_token token) override
    {
        return queue.WaitForCapacity(bytes, std::move(callback), token);
    }
    bool open = true;
    std::function<void()> onSend;
    ServerCore::Net::SendQueue queue;
};

void ResponseWaitLifetimeAndReentrancy()
{
    constexpr std::size_t capacity = 4096;
    auto budget = std::make_shared<ServerCore::Net::SendBudget>(capacity);
    auto connection = std::make_shared<ResponseTestConnection>(budget);
    auto sender = std::make_shared<Detail::TrackedSender>(connection);
    Web::HttpServerOptions options;
    options.maxTotalSendQueueCapacityBytes = capacity;
    options.maxHeaderBytes = 1024;
    auto first = std::make_shared<Detail::HttpResponseState>(
        sender, options, false, false, false, nullptr, nullptr);
    Web::HttpResponseHead head;
    head.contentLength = 0;
    ExpectTrue(first->Start(head).IsOk(), "first fixed empty response starts");
    connection->queue.Clear();
    ServerCore::Net::SendQueue blocker(budget);
    const std::string payload(capacity, 'x');
    ExpectTrue(
        blocker.Enqueue(Bytes(payload)).IsOk(), "another queue consumes all shared capacity");
    unsigned calls = 0;
    auto wait = first->WaitForWriteCapacity(1,
        [&](Status result)
        {
            ++calls;
            ExpectCode(ErrorCode::Cancelled, result,
                "successful Finish retires its accepted capacity wait");
            ExpectCode(ErrorCode::Closed, first->Finish(),
                "terminal wait callback can reenter the writer without a lock");
        });
    ExpectTrue(
        wait.IsOk() && wait.Value().IsPending(), "first response owns a pending capacity wait");
    ExpectTrue(first->Finish().IsOk(), "fixed empty Finish needs no transport capacity");
    ExpectEqual(1u, calls, "Finish retires its wait exactly once");
    ExpectTrue(!first->GetCancellationToken().stop_requested(),
        "successful response does not cancel application work");
    auto second = std::make_shared<Detail::HttpResponseState>(
        sender, options, false, false, false, nullptr, nullptr);
    auto successor = second->WaitForWriteCapacity(0, [](Status) {});
    ExpectTrue(
        successor.IsOk(), "next response can wait even while the old subscription remains owned");
    if (successor.IsOk())
        successor.Value().Reset();
    blocker.Clear();
    budget->Notify();
    first->Abort();
    ExpectTrue(connection->IsOpen(), "late Abort cannot close the next response's transport");
    head.contentLength = 2;
    ExpectTrue(second->Start(head).IsOk(), "successor begins after capacity returns");
    unsigned inlineCalls = 0;
    connection->onSend = [&]
    {
        ExpectCode(ErrorCode::WouldBlock, second->Write(Bytes("b")),
            "reentrant write cannot overlap an active admission");
        auto retry = second->WaitForWriteCapacity(1, [&](Status) { ++inlineCalls; });
        ExpectCode(ErrorCode::WouldBlock, retry.GetStatus(),
            "capacity registration cannot recursively retry an unfinished admission");
    };
    ExpectTrue(
        second->Write(Bytes("a")).IsOk(), "outer send completes after bounded reentrant calls");
    ExpectEqual(0u, inlineCalls, "rejected concurrent wait never invokes its callback");
    ExpectTrue(second->Write(Bytes("b")).IsOk() && second->Finish().IsOk(),
        "serialized producer resumes after admission returns");
}

const ServerCoreTest::CheckRegistration framing(
    "Web.StreamingResponseFraming", StreamingResponseFraming);
const ServerCoreTest::CheckRegistration validation(
    "Web.StreamingResponseRejectsInvalidHeaders", StreamingResponseRejectsInvalidHeaders);
const ServerCoreTest::CheckRegistration chunks("Web.HttpChunkEncoding", HttpChunkEncoding);
const ServerCoreTest::CheckRegistration sse("Web.SseEncoding", SseEncoding);
const ServerCoreTest::CheckRegistration file(
    "Web.FileTaskQueuedCancellation", FileTaskQueuedCancellation);
const ServerCoreTest::CheckRegistration lifetime(
    "Web.ResponseWaitLifetimeAndReentrancy", ResponseWaitLifetimeAndReentrancy);
}
