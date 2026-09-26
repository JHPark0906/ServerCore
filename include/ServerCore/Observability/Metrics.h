#pragma once
#include "ServerCore/Observability/Observation.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ServerCore::Observability
{
inline constexpr std::array<std::uint64_t, 11> LatencyBucketUpperBoundsNanoseconds{ 100000, 250000,
    500000, 1000000, 2500000, 5000000, 10000000, 25000000, 50000000, 100000000, 1000000000 };
struct LatencyHistogramSnapshot
{
    // Noncumulative bins, inclusive upper limits above; last bin is +infinity.
    // Fixed storage and fixed labels keep memory independent of requests/users.
    std::array<std::uint64_t, 12> buckets{};
};
struct JobRunnerMetricsSnapshot
{
    std::size_t pendingJobs = 0, outstandingJobs = 0, retainedBytes = 0;
    // Only actually posted jobs count; unused reservations affect gauges only.
    std::uint64_t acceptedJobs = 0, completedJobs = 0;
    std::uint64_t totalLatencyNanoseconds = 0, maxLatencyNanoseconds = 0;
    LatencyHistogramSnapshot latencyHistogram;
};
// Lifetime counters saturate at UINT64_MAX. Latency is admission-to-terminal,
// including queueing and cancellation cleanup; totals permit an average.
struct TaskExecutorMetricsSnapshot
{
    std::size_t pendingTasks = 0, runningTasks = 0, retainedBytes = 0;
    std::uint64_t acceptedTasks = 0, completedTasks = 0, failedTasks = 0;
    std::uint64_t cancelledTasks = 0, timedOutTasks = 0, rejectedTasks = 0;
    std::uint64_t totalLatencyNanoseconds = 0, maxLatencyNanoseconds = 0;
    LatencyHistogramSnapshot latencyHistogram;
    Lifecycle lifecycle = Lifecycle::Created;
};

// Fixed fields only: no per-path, per-user, or per-request metric labels.
// Gauges and counters may be read at different instants. Completed includes all
// terminal outcomes; cancelled/timeouts/failed are disjoint subsets.
struct HttpServerMetricsSnapshot
{
    std::size_t activeConnections = 0, activeRequests = 0, retainedRequestBytes = 0;
    std::size_t retainedSendBytes = 0, pendingTraceEvents = 0;
    std::uint64_t acceptedConnections = 0, closedConnections = 0, rejectedConnections = 0;
    std::uint64_t acceptedRequests = 0, completedRequests = 0, failedRequests = 0;
    std::uint64_t cancelledRequests = 0, timedOutRequests = 0, rejectedRequests = 0;
    std::uint64_t protocolErrors = 0, droppedTraceEvents = 0, traceCallbackErrors = 0;
    std::uint64_t totalLatencyNanoseconds = 0, maxLatencyNanoseconds = 0;
    TaskExecutorMetricsSnapshot handlers;
    LatencyHistogramSnapshot latencyHistogram;
    Lifecycle lifecycle = Lifecycle::Created;
};
}
