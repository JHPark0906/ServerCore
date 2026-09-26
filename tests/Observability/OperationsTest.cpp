#include "Observability/MetricsInternal.h"
#include "ServerCore/Observability/AsyncLogger.h"
#include "ServerCore/Observability/Prometheus.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Web/HttpServer.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"
#include "Web/HttpObservationInternal.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using ServerCore::Core::ErrorCode;
using ServerCore::Core::LogLevel;
using ServerCore::Core::Status;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
namespace Obs = ServerCore::Observability;
namespace Runtime = ServerCore::Runtime;
namespace Web = ServerCore::Web;
using namespace std::chrono_literals;

class Gate
{
public:
    bool Wait()
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return released; });
    }
    void Release()
    {
        const std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
};
struct ReleaseOnExit
{
    std::shared_ptr<Gate> gate;
    ~ReleaseOnExit() { gate->Release(); }
};
struct TempDirectory
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("servercore-operations-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { std::filesystem::create_directory(path); }
    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void LoggerFilteringAndRotation()
{
    TempDirectory directory;
    Obs::LoggerOptions options;
    options.console = false;
    options.file = directory.path / "application.log";
    options.maxQueuedMessages = 128;
    options.maxMessageBytes = 32;
    options.maxRetainedBytes = 8192;
    options.maxFileBytes = 4096;
    Obs::AsyncLogger logger;
    ExpectTrue(logger.Start(options).IsOk(), "asynchronous file logger starts");
    logger.Write(LogLevel::Debug, "filtered");
    const std::string controls("line\n\0\r", 7);
    ExpectTrue(logger.TryWrite(LogLevel::Info, controls).IsOk(),
        "control bytes enqueue without performing output on the caller");
    ExpectTrue(logger.SetMinimumLevel(LogLevel::Warn).IsOk(), "minimum log level changes safely");
    logger.Write(LogLevel::Info, "also filtered");
    logger.Write(LogLevel::Warn, "visible");
    std::array<std::thread, 4> producers;
    for (auto& producer : producers)
        producer = std::thread(
            [&]
            {
                for (unsigned index = 0; index < 10; ++index)
                    logger.Write(LogLevel::Warn, "parallel");
            });
    for (auto& producer : producers)
        producer.join();
    ExpectTrue(logger.TryWrite(LogLevel::Error, std::string(33, 'x')).Code() == ErrorCode::TooLarge,
        "oversized log text is rejected before copying");
    ExpectTrue(
        logger.TryWrite(static_cast<LogLevel>(100), "invalid").Code() == ErrorCode::InvalidArgument,
        "invalid log level is rejected");
    ExpectTrue(logger.Stop().IsOk(), "Stop drains every accepted log record");
    const auto metrics = logger.GetMetrics();
    ExpectEqual(std::uint64_t{ 42 }, metrics.acceptedMessages,
        "all concurrent accepted records are accounted");
    ExpectEqual(metrics.acceptedMessages, metrics.writtenMessages,
        "all accepted records reach the file sink");
    ExpectEqual(std::uint64_t{ 2 }, metrics.filteredMessages,
        "level filtering is separate from overload drops");
    ExpectEqual(std::uint64_t{ 2 }, metrics.droppedMessages,
        "invalid and oversized records are observable");
    ExpectEqual(std::size_t{ 0 }, metrics.retainedBytes, "drain releases retained log text");
    const auto content = ReadFile(options.file);
    ExpectTrue(content.find("line\\x0a\\x00\\x0d") != std::string::npos &&
                   content.find('\0') == std::string::npos,
        "untrusted controls cannot inject physical log records");
    ExpectEqual(std::size_t{ 42 },
        static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n')),
        "concurrent records each occupy exactly one line");
    ExpectTrue(logger.TryWrite(LogLevel::Error, "late").Code() == ErrorCode::Closed,
        "closed logger rejects late writes");
    options.file = directory.path / "rotating.log";
    options.maxFileBytes = 256;
    options.retainedFiles = 2;
    Obs::AsyncLogger rotating;
    ExpectTrue(rotating.Start(options).IsOk(), "rotating file sink starts");
    for (unsigned index = 0; index < 40; ++index)
        rotating.Write(LogLevel::Warn, "record-" + std::to_string(index));
    ExpectTrue(rotating.Stop().IsOk(), "rotation and drain succeed");
    for (const auto suffix : { "", ".1", ".2" })
    {
        auto path = options.file;
        path += suffix;
        ExpectTrue(std::filesystem::exists(path) && std::filesystem::file_size(path) <= 256,
            "active and retained log files respect the configured size");
    }
    auto excess = options.file;
    excess += ".3";
    ExpectTrue(
        !std::filesystem::exists(excess), "rotation retains only the configured number of backups");

    options.file = directory.path / "failed-rotation.log";
    auto blockedBackup = options.file;
    blockedBackup += ".2";
    std::filesystem::create_directory(blockedBackup);
    {
        std::ofstream blocker(blockedBackup / "occupied");
        blocker << "prevent removal";
    }
    Obs::AsyncLogger failing;
    ExpectTrue(
        failing.Start(options).IsOk(), "logger starts before a deterministic rotation failure");
    for (unsigned index = 0; index < 40; ++index)
        failing.Write(LogLevel::Warn, "failure-record-" + std::to_string(index));
    ExpectTrue(
        failing.Stop().Code() == ErrorCode::PlatformError, "rotation failure is reported by Stop");
    const auto failureMetrics = failing.GetMetrics();
    ExpectTrue(failureMetrics.outputErrors > 0, "failed file output contributes a terminal error");
    ExpectEqual(failureMetrics.acceptedMessages,
        failureMetrics.writtenMessages + failureMetrics.outputErrors,
        "every accepted log record has exactly one output outcome even when the sink fails");
    ExpectTrue(failureMetrics.retainedBytes == 0 && failureMetrics.pendingMessages == 0,
        "failed output also releases all message storage and pending slots");
}

/// <summary>logger가 받은 기록을 모두 출력 단계까지 넘길 때까지 기다린다.</summary>
/// <returns>제한 시간 안에 비면 true다. 시간은 매달림 판정에만 쓰고 결과 판정에는 쓰지 않는다.</returns>
bool WaitForLoggerDrain(const Obs::AsyncLogger& logger)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (logger.GetMetrics().outstandingMessages != 0)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void LoggerRotationFailureKeepsArchives()
{
    TempDirectory directory;
    Obs::LoggerOptions options;
    options.console = false;
    options.file = directory.path / "held.log";
    options.maxQueuedMessages = 128;
    options.maxMessageBytes = 32;
    options.maxRetainedBytes = 8192;
    options.maxFileBytes = 256;
    options.retainedFiles = 3;
    const auto archive = [&](const unsigned index)
    {
        auto path = options.file;
        path += "." + std::to_string(index);
        return path;
    };
    Obs::AsyncLogger logger;
    ExpectTrue(logger.Start(options).IsOk(), "rotating logger starts");
    for (unsigned index = 0; index < 40; ++index)
        logger.Write(LogLevel::Warn, "before-" + std::to_string(index));
    ExpectTrue(WaitForLoggerDrain(logger), "records written before the rename is blocked drain");
    for (unsigned index = 1; index <= options.retainedFiles; ++index)
        ExpectTrue(std::filesystem::exists(archive(index)),
            "every archive slot is filled before the rename is blocked");

    // 활성 파일의 rename만 실패하게 만든다. Windows는 삭제 공유 없이 연 핸들로 공유 위반을,
    // POSIX는 활성 파일을 지워 ENOENT를 만든다. 둘 다 뷰어·백신·운영자가 실제로 만드는 상황이다.
#ifdef _WIN32
    const HANDLE holder = CreateFileW(options.file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ExpectTrue(holder != INVALID_HANDLE_VALUE, "the active log is held without delete sharing");
#else
    std::filesystem::remove(options.file);
#endif
    for (unsigned index = 0; index < 20; ++index)
        logger.Write(LogLevel::Warn, "blocked-" + std::to_string(index));
    ExpectTrue(WaitForLoggerDrain(logger), "records written while the rename is blocked drain");
    for (unsigned index = 1; index <= options.retainedFiles; ++index)
        ExpectTrue(std::filesystem::exists(archive(index)),
            "a failed rotation leaves every archive in place");
#ifdef _WIN32
    if (holder != INVALID_HANDLE_VALUE)
        CloseHandle(holder);
#endif

    logger.Write(LogLevel::Warn, "after-release");
    (void)logger.Stop();
    ExpectTrue(logger.GetMetrics().outputErrors > 0,
        "the failed rotation stays visible as an output error");
    ExpectTrue(ReadFile(options.file).find("after-release") != std::string::npos,
        "file logging resumes in the active file once the rename can succeed");
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(directory.path))
        ++files;
    ExpectEqual(std::size_t{ options.retainedFiles + 1 }, files,
        "only the active file and the retained archives remain after recovery");
}

void LoggerStartCanRetryAfterFailure()
{
    TempDirectory directory;
    Obs::LoggerOptions options;
    options.console = false;
    options.file = directory.path / "missing" / "service.log";
    Obs::AsyncLogger logger;
    ExpectTrue(logger.Start(options).Code() == ErrorCode::PlatformError,
        "a logger whose file cannot be opened fails to start");
    ExpectTrue(logger.GetMetrics().lifecycle == Obs::Lifecycle::Created,
        "a failed start leaves the logger unstarted");

    options.file = directory.path / "service.log";
    ExpectTrue(logger.Start(options).IsOk(), "the same logger starts once the cause is fixed");
    ExpectTrue(logger.Start(options).Code() == ErrorCode::Closed,
        "a started logger still refuses a second start");
    ExpectTrue(
        logger.TryWrite(LogLevel::Warn, "started").IsOk(), "the retried logger accepts records");
    ExpectTrue(logger.Stop().IsOk(), "the retried logger drains");
    ExpectTrue(ReadFile(options.file).find("started") != std::string::npos,
        "the retried logger writes to its file");
}

void TaskMetrics()
{
    Runtime::TaskExecutor executor;
    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    const ReleaseOnExit releaseOnExit{ release };
    ExpectTrue(executor.Start({ 1, 2, 128 }).IsOk(), "metric executor starts");
    auto running = executor.Submit(
        [entered, release](std::stop_token)
        {
            entered->Release();
            (void)release->Wait();
            return Status::Ok();
        },
        { 16 });
    ExpectTrue(running.IsOk() && entered->Wait(),
        "worker is occupied for deterministic admission metrics");
    auto cancelled = executor.Submit([](std::stop_token) { return Status::Ok(); }, { 32 });
    Runtime::TaskOptions timed;
    timed.retainedBytes = 32;
    timed.deadline = std::chrono::steady_clock::now() + 80ms;
    auto expired = executor.Submit([](std::stop_token) { return Status::Ok(); }, timed);
    ExpectTrue(cancelled.IsOk() && expired.IsOk(), "queued tasks are admitted");
    ExpectTrue(executor.Submit([](std::stop_token) { return Status::Ok(); }).GetStatus().Code() ==
                   ErrorCode::WouldBlock,
        "full pending queue rejection is observed");
    ExpectTrue(executor.Submit({}, {}).GetStatus().Code() == ErrorCode::InvalidArgument,
        "empty task rejection is observed");
    ExpectTrue(
        executor.Submit([](std::stop_token) { return Status::Ok(); }, { 129 }).GetStatus().Code() ==
            ErrorCode::TooLarge,
        "impossible byte reservation is observed");
    if (cancelled.IsOk())
    {
        (void)cancelled.Value().RequestCancel();
        ExpectTrue(cancelled.Value().WaitUntil(std::chrono::steady_clock::now() + 2s).Code() ==
                       ErrorCode::Cancelled,
            "queued cancellation reaches a terminal metric");
    }
    if (expired.IsOk())
        ExpectTrue(expired.Value().WaitUntil(std::chrono::steady_clock::now() + 2s).Code() ==
                       ErrorCode::Timeout,
            "queued deadline reaches a distinct timeout metric");
    const auto busy = executor.GetMetrics();
    ExpectEqual(
        std::size_t{ 1 }, busy.runningTasks, "running gauge excludes cancelled queued tasks");
    ExpectEqual(std::size_t{ 16 }, busy.retainedBytes,
        "cancelled storage leaves the retained gauge promptly");
    release->Release();
    if (running.IsOk())
        ExpectTrue(running.Value().Wait().IsOk(), "successful task completes");
    auto failed = executor.Submit(
        [](std::stop_token) -> Status { throw std::runtime_error("task failure"); });
    if (failed.IsOk())
        ExpectTrue(failed.Value().Wait().Code() == ErrorCode::PlatformError,
            "task exception becomes failed outcome");
    ExpectTrue(executor.Stop().IsOk(), "metric executor stops");
    const auto result = executor.GetMetrics();
    ExpectEqual(std::uint64_t{ 4 }, result.acceptedTasks, "accepted task lifetime count is exact");
    ExpectEqual(
        result.acceptedTasks, result.completedTasks, "all accepted tasks are terminal after Stop");
    ExpectEqual(result.completedTasks,
        std::accumulate(result.latencyHistogram.buckets.begin(),
            result.latencyHistogram.buckets.end(), std::uint64_t{ 0 }),
        "each task enters one latency bin");
    ExpectEqual(std::uint64_t{ 1 }, result.cancelledTasks, "cancelled outcomes are counted once");
    ExpectEqual(std::uint64_t{ 1 }, result.timedOutTasks, "timeouts are counted separately");
    ExpectEqual(std::uint64_t{ 1 }, result.failedTasks, "exceptions are counted separately");
    ExpectEqual(std::uint64_t{ 3 }, result.rejectedTasks,
        "all rejection classes share a fixed rejection counter");
    ExpectTrue(result.maxLatencyNanoseconds > 0 &&
                   result.totalLatencyNanoseconds >= result.maxLatencyNanoseconds,
        "admission-to-terminal latency includes queued wait time");
    ExpectTrue(result.pendingTasks == 0 && result.runningTasks == 0 && result.retainedBytes == 0,
        "all task gauges return to zero");
}

class Client
{
public:
    ~Client() { ServerCoreTest::CloseSocket(socket); }
    bool Open(std::uint16_t port, std::string_view request)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == ServerCoreTest::InvalidSocket)
            return false;
        if (!ServerCoreTest::SetSocketTimeouts(socket, 5000))
            return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            return false;
        while (!request.empty())
        {
            const auto sent =
                ServerCoreTest::Send(socket, request.data(), static_cast<int>(request.size()));
            if (sent <= 0)
                return false;
            request.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }
    std::string Read()
    {
        std::string result;
        std::array<char, 1024> buffer{};
        for (;;)
        {
            const auto received =
                ServerCoreTest::Receive(socket, buffer.data(), static_cast<int>(buffer.size()));
            if (received <= 0)
                return result;
            result.append(buffer.data(), static_cast<std::size_t>(received));
            if (result.size() > 16384)
                return result;
        }
    }

private:
    ServerCoreTest::Socket socket = ServerCoreTest::InvalidSocket;
};
std::uint16_t Port()
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ServerCoreTest::SocketLength length = sizeof(address);
    const bool valid =
        ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ServerCoreTest::CloseSocket(socket);
    return valid ? ntohs(address.sin_port) : 0;
}
std::string Request(std::string_view path)
{
    return "GET " + std::string(path) +
           " HTTP/1.1\r\nHost: localhost\r\nAuthorization: secret\r\nConnection: close\r\n\r\n";
}
void ObservationFailureAccounting()
{
    auto collector = std::make_shared<Web::Detail::HttpObservation>(nullptr);
    try
    {
        const auto observed =
            std::make_shared<Web::Detail::RequestObservation>(collector, 1, 1, "GET");
        (void)observed;
        // Model a later response/context allocation failing after observation
        // admission, before there is a response that can explicitly terminate.
        throw std::bad_alloc();
    }
    catch (const std::bad_alloc&)
    {
    }
    auto metrics = collector->Snapshot();
    ExpectEqual(std::uint64_t{ 1 }, metrics.acceptedRequests,
        "observation is admitted before downstream construction");
    ExpectEqual(metrics.acceptedRequests, metrics.completedRequests,
        "exception unwinding completes admitted observation");
    ExpectEqual(std::uint64_t{ 1 }, metrics.failedRequests,
        "unpublished response construction failure is counted once");
    {
        Web::Detail::RequestObservation observed(collector, 2, 1, "GET");
        observed.Finish(ErrorCode::Ok, 200);
        observed.Finish(ErrorCode::Timeout, 0);
    }
    {
        Web::Detail::RequestObservation observed(collector, 3, 1, "GET");
        observed.Finish(ErrorCode::Cancelled, 0);
    }
    {
        Web::Detail::RequestObservation observed(collector, 4, 1, "GET");
        observed.Finish(ErrorCode::Timeout, 0);
    }
    metrics = collector->Snapshot();
    ExpectEqual(std::uint64_t{ 4 }, metrics.completedRequests,
        "explicit completion and destruction never double-count");
    ExpectEqual(metrics.acceptedRequests, metrics.completedRequests,
        "all observation outcomes preserve the terminal invariant");
    ExpectTrue(metrics.failedRequests == 1 && metrics.cancelledRequests == 1 &&
                   metrics.timedOutRequests == 1,
        "terminal failure classes remain distinct and a late timeout cannot replace success");
}
void HttpMetricsAndTracing()
{
    ObservationFailureAccounting();
    ServerCoreTest::SocketRuntime runtime;
    ExpectTrue(runtime.IsReady(), "HTTP observability socket runtime starts");
    auto traceEntered = std::make_shared<Gate>();
    auto traceRelease = std::make_shared<Gate>();
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Obs::RequestTrace> traces;
    std::shared_ptr<const Web::HttpRequestContext> pending;
    std::atomic<ErrorCode> selfStop{ ErrorCode::Ok };
    std::atomic<bool> queried{ false };
    Web::HttpServer server;
    const ReleaseOnExit releaseOnExit{ traceRelease };
    ExpectTrue(server
                   .SetRequestTraceHandler(
                       [&](Obs::RequestTrace trace)
                       {
                           bool first;
                           {
                               const std::lock_guard lock(mutex);
                               first = traces.empty();
                               traces.push_back(std::move(trace));
                           }
                           if (first)
                           {
                               selfStop.store(server.Stop().Code());
                               queried.store(server.GetMetrics().completedRequests > 0);
                               traceEntered->Release();
                               (void)traceRelease->Wait();
                               throw std::runtime_error("trace sink failure");
                           }
                       },
                       1)
                   .IsOk(),
        "bounded trace sink registers");
    ExpectTrue(server
                   .RegisterRoute("GET", "/ok",
                       [](const Web::HttpRequest&) { return Web::HttpResponse{ 200, {}, "ok" }; })
                   .IsOk(),
        "successful observed route registers");
    ExpectTrue(server
                   .RegisterAsyncRoute("GET", "/pending",
                       [&](auto context)
                       {
                           {
                               const std::lock_guard lock(mutex);
                               pending = std::move(context);
                           }
                           changed.notify_all();
                       })
                   .IsOk(),
        "deferred observed route registers");
    Web::HttpServerOptions options;
    options.port = Port();
    options.maxActiveHttpRequests = 1;
    options.handlerTimeout = 500ms;
    if (!server.Start(options).IsOk())
    {
        ExpectTrue(false, "observed HTTP server starts");
        return;
    }
    const auto get = [&](std::string_view path, unsigned expected)
    {
        Client client;
        ExpectTrue(client.Open(server.Port(), Request(path)), "observed request connects");
        ExpectTrue(client.Read().starts_with("HTTP/1.1 " + std::to_string(expected)),
            "observed request receives expected HTTP status");
    };
    get("/ok?token=private", 200);
    ExpectTrue(traceEntered->Wait(), "first trace callback is occupied independently of I/O");
    get("/ok", 200);
    get("/ok", 200);
    const auto nextPending = [&]()
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, 3s, [&] { return pending != nullptr; }))
            return std::shared_ptr<const Web::HttpRequestContext>{};
        return std::exchange(pending, {});
    };
    Client cancelledClient;
    ExpectTrue(cancelledClient.Open(server.Port(), Request("/pending")),
        "cancellable observed request connects");
    auto cancelled = nextPending();
    if (!cancelled)
    {
        ExpectTrue(false, "cancellable context arrives");
        return;
    }
    get("/ok", 503);
    cancelled->response->Abort();
    Client timedClient;
    ExpectTrue(
        timedClient.Open(server.Port(), Request("/pending")), "timed observed request connects");
    auto timed = nextPending();
    if (!timed)
    {
        ExpectTrue(false, "timed context arrives");
        return;
    }
    auto timedOut = std::make_shared<Gate>();
    std::stop_callback token(
        timed->response->GetCancellationToken(), [timedOut] { timedOut->Release(); });
    ExpectTrue(timedOut->Wait(), "HTTP handler timeout propagates cancellation and metrics");
    traceRelease->Release();
    ExpectTrue(server.Stop().IsOk(), "Stop drains trace callbacks after transport and handlers");
    const auto metrics = server.GetMetrics();
    ExpectEqual(
        std::uint64_t{ 5 }, metrics.acceptedRequests, "all routed admitted requests are counted");
    ExpectEqual(metrics.acceptedRequests, metrics.completedRequests,
        "each accepted HTTP response reaches one terminal outcome");
    ExpectEqual(metrics.completedRequests,
        std::accumulate(metrics.latencyHistogram.buckets.begin(),
            metrics.latencyHistogram.buckets.end(), std::uint64_t{ 0 }),
        "each HTTP terminal enters one latency bin");
    ExpectEqual(std::uint64_t{ 1 }, metrics.cancelledRequests, "explicit abort is counted once");
    ExpectEqual(std::uint64_t{ 1 }, metrics.timedOutRequests,
        "deadline is distinct from generic disconnect cancellation");
    ExpectEqual(std::uint64_t{ 1 }, metrics.rejectedRequests,
        "active request admission rejection is counted");
    ExpectEqual(std::uint64_t{ 3 }, metrics.droppedTraceEvents,
        "bounded trace overflow drops events instead of blocking responses");
    ExpectEqual(std::uint64_t{ 1 }, metrics.traceCallbackErrors,
        "trace exceptions are contained and counted");
    ExpectTrue(selfStop.load() == ErrorCode::InvalidArgument && queried.load(),
        "trace can query metrics but cannot self-join Stop");
    ExpectTrue(metrics.activeConnections == 0 && metrics.activeRequests == 0 &&
                   metrics.pendingTraceEvents == 0,
        "shutdown returns active and trace gauges to zero");
    ExpectEqual(metrics.acceptedConnections, metrics.closedConnections,
        "all accepted connections close by Stop");
    ExpectTrue(metrics.maxLatencyNanoseconds > 0 &&
                   metrics.totalLatencyNanoseconds >= metrics.maxLatencyNanoseconds,
        "HTTP latency accumulates terminal request durations");
    ExpectTrue(metrics.retainedRequestBytes > 0,
        "retained cancelled contexts remain visible in byte metrics");
    cancelled.reset();
    timed.reset();
    ExpectEqual(std::size_t{ 0 }, server.GetMetrics().retainedRequestBytes,
        "last context release updates the byte gauge after Stop");
    ExpectEqual(
        std::size_t{ 2 }, traces.size(), "only bounded admitted trace events are delivered");
    if (traces.size() == 2)
    {
        ExpectTrue(traces[0].requestId != traces[1].requestId &&
                       traces[0].connectionId != traces[1].connectionId,
            "owned trace IDs distinguish requests and connections without metric labels");
        ExpectEqual(
            std::string("GET"), traces[0].method, "safe trace metadata contains method only");
        ExpectTrue(traces[0].status == 200 && traces[0].outcome == ErrorCode::Ok,
            "trace reports local successful completion");
    }
}
const ServerCoreTest::CheckRegistration logger(
    "Observability.LoggerFilteringAndRotation", LoggerFilteringAndRotation);
