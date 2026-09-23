#ifndef SERVERCORE_C_NET_H
#define SERVERCORE_C_NET_H
#include "ServerCore/C/Types.h"
#include "ServerCore/C/Endpoint.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sc_tcp_server sc_tcp_server;
typedef struct sc_tcp_connection sc_tcp_connection;
typedef struct sc_tcp_event sc_tcp_event;
typedef struct sc_tcp_options {
    uint32_t abi_version, struct_size;
    sc_bytes listen_address;
    uint16_t port, reserved;
    uint32_t io_threads;
    size_t max_connections, max_event_count, max_event_bytes;
    size_t connection_send_bytes, total_send_bytes;
} sc_tcp_options;
enum { SC_TCP_BYTES = 1, SC_TCP_CLOSED = 2 };
typedef struct sc_tcp_event_view {
    uint32_t abi_version, struct_size;
    uint32_t kind;
    sc_status status;
    sc_bytes bytes;
} sc_tcp_event_view;
SC_API sc_status sc_tcp_options_init(sc_tcp_options* options, size_t size);
SC_API sc_status sc_tcp_server_create(const sc_tcp_options* options, sc_tcp_server** out);
/* Same options/layout; explicit IPv6-only policy (0 dual stack, 1 IPv6 only).
 * The original create defaults to 1. This flag is ignored for IPv4 literals. */
SC_API sc_status sc_tcp_server_create_ex(const sc_tcp_options*,uint32_t ipv6_only,sc_tcp_server** out);
SC_API sc_status sc_tcp_server_start(sc_tcp_server* server);
SC_API uint16_t sc_tcp_server_port(const sc_tcp_server* server);
SC_API sc_status sc_tcp_server_local_endpoint(const sc_tcp_server*,sc_ip_endpoint* out);
/* Accept queue and live connections are bounded by max_connections. */
SC_API sc_status sc_tcp_server_accept(sc_tcp_server* server, uint32_t timeout_ms, sc_tcp_connection** out);
SC_API sc_status sc_tcp_server_stop(sc_tcp_server* server);
SC_API void sc_tcp_server_destroy(sc_tcp_server* server);
SC_API sc_status sc_tcp_connection_retain(const sc_tcp_connection* connection, sc_tcp_connection** out);
SC_API uint64_t sc_tcp_connection_id(const sc_tcp_connection* connection);
/* Snapshots remain available after close. Neither output may be NULL. */
SC_API sc_status sc_tcp_connection_endpoints(const sc_tcp_connection*,sc_ip_endpoint* local,sc_ip_endpoint* remote);
SC_API sc_status sc_tcp_connection_send(sc_tcp_connection* connection, sc_bytes bytes);
SC_API sc_status sc_tcp_connection_next(sc_tcp_connection* connection, uint32_t timeout_ms, sc_tcp_event** out);
SC_API sc_status sc_tcp_connection_pause(sc_tcp_connection* connection);
SC_API sc_status sc_tcp_connection_resume(sc_tcp_connection* connection);
SC_API sc_status sc_tcp_connection_wait_capacity(sc_tcp_connection* connection, size_t bytes, sc_wait** out);
SC_API void sc_tcp_connection_close(sc_tcp_connection* connection);
SC_API sc_status sc_tcp_event_get_view(const sc_tcp_event* event, sc_tcp_event_view* view);
SC_API void sc_tcp_event_destroy(sc_tcp_event* event);
/* Last public connection handle closes it; no implicit framing/JSON is added.
 * Receive queue overflow closes with TooLarge; terminal events are reserved. */
SC_API void sc_tcp_connection_destroy(sc_tcp_connection* connection);
SC_API sc_status sc_tcp_server_subscribe(sc_tcp_server*, sc_notifier*, uint64_t key, sc_subscription** out);
SC_API sc_status sc_tcp_connection_subscribe(sc_tcp_connection*, sc_notifier*, uint64_t key, sc_subscription** out);
#ifdef __cplusplus
}
#endif
#endif
