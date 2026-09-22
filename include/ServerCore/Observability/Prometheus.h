#pragma once
#include "ServerCore/Core/Error.h"
#include "ServerCore/Observability/Metrics.h"
#include "ServerCore/Runtime/Metrics.h"
#include <string>

namespace ServerCore::Observability
{
// Prometheus text exposition 0.0.4. Fixed metric names, no request/user labels;
// instance labels and scrape authentication belong to the collector/application.
// Pure snapshot adapters: no thread, global registry or automatic HTTP endpoint.
// Histograms use seconds and cumulative buckets; snapshot bins are noncumulative.
Core::Result<std::string> RenderPrometheus(const TaskExecutorMetricsSnapshot& snapshot);
Core::Result<std::string> RenderPrometheus(const HttpServerMetricsSnapshot& snapshot);
Core::Result<std::string> RenderPrometheus(const JobRunnerMetricsSnapshot& snapshot);
Core::Result<std::string> RenderPrometheus(const Runtime::ServerMetricsSnapshot& snapshot);
}
