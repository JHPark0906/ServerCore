#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Observability/Observation.h"
namespace ServerCore::Web { class HttpServer; }
namespace ServerCore::Runtime { class TaskExecutor; class ServerHost; class DatagramTransport; struct ServerMetricsSnapshot; }
namespace ServerCore::Observability
{
class AsyncLogger;
struct HttpServerMetricsSnapshot;
struct TaskExecutorMetricsSnapshot;
struct LoggerMetricsSnapshot;
// Pull adapters over the canonical metrics, no registry/background worker.
// Gauges are independently sampled and are not a health/readiness decision.
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const HttpServerMetricsSnapshot&) noexcept;
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const TaskExecutorMetricsSnapshot&) noexcept;
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const LoggerMetricsSnapshot&) noexcept;
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const Runtime::ServerMetricsSnapshot&) noexcept;
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const Web::HttpServer&) noexcept;
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const Runtime::TaskExecutor&) noexcept;
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const AsyncLogger&) noexcept;
[[nodiscard]] SERVERCORE_API ObservationSnapshot Observe(const Runtime::DatagramTransport&) noexcept;
// Preserves ServerHost::SnapshotMetrics's JobRunner-context requirement.
[[nodiscard]] SERVERCORE_API Core::Result<ObservationSnapshot> Observe(const Runtime::ServerHost&);
}
