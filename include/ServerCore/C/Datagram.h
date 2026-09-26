#ifndef SERVERCORE_C_DATAGRAM_H
#define SERVERCORE_C_DATAGRAM_H
#include "ServerCore/C/Endpoint.h"
#include "ServerCore/C/Observability.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct sc_udp_transport sc_udp_transport;
    typedef struct sc_udp_event sc_udp_event;
    typedef struct sc_udp_token
    {
        uint8_t bytes[16];
    } sc_udp_token;
    enum
    {
        SC_UDP_JSON = 0,
        SC_UDP_BINARY = 1,
        SC_UDP_MESSAGE = 0,
        SC_UDP_CLOSED = 1
    };
    typedef struct sc_udp_rate
    {
        uint64_t bytes_per_interval, burst_bytes;
        uint32_t interval_ms, reserved;
    } sc_udp_rate;
    typedef struct sc_udp_options
    {
        uint32_t abi_version, struct_size;
        sc_bytes listen_address;
        uint16_t port, reserved;
        uint32_t ipv6_only, payload_mode;
        size_t max_sessions, max_event_count, max_event_bytes;
        size_t max_datagrams_per_batch, max_bytes_per_batch;
        sc_udp_rate total_send_rate, peer_send_rate;
        /* max_sequence_jump bounds how far one packet may advance a session's
         * receive sequence (default 1024; 0 is InvalidArgument). Nonzero
         * allow_endpoint_migration (default 0) lets a valid packet from a new
         * source move the session's return endpoint; without path validation
         * that permits reflection, so enable it only for NAT rebinding and with
         * peer_send_rate. Rejections count in sequence_jump_datagrams and
         * endpoint_mismatch_datagrams. reserved2 must be 0. */
        uint64_t max_sequence_jump;
        uint32_t allow_endpoint_migration, reserved2;
    } sc_udp_options;
    typedef struct sc_udp_event_view
    {
        uint32_t abi_version, struct_size;
        uint32_t kind, payload_mode;
        sc_status status;
        uint32_t binary_type;
        uint64_t session_id, sequence;
        sc_ip_endpoint remote_endpoint;
        /* JSON: original validated envelope. Binary: application bytes excluding type. */
        sc_bytes payload;
    } sc_udp_event_view;
    typedef struct sc_udp_metrics
    {
        uint32_t abi_version, struct_size;
        uint64_t received_datagrams, received_bytes, sent_datagrams, sent_bytes;
        uint64_t rejected_datagrams, send_would_block, socket_errors;
        uint64_t truncated_datagrams, malformed_datagrams, unknown_token_datagrams,
            replayed_datagrams;
        uint64_t invalid_payload_datagrams, admission_rejected_datagrams, stale_datagrams,
            callback_failures;
        uint64_t send_rate_limited, send_limited_global, send_limited_peer, send_not_ready;
        uint64_t pump_batches, pump_failures, poll_contentions, event_queue_drops;
        size_t registered_sessions, pending_batches, retained_events, retained_event_bytes;
        uint32_t bound, receiving;
        /* Both also count in rejected_datagrams. */
        uint64_t endpoint_mismatch_datagrams, sequence_jump_datagrams;
    } sc_udp_metrics;
    SC_API sc_status sc_udp_options_init(sc_udp_options*, size_t) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_create(const sc_udp_options*, sc_udp_transport**) SC_NOEXCEPT;
    /* Start owns one bounded executor and readiness pump. Single-use C owner. */
    SC_API sc_status sc_udp_transport_start(sc_udp_transport*) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_local_endpoint(
        const sc_udp_transport*, sc_ip_endpoint*) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_remote_endpoint(
        const sc_udp_transport*, uint64_t, sc_ip_endpoint*) SC_NOEXCEPT;
    /* Tokens must be distributed on an authenticated reliable control channel.
 * Registration does not trust an endpoint; only a valid admitted packet binds it. */
    SC_API sc_status sc_udp_transport_register(
        sc_udp_transport*, uint64_t, sc_udp_token*) SC_NOEXCEPT;
    SC_API void sc_udp_transport_unregister(sc_udp_transport*, uint64_t) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_send_json(sc_udp_transport*, uint64_t, sc_bytes) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_send_binary(
        sc_udp_transport*, uint64_t, uint32_t, sc_bytes) SC_NOEXCEPT;
    /* Event budget includes popped/held events. Overflow discards the packet before
 * endpoint/sequence commit; it never creates an unbounded queue. A reserved
 * Closed event follows queued events on Stop. Views outlive transport via owner. */
    SC_API sc_status sc_udp_transport_next(
        sc_udp_transport*, uint32_t timeout_ms, sc_udp_event**) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_subscribe(
        sc_udp_transport*, sc_notifier*, uint64_t, sc_subscription**) SC_NOEXCEPT;
    SC_API sc_status sc_udp_event_get(const sc_udp_event*, sc_udp_event_view*) SC_NOEXCEPT;
    SC_API void sc_udp_event_destroy(sc_udp_event*) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_get_metrics(
        const sc_udp_transport*, sc_udp_metrics*) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_get_observation(
        const sc_udp_transport*, sc_observation*) SC_NOEXCEPT;
    SC_API sc_status sc_udp_transport_stop(sc_udp_transport*) SC_NOEXCEPT;
    SC_API void sc_udp_transport_destroy(sc_udp_transport*) SC_NOEXCEPT;
    /* Shared DatagramCodec helpers; no retransmission/replay state is added here.
 * Encode writes no partial packet; required reports the complete wire size.
 * Decode payload borrows the input packet until the caller releases it. */
    /* Output: initialize abi_version and struct_size before the call. */
    typedef struct sc_udp_packet_view
    {
        uint32_t abi_version, struct_size;
        sc_udp_token token;
        uint64_t sequence;
        sc_bytes payload;
    } sc_udp_packet_view;
    /* payload must not overlap the bytes the packet will occupy in output;
     * overlap is InvalidArgument with *written = 0
     * (CAbi.DatagramCodecAndValidation). */
    SC_API sc_status sc_udp_packet_encode(
        const sc_udp_token*, uint64_t, sc_bytes, uint8_t*, size_t, size_t*) SC_NOEXCEPT;
    SC_API sc_status sc_udp_packet_decode(sc_bytes, sc_udp_packet_view*) SC_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
