#ifndef SERVERCORE_C_REQUEST_LIMITER_H
#define SERVERCORE_C_REQUEST_LIMITER_H
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct sc_request_limiter sc_request_limiter;
    typedef struct sc_request_permit sc_request_permit;
    typedef struct sc_request_limiter_options
    {
        uint32_t abi_version, struct_size;
        size_t max_keys, max_key_bytes, max_retained_key_bytes;
        uint64_t refill_tokens, burst_tokens;
        uint32_t refill_interval_ms;
        size_t max_concurrent_per_key, max_concurrent_total;
        uint32_t idle_expiry_ms;
    } sc_request_limiter_options;
    enum
    {
        SC_LIMIT_NONE = 0,
        SC_LIMIT_RATE = 1,
        SC_LIMIT_KEY_CONCURRENCY = 2,
        SC_LIMIT_TOTAL_CONCURRENCY = 3,
        SC_LIMIT_KEY_CAPACITY = 4
    };
    typedef struct sc_request_limit_decision
    {
        uint32_t abi_version, struct_size;
        uint32_t reason;
        uint64_t retry_after_ms;
    } sc_request_limit_decision;
    SC_API sc_status sc_request_limiter_options_init(
        sc_request_limiter_options* options, size_t size);
    SC_API sc_status sc_request_limiter_create(
        const sc_request_limiter_options* options, sc_request_limiter** out);
    SC_API void sc_request_limiter_destroy(sc_request_limiter* limiter);
    SC_API void sc_request_limiter_close(sc_request_limiter* limiter);
    /* Success includes a policy denial. A non-NULL permit means admission; otherwise
 * reason describes pressure. Keys are copied only on first admission to the
 * bounded registry. Permit lifetime accounts for real work, even after limiter
 * destruction. Destroying a permit returns concurrency but never rate tokens. */
    SC_API sc_status sc_request_limiter_acquire(sc_request_limiter* limiter, sc_bytes key,
        uint64_t cost, sc_request_limit_decision* decision, sc_request_permit** permit);
    SC_API void sc_request_permit_destroy(sc_request_permit* permit);
    SC_API size_t sc_request_limiter_prune(sc_request_limiter* limiter);
    SC_API size_t sc_request_limiter_key_count(const sc_request_limiter* limiter);
    SC_API size_t sc_request_limiter_active_count(const sc_request_limiter* limiter);
#ifdef __cplusplus
}
#endif
#endif
