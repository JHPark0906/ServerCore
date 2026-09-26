#ifndef SERVERCORE_C_WEB_POLICY_H
#define SERVERCORE_C_WEB_POLICY_H
#include "ServerCore/C/Endpoint.h"
#include "ServerCore/C/Web.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct sc_http_policy sc_http_policy;
    typedef struct sc_http_policy_result sc_http_policy_result;
    typedef struct sc_trusted_proxy sc_trusted_proxy;
    typedef struct sc_proxy_peer sc_proxy_peer;
    typedef struct sc_http_policy_options
    {
        uint32_t abi_version, struct_size;
        size_t max_rules, max_metadata_bytes, max_response_body_bytes;
    } sc_http_policy_options;
    typedef struct sc_cors_options
    {
        uint32_t abi_version, struct_size;
        const sc_bytes* origins;
        size_t origin_count;
        const sc_bytes* methods;
        size_t method_count;
        const sc_bytes* allowed_headers;
        size_t allowed_header_count;
        const sc_bytes* exposed_headers;
        size_t exposed_header_count;
        uint32_t allow_credentials, max_age_seconds;
    } sc_cors_options;
    enum
    {
        SC_PROXY_FORWARDED = 0,
        SC_PROXY_X_FORWARDED = 1
    };
    typedef struct sc_proxy_options
    {
        uint32_t abi_version, struct_size;
        const sc_bytes* trusted_cidrs;
        size_t trusted_cidr_count;
        uint32_t mode, accept_proto, accept_host, normalize_mapped_ipv4;
        size_t max_hops, max_header_bytes;
    } sc_proxy_options;
    typedef struct sc_proxy_peer_view
    {
        uint32_t abi_version, struct_size;
        sc_ip_endpoint transport_peer, client;
        size_t accepted_hops;
        sc_bytes proto, host;
    } sc_proxy_peer_view;
    typedef struct sc_http_policy_view
    {
        uint32_t abi_version, struct_size;
        uint32_t has_response, status, close;
        const sc_header* response_headers;
        size_t response_header_count;
        const sc_header* attributes;
        size_t attribute_count;
        sc_bytes body;
    } sc_http_policy_view;
    SC_API sc_status sc_http_policy_options_init(sc_http_policy_options*, size_t) SC_NOEXCEPT;
    SC_API sc_status sc_cors_options_init(sc_cors_options*, size_t) SC_NOEXCEPT;
    SC_API sc_status sc_proxy_options_init(sc_proxy_options*, size_t) SC_NOEXCEPT;
    SC_API sc_status sc_trusted_proxy_create(
        const sc_proxy_options*, sc_trusted_proxy**) SC_NOEXCEPT;
    SC_API void sc_trusted_proxy_destroy(sc_trusted_proxy*) SC_NOEXCEPT;
    SC_API sc_status sc_trusted_proxy_resolve(const sc_trusted_proxy*, const sc_ip_endpoint*,
        const sc_header*, size_t, sc_proxy_peer**) SC_NOEXCEPT;
    SC_API sc_status sc_proxy_peer_get(const sc_proxy_peer*, sc_proxy_peer_view*) SC_NOEXCEPT;
    SC_API void sc_proxy_peer_destroy(sc_proxy_peer*) SC_NOEXCEPT;
    SC_API sc_status sc_http_policy_create(
        const sc_http_policy_options*, sc_http_policy**) SC_NOEXCEPT;
    /* Add before first evaluation; configuration is frozen by evaluation. Copies
 * inputs, including proxy configuration. Prefixes use decoded whole segments. */
    SC_API sc_status sc_http_policy_add_headers(
        sc_http_policy*, sc_bytes prefix, const sc_header*, size_t) SC_NOEXCEPT;
    SC_API sc_status sc_http_policy_add_cors(
        sc_http_policy*, sc_bytes prefix, const sc_cors_options*) SC_NOEXCEPT;
    SC_API sc_status sc_http_policy_add_request_id(sc_http_policy*, sc_bytes prefix,
        sc_bytes header, uint32_t accept_incoming, size_t max_bytes) SC_NOEXCEPT;
    SC_API sc_status sc_http_policy_add_proxy(
        sc_http_policy*, sc_bytes prefix, const sc_trusted_proxy*) SC_NOEXCEPT;
    SC_API sc_status sc_http_policy_evaluate(sc_http_policy*, const sc_request_view*,
        const sc_ip_endpoint* peer, uint32_t websocket_upgrade,
        sc_http_policy_result**) SC_NOEXCEPT;
    /* Works on HTTP and policy events; result owns all storage and outlives event. */
    SC_API sc_status sc_http_policy_evaluate_event(
        sc_http_policy*, const sc_web_event*, sc_http_policy_result**) SC_NOEXCEPT;
    SC_API sc_status sc_http_policy_result_get(
        const sc_http_policy_result*, sc_http_policy_view*) SC_NOEXCEPT;
    SC_API sc_status sc_http_policy_result_apply(
        const sc_http_policy_result*, sc_request_decision*) SC_NOEXCEPT;
    SC_API void sc_http_policy_result_destroy(sc_http_policy_result*) SC_NOEXCEPT;
    SC_API void sc_http_policy_destroy(sc_http_policy*) SC_NOEXCEPT;
    /* Validates/canonicalizes JSON via native JsonValue; errors do not expose native
 * diagnostic messages. Results can be used with response_complete or rejection. */
    SC_API sc_status sc_http_json_response(
        sc_bytes json, uint32_t status, size_t max_bytes, sc_http_policy_result**) SC_NOEXCEPT;
    SC_API sc_status sc_http_error_response(
        sc_status error, sc_bytes request_id, uint32_t status, sc_http_policy_result**) SC_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
