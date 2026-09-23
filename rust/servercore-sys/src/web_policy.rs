use crate::*;
macro_rules! handles {
    ($($name:ident),* $(,)?) => { $(#[repr(C)] pub struct $name { _private: [u8; 0] })* };
}
handles!(
    sc_http_policy,
    sc_http_policy_result,
    sc_trusted_proxy,
    sc_proxy_peer
);
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_http_policy_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub max_rules: usize,
    pub max_metadata_bytes: usize,
    pub max_response_body_bytes: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_cors_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub origins: *const sc_bytes,
    pub origin_count: usize,
    pub methods: *const sc_bytes,
    pub method_count: usize,
    pub allowed_headers: *const sc_bytes,
    pub allowed_header_count: usize,
    pub exposed_headers: *const sc_bytes,
    pub exposed_header_count: usize,
    pub allow_credentials: u32,
    pub max_age_seconds: u32,
}
pub const SC_PROXY_FORWARDED: u32 = 0;
pub const SC_PROXY_X_FORWARDED: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_proxy_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub trusted_cidrs: *const sc_bytes,
    pub trusted_cidr_count: usize,
    pub mode: u32,
    pub accept_proto: u32,
    pub accept_host: u32,
    pub normalize_mapped_ipv4: u32,
    pub max_hops: usize,
    pub max_header_bytes: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_proxy_peer_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub transport_peer: sc_ip_endpoint,
    pub client: sc_ip_endpoint,
    pub accepted_hops: usize,
    pub proto: sc_bytes,
    pub host: sc_bytes,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_http_policy_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub has_response: u32,
    pub status: u32,
    pub close: u32,
    pub response_headers: *const sc_header,
    pub response_header_count: usize,
    pub attributes: *const sc_header,
    pub attribute_count: usize,
    pub body: sc_bytes,
}
extern "C" {
    pub fn sc_http_policy_options_init(
        options: *mut sc_http_policy_options,
        size: usize,
    ) -> sc_status;
    pub fn sc_cors_options_init(options: *mut sc_cors_options, size: usize) -> sc_status;
    pub fn sc_proxy_options_init(options: *mut sc_proxy_options, size: usize) -> sc_status;
    pub fn sc_trusted_proxy_create(
        options: *const sc_proxy_options,
        out: *mut *mut sc_trusted_proxy,
    ) -> sc_status;
    pub fn sc_trusted_proxy_destroy(proxy: *mut sc_trusted_proxy);
    pub fn sc_trusted_proxy_resolve(
        proxy: *const sc_trusted_proxy,
        peer: *const sc_ip_endpoint,
        headers: *const sc_header,
        count: usize,
        out: *mut *mut sc_proxy_peer,
    ) -> sc_status;
    pub fn sc_proxy_peer_get(
        peer: *const sc_proxy_peer,
        view: *mut sc_proxy_peer_view,
    ) -> sc_status;
    pub fn sc_proxy_peer_destroy(peer: *mut sc_proxy_peer);
    pub fn sc_http_policy_create(
        options: *const sc_http_policy_options,
        out: *mut *mut sc_http_policy,
    ) -> sc_status;
    pub fn sc_http_policy_add_headers(
        policy: *mut sc_http_policy,
        prefix: sc_bytes,
        headers: *const sc_header,
        count: usize,
    ) -> sc_status;
    pub fn sc_http_policy_add_cors(
        policy: *mut sc_http_policy,
        prefix: sc_bytes,
        options: *const sc_cors_options,
    ) -> sc_status;
    pub fn sc_http_policy_add_request_id(
        policy: *mut sc_http_policy,
        prefix: sc_bytes,
        header: sc_bytes,
        accept_incoming: u32,
        max_bytes: usize,
    ) -> sc_status;
    pub fn sc_http_policy_add_proxy(
        policy: *mut sc_http_policy,
        prefix: sc_bytes,
        proxy: *const sc_trusted_proxy,
    ) -> sc_status;
    pub fn sc_http_policy_evaluate(
        policy: *mut sc_http_policy,
        request: *const sc_request_view,
        peer: *const sc_ip_endpoint,
        websocket_upgrade: u32,
        out: *mut *mut sc_http_policy_result,
    ) -> sc_status;
    pub fn sc_http_policy_evaluate_event(
        policy: *mut sc_http_policy,
        event: *const sc_web_event,
        out: *mut *mut sc_http_policy_result,
    ) -> sc_status;
    pub fn sc_http_policy_result_get(
        result: *const sc_http_policy_result,
        view: *mut sc_http_policy_view,
    ) -> sc_status;
    pub fn sc_http_policy_result_apply(
        result: *const sc_http_policy_result,
        decision: *mut sc_request_decision,
    ) -> sc_status;
    pub fn sc_http_policy_result_destroy(result: *mut sc_http_policy_result);
    pub fn sc_http_policy_destroy(policy: *mut sc_http_policy);
    pub fn sc_http_json_response(
        json: sc_bytes,
        status: u32,
        max_bytes: usize,
        out: *mut *mut sc_http_policy_result,
    ) -> sc_status;
    pub fn sc_http_error_response(
        error: sc_status,
        request_id: sc_bytes,
        status: u32,
        out: *mut *mut sc_http_policy_result,
    ) -> sc_status;
}
