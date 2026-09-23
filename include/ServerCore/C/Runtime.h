#ifndef SERVERCORE_C_RUNTIME_H
#define SERVERCORE_C_RUNTIME_H
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sc_executor sc_executor;
typedef struct sc_keyed_executor sc_keyed_executor;
typedef struct sc_timer_scheduler sc_timer_scheduler;
typedef struct sc_task_group sc_task_group;
typedef struct sc_runtime_task sc_runtime_task;
typedef struct sc_cancellation sc_cancellation;
typedef struct sc_runtime_token sc_runtime_token;
typedef struct sc_group_completions sc_group_completions;

/* Runtime callbacks explicitly run foreign work on native workers. work and
 * release must not throw/unwind across C, block indefinitely, or call a wait/
 * stop requiring their own worker. token is borrowed only during work. Context
 * ownership transfers on EVERY submit/schedule call, including rejection:
 * release(context) runs exactly once when non-NULL. Retained bytes are supplied
 * by the caller and must include dynamically owned callback data.
 * Last owner-state release requests cancellation and defers quiescent
 * destruction to one shared cleanup worker. This permits last-owner release
 * in its own callback. Schedulers/groups retain their bound executor state;
 * destroying the original executor handle preserves those dependents.
 * Stop explicitly joins; unfinished self-joins return InvalidArgument. There
 * are at most 256 live/retiring executor/scheduler/group owner states per DLL.
 * Task handles are observations: destroying one does not cancel work.
 * Before unloading the DLL, destroy every runtime owner, call
 * sc_runtime_shutdown from a control thread, and release all remaining task,
 * cancellation, completion and subscription handles. No cleanup runs in DLL
 * detach; no handle or active ABI call may outlive the loaded module. */
typedef sc_status (*sc_runtime_work_fn)(void* context, const sc_runtime_token* token);
typedef void (*sc_runtime_release_fn)(void* context);
typedef struct sc_runtime_work {
    void* context; sc_runtime_work_fn work; sc_runtime_release_fn release;
} sc_runtime_work;
typedef struct sc_executor_options {
    uint32_t abi_version, struct_size;
    size_t worker_count, max_pending_tasks, max_retained_bytes;
} sc_executor_options;
typedef struct sc_keyed_executor_options {
    uint32_t abi_version, struct_size;
    size_t worker_count, max_keys, max_outstanding_tasks, max_retained_bytes;
    size_t max_outstanding_tasks_per_key, max_retained_bytes_per_key;
} sc_keyed_executor_options;
typedef struct sc_task_options {
    uint32_t abi_version, struct_size;
    size_t retained_bytes;
    const sc_cancellation* parent;
    uint32_t deadline_ms; /* UINT32_MAX: no deadline; 0: already expired. */
} sc_task_options;
typedef struct sc_timer_scheduler_options {
    uint32_t abi_version, struct_size;
    size_t max_timers, max_retained_bytes;
} sc_timer_scheduler_options;
typedef struct sc_timer_options {
    uint32_t abi_version, struct_size;
    size_t retained_bytes;
    const sc_cancellation* parent;
    uint32_t delay_ms, repeat_ms; /* repeat 0: one shot, otherwise fixed delay. */
} sc_timer_options;
typedef struct sc_task_group_options {
    uint32_t abi_version, struct_size;
    size_t max_children, max_retained_bytes;
    const sc_cancellation* parent;
    uint32_t deadline_ms;
} sc_task_group_options;
typedef struct sc_group_completion { uint64_t id; sc_status status; } sc_group_completion;
SC_API sc_status sc_executor_options_init(sc_executor_options*, size_t);
SC_API sc_status sc_keyed_executor_options_init(sc_keyed_executor_options*, size_t);
SC_API sc_status sc_task_options_init(sc_task_options*, size_t);
SC_API sc_status sc_timer_scheduler_options_init(sc_timer_scheduler_options*, size_t);
SC_API sc_status sc_timer_options_init(sc_timer_options*, size_t);
SC_API sc_status sc_task_group_options_init(sc_task_group_options*, size_t);
SC_API sc_status sc_cancellation_create(sc_cancellation**);
SC_API void sc_cancellation_request(sc_cancellation*);
SC_API int sc_cancellation_requested(const sc_cancellation*);
SC_API void sc_cancellation_destroy(sc_cancellation*);
SC_API int sc_runtime_token_requested(const sc_runtime_token*);
/* Permanently closes runtime owner admission, then joins deferred cleanup.
 * Destroy owners before calling. 0 polls (WouldBlock), UINT32_MAX waits.
 * Positive timeout returns Timeout and permits a later retry. Calls from any
 * runtime work/release callback or cleanup worker return InvalidArgument. */
