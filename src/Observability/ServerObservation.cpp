#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Observability/AsyncLogger.h"
#include "ServerCore/Runtime/DatagramTransport.h"
#include "ServerCore/Runtime/ServerHost.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Web/HttpServer.h"
#include <limits>

namespace ServerCore::Observability
{
namespace
{
std::uint64_t Add(std::uint64_t a, std::uint64_t b) noexcept
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    return b > maximum - a ? maximum : a + b;
}
void Drain(ObservationSnapshot& snapshot, std::uint64_t remaining) noexcept
{
    if (snapshot.lifecycle == Lifecycle::Draining || snapshot.lifecycle == Lifecycle::Stopped)
    {
        snapshot.available |= DrainRemaining;
        snapshot.drainRemaining = remaining;
    }
}
}
ObservationSnapshot Observe(const HttpServerMetricsSnapshot& value) noexcept
{
    ObservationSnapshot result;
    result.protocol = Protocol::Http;
    result.lifecycle = value.lifecycle;
    result.available = Connections | PendingWork | ReceiveBytes | SendBytes | RetainedBytes |
                       ClosedEvents | RejectedEvents | TimedOutEvents;
    // HTTP and upgraded WS share the listener. Existing metrics intentionally
    // do not distinguish their connection totals or guess close reasons.
    result.connections = value.activeConnections;
    result.pendingWork = value.activeRequests;
    result.receiveBytes = value.retainedRequestBytes;
    result.sendBytes = value.retainedSendBytes;
    // Handler admission charges the same shared request storage; adding it
    // again would double-count an allocation already owned by RequestBudget.
    result.retainedBytes = Add(result.receiveBytes, result.sendBytes);
    result.closed[0] = value.closedConnections;
    result.rejected[0] = Add(value.rejectedConnections, value.rejectedRequests);
    result.timedOut = value.timedOutRequests;
    Drain(result, Add(value.activeConnections, value.activeRequests));
    return result;
}
ObservationSnapshot Observe(const TaskExecutorMetricsSnapshot& value) noexcept
{
    ObservationSnapshot result;
    result.protocol = Protocol::Task;
    result.lifecycle = value.lifecycle;
    result.available = PendingWork | RetainedBytes | RejectedEvents | TimedOutEvents;
    result.pendingWork = Add(value.pendingTasks, value.runningTasks);
    result.retainedBytes = value.retainedBytes;
    result.rejected[0] = value.rejectedTasks;
    result.timedOut = value.timedOutTasks;
    Drain(result, result.pendingWork);
    return result;
}
ObservationSnapshot Observe(const LoggerMetricsSnapshot& value) noexcept
{
    ObservationSnapshot result;
    result.protocol = Protocol::Logger;
    result.lifecycle = value.lifecycle;
    result.available = PendingWork | RetainedBytes | RejectedEvents;
    // Includes the record currently being written, unlike the queue-only gauge.
    result.pendingWork = value.outstandingMessages;
    result.retainedBytes = value.retainedBytes;
    result.rejected[0] = value.droppedMessages;
    Drain(result, result.pendingWork);
    return result;
}
ObservationSnapshot Observe(const Runtime::ServerMetricsSnapshot& value) noexcept
{
    ObservationSnapshot result;
    result.protocol = Protocol::Host;
    result.lifecycle = value.lifecycle;
    result.available = Connections | PendingWork | ReceiveBytes | SendBytes | RetainedBytes;
    result.connections = value.activeSessionCount;
    result.pendingWork = Add(value.jobs.outstandingJobs, value.pendingParseTaskCount);
    result.receiveBytes = Add(value.pendingReceiveBytes, value.pendingParseBytes);
    result.sendBytes = value.retainedSendBytes;
    result.retainedBytes =
        Add(Add(result.receiveBytes, result.sendBytes), value.jobs.retainedBytes);
    Drain(result, Add(result.connections, result.pendingWork));
    return result;
}
ObservationSnapshot Observe(const Web::HttpServer& server) noexcept
{
    return Observe(server.GetMetrics());
}
ObservationSnapshot Observe(const Runtime::TaskExecutor& executor) noexcept
{
    return Observe(executor.GetMetrics());
}
ObservationSnapshot Observe(const AsyncLogger& logger) noexcept
{
    return Observe(logger.GetMetrics());
}
ObservationSnapshot Observe(const Runtime::DatagramTransport& transport) noexcept
{
    const auto value = transport.SnapshotMetrics();
    ObservationSnapshot result;
    result.protocol = Protocol::Udp;
    result.lifecycle = value.bound ? Lifecycle::Running : Lifecycle::Unknown;
    result.available = PendingWork | RejectedEvents;
    result.pendingWork = value.pendingBatches;
    auto& protocol = result.rejected[static_cast<std::size_t>(EventReason::ProtocolError)];
    auto& capacity = result.rejected[static_cast<std::size_t>(EventReason::Capacity)];
    auto& policy = result.rejected[static_cast<std::size_t>(EventReason::Policy)];
    protocol = Add(value.malformedDatagrams, value.invalidPayloadDatagrams);
    capacity = value.truncatedDatagrams;
    policy = Add(Add(value.unknownTokenDatagrams, value.replayedDatagrams),
        Add(value.admissionRejectedDatagrams, value.staleDatagrams));
    const auto known = Add(Add(protocol, capacity), policy);
    result.rejected[0] = Add(
        value.rejectedDatagrams > known ? value.rejectedDatagrams - known : 0, value.sendNotReady);
    capacity = Add(capacity, Add(value.sendWouldBlock, value.sendRateLimited));
    return result;
}
Core::Result<ObservationSnapshot> Observe(const Runtime::ServerHost& host)
{
    auto metrics = host.SnapshotMetrics();
    if (!metrics.IsOk())
        return Core::Result<ObservationSnapshot>::FromStatus(std::move(metrics).TakeStatus());
    return Core::Result<ObservationSnapshot>::FromValue(Observe(metrics.Value()));
}
}
