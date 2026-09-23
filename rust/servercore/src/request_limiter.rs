//! Protocol-neutral token buckets and concurrency permits with bounded key storage.
use crate::{check, pointer, sys, Result};
use std::{mem::size_of, ptr::NonNull, sync::Arc, time::Duration};
use sys::request_limiter as ffi;

pub struct Options {
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
impl Default for Options {
    fn default() -> Self {
        Self {
            max_keys: 1024,
            max_key_bytes: 128,
            max_retained_key_bytes: 128 * 1024,
            refill_tokens: 100,
            burst_tokens: 200,
            refill_interval_ms: 1000,
            max_concurrent_per_key: 8,
            max_concurrent_total: 1024,
            idle_expiry_ms: 300000,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reason {
    Rate,
    KeyConcurrency,
    TotalConcurrency,
    KeyCapacity,
}
pub enum Admission {
    Allowed(Permit),
    Denied {
        reason: Reason,
        retry_after: Duration,
    },
}
struct Owner(NonNull<ffi::sc_request_limiter>);
// Native state serializes all operations; Arc prevents destruction during calls.
unsafe impl Send for Owner {}
unsafe impl Sync for Owner {}
impl Drop for Owner {
    fn drop(&mut self) {
        unsafe { ffi::sc_request_limiter_destroy(self.0.as_ptr()) }
    }
}
#[derive(Clone)]
pub struct RequestLimiter(Arc<Owner>);
pub struct Permit(NonNull<ffi::sc_request_permit>);
// A permit only exposes ownership; destruction releases a native synchronized lease.
unsafe impl Send for Permit {}
unsafe impl Sync for Permit {}
impl Drop for Permit {
    fn drop(&mut self) {
        unsafe { ffi::sc_request_permit_destroy(self.0.as_ptr()) }
    }
}
impl RequestLimiter {
    pub fn new(o: &Options) -> Result<Self> {
        crate::verify_abi()?;
        let native = ffi::sc_request_limiter_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<ffi::sc_request_limiter_options>() as u32,
            max_keys: o.max_keys,
            max_key_bytes: o.max_key_bytes,
            max_retained_key_bytes: o.max_retained_key_bytes,
            refill_tokens: o.refill_tokens,
            burst_tokens: o.burst_tokens,
            refill_interval_ms: o.refill_interval_ms,
            max_concurrent_per_key: o.max_concurrent_per_key,
            max_concurrent_total: o.max_concurrent_total,
            idle_expiry_ms: o.idle_expiry_ms,
        };
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::sc_request_limiter_create(&native, &mut raw) })?;
        Ok(Self(Arc::new(Owner(pointer(raw)?))))
    }
    pub fn try_acquire(&self, key: &[u8], cost: u64) -> Result<Admission> {
        let mut decision = ffi::sc_request_limit_decision {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<ffi::sc_request_limit_decision>() as u32,
            reason: 0,
            retry_after_ms: 0,
        };
        let mut permit = std::ptr::null_mut();
        check(unsafe {
            ffi::sc_request_limiter_acquire(
                self.0 .0.as_ptr(),
                sys::sc_bytes {
                    data: key.as_ptr(),
                    len: key.len(),
                },
                cost,
                &mut decision,
                &mut permit,
            )
        })?;
        if let Some(permit) = NonNull::new(permit) {
            return Ok(Admission::Allowed(Permit(permit)));
        }
        let reason = match decision.reason {
            1 => Reason::Rate,
            2 => Reason::KeyConcurrency,
            3 => Reason::TotalConcurrency,
            4 => Reason::KeyCapacity,
            _ => return Err(crate::Error::PLATFORM),
        };
        Ok(Admission::Denied {
            reason,
            retry_after: Duration::from_millis(decision.retry_after_ms),
        })
    }
    pub fn close(&self) {
        unsafe { ffi::sc_request_limiter_close(self.0 .0.as_ptr()) }
    }
    pub fn prune_expired(&self) -> usize {
        unsafe { ffi::sc_request_limiter_prune(self.0 .0.as_ptr()) }
    }
    pub fn key_count(&self) -> usize {
        unsafe { ffi::sc_request_limiter_key_count(self.0 .0.as_ptr()) }
    }
    pub fn active_count(&self) -> usize {
        unsafe { ffi::sc_request_limiter_active_count(self.0 .0.as_ptr()) }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn permit_tracks_work_and_survives_owner() {
        let limiter = RequestLimiter::new(&Options {
            max_concurrent_per_key: 1,
            ..Options::default()
        })
        .unwrap();
        let permit = match limiter.try_acquire(b"actor", 1).unwrap() {
            Admission::Allowed(p) => p,
            _ => panic!("admit"),
        };
        assert!(matches!(
            limiter.try_acquire(b"actor", 1).unwrap(),
            Admission::Denied {
                reason: Reason::KeyConcurrency,
                ..
            }
        ));
        assert_eq!(limiter.active_count(), 1);
        limiter.close();
        drop(limiter);
        drop(permit);
    }
}
