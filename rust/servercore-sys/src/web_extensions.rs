use crate::*;
#[repr(C)]
pub struct sc_http_body {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_body_chunk {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_request_decision {
    _private: [u8; 0],
}
pub const SC_WEB_POLICY: u32 = 3;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_body_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub max_buffered_bytes: usize,
    pub max_body_bytes: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_headers_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub headers: *const sc_header,
    pub count: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_policy_allow {
    pub abi_version: u32,
    pub struct_size: u32,
    pub response_headers: *const sc_header,
    pub response_header_count: usize,
    pub attributes: *const sc_header,
    pub attribute_count: usize,
}
extern "C" {
    pub fn sc_body_options_init(options: *mut sc_body_options, size: usize) -> sc_status;
    pub fn sc_web_server_set_body_limits(
        server: *mut sc_web_server,
        options: *const sc_body_options,
    ) -> sc_status;
    pub fn sc_web_server_stream_route(
        server: *mut sc_web_server,
        method: sc_bytes,
        path: sc_bytes,
        pattern: u32,
    ) -> sc_status;
    pub fn sc_web_server_enable_policy(server: *mut sc_web_server) -> sc_status;
    pub fn sc_web_server_websocket_policy(
        server: *mut sc_web_server,
        path: sc_bytes,
        pattern: u32,
    ) -> sc_status;
    pub fn sc_web_server_begin_drain(server: *mut sc_web_server) -> sc_status;
    pub fn sc_web_server_drain_status(server: *const sc_web_server) -> sc_status;
    pub fn sc_web_server_stop_gracefully(server: *mut sc_web_server, timeout_ms: u32) -> sc_status;
    pub fn sc_web_event_attributes(
        event: *const sc_web_event,
        view: *mut sc_headers_view,
    ) -> sc_status;
    pub fn sc_web_event_body(event: *mut sc_web_event, out: *mut *mut sc_http_body) -> sc_status;
    pub fn sc_web_event_decision(
        event: *mut sc_web_event,
        out: *mut *mut sc_request_decision,
    ) -> sc_status;
    pub fn sc_web_event_policy_is_websocket(event: *const sc_web_event) -> u32;
    pub fn sc_request_decision_allow(
        decision: *mut sc_request_decision,
        options: *const sc_policy_allow,
    ) -> sc_status;
    pub fn sc_request_decision_reject(
        decision: *mut sc_request_decision,
        head: *const sc_response_head,
        body: sc_bytes,
    ) -> sc_status;
    pub fn sc_request_decision_cancelled(decision: *const sc_request_decision) -> u32;
    pub fn sc_request_decision_abort(decision: *mut sc_request_decision);
    pub fn sc_request_decision_destroy(decision: *mut sc_request_decision);
    pub fn sc_http_body_read(body: *mut sc_http_body, out: *mut *mut sc_body_chunk) -> sc_status;
    pub fn sc_http_body_retained_bytes(body: *const sc_http_body) -> usize;
    pub fn sc_http_body_cancelled(body: *const sc_http_body) -> u32;
    pub fn sc_http_body_cancel(body: *mut sc_http_body);
    pub fn sc_http_body_destroy(body: *mut sc_http_body);
    pub fn sc_body_chunk_view(chunk: *const sc_body_chunk) -> sc_bytes;
    pub fn sc_body_chunk_destroy(chunk: *mut sc_body_chunk);
}
