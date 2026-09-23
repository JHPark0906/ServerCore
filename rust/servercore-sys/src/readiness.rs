use crate::*;
#[repr(C)]
pub struct sc_notifier {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_subscription {
    _private: [u8; 0],
}
pub const SC_CAP_READINESS: u32 = 32;
extern "C" {
    pub fn sc_notifier_create(capacity: usize, out: *mut *mut sc_notifier) -> sc_status;
    pub fn sc_notifier_next(
        notifier: *mut sc_notifier,
        timeout_ms: u32,
        key: *mut u64,
    ) -> sc_status;
    pub fn sc_notifier_interrupt(notifier: *mut sc_notifier);
    pub fn sc_notifier_close(notifier: *mut sc_notifier);
    pub fn sc_notifier_destroy(notifier: *mut sc_notifier);
    pub fn sc_subscription_destroy(subscription: *mut sc_subscription);
    pub fn sc_wait_subscribe(
        wait: *mut sc_wait,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_web_server_subscribe(
        server: *mut sc_web_server,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_web_server_subscribe_drain(
        server: *mut sc_web_server,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_http_body_subscribe(
        body: *mut sc_http_body,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_http_response_subscribe_cancelled(
        response: *mut sc_http_response,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_request_decision_subscribe_cancelled(
        decision: *mut sc_request_decision,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_websocket_subscribe(
        socket: *mut sc_websocket,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_tcp_server_subscribe(
        server: *mut sc_tcp_server,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_tcp_connection_subscribe(
        connection: *mut sc_tcp_connection,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
}
