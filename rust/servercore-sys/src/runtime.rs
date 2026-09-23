use crate::*;
use std::ffi::{c_int, c_void};
macro_rules! handles {
    ($($name:ident),* $(,)?) => { $(#[repr(C)] pub struct $name { _private: [u8; 0] })* };
}
handles!(
    sc_executor,
    sc_keyed_executor,
    sc_timer_scheduler,
    sc_task_group,
    sc_runtime_task,
    sc_cancellation,
    sc_runtime_token,
    sc_group_completions
);
pub const SC_CAP_RUNTIME: u32 = 128;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_runtime_work {
    pub context: *mut c_void,
    pub work: Option<unsafe extern "C" fn(*mut c_void, *const sc_runtime_token) -> sc_status>,
    pub release: Option<unsafe extern "C" fn(*mut c_void)>,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_executor_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub worker_count: usize,
    pub max_pending_tasks: usize,
    pub max_retained_bytes: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_keyed_executor_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub worker_count: usize,
    pub max_keys: usize,
    pub max_outstanding_tasks: usize,
    pub max_retained_bytes: usize,
    pub max_outstanding_tasks_per_key: usize,
    pub max_retained_bytes_per_key: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_task_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub retained_bytes: usize,
    pub parent: *const sc_cancellation,
    pub deadline_ms: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_timer_scheduler_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub max_timers: usize,
    pub max_retained_bytes: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_timer_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub retained_bytes: usize,
    pub parent: *const sc_cancellation,
    pub delay_ms: u32,
    pub repeat_ms: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_task_group_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub max_children: usize,
    pub max_retained_bytes: usize,
    pub parent: *const sc_cancellation,
    pub deadline_ms: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_group_completion {
    pub id: u64,
    pub status: sc_status,
}
extern "C" {
    pub fn sc_executor_options_init(options: *mut sc_executor_options, size: usize) -> sc_status;
    pub fn sc_keyed_executor_options_init(
        options: *mut sc_keyed_executor_options,
        size: usize,
    ) -> sc_status;
    pub fn sc_task_options_init(options: *mut sc_task_options, size: usize) -> sc_status;
    pub fn sc_timer_scheduler_options_init(
        options: *mut sc_timer_scheduler_options,
        size: usize,
    ) -> sc_status;
    pub fn sc_timer_options_init(options: *mut sc_timer_options, size: usize) -> sc_status;
    pub fn sc_task_group_options_init(
        options: *mut sc_task_group_options,
        size: usize,
    ) -> sc_status;
    pub fn sc_cancellation_create(out: *mut *mut sc_cancellation) -> sc_status;
    pub fn sc_cancellation_request(value: *mut sc_cancellation);
    pub fn sc_cancellation_requested(value: *const sc_cancellation) -> c_int;
    pub fn sc_cancellation_destroy(value: *mut sc_cancellation);
    pub fn sc_runtime_token_requested(value: *const sc_runtime_token) -> c_int;
    pub fn sc_runtime_shutdown(timeout_ms: u32) -> sc_status;
    pub fn sc_executor_create(
        options: *const sc_executor_options,
        out: *mut *mut sc_executor,
    ) -> sc_status;
    pub fn sc_executor_submit(
        value: *mut sc_executor,
        work: sc_runtime_work,
        options: *const sc_task_options,
        out: *mut *mut sc_runtime_task,
    ) -> sc_status;
    pub fn sc_executor_request_stop(value: *mut sc_executor);
    pub fn sc_executor_stop(value: *mut sc_executor) -> sc_status;
    pub fn sc_executor_destroy(value: *mut sc_executor);
    pub fn sc_keyed_executor_create(
        options: *const sc_keyed_executor_options,
        out: *mut *mut sc_keyed_executor,
    ) -> sc_status;
    pub fn sc_keyed_executor_submit(
        value: *mut sc_keyed_executor,
        key: u64,
        work: sc_runtime_work,
        options: *const sc_task_options,
        out: *mut *mut sc_runtime_task,
    ) -> sc_status;
    pub fn sc_keyed_executor_request_stop(value: *mut sc_keyed_executor);
    pub fn sc_keyed_executor_stop(value: *mut sc_keyed_executor) -> sc_status;
    pub fn sc_keyed_executor_destroy(value: *mut sc_keyed_executor);
    pub fn sc_timer_scheduler_create(
        executor: *mut sc_executor,
        options: *const sc_timer_scheduler_options,
        out: *mut *mut sc_timer_scheduler,
    ) -> sc_status;
    pub fn sc_timer_scheduler_schedule(
        value: *mut sc_timer_scheduler,
        work: sc_runtime_work,
        options: *const sc_timer_options,
        out: *mut *mut sc_runtime_task,
    ) -> sc_status;
    pub fn sc_timer_scheduler_request_stop(value: *mut sc_timer_scheduler);
    pub fn sc_timer_scheduler_stop(value: *mut sc_timer_scheduler) -> sc_status;
    pub fn sc_timer_scheduler_destroy(value: *mut sc_timer_scheduler);
    pub fn sc_task_group_create(
        executor: *mut sc_executor,
        options: *const sc_task_group_options,
        out: *mut *mut sc_task_group,
    ) -> sc_status;
    pub fn sc_task_group_submit(
        value: *mut sc_task_group,
        work: sc_runtime_work,
        options: *const sc_task_options,
        id: *mut u64,
        out: *mut *mut sc_runtime_task,
    ) -> sc_status;
    pub fn sc_task_group_close(value: *mut sc_task_group);
    pub fn sc_task_group_cancel(value: *mut sc_task_group);
    pub fn sc_task_group_result(value: *const sc_task_group) -> sc_status;
    pub fn sc_task_group_wait(value: *mut sc_task_group, timeout_ms: u32) -> sc_status;
    pub fn sc_task_group_subscribe(
        value: *mut sc_task_group,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_task_group_stop(value: *mut sc_task_group) -> sc_status;
    pub fn sc_task_group_take_completions(
        value: *mut sc_task_group,
        out: *mut *mut sc_group_completions,
    ) -> sc_status;
    pub fn sc_group_completions_data(
        value: *const sc_group_completions,
        count: *mut usize,
    ) -> *const sc_group_completion;
    pub fn sc_group_completions_destroy(value: *mut sc_group_completions);
    pub fn sc_task_group_destroy(value: *mut sc_task_group);
    pub fn sc_runtime_task_cancel(value: *mut sc_runtime_task);
    pub fn sc_runtime_task_result(value: *const sc_runtime_task) -> sc_status;
    pub fn sc_runtime_task_finished(value: *const sc_runtime_task) -> u32;
    pub fn sc_runtime_task_wait(value: *mut sc_runtime_task, timeout_ms: u32) -> sc_status;
    pub fn sc_runtime_task_subscribe(
        value: *mut sc_runtime_task,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_runtime_task_reschedule(value: *mut sc_runtime_task, delay_ms: u32) -> sc_status;
    pub fn sc_runtime_task_destroy(value: *mut sc_runtime_task);
}
