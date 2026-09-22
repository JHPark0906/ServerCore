#include "ServerCore/Observability/Prometheus.h"
#include "Observability/MetricsInternal.h"
#include <charconv>
#include <string_view>

namespace ServerCore::Observability
{
namespace
{
void Number(std::string& out, std::uint64_t value)
{
    char buffer[32];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, converted.ptr);
}
void Seconds(std::string& out, std::uint64_t nanoseconds)
{
    Number(out, nanoseconds / 1000000000);
    out.push_back('.');
    const auto fraction = nanoseconds % 1000000000;
    char digits[9];
    auto remaining = fraction;
    for (std::size_t index = 9; index > 0; --index)
    {
        digits[index - 1] = static_cast<char>('0' + remaining % 10);
        remaining /= 10;
    }
    out.append(digits, sizeof(digits));
}
void Scalar(std::string& out, std::string_view prefix, std::string_view field,
    std::string_view kind, std::uint64_t value)
{
    out += "# TYPE ";
    out += prefix;
    out += field;
    out += ' ';
    out += kind;
    out += '\n';
    out += prefix;
    out += field;
    out += ' ';
    Number(out, value);
    out += '\n';
}
void Histogram(std::string& out, std::string_view prefix, const LatencyHistogramSnapshot& histogram,
    std::uint64_t totalNanoseconds)
{
    const auto name = std::string(prefix) + "latency_seconds";
    out += "# TYPE " + name + " histogram\n";
    std::uint64_t count = 0;
    for (std::size_t index = 0; index < histogram.buckets.size(); ++index)
    {
        Detail::Add(count, histogram.buckets[index]);
        out += name + "_bucket{le=\"";
        if (index < LatencyBucketUpperBoundsNanoseconds.size())
            Seconds(out, LatencyBucketUpperBoundsNanoseconds[index]);
        else
            out += "+Inf";
        out += "\"} ";
        Number(out, count);
        out += '\n';
    }
    out += name + "_count ";
    Number(out, count);
    out += '\n';
    out += name + "_sum ";
    Seconds(out, totalNanoseconds);
    out += '\n';
}
void Tasks(std::string& out, const TaskExecutorMetricsSnapshot& s, std::string_view prefix)
{
    Scalar(out, prefix, "pending", "gauge", s.pendingTasks);
    Scalar(out, prefix, "running", "gauge", s.runningTasks);
    Scalar(out, prefix, "retained_bytes", "gauge", s.retainedBytes);
    Scalar(out, prefix, "accepted_total", "counter", s.acceptedTasks);
    Scalar(out, prefix, "completed_total", "counter", s.completedTasks);
    Scalar(out, prefix, "failed_total", "counter", s.failedTasks);
    Scalar(out, prefix, "cancelled_total", "counter", s.cancelledTasks);
    Scalar(out, prefix, "timed_out_total", "counter", s.timedOutTasks);
    Scalar(out, prefix, "rejected_total", "counter", s.rejectedTasks);
    Scalar(out, prefix, "max_latency_nanoseconds", "gauge", s.maxLatencyNanoseconds);
    Histogram(out, prefix, s.latencyHistogram, s.totalLatencyNanoseconds);
}
void Jobs(std::string& out, const JobRunnerMetricsSnapshot& s)
{
    constexpr std::string_view prefix = "servercore_jobs_";
    Scalar(out, prefix, "pending", "gauge", s.pendingJobs);
    Scalar(out, prefix, "outstanding", "gauge", s.outstandingJobs);
    Scalar(out, prefix, "retained_bytes", "gauge", s.retainedBytes);
    Scalar(out, prefix, "accepted_total", "counter", s.acceptedJobs);
    Scalar(out, prefix, "completed_total", "counter", s.completedJobs);
    Scalar(out, prefix, "max_latency_nanoseconds", "gauge", s.maxLatencyNanoseconds);
    Histogram(out, prefix, s.latencyHistogram, s.totalLatencyNanoseconds);
}
}
Core::Result<std::string> RenderPrometheus(const JobRunnerMetricsSnapshot& s)
{
    try
    {
        std::string out;
        Jobs(out, s);
        return Core::Result<std::string>::FromValue(std::move(out));
    }
    catch (...)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<std::string> RenderPrometheus(const Runtime::ServerMetricsSnapshot& s)
{
    try
    {
        std::string out;
        constexpr std::string_view prefix = "servercore_game_";
        Scalar(out, prefix, "active_sessions", "gauge", s.activeSessionCount);
        Scalar(out, prefix, "pending_receive_bytes", "gauge", s.pendingReceiveBytes);
        Scalar(out, prefix, "pending_parse_bytes", "gauge", s.pendingParseBytes);
        Scalar(out, prefix, "pending_parse_tasks", "gauge", s.pendingParseTaskCount);
        Scalar(out, prefix, "received_frames_total", "counter", s.receivedFrameCount);
        Scalar(out, prefix, "queued_send_frames_total", "counter", s.queuedSendFrameCount);
        Scalar(out, prefix, "errors_total", "counter", s.errorCount);
        Scalar(out, prefix, "skipped_periods_total", "counter", s.skippedPeriodCount);
        std::uint64_t sendBytes = 0;
        for (const auto& session : s.sessionSendQueues)
            Detail::Add(sendBytes, session.queuedBytes);
        Scalar(out, prefix, "retained_send_bytes", "gauge", sendBytes);
        Jobs(out, s.jobs);
        return Core::Result<std::string>::FromValue(std::move(out));
    }
    catch (...)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<std::string> RenderPrometheus(const TaskExecutorMetricsSnapshot& snapshot)
{
    try
    {
        std::string result;
        Tasks(result, snapshot, "servercore_tasks_");
        return Core::Result<std::string>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<std::string> RenderPrometheus(const HttpServerMetricsSnapshot& s)
{
    try
    {
        std::string out;
        constexpr std::string_view prefix = "servercore_http_";
        Scalar(out, prefix, "active_connections", "gauge", s.activeConnections);
        Scalar(out, prefix, "active_requests", "gauge", s.activeRequests);
        Scalar(out, prefix, "retained_request_bytes", "gauge", s.retainedRequestBytes);
        Scalar(out, prefix, "retained_send_bytes", "gauge", s.retainedSendBytes);
        Scalar(out, prefix, "pending_trace_events", "gauge", s.pendingTraceEvents);
        Scalar(out, prefix, "accepted_connections_total", "counter", s.acceptedConnections);
        Scalar(out, prefix, "closed_connections_total", "counter", s.closedConnections);
        Scalar(out, prefix, "rejected_connections_total", "counter", s.rejectedConnections);
        Scalar(out, prefix, "accepted_requests_total", "counter", s.acceptedRequests);
        Scalar(out, prefix, "completed_requests_total", "counter", s.completedRequests);
        Scalar(out, prefix, "failed_requests_total", "counter", s.failedRequests);
        Scalar(out, prefix, "cancelled_requests_total", "counter", s.cancelledRequests);
        Scalar(out, prefix, "timed_out_requests_total", "counter", s.timedOutRequests);
        Scalar(out, prefix, "rejected_requests_total", "counter", s.rejectedRequests);
        Scalar(out, prefix, "protocol_errors_total", "counter", s.protocolErrors);
        Scalar(out, prefix, "dropped_trace_events_total", "counter", s.droppedTraceEvents);
        Scalar(out, prefix, "trace_callback_errors_total", "counter", s.traceCallbackErrors);
        Scalar(out, prefix, "max_latency_nanoseconds", "gauge", s.maxLatencyNanoseconds);
        Histogram(out, prefix, s.latencyHistogram, s.totalLatencyNanoseconds);
        Tasks(out, s.handlers, "servercore_http_handlers_");
        return Core::Result<std::string>::FromValue(std::move(out));
    }
    catch (...)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
}
}
