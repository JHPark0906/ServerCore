#ifndef SERVERCORE_C_WEB_H
#define SERVERCORE_C_WEB_H
#include "ServerCore/C/Types.h"
#include "ServerCore/C/Observability.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct sc_web_server sc_web_server;
typedef struct sc_web_event sc_web_event;
typedef struct sc_http_response sc_http_response;
typedef struct sc_websocket sc_websocket;
typedef struct sc_websocket_event sc_websocket_event;
typedef struct sc_http_body sc_http_body;
typedef struct sc_body_chunk sc_body_chunk;
typedef struct sc_request_decision sc_request_decision;

typedef struct sc_web_options {
    uint32_t abi_version, struct_size;
    sc_bytes listen_address;
    uint16_t port, reserved;
    uint32_t io_threads, handler_threads;
    size_t max_connections, max_header_bytes, max_body_bytes, max_buffered_response_bytes;
    size_t max_active_requests, max_request_bytes;
    /* Server-wide limits include queued and consumer-held events. */
    size_t max_event_count, max_event_bytes;
    size_t max_ws_event_count, max_ws_event_bytes;
    size_t max_send_bytes, max_chunk_bytes;
    uint32_t handler_timeout_ms, stream_idle_timeout_ms, send_stall_timeout_ms;
} sc_web_options;

enum { SC_WEB_REQUEST = 1, SC_WEB_WEBSOCKET = 2, SC_WEB_POLICY = 3 };
/* Separate descriptors preserve every preexisting ABI 1 structure layout. */
typedef struct sc_body_options {
    uint32_t abi_version, struct_size;
    /* max_body_bytes must be >= sc_web_options.max_body_bytes. */
    size_t max_buffered_bytes, max_body_bytes;
} sc_body_options;
typedef struct sc_headers_view {
    uint32_t abi_version, struct_size;
    const sc_header* headers;
    size_t count;
} sc_headers_view;
typedef struct sc_policy_allow {
    uint32_t abi_version, struct_size;
    const sc_header* response_headers;
    size_t response_header_count;
    const sc_header* attributes;
    size_t attribute_count;
} sc_policy_allow;
typedef struct sc_request_view {
    uint32_t abi_version, struct_size;
    sc_bytes method, target, body;
    const sc_header* headers;
    size_t header_count;
    const sc_header* parameters;
    size_t parameter_count;
} sc_request_view;
enum { SC_RESPONSE_CLOSE = 1u, SC_RESPONSE_HAS_LENGTH = 2u };
typedef struct sc_response_head {
    uint32_t abi_version, struct_size;
    uint32_t status, flags;
    uint64_t content_length;
    const sc_header* headers;
    size_t header_count;
} sc_response_head;

SC_API sc_status sc_web_options_init(sc_web_options* options, size_t size);
SC_API sc_status sc_response_head_init(sc_response_head* head, size_t size);
SC_API sc_status sc_web_server_create(const sc_web_options* options, sc_web_server** out);
/* pattern=0 means exact path; pattern=1 means explicit {parameter} segments. */
SC_API sc_status sc_web_server_route(sc_web_server* server, sc_bytes method, sc_bytes path, uint32_t pattern);
SC_API sc_status sc_web_server_websocket(sc_web_server* server, sc_bytes path, uint32_t pattern);
/* Opt-in bounded upload streaming. Initial request body is empty; claim the
 * reader. All limits/configuration are set before Start. */
SC_API sc_status sc_body_options_init(sc_body_options* options, size_t size);
SC_API sc_status sc_web_server_set_body_limits(sc_web_server* server, const sc_body_options* options);
SC_API sc_status sc_web_server_stream_route(sc_web_server* server, sc_bytes method, sc_bytes path, uint32_t pattern);
/* Policy events run before selected HTTP handlers and before 101. Buffered
 * routes already have their body; streaming routes gate at headers. Protocol
 * and routing errors bypass policy. Global and route-specific WebSocket policy
 * are independent sequential gates. */
SC_API sc_status sc_web_server_enable_policy(sc_web_server* server);
SC_API sc_status sc_web_server_websocket_policy(sc_web_server* server, sc_bytes path, uint32_t pattern);
SC_API sc_status sc_web_server_start(sc_web_server* server);
SC_API uint16_t sc_web_server_port(const sc_web_server* server);
/* Queue bounds include events held by the consumer until event_destroy. HTTP
 * admission overflow produces 503; WebSocket overflow closes that connection.
 * No foreign callbacks run on native I/O/handler threads. */
SC_API sc_status sc_web_server_next(sc_web_server* server, uint32_t timeout_ms, sc_web_event** out);
SC_API sc_status sc_web_server_stop(sc_web_server* server);
/* Drain stops admission, finishes admitted HTTP responses, closes WebSockets
 * with 1001. Status is nonblocking (WouldBlock/Ok). Stop remains immediate. */