const ServerCoreTest::CheckRegistration loggerRotationFailure(
    "Observability.LoggerRotationFailureKeepsArchives", LoggerRotationFailureKeepsArchives);
const ServerCoreTest::CheckRegistration loggerStartRetry(
    "Observability.LoggerStartCanRetryAfterFailure", LoggerStartCanRetryAfterFailure);
void HistogramAndPrometheus()
{
    Obs::TaskExecutorMetricsSnapshot snapshot;
    const std::array<std::uint64_t, 4> elapsed{ 100000, 100001, 1000000000, 1000000001 };
    for (const auto value : elapsed)
    {
        Obs::Detail::ObserveLatency(snapshot.latencyHistogram, value);
        Obs::Detail::Add(snapshot.totalLatencyNanoseconds, value);
    }
    ExpectEqual(
        std::uint64_t{ 1 }, snapshot.latencyHistogram.buckets[0], "inclusive finite upper bound");
    ExpectEqual(std::uint64_t{ 1 }, snapshot.latencyHistogram.buckets[1],
        "next nanosecond enters next bin");
    ExpectEqual(std::uint64_t{ 1 }, snapshot.latencyHistogram.buckets[10], "last finite bound");
    ExpectEqual(
        std::uint64_t{ 1 }, snapshot.latencyHistogram.buckets[11], "overflow enters infinity bin");
    auto rendered = Obs::RenderPrometheus(snapshot);
    ExpectTrue(rendered.IsOk(), "export succeeds");
    if (rendered.IsOk())
    {
        const auto& value = rendered.Value();
        ExpectTrue(value.find("servercore_tasks_latency_seconds_bucket{le=\"0.000100000\"} 1\n") !=
                       std::string::npos,
            "nanoseconds exported in seconds");
        ExpectTrue(value.find("servercore_tasks_latency_seconds_bucket{le=\"0.000250000\"} 2\n") !=
                       std::string::npos,
            "bins become cumulative buckets");
        ExpectTrue(
            value.find("servercore_tasks_latency_seconds_bucket{le=\"+Inf\"} 4\n") !=
                    std::string::npos &&
                value.find("servercore_tasks_latency_seconds_count 4\n") != std::string::npos,
            "infinity bucket equals exported count");
        ExpectTrue(
            value.find("servercore_tasks_latency_seconds_sum 2.000200002\n") != std::string::npos,
            "seconds preserve exact integer nanosecond sum");
        ExpectTrue(!value.empty() && value.back() == '\n', "exposition ends in newline");
    }
    snapshot.latencyHistogram.buckets[0] = (std::numeric_limits<std::uint64_t>::max)();
    Obs::Detail::ObserveLatency(snapshot.latencyHistogram, 1);
    ExpectEqual((std::numeric_limits<std::uint64_t>::max)(), snapshot.latencyHistogram.buckets[0],
        "bin count saturates without wrap");
    Runtime::ServerMetricsSnapshot game;
    game.activeSessionCount = 2;
    game.sessionSendQueues = { { static_cast<ServerCore::Session::SessionId>(1), 4 },
        { static_cast<ServerCore::Session::SessionId>(2), 6 } };
    game.retainedSendBytes = 15;
    auto gameText = Obs::RenderPrometheus(game);
    ExpectTrue(
        gameText.IsOk() &&
            gameText.Value().find("servercore_game_retained_send_bytes 15\n") != std::string::npos,
        "game collector uses retained storage rather than remaining wire bytes without per-session "
        "labels");
}
const ServerCoreTest::CheckRegistration histogram(
    "Observability.HistogramAndPrometheus", HistogramAndPrometheus);
const ServerCoreTest::CheckRegistration tasks("Observability.TaskMetrics", TaskMetrics);
const ServerCoreTest::CheckRegistration http(
    "Observability.HttpMetricsAndTracing", HttpMetricsAndTracing);
}
