#ifndef SERVERCORE_C_GAME_EXECUTION_H
#define SERVERCORE_C_GAME_EXECUTION_H
#include "ServerCore/C/Net.h"
#include "ServerCore/C/Runtime.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct sc_tick sc_tick;
    typedef struct sc_tick_token sc_tick_token;
    typedef struct sc_outbound_queue sc_outbound_queue;
    enum
    {
        SC_TICK_SKIP = 0,
        SC_TICK_CATCH_UP = 1
    };
    typedef struct sc_tick_options
    {
        uint32_t abi_version, struct_size, interval_ms, lag_policy;
        size_t max_catch_up, retained_bytes;
    } sc_tick_options;
    typedef struct sc_tick_info
    {
        uint64_t index, lateness_ns, skipped;
    } sc_tick_info;
    typedef struct sc_tick_metrics
    {
        uint64_t executed, skipped, last_lateness_ns, max_lateness_ns;
    } sc_tick_metrics;
    typedef sc_status (*sc_tick_work_fn)(void*, const sc_tick_info*, const sc_tick_token*);
    typedef struct sc_tick_work
    {
        void* context;
        sc_tick_work_fn work;
        sc_runtime_release_fn release;
    } sc_tick_work;
    /* Work ownership transfers even on rejection. Callback/release never unwind.
 * Token/info borrow only during callback. Destroy cancels without joining;
 * explicit wait is quiescent and rejects self-wait. Scheduler/executor owners
 * are retained through capture cleanup. Existing runtime shutdown rules apply. */
    SC_API sc_status sc_tick_options_init(sc_tick_options*, size_t);
    SC_API sc_status sc_tick_start(
        sc_timer_scheduler*, const sc_tick_options*, sc_tick_work, sc_tick**);
    SC_API int sc_tick_token_requested(const sc_tick_token*);
    SC_API void sc_tick_cancel(sc_tick*);
    SC_API sc_status sc_tick_result(const sc_tick*);
    SC_API int sc_tick_finished(const sc_tick*);
    SC_API sc_status sc_tick_wait(const sc_tick*, uint32_t timeout_ms);
    SC_API sc_status sc_tick_get_metrics(const sc_tick*, sc_tick_metrics*);
    SC_API sc_status sc_tick_subscribe(const sc_tick*, sc_notifier*, uint64_t, sc_subscription**);
    SC_API void sc_tick_destroy(sc_tick*);
    typedef struct sc_outbound_options
    {
        uint32_t abi_version, struct_size;
        size_t max_messages, max_retained_bytes, max_pump_messages;
    } sc_outbound_options;
    typedef struct sc_outbound_item_options
    {
        uint32_t abi_version, struct_size, has_latest_key, expiry_ms;
        uint64_t latest_key; /* expiry UINT32_MAX: none; 0: expired. */
    } sc_outbound_item_options;
    typedef struct sc_outbound_metrics
    {
        size_t pending, retained_bytes;
        uint64_t enqueued, sent, replaced, expired, discarded;
    } sc_outbound_metrics;
    SC_API sc_status sc_outbound_options_init(sc_outbound_options*, size_t);
    SC_API sc_status sc_outbound_item_options_init(sc_outbound_item_options*, size_t);
    /* Retains TCP public ownership, copies input before return; success is staging
 * admission only. Queue Close/destroy does not undo accepted transport bytes.
 * Scheduler must stay usable; scheduler shutdown can terminate queue progress.
 * One transport capacity wait is exclusively used by this queue while blocked. */
    SC_API sc_status sc_outbound_create(
        sc_timer_scheduler*, sc_tcp_connection*, const sc_outbound_options*, sc_outbound_queue**);
    SC_API sc_status sc_outbound_enqueue(
        sc_outbound_queue*, sc_bytes, const sc_outbound_item_options*);
    /* At most4096 recipients; caller provides count status slots. Copies payload once.
 * Return OK means result slots are populated, not that every recipient accepted.
 * Invalid slot pointers/counts fail before any recipient admission. */
    SC_API sc_status sc_outbound_batch(sc_outbound_queue* const*, size_t, sc_bytes,
        const sc_outbound_item_options*, sc_status* results, size_t result_count);
    SC_API void sc_outbound_begin_drain(sc_outbound_queue*);
    SC_API void sc_outbound_close(sc_outbound_queue*);
    SC_API int sc_outbound_finished(const sc_outbound_queue*);
    SC_API sc_status sc_outbound_result(const sc_outbound_queue*);
    SC_API sc_status sc_outbound_get_metrics(const sc_outbound_queue*, sc_outbound_metrics*);
    SC_API sc_status sc_outbound_subscribe(
        const sc_outbound_queue*, sc_notifier*, uint64_t, sc_subscription**);
    SC_API void sc_outbound_destroy(sc_outbound_queue*);
#ifdef __cplusplus
}
#endif
#endif
