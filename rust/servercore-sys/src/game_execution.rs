use crate::*;
#[repr(C)]
pub struct sc_tick {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_tick_token {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_outbound_queue {
    _private: [u8; 0],
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_tick_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub interval_ms: u32,
    pub lag_policy: u32,
    pub max_catch_up: usize,
    pub retained_bytes: usize,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct sc_tick_info {
    pub index: u64,
    pub lateness_ns: u64,
    pub skipped: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct sc_tick_metrics {
    pub executed: u64,
    pub skipped: u64,
    pub last_lateness_ns: u64,
    pub max_lateness_ns: u64,
}
pub type sc_tick_work_fn = Option<
    unsafe extern "C" fn(
        *mut std::ffi::c_void,
        *const sc_tick_info,
        *const sc_tick_token,
    ) -> sc_status,
>;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_tick_work {
    pub context: *mut std::ffi::c_void,
    pub work: sc_tick_work_fn,
    pub release: Option<unsafe extern "C" fn(*mut std::ffi::c_void)>,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_outbound_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub max_messages: usize,
    pub max_retained_bytes: usize,
    pub max_pump_messages: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_outbound_item_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub has_latest_key: u32,
    pub expiry_ms: u32,
    pub latest_key: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct sc_outbound_metrics {
    pub pending: usize,
    pub retained_bytes: usize,
    pub enqueued: u64,
    pub sent: u64,
    pub replaced: u64,
    pub expired: u64,
    pub discarded: u64,
}
extern "C" {
    pub fn sc_tick_options_init(out: *mut sc_tick_options, size: usize) -> sc_status;
    pub fn sc_tick_start(
        scheduler: *mut sc_timer_scheduler,
        options: *const sc_tick_options,
        work: sc_tick_work,
        out: *mut *mut sc_tick,
    ) -> sc_status;
    pub fn sc_tick_token_requested(token: *const sc_tick_token) -> i32;
    pub fn sc_tick_cancel(tick: *mut sc_tick);
    pub fn sc_tick_result(tick: *const sc_tick) -> sc_status;
    pub fn sc_tick_finished(tick: *const sc_tick) -> i32;
    pub fn sc_tick_wait(tick: *const sc_tick, timeout_ms: u32) -> sc_status;
    pub fn sc_tick_get_metrics(tick: *const sc_tick, out: *mut sc_tick_metrics) -> sc_status;
    pub fn sc_tick_subscribe(
        tick: *const sc_tick,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_tick_destroy(tick: *mut sc_tick);
    pub fn sc_outbound_options_init(out: *mut sc_outbound_options, size: usize) -> sc_status;
    pub fn sc_outbound_item_options_init(
        out: *mut sc_outbound_item_options,
        size: usize,
    ) -> sc_status;
    pub fn sc_outbound_create(
        scheduler: *mut sc_timer_scheduler,
        connection: *mut sc_tcp_connection,
        options: *const sc_outbound_options,
        out: *mut *mut sc_outbound_queue,
    ) -> sc_status;
    pub fn sc_outbound_enqueue(
        queue: *mut sc_outbound_queue,
        bytes: sc_bytes,
        options: *const sc_outbound_item_options,
    ) -> sc_status;
    pub fn sc_outbound_batch(
        recipients: *const *mut sc_outbound_queue,
        count: usize,
        bytes: sc_bytes,
        options: *const sc_outbound_item_options,
        results: *mut sc_status,
        result_count: usize,
    ) -> sc_status;
    pub fn sc_outbound_begin_drain(queue: *mut sc_outbound_queue);
    pub fn sc_outbound_close(queue: *mut sc_outbound_queue);
    pub fn sc_outbound_finished(queue: *const sc_outbound_queue) -> i32;
    pub fn sc_outbound_result(queue: *const sc_outbound_queue) -> sc_status;
    pub fn sc_outbound_get_metrics(
        queue: *const sc_outbound_queue,
        out: *mut sc_outbound_metrics,
    ) -> sc_status;
    pub fn sc_outbound_subscribe(
        queue: *const sc_outbound_queue,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_outbound_destroy(queue: *mut sc_outbound_queue);
}
