use crate::{sc_bytes, sc_status};
#[repr(C)]
pub struct sc_request_limiter {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_request_permit {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_request_limiter_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub max_keys: usize,
    pub max_key_bytes: usize,
    pub max_retained_key_bytes: usize,
    pub refill_tokens: u64,
    pub burst_tokens: u64,
    pub refill_interval_ms: u32,
    pub max_concurrent_per_key: usize,
    pub max_concurrent_total: usize,
    pub idle_expiry_ms: u32,
}
#[repr(C)]
pub struct sc_request_limit_decision {
    pub abi_version: u32,
    pub struct_size: u32,
    pub reason: u32,
    pub retry_after_ms: u64,
}
extern "C" {
    pub fn sc_request_limiter_options_init(
        options: *mut sc_request_limiter_options,
        size: usize,
    ) -> sc_status;
    pub fn sc_request_limiter_create(
        options: *const sc_request_limiter_options,
        out: *mut *mut sc_request_limiter,
    ) -> sc_status;
    pub fn sc_request_limiter_destroy(limiter: *mut sc_request_limiter);
    pub fn sc_request_limiter_close(limiter: *mut sc_request_limiter);
    pub fn sc_request_limiter_acquire(
        limiter: *mut sc_request_limiter,
        key: sc_bytes,
        cost: u64,
        decision: *mut sc_request_limit_decision,
        permit: *mut *mut sc_request_permit,
    ) -> sc_status;
    pub fn sc_request_permit_destroy(permit: *mut sc_request_permit);
    pub fn sc_request_limiter_prune(limiter: *mut sc_request_limiter) -> usize;
    pub fn sc_request_limiter_key_count(limiter: *const sc_request_limiter) -> usize;
    pub fn sc_request_limiter_active_count(limiter: *const sc_request_limiter) -> usize;
}
