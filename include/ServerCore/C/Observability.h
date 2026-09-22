#ifndef SERVERCORE_C_OBSERVABILITY_H
#define SERVERCORE_C_OBSERVABILITY_H
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct sc_logger sc_logger;
typedef struct sc_owned_text sc_owned_text;
typedef struct sc_web_server sc_web_server;
enum { SC_LOG_TRACE = 0, SC_LOG_DEBUG = 1, SC_LOG_INFO = 2, SC_LOG_WARN = 3, SC_LOG_ERROR = 4 };
typedef struct sc_logger_options {
    uint32_t abi_version, struct_size, minimum_level, console;
    sc_bytes file; /* UTF-8 path, empty disables file output. Parent must exist. */
    size_t max_queued_messages, max_retained_bytes, max_message_bytes;
    uint64_t max_file_bytes;
    uint32_t retained_files;
} sc_logger_options;
typedef struct sc_logger_metrics {
    uint32_t abi_version, struct_size;
    size_t pending_messages, retained_bytes;
    uint64_t accepted_messages, written_messages, filtered_messages, dropped_messages, output_errors;
} sc_logger_metrics;

/* Noncumulative latency bins, inclusive upper bounds in nanoseconds:
 * 100000,250000,500000,1000000,2500000,5000000,10000000,25000000,
 * 50000000,100000000,1000000000, then infinity. */
typedef struct sc_task_metrics {
    size_t pending_tasks, running_tasks, retained_bytes;
    uint64_t accepted_tasks, completed_tasks, failed_tasks, cancelled_tasks, timed_out_tasks, rejected_tasks;
    uint64_t total_latency_nanoseconds, max_latency_nanoseconds, latency_buckets[12];
} sc_task_metrics;
typedef struct sc_web_metrics {
    uint32_t abi_version, struct_size;
    size_t active_connections, active_requests, retained_request_bytes, retained_send_bytes, pending_trace_events;
    uint64_t accepted_connections, closed_connections, rejected_connections;
    uint64_t accepted_requests, completed_requests, failed_requests, cancelled_requests, timed_out_requests, rejected_requests;
    uint64_t protocol_errors, dropped_trace_events, trace_callback_errors;
    uint64_t total_latency_nanoseconds, max_latency_nanoseconds, latency_buckets[12];
    sc_task_metrics handlers;
} sc_web_metrics;

SC_API sc_status sc_logger_options_init(sc_logger_options* options, size_t size);
SC_API sc_status sc_logger_create(const sc_logger_options* options, sc_logger** out);
SC_API sc_status sc_logger_try_write(sc_logger* logger, uint32_t level, sc_bytes message);
SC_API sc_status sc_logger_set_minimum_level(sc_logger* logger, uint32_t level);
SC_API sc_status sc_logger_get_metrics(const sc_logger* logger, sc_logger_metrics* out);
SC_API void sc_logger_request_stop(sc_logger* logger);
/* Drains and joins; cannot bound blocked filesystem/console operations. */
SC_API sc_status sc_logger_stop(sc_logger* logger);
/* An attached server retains its logger. Final native owner drains and joins. */
SC_API void sc_logger_destroy(sc_logger* logger);
SC_API sc_status sc_web_server_set_logger(sc_web_server* server, const sc_logger* logger);
/* Initialize output abi_version/struct_size. Snapshot fields are not atomic as a
 * group. No URL, credential, user or other arbitrary high-cardinality labels. */
SC_API sc_status sc_web_server_get_metrics(const sc_web_server* server, sc_web_metrics* out);
SC_API sc_status sc_web_server_metrics_prometheus(const sc_web_server* server, sc_owned_text** out);
SC_API sc_bytes sc_owned_text_view(const sc_owned_text* text);
SC_API void sc_owned_text_destroy(sc_owned_text* text);

#ifdef __cplusplus
}
#endif
#endif