SC_API sc_status sc_web_server_begin_drain(sc_web_server* server);
SC_API sc_status sc_web_server_drain_status(const sc_web_server* server);
/* Blocks caller; expiry forces Stop and reports Timeout. UINT32_MAX is infinite. */
SC_API sc_status sc_web_server_stop_gracefully(sc_web_server* server, uint32_t timeout_ms);
SC_API void sc_web_server_destroy(sc_web_server* server);
SC_API uint32_t sc_web_event_kind(const sc_web_event* event);
/* Borrowed views remain valid until event_destroy, even after server_stop. */
SC_API sc_status sc_web_event_request(const sc_web_event* event, sc_request_view* view);
/* Obtain independent owning handles. Dropping an unclaimed HTTP event aborts
 * its response. The last response handle aborts an unfinished response. */
SC_API sc_status sc_web_event_response(sc_web_event* event, sc_http_response** out);
SC_API sc_status sc_web_event_socket(sc_web_event* event, sc_websocket** out);
SC_API sc_status sc_web_event_attributes(const sc_web_event* event, sc_headers_view* view);
SC_API sc_status sc_web_event_body(sc_web_event* event, sc_http_body** out);
SC_API sc_status sc_web_event_decision(sc_web_event* event, sc_request_decision** out);
SC_API uint32_t sc_web_event_policy_is_websocket(const sc_web_event* event);
SC_API void sc_web_event_destroy(sc_web_event* event);

/* Allow/Reject are single use. Reject sends HTTP, never a WebSocket 101.
 * Attributes are application metadata available on the admitted HTTP event;
 * opened WebSocket events only expose the original request. */
SC_API sc_status sc_request_decision_allow(sc_request_decision* decision, const sc_policy_allow* options);
SC_API sc_status sc_request_decision_reject(sc_request_decision* decision, const sc_response_head* head, sc_bytes body);
SC_API uint32_t sc_request_decision_cancelled(const sc_request_decision* decision);
SC_API void sc_request_decision_abort(sc_request_decision* decision);
SC_API void sc_request_decision_destroy(sc_request_decision* decision);
/* Nonblocking read: Ok + owned chunk, Ok + NULL at clean EOF, WouldBlock while
 * pending, or terminal error. Retaining a chunk retains native receive capacity.
 * Last reader handle (or unclaimed streaming event) aborts before clean EOF. */
SC_API sc_status sc_http_body_read(sc_http_body* body, sc_body_chunk** out);
SC_API size_t sc_http_body_retained_bytes(const sc_http_body* body);
SC_API uint32_t sc_http_body_cancelled(const sc_http_body* body);
SC_API void sc_http_body_cancel(sc_http_body* body);
SC_API void sc_http_body_destroy(sc_http_body* body);
SC_API sc_bytes sc_body_chunk_view(const sc_body_chunk* chunk);
SC_API void sc_body_chunk_destroy(sc_body_chunk* chunk);

SC_API sc_status sc_http_response_retain(const sc_http_response* response, sc_http_response** out);
SC_API sc_status sc_http_response_start(sc_http_response* response, const sc_response_head* head);
SC_API sc_status sc_http_response_write(sc_http_response* response, sc_bytes bytes);
SC_API sc_status sc_http_response_finish(sc_http_response* response);
SC_API sc_status sc_http_response_complete(sc_http_response* response, const sc_response_head* head, sc_bytes body);
SC_API sc_status sc_http_response_wait_capacity(sc_http_response* response, size_t bytes, sc_wait** out);
SC_API uint32_t sc_http_response_cancelled(const sc_http_response* response);
SC_API uint32_t sc_http_response_is_head(const sc_http_response* response);
SC_API size_t sc_http_response_max_write(const sc_http_response* response);
SC_API void sc_http_response_abort(sc_http_response* response);
SC_API void sc_http_response_destroy(sc_http_response* response);

enum { SC_WS_TEXT = 1, SC_WS_BINARY = 2, SC_WS_CLOSED = 3 };
typedef struct sc_websocket_event_view {
    uint32_t abi_version, struct_size;
    uint32_t kind;
    uint16_t close_code, reserved;
    sc_bytes bytes;
} sc_websocket_event_view;
SC_API sc_status sc_websocket_retain(const sc_websocket* socket, sc_websocket** out);
SC_API uint64_t sc_websocket_id(const sc_websocket* socket);
SC_API sc_status sc_websocket_send(sc_websocket* socket, uint32_t kind, sc_bytes bytes);
SC_API sc_status sc_websocket_close(sc_websocket* socket, uint16_t code, sc_bytes reason);
SC_API sc_status sc_websocket_next(sc_websocket* socket, uint32_t timeout_ms, sc_websocket_event** out);
SC_API sc_status sc_websocket_wait_capacity(sc_websocket* socket, size_t bytes, sc_wait** out);
SC_API sc_status sc_websocket_event_get_view(const sc_websocket_event* event, sc_websocket_event_view* view);
SC_API void sc_websocket_event_destroy(sc_websocket_event* event);
/* Last socket handle requests close. Held message events remain readable. */
SC_API void sc_websocket_destroy(sc_websocket* socket);

#ifdef __cplusplus
}
#endif
#endif