SC_API sc_status sc_runtime_shutdown(uint32_t timeout_ms);
SC_API sc_status sc_executor_create(const sc_executor_options*, sc_executor**);
SC_API sc_status sc_executor_submit(sc_executor*, sc_runtime_work, const sc_task_options*, sc_runtime_task**);
SC_API void sc_executor_request_stop(sc_executor*);
SC_API sc_status sc_executor_stop(sc_executor*);
SC_API void sc_executor_destroy(sc_executor*);
SC_API sc_status sc_keyed_executor_create(const sc_keyed_executor_options*, sc_keyed_executor**);
SC_API sc_status sc_keyed_executor_submit(sc_keyed_executor*, uint64_t key, sc_runtime_work, const sc_task_options*, sc_runtime_task**);
SC_API void sc_keyed_executor_request_stop(sc_keyed_executor*);
SC_API sc_status sc_keyed_executor_stop(sc_keyed_executor*);
SC_API void sc_keyed_executor_destroy(sc_keyed_executor*);
SC_API sc_status sc_timer_scheduler_create(sc_executor*, const sc_timer_scheduler_options*, sc_timer_scheduler**);
SC_API sc_status sc_timer_scheduler_schedule(sc_timer_scheduler*, sc_runtime_work, const sc_timer_options*, sc_runtime_task**);
SC_API void sc_timer_scheduler_request_stop(sc_timer_scheduler*);
SC_API sc_status sc_timer_scheduler_stop(sc_timer_scheduler*);
SC_API void sc_timer_scheduler_destroy(sc_timer_scheduler*);
SC_API sc_status sc_task_group_create(sc_executor*, const sc_task_group_options*, sc_task_group**);
SC_API sc_status sc_task_group_submit(sc_task_group*, sc_runtime_work, const sc_task_options*, uint64_t* id, sc_runtime_task**);
SC_API void sc_task_group_close(sc_task_group*);
SC_API void sc_task_group_cancel(sc_task_group*);
SC_API sc_status sc_task_group_result(const sc_task_group*);
/* wait and subscribe close admission. Timeout does not cancel children. */
SC_API sc_status sc_task_group_wait(sc_task_group*, uint32_t timeout_ms);
SC_API sc_status sc_task_group_subscribe(sc_task_group*, sc_notifier*, uint64_t key, sc_subscription**);
SC_API sc_status sc_task_group_stop(sc_task_group*);
SC_API sc_status sc_task_group_take_completions(sc_task_group*, sc_group_completions**);
SC_API const sc_group_completion* sc_group_completions_data(const sc_group_completions*, size_t* count);
SC_API void sc_group_completions_destroy(sc_group_completions*);
SC_API void sc_task_group_destroy(sc_task_group*);
SC_API void sc_runtime_task_cancel(sc_runtime_task*);
SC_API sc_status sc_runtime_task_result(const sc_runtime_task*);
/* Distinguishes pending from a callback's terminal WouldBlock result. */
SC_API uint32_t sc_runtime_task_finished(const sc_runtime_task*);
SC_API sc_status sc_runtime_task_wait(sc_runtime_task*, uint32_t timeout_ms);
SC_API sc_status sc_runtime_task_subscribe(sc_runtime_task*, sc_notifier*, uint64_t key, sc_subscription**);
/* Only timer handles support reschedule; other tasks return InvalidArgument. */
SC_API sc_status sc_runtime_task_reschedule(sc_runtime_task*, uint32_t delay_ms);
SC_API void sc_runtime_task_destroy(sc_runtime_task*);
#ifdef __cplusplus
}
#endif
#endif
