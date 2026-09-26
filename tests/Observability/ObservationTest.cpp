#include "ServerCore/Observability/AsyncLogger.h"
#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Runtime/Metrics.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Web/HttpServer.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>

namespace
{
namespace C = ServerCore::Core;
namespace O = ServerCore::Observability;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using namespace std::chrono_literals;
struct TextSink final : C::ILogger
{
    std::string text;
    void Write(C::LogLevel, std::string_view value) noexcept override
    {
        try
        {
            text = value;
        }
        catch (...)
        {
        }
    }
};
void StructuredRecordsAndBounds()
{
    const std::array<C::LogField, 2> fields{ { { "operation", "join" },
        { "untrusted", "\"\n\\" } } };
    C::LogRecord record{ C::LogLevel::Info, "request\ncomplete", fields, { 11, 12, 13, 14 } };
    auto encoded = C::FormatLogRecord(record);
    ExpectTrue(encoded.IsOk(), "structured fields and explicit correlation encode");
    if (!encoded.IsOk())
        return;
    ExpectTrue(ServerCore::Protocol::JsonValue::Parse(encoded.Value()).IsOk(),
        "bounded encoder emits valid JSON");
    ExpectTrue(encoded.Value().find(
                   "\"request_id\":11,\"session_id\":12,\"task_id\":13,\"connection_id\":14") !=
                   std::string::npos,
        "all explicit correlation identifiers survive serialization");
    ExpectTrue(encoded.Value().find('\n') == std::string::npos,
        "untrusted values cannot inject another physical log line");
    TextSink legacy;
    ExpectTrue(C::WriteLog(legacy, record).IsOk() && legacy.text == encoded.Value(),
        "existing ILogger remains source compatible and retains fields");
    ExpectTrue(C::FormatLogRecord(record, encoded.Value().size()).IsOk(),
        "exact encoded byte bound admits");
    ExpectTrue(C::FormatLogRecord(record, encoded.Value().size() - 1).GetStatus().Code() ==
                   C::ErrorCode::TooLarge,
        "escaping and identifiers count toward the limit");
    const std::array<C::LogField, 2> duplicate{ { { "key", "first" }, { "key", "second" } } };
    record.fields = duplicate;
    ExpectTrue(C::FormatLogRecord(record).GetStatus().Code() == C::ErrorCode::InvalidArgument,
        "duplicate structured keys reject instead of hiding values");
    record.fields = {};
    record.message = std::string_view("\xff", 1);
    ExpectTrue(C::FormatLogRecord(record).GetStatus().Code() == C::ErrorCode::InvalidArgument,
        "malformed UTF-8 caller input rejects");
    std::array<C::LogField, C::MaxLogFields + 1> excess{};
    record.fields = excess;
    record.message = "valid";
    ExpectTrue(C::FormatLogRecord(record).GetStatus().Code() == C::ErrorCode::InvalidArgument,
        "field count is independently bounded");
}
void StructuredLoggerOwnershipAndDrain()
{
    const auto path =
        std::filesystem::temp_directory_path() /
        ("servercore-structured-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{ path };
    O::AsyncLogger logger;
    O::LoggerOptions options;
    options.console = false;
    options.file = path;
    options.maxMessageBytes = 512;
    ExpectTrue(logger.Start(options).IsOk(), "structured async logger starts");
    std::string input = "owned value";
    const C::LogField field{ "user_supplied", input };
    ExpectTrue(
        C::WriteLog(logger, { C::LogLevel::Info, "event", { &field, 1 }, { 23, 0, 41, 0 } }).IsOk(),
        "record is admitted");
    input.assign(input.size(), 'x');
    logger.RequestStop();
    ExpectTrue(O::Observe(logger).lifecycle == O::Lifecycle::Draining,
        "request stop is observable independently of empty queue");
    ExpectTrue(logger.Stop().IsOk(), "Stop joins the output worker");
    const auto observation = O::Observe(logger);
    ExpectTrue(observation.lifecycle == O::Lifecycle::Stopped && observation.pendingWork == 0 &&
                   observation.retainedBytes == 0,
        "joined logger releases every retained record");
    std::ifstream file(path, std::ios::binary);
    const std::string line{ std::istreambuf_iterator<char>(file), {} };
    ExpectTrue(line.find("owned value") != std::string::npos &&
                   line.find("\"timestamp_ms\":") != std::string::npos,
        "queued structured fields are copied before caller returns");
    ExpectTrue(ServerCore::Protocol::JsonValue::Parse(line).IsOk(),
        "async structured output is a standalone JSON line");
}
void ObservationAvailabilityAndLifecycle()
{
    O::HttpServerMetricsSnapshot http;
    http.activeConnections = 2;
    http.activeRequests = 3;
    http.closedConnections = 4;
    http.rejectedConnections = 5;
    http.rejectedRequests = 6;
    http.timedOutRequests = 7;
    http.lifecycle = O::Lifecycle::Draining;
    auto value = O::Observe(http);
    ExpectTrue(value.Has(O::ClosedEvents) && value.closed[0] == 4 && value.rejected[0] == 11 &&
                   value.timedOut == 7,
        "existing canonical counters retain unknown reasons without invention");
    ExpectTrue(value.drainRemaining == 5 && value.Has(O::DrainRemaining),
        "drain view includes admitted requests and connections");
    http.retainedRequestBytes = 20;
    http.retainedSendBytes = 5;
    http.handlers.retainedBytes = 20;
    ExpectEqual(std::uint64_t{ 25 }, O::Observe(http).retainedBytes,
        "handler admission charge does not duplicate the same shared request storage");
    ServerCore::Runtime::ServerMetricsSnapshot host;
    host.activeSessionCount = 2;
    host.sessionSendQueues = { { static_cast<ServerCore::Session::SessionId>(1), 11 } };
    host.retainedSendBytes = 17;
    value = O::Observe(host);
    ExpectTrue(
        !value.Has(O::ClosedEvents) && !value.Has(O::RejectedEvents) && value.sendBytes == 17,
        "unmeasured Host terminal events remain unavailable");
    http.retainedRequestBytes = (std::numeric_limits<std::size_t>::max)();
    http.retainedSendBytes = 10;
    ExpectEqual((std::numeric_limits<std::uint64_t>::max)(), O::Observe(http).retainedBytes,
        "aggregate gauges saturate rather than wrap");
    ServerCore::Runtime::TaskExecutor executor;
    ExpectTrue(O::Observe(executor).lifecycle == O::Lifecycle::Created,
        "unstarted executor is distinguishable from stopped");
    ExpectTrue(executor.Start().IsOk(), "executor starts");
    std::mutex mutex;
    std::condition_variable ready;
    bool entered = false, release = false;
    auto task = executor.Submit(
        [&](std::stop_token)
        {
            std::unique_lock lock(mutex);
            entered = true;
            ready.notify_all();
            ready.wait(lock, [&] { return release; });
            return C::Status::Ok();
        });
    ExpectTrue(task.IsOk(), "tracked task is admitted");
    {
        std::unique_lock lock(mutex);
        ExpectTrue(ready.wait_for(lock, 5s, [&] { return entered; }), "work starts");
    }
    executor.RequestStop();
    value = O::Observe(executor);
    ExpectTrue(value.lifecycle == O::Lifecycle::Draining && value.pendingWork == 1,
        "cooperative work remains visible after stop requested");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    ready.notify_all();
    ExpectTrue(executor.Stop().IsOk() && O::Observe(executor).lifecycle == O::Lifecycle::Stopped,
        "joined executor reports stopped");
    ServerCoreTest::SocketRuntime sockets;
    const auto probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ExpectTrue(
        sockets.IsReady() && probe != ServerCoreTest::InvalidSocket, "loopback port probe opens");
    if (probe == ServerCoreTest::InvalidSocket)
        return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ServerCoreTest::SocketLength length = sizeof(address);
    const bool bound =
        ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ServerCoreTest::CloseSocket(probe);
    ExpectTrue(bound, "available loopback port is discovered");
    if (!bound)
        return;
    ServerCore::Web::HttpServer server;
    ServerCore::Web::HttpServerOptions options;
    options.port = ntohs(address.sin_port);
    ExpectTrue(server.Start(options).IsOk(), "HTTP observer fixture starts");
    ExpectTrue(server.BeginDrain().IsOk(), "HTTP begins drain");
    value = O::Observe(server);
    ExpectTrue(value.lifecycle == O::Lifecycle::Draining && value.drainRemaining == 0,
        "empty drain is not falsely reported as stopped");
    ExpectTrue(server.Stop().IsOk() && O::Observe(server).lifecycle == O::Lifecycle::Stopped,
        "HTTP stop completion is explicit");
}
const ServerCoreTest::CheckRegistration records(
    "Observability.StructuredRecordsAndBounds", StructuredRecordsAndBounds);
const ServerCoreTest::CheckRegistration logger(
    "Observability.StructuredLoggerOwnershipAndDrain", StructuredLoggerOwnershipAndDrain);
const ServerCoreTest::CheckRegistration snapshots(
    "Observability.ObservationAvailabilityAndLifecycle", ObservationAvailabilityAndLifecycle);
}
