use crate::*;
#[repr(C)]
pub struct sc_logger {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_owned_text {
    _private: [u8; 0],
}
pub const SC_CAP_WEB_EXTENSIONS: u32 = 8;
pub const SC_CAP_OBSERVABILITY: u32 = 16;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_logger_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub minimum_level: u32,
    pub console: u32,
    pub file: sc_bytes,
    pub max_queued_messages: usize,
    pub max_retained_bytes: usize,
    pub max_message_bytes: usize,
    pub max_file_bytes: u64,
    pub retained_files: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sc_logger_metrics {
    pub abi_version: u32,
    pub struct_size: u32,
    pub pending_messages: usize,
    pub retained_bytes: usize,
    pub accepted_messages: u64,
    pub written_messages: u64,
    pub filtered_messages: u64,
    pub dropped_messages: u64,
    pub output_errors: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sc_task_metrics {
    pub pending_tasks: usize,
    pub running_tasks: usize,
    pub retained_bytes: usize,
    pub accepted_tasks: u64,
    pub completed_tasks: u64,
    pub failed_tasks: u64,
    pub cancelled_tasks: u64,
    pub timed_out_tasks: u64,
    pub rejected_tasks: u64,
    pub total_latency_nanoseconds: u64,
    pub max_latency_nanoseconds: u64,
    pub latency_buckets: [u64; 12],
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sc_web_metrics {
    pub abi_version: u32,
    pub struct_size: u32,
    pub active_connections: usize,
    pub active_requests: usize,
    pub retained_request_bytes: usize,
    pub retained_send_bytes: usize,
    pub pending_trace_events: usize,
    pub accepted_connections: u64,
    pub closed_connections: u64,
    pub rejected_connections: u64,
    pub accepted_requests: u64,
    pub completed_requests: u64,
    pub failed_requests: u64,
    pub cancelled_requests: u64,
    pub timed_out_requests: u64,
    pub rejected_requests: u64,
    pub protocol_errors: u64,
    pub dropped_trace_events: u64,
    pub trace_callback_errors: u64,
    pub total_latency_nanoseconds: u64,
    pub max_latency_nanoseconds: u64,
    pub latency_buckets: [u64; 12],
    pub handlers: sc_task_metrics,
}
extern "C" {
    pub fn sc_logger_options_init(options: *mut sc_logger_options, size: usize) -> sc_status;
    pub fn sc_logger_create(
        options: *const sc_logger_options,
        out: *mut *mut sc_logger,
    ) -> sc_status;
    pub fn sc_logger_try_write(logger: *mut sc_logger, level: u32, message: sc_bytes) -> sc_status;
    pub fn sc_logger_set_minimum_level(logger: *mut sc_logger, level: u32) -> sc_status;
    pub fn sc_logger_get_metrics(
        logger: *const sc_logger,
        out: *mut sc_logger_metrics,
    ) -> sc_status;
    pub fn sc_logger_request_stop(logger: *mut sc_logger);
    pub fn sc_logger_stop(logger: *mut sc_logger) -> sc_status;
    pub fn sc_logger_destroy(logger: *mut sc_logger);
    pub fn sc_web_server_set_logger(
        server: *mut sc_web_server,
        logger: *const sc_logger,
    ) -> sc_status;
    pub fn sc_web_server_get_metrics(
        server: *const sc_web_server,
        out: *mut sc_web_metrics,
    ) -> sc_status;
    pub fn sc_web_server_metrics_prometheus(
        server: *const sc_web_server,
        out: *mut *mut sc_owned_text,
    ) -> sc_status;
    pub fn sc_owned_text_view(text: *const sc_owned_text) -> sc_bytes;
    pub fn sc_owned_text_destroy(text: *mut sc_owned_text);
}
