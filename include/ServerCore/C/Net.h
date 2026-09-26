#ifndef SERVERCORE_C_NET_H
#define SERVERCORE_C_NET_H
#include "ServerCore/C/Endpoint.h"
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct sc_tcp_connection sc_tcp_connection;
    typedef struct sc_tcp_event sc_tcp_event;
    typedef struct sc_tcp_options
    {
        uint32_t abi_version, struct_size;
        sc_bytes listen_address;
        /* port must be nonzero: the initializer leaves 0, which Start rejects with
     * InvalidArgument (no ephemeral port; CAbi.TcpOwnershipAndTerminal). */
        uint16_t port, reserved;
        uint32_t io_threads;
        /* Received events count against the connection's own budget
     * (max_connection_event_count/bytes, defaults 64 and 1 MiB) and the server-wide
     * memory cap (max_event_count/bytes), including events the consumer has taken
     * but not destroyed. When either is full, only that connection stops
     * receiving; destroying its events (or, for the cap, any events) resumes it.
     * No connection is closed for a full budget, so one that is not consumed
     * cannot disconnect others (CAbi.TcpRetainedEventOverflow). A receive already
     * in progress when the pause starts (at most 16 KiB) is still admitted, so a
     * budget can be exceeded by that much per connection. */
        size_t max_connections, max_event_count, max_event_bytes;
        size_t connection_send_bytes, total_send_bytes;
        size_t max_connection_event_count, max_connection_event_bytes;
    } sc_tcp_options;
    enum
    {
        SC_TCP_BYTES = 1,
        SC_TCP_CLOSED = 2
    };
    typedef struct sc_tcp_event_view
    {
        uint32_t abi_version, struct_size;
        uint32_t kind;
        sc_status status;
        sc_bytes bytes;
    } sc_tcp_event_view;
    SC_API sc_status sc_tcp_options_init(sc_tcp_options* options, size_t size) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_server_create(
        const sc_tcp_options* options, sc_tcp_server** out) SC_NOEXCEPT;
    /* Same options/layout; explicit IPv6-only policy (0 dual stack, 1 IPv6 only).
 * The original create defaults to 1. This flag is ignored for IPv4 literals. */
    SC_API sc_status sc_tcp_server_create_ex(
        const sc_tcp_options*, uint32_t ipv6_only, sc_tcp_server** out) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_server_start(sc_tcp_server* server) SC_NOEXCEPT;
    SC_API uint16_t sc_tcp_server_port(const sc_tcp_server* server) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_server_local_endpoint(
        const sc_tcp_server*, sc_ip_endpoint* out) SC_NOEXCEPT;
    /* Accept queue and live connections are bounded by max_connections. */
    SC_API sc_status sc_tcp_server_accept(
        sc_tcp_server* server, uint32_t timeout_ms, sc_tcp_connection** out) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_server_stop(sc_tcp_server* server) SC_NOEXCEPT;
    SC_API void sc_tcp_server_destroy(sc_tcp_server* server) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_connection_retain(
        const sc_tcp_connection* connection, sc_tcp_connection** out) SC_NOEXCEPT;
    SC_API uint64_t sc_tcp_connection_id(const sc_tcp_connection* connection) SC_NOEXCEPT;
    /* Snapshots remain available after close. Neither output may be NULL. */
    SC_API sc_status sc_tcp_connection_endpoints(
        const sc_tcp_connection*, sc_ip_endpoint* local, sc_ip_endpoint* remote) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_connection_send(
        sc_tcp_connection* connection, sc_bytes bytes) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_connection_next(
        sc_tcp_connection* connection, uint32_t timeout_ms, sc_tcp_event** out) SC_NOEXCEPT;
    /* The caller's pause is independent of budget backpressure: resume lifts only
 * the caller's pause, and receiving restarts once no budget is full either. */
    SC_API sc_status sc_tcp_connection_pause(sc_tcp_connection* connection) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_connection_resume(sc_tcp_connection* connection) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_connection_wait_capacity(
        sc_tcp_connection* connection, size_t bytes, sc_wait** out) SC_NOEXCEPT;
    SC_API void sc_tcp_connection_close(sc_tcp_connection* connection) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_event_get_view(
        const sc_tcp_event* event, sc_tcp_event_view* view) SC_NOEXCEPT;
    SC_API void sc_tcp_event_destroy(sc_tcp_event* event) SC_NOEXCEPT;
    /* Last public connection handle closes it; no implicit framing/JSON is added.
 * A full receive budget pauses instead of closing; terminal events are reserved. */
    SC_API void sc_tcp_connection_destroy(sc_tcp_connection* connection) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_server_subscribe(
        sc_tcp_server*, sc_notifier*, uint64_t key, sc_subscription** out) SC_NOEXCEPT;
    SC_API sc_status sc_tcp_connection_subscribe(
        sc_tcp_connection*, sc_notifier*, uint64_t key, sc_subscription** out) SC_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
