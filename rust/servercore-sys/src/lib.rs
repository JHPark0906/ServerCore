//! Raw ServerCore ABI v2. Every pointer/length and ownership rule in the C
//! headers is the caller's responsibility; prefer the `servercore` crate.
#![allow(non_camel_case_types)]

use std::ffi::c_char;
mod channel;
pub mod binary;
pub mod files;
pub mod datagram;
pub mod game_execution;
pub use datagram::*;
pub use game_execution::*;
mod observability;
mod readiness;
mod runtime;
pub mod request_limiter;
mod endpoint;
pub mod web_data;
pub mod web_policy;
pub use web_policy::*;
pub use endpoint::*;
pub use web_data::*;
mod web_extensions;
pub use channel::*;
pub use observability::*;
pub use readiness::*;
pub use runtime::*;
pub use web_extensions::*;
pub const SC_ABI_VERSION: u32 = 2;
pub type sc_status = i32;
pub const SC_OK: sc_status = 0;
pub const SC_INVALID_ARGUMENT: sc_status = 1;
pub const SC_INVALID_FORMAT: sc_status = 2;
pub const SC_TOO_LARGE: sc_status = 3;
pub const SC_NOT_FOUND: sc_status = 4;
pub const SC_ALREADY_EXISTS: sc_status = 5;
pub const SC_CLOSED: sc_status = 6;
pub const SC_WOULD_BLOCK: sc_status = 7;
pub const SC_PLATFORM_ERROR: sc_status = 8;
pub const SC_UNIMPLEMENTED: sc_status = 9;
pub const SC_UNKNOWN_TYPE: sc_status = 10;
pub const SC_TIMEOUT: sc_status = 11;
pub const SC_CANCELLED: sc_status = 12;
pub const SC_CAP_WEB: u32 = 1;
pub const SC_CAP_TCP: u32 = 2;
pub const SC_CAP_ENDPOINT: u32 = 256;
pub const SC_CAP_REQUEST_LIMITER: u32 = 512;
pub const SC_CAP_WEB_POLICIES: u32 = 1024;
pub const SC_CAP_WEB_DATA: u32 = 2048;
pub const SC_CAP_WS_EXTENSIONS: u32 = 4096;
pub const SC_CAP_GAME_EXECUTION: u32 = 8192;
pub const SC_CAP_BINARY_IO: u32 = 16384;
pub const SC_CAP_DATAGRAM: u32 = 32768;
pub const SC_CAP_ATOMIC_FILE: u32 = 65536;
pub const SC_CAP_OPERATIONS: u32 = 131072;
// Capability value 4 is reserved; existing capability values must not shift.

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct sc_bytes {
    pub data: *const u8,
    pub len: usize,
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct sc_header {
    pub name: sc_bytes,
    pub value: sc_bytes,
}

macro_rules! opaque {
    ($($name:ident),* $(,)?) => { $(#[repr(C)] pub struct $name { _private: [u8; 0] })* };
}
opaque!(
    sc_wait,
    sc_web_server,
    sc_web_event,
    sc_http_response,
    sc_websocket,
    sc_websocket_event,
    sc_tcp_server,
    sc_tcp_connection,
    sc_tcp_event
);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_web_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub listen_address: sc_bytes,
    pub port: u16,
    pub reserved: u16,
    pub io_threads: u32,
    pub handler_threads: u32,
    pub max_connections: usize,
    pub max_header_bytes: usize,
    pub max_body_bytes: usize,
    pub max_buffered_response_bytes: usize,
    pub max_active_requests: usize,
    pub max_request_bytes: usize,
    pub max_event_count: usize,
    pub max_event_bytes: usize,
    pub max_ws_event_count: usize,
    pub max_ws_event_bytes: usize,
    pub max_send_bytes: usize,
    pub max_chunk_bytes: usize,
    pub handler_timeout_ms: u32,
    pub stream_idle_timeout_ms: u32,
    pub send_stall_timeout_ms: u32,
    pub reserved2: u32,
    pub max_ws_connection_event_count: usize,
    pub max_ws_connection_event_bytes: usize,
}
// ABI 2 layout of Web.h on 64-bit targets.
#[cfg(target_pointer_width = "64")]
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<sc_web_options>() == 168);
    assert!(offset_of!(sc_web_options, max_chunk_bytes) == 128);
    assert!(offset_of!(sc_web_options, send_stall_timeout_ms) == 144);
    assert!(offset_of!(sc_web_options, reserved2) == 148);
    assert!(offset_of!(sc_web_options, max_ws_connection_event_count) == 152);
    assert!(offset_of!(sc_web_options, max_ws_connection_event_bytes) == 160);
};
pub const SC_WEB_REQUEST: u32 = 1;
pub const SC_WEB_WEBSOCKET: u32 = 2;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_request_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub method: sc_bytes,
    pub target: sc_bytes,
    pub body: sc_bytes,
    pub headers: *const sc_header,
    pub header_count: usize,
    pub parameters: *const sc_header,
    pub parameter_count: usize,
}
pub const SC_RESPONSE_CLOSE: u32 = 1;
pub const SC_RESPONSE_HAS_LENGTH: u32 = 2;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_response_head {
    pub abi_version: u32,
    pub struct_size: u32,
    pub status: u32,
    pub flags: u32,
    pub content_length: u64,
    pub headers: *const sc_header,
    pub header_count: usize,
}
pub const SC_WS_TEXT: u32 = 1;
pub const SC_WS_BINARY: u32 = 2;
pub const SC_WS_CLOSED: u32 = 3;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_websocket_event_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub kind: u32,
    pub close_code: u16,
    pub reserved: u16,
    pub bytes: sc_bytes,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_tcp_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub listen_address: sc_bytes,
    pub port: u16,
    pub reserved: u16,
    pub io_threads: u32,
    pub max_connections: usize,
    pub max_event_count: usize,
    pub max_event_bytes: usize,
    pub connection_send_bytes: usize,
    pub total_send_bytes: usize,
    pub max_connection_event_count: usize,
    pub max_connection_event_bytes: usize,
}
// ABI 2 layout of Net.h on 64-bit targets.
#[cfg(target_pointer_width = "64")]
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<sc_tcp_options>() == 88);
    assert!(offset_of!(sc_tcp_options, max_connection_event_count) == 72);
    assert!(offset_of!(sc_tcp_options, max_connection_event_bytes) == 80);
};
pub const SC_TCP_BYTES: u32 = 1;
pub const SC_TCP_CLOSED: u32 = 2;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_tcp_event_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub kind: u32,
    pub status: sc_status,
    pub bytes: sc_bytes,
}

