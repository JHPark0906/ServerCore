#ifndef SERVERCORE_C_CHANNEL_H
#define SERVERCORE_C_CHANNEL_H
#include "ServerCore/C/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sc_channel sc_channel;
typedef struct sc_latest_value sc_latest_value;
typedef struct sc_channel_value sc_channel_value;
typedef struct sc_channel_options {
    uint32_t abi_version;
    uint32_t struct_size;
    size_t max_messages;
    size_t max_retained_bytes;
} sc_channel_options;

/* Bounded immutable byte messages. Sending copies its input during the call.
 * Queue byte/count budgets stop charging on receive; values retained by callers
 * are outside that queue budget. Empty byte messages are valid. Close rejects
 * sends, preserves queued values, then receive returns Closed after draining.
 * Retain makes an independent handle; destroy closes only the last shared state.
 * Concurrent operations are safe; destroy must not race calls on its handle. */
SC_API sc_status sc_channel_create(const sc_channel_options* options, sc_channel** out);
SC_API sc_status sc_channel_retain(const sc_channel* channel, sc_channel** out);
SC_API void sc_channel_destroy(sc_channel* channel);
SC_API void sc_channel_close(sc_channel* channel);
SC_API sc_status sc_channel_try_send(sc_channel* channel, sc_bytes value);
SC_API sc_status sc_channel_try_receive(sc_channel* channel, sc_channel_value** out);
SC_API size_t sc_channel_size(const sc_channel* channel);
SC_API size_t sc_channel_retained_bytes(const sc_channel* channel);
/* One pending subscription per direction per shared channel. AlreadyExists
 * rejects a second waiter. Readiness is one-shot and advisory; register before
 * checking try_send/try_receive. Destroy subscription to cancel/rearm safely. */
SC_API sc_status sc_channel_subscribe_read(sc_channel* channel, sc_notifier* notifier,
    uint64_t key, sc_subscription** out);
SC_API sc_status sc_channel_subscribe_write(sc_channel* channel, size_t required_bytes,
    sc_notifier* notifier, uint64_t key, sc_subscription** out);

/* Latest-value channel: publication replaces the current snapshot. Version 0
 * means never observed; versions increase without wrapping. ReadAfter does not
 * consume a value and reports WouldBlock for the current version, InvalidArgument
 * for a future version. Close preserves the final unseen snapshot, then Closed.
 * Only the current snapshot counts against max_retained_bytes; caller-owned
 * old snapshots remain valid independently. Up to 64 pending change subscriptions
 * share a state; further subscriptions return WouldBlock. */
SC_API sc_status sc_latest_value_create(size_t max_retained_bytes, sc_latest_value** out);
SC_API sc_status sc_latest_value_retain(const sc_latest_value* value, sc_latest_value** out);
SC_API void sc_latest_value_destroy(sc_latest_value* value);
SC_API void sc_latest_value_close(sc_latest_value* value);
SC_API sc_status sc_latest_value_publish(sc_latest_value* value, sc_bytes bytes);
SC_API sc_status sc_latest_value_read_after(sc_latest_value* value, uint64_t version,
    sc_channel_value** out);
SC_API size_t sc_latest_value_retained_bytes(const sc_latest_value* value);
SC_API sc_status sc_latest_value_subscribe(sc_latest_value* value, uint64_t version,
    sc_notifier* notifier, uint64_t key, sc_subscription** out);

/* View lifetime is exactly that of this immutable owned value, independent of
 * the channel/producer. FIFO values have version 0; latest values carry their
 * publication version. Destroy exactly once; views must not outlive the owner. */
SC_API sc_status sc_channel_value_view(const sc_channel_value* value, sc_bytes* out);
SC_API uint64_t sc_channel_value_version(const sc_channel_value* value);
SC_API void sc_channel_value_destroy(sc_channel_value* value);

#ifdef __cplusplus
}
#endif
#endif