extern "C" {
    pub fn sc_abi_version() -> u32;
    pub fn sc_capabilities() -> u32;
    pub fn sc_status_name(status: sc_status) -> *const c_char;
    pub fn sc_wait_result(wait: *const sc_wait) -> sc_status;
    pub fn sc_wait_wait(wait: *mut sc_wait, timeout_ms: u32) -> sc_status;
    pub fn sc_wait_cancel(wait: *mut sc_wait);
    pub fn sc_wait_destroy(wait: *mut sc_wait);
    pub fn sc_web_options_init(options: *mut sc_web_options, size: usize) -> sc_status;
    pub fn sc_response_head_init(head: *mut sc_response_head, size: usize) -> sc_status;
    pub fn sc_web_server_create(
        options: *const sc_web_options,
        out: *mut *mut sc_web_server,
    ) -> sc_status;
    pub fn sc_web_server_route(
        server: *mut sc_web_server,
        method: sc_bytes,
        path: sc_bytes,
        pattern: u32,
    ) -> sc_status;
    pub fn sc_web_server_websocket(
        server: *mut sc_web_server,
        path: sc_bytes,
        pattern: u32,
    ) -> sc_status;
    pub fn sc_web_server_start(server: *mut sc_web_server) -> sc_status;
    pub fn sc_web_server_port(server: *const sc_web_server) -> u16;
    pub fn sc_web_server_next(
        server: *mut sc_web_server,
        timeout_ms: u32,
        out: *mut *mut sc_web_event,
    ) -> sc_status;
    pub fn sc_web_server_stop(server: *mut sc_web_server) -> sc_status;
    pub fn sc_web_server_destroy(server: *mut sc_web_server);
    pub fn sc_web_event_kind(event: *const sc_web_event) -> u32;
    pub fn sc_web_event_request(
        event: *const sc_web_event,
        view: *mut sc_request_view,
    ) -> sc_status;
    pub fn sc_web_event_response(
        event: *mut sc_web_event,
        out: *mut *mut sc_http_response,
    ) -> sc_status;
    pub fn sc_web_event_socket(event: *mut sc_web_event, out: *mut *mut sc_websocket) -> sc_status;
    pub fn sc_web_event_destroy(event: *mut sc_web_event);
    pub fn sc_http_response_retain(
        response: *const sc_http_response,
        out: *mut *mut sc_http_response,
    ) -> sc_status;
    pub fn sc_http_response_start(
        response: *mut sc_http_response,
        head: *const sc_response_head,
    ) -> sc_status;
    pub fn sc_http_response_write(response: *mut sc_http_response, bytes: sc_bytes) -> sc_status;
    pub fn sc_http_response_finish(response: *mut sc_http_response) -> sc_status;
    pub fn sc_http_response_complete(
        response: *mut sc_http_response,
        head: *const sc_response_head,
        body: sc_bytes,
    ) -> sc_status;
    pub fn sc_http_response_wait_capacity(
        response: *mut sc_http_response,
        bytes: usize,
        out: *mut *mut sc_wait,
    ) -> sc_status;
    pub fn sc_http_response_cancelled(response: *const sc_http_response) -> u32;
    pub fn sc_http_response_is_head(response: *const sc_http_response) -> u32;
    pub fn sc_http_response_max_write(response: *const sc_http_response) -> usize;
    pub fn sc_http_response_abort(response: *mut sc_http_response);
    pub fn sc_http_response_destroy(response: *mut sc_http_response);
    pub fn sc_websocket_retain(
        socket: *const sc_websocket,
        out: *mut *mut sc_websocket,
    ) -> sc_status;
    pub fn sc_websocket_id(socket: *const sc_websocket) -> u64;
    pub fn sc_websocket_send(socket: *mut sc_websocket, kind: u32, bytes: sc_bytes) -> sc_status;
    pub fn sc_websocket_close(socket: *mut sc_websocket, code: u16, reason: sc_bytes) -> sc_status;
    pub fn sc_websocket_next(
        socket: *mut sc_websocket,
        timeout_ms: u32,
        out: *mut *mut sc_websocket_event,
    ) -> sc_status;
    pub fn sc_websocket_wait_capacity(
        socket: *mut sc_websocket,
        bytes: usize,
        out: *mut *mut sc_wait,
    ) -> sc_status;
    pub fn sc_websocket_event_get_view(
        event: *const sc_websocket_event,
        view: *mut sc_websocket_event_view,
    ) -> sc_status;
    pub fn sc_websocket_event_destroy(event: *mut sc_websocket_event);
    pub fn sc_websocket_destroy(socket: *mut sc_websocket);
    pub fn sc_tcp_options_init(options: *mut sc_tcp_options, size: usize) -> sc_status;
    pub fn sc_tcp_server_create(
        options: *const sc_tcp_options,
        out: *mut *mut sc_tcp_server,
    ) -> sc_status;
    pub fn sc_tcp_server_start(server: *mut sc_tcp_server) -> sc_status;
    pub fn sc_tcp_server_port(server: *const sc_tcp_server) -> u16;
    pub fn sc_tcp_server_accept(
        server: *mut sc_tcp_server,
        timeout_ms: u32,
        out: *mut *mut sc_tcp_connection,
    ) -> sc_status;
    pub fn sc_tcp_server_stop(server: *mut sc_tcp_server) -> sc_status;
    pub fn sc_tcp_server_destroy(server: *mut sc_tcp_server);
    pub fn sc_tcp_connection_retain(
        connection: *const sc_tcp_connection,
        out: *mut *mut sc_tcp_connection,
    ) -> sc_status;
    pub fn sc_tcp_connection_id(connection: *const sc_tcp_connection) -> u64;
    pub fn sc_tcp_connection_send(connection: *mut sc_tcp_connection, bytes: sc_bytes)
        -> sc_status;
    pub fn sc_tcp_connection_next(
        connection: *mut sc_tcp_connection,
        timeout_ms: u32,
        out: *mut *mut sc_tcp_event,
    ) -> sc_status;
    pub fn sc_tcp_connection_pause(connection: *mut sc_tcp_connection) -> sc_status;
    pub fn sc_tcp_connection_resume(connection: *mut sc_tcp_connection) -> sc_status;
    pub fn sc_tcp_connection_wait_capacity(
        connection: *mut sc_tcp_connection,
        bytes: usize,
        out: *mut *mut sc_wait,
    ) -> sc_status;
    pub fn sc_tcp_connection_close(connection: *mut sc_tcp_connection);
    pub fn sc_tcp_event_get_view(
        event: *const sc_tcp_event,
        view: *mut sc_tcp_event_view,
    ) -> sc_status;
    pub fn sc_tcp_event_destroy(event: *mut sc_tcp_event);
    pub fn sc_tcp_connection_destroy(connection: *mut sc_tcp_connection);
}
