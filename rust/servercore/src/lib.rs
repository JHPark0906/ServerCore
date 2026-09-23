//! Safe owned handles for ServerCore's bounded C ABI.
//!
//! Async operations implement standard [`std::future::Future`]; no Tokio runtime
//! is required. A shared, bounded readiness thread blocks on native event
//! notifications and explicit deadlines; it never periodically polls pending
//! operations. Readiness never calls Rust closures from native workers. Dropping an unfinished
//! response aborts it; dropping a connection closes it; dropping a server stops
//! and joins its native workers. Keep immutable events alive while borrowing
//! their bytes, and release them promptly to return the native queue budget.
#![deny(unsafe_op_in_unsafe_fn)]

pub use servercore_sys as sys;
use std::{fmt, ptr::NonNull, time::Duration};
pub mod channel;
pub mod binary;
pub mod files;
pub mod datagram;
pub mod game_execution;
pub mod net;
pub mod observability;
mod reactor;
pub mod runtime;
pub mod request_limiter;
pub mod endpoint;
pub mod web_data;
pub mod web_policy;
pub mod web;

pub type Result<T> = std::result::Result<T, Error>;

/// An ABI status code. Unknown future status values remain representable.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error(pub i32);
impl Error {
    pub const INVALID_ARGUMENT: Self = Self(sys::SC_INVALID_ARGUMENT);
    pub const INVALID_FORMAT: Self = Self(sys::SC_INVALID_FORMAT);
    pub const TOO_LARGE: Self = Self(sys::SC_TOO_LARGE);
    pub const NOT_FOUND: Self = Self(sys::SC_NOT_FOUND);
    pub const ALREADY_EXISTS: Self = Self(sys::SC_ALREADY_EXISTS);
    pub const CLOSED: Self = Self(sys::SC_CLOSED);
    pub const WOULD_BLOCK: Self = Self(sys::SC_WOULD_BLOCK);
    pub const PLATFORM: Self = Self(sys::SC_PLATFORM_ERROR);
    pub const UNIMPLEMENTED: Self = Self(sys::SC_UNIMPLEMENTED);
    pub const TIMEOUT: Self = Self(sys::SC_TIMEOUT);
    pub const CANCELLED: Self = Self(sys::SC_CANCELLED);
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // The ABI returns an immutable process-lifetime NUL-terminated string.
        let name = unsafe { std::ffi::CStr::from_ptr(sys::sc_status_name(self.0)) };
        write!(f, "{} ({})", name.to_string_lossy(), self.0)
    }
}
impl std::error::Error for Error {}
pub(crate) fn check(status: i32) -> Result<()> {
    if status == sys::SC_OK {
        Ok(())
    } else {
        Err(Error(status))
    }
}
pub(crate) fn bytes(value: &[u8]) -> sys::sc_bytes {
    sys::sc_bytes {
        data: value.as_ptr(),
        len: value.len(),
    }
}
pub(crate) fn pointer<T>(value: *mut T) -> Result<NonNull<T>> {
    NonNull::new(value).ok_or(Error::PLATFORM)
}
pub(crate) fn timeout_ms(timeout: Duration) -> Result<u32> {
    if timeout.is_zero() {
        return Ok(0);
    }
    let ms = timeout
        .as_millis()
        .saturating_add(u128::from(timeout.subsec_nanos() % 1_000_000 != 0));
    if ms >= u128::from(u32::MAX) {
        return Err(Error::INVALID_ARGUMENT);
    }
    Ok(ms as u32)
}
pub(crate) fn verify_abi() -> Result<()> {
    if unsafe { sys::sc_abi_version() } == sys::SC_ABI_VERSION {
        Ok(())
    } else {
        Err(Error::UNIMPLEMENTED)
    }
}
/// # Safety
/// `value` must be an ABI view borrowed from an immutable live owner for `'a`.
pub(crate) unsafe fn borrowed_bytes<'a>(value: sys::sc_bytes) -> &'a [u8] {
    if value.len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(value.data, value.len) }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    pub name: String,
    pub value: String,
}
impl Header {
    pub fn new(name: impl Into<String>, value: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            value: value.into(),
        }
    }
}
pub(crate) fn raw_headers(headers: &[Header]) -> Vec<sys::sc_header> {
    headers
        .iter()
        .map(|h| sys::sc_header {
            name: bytes(h.name.as_bytes()),
            value: bytes(h.value.as_bytes()),
        })
        .collect()
}

/// Borrowed repeated headers (or decoded route parameters); no allocation or
/// lossy UTF-8 conversion is performed. Values can contain non-UTF-8 wire bytes.
#[derive(Clone, Copy)]
pub struct Headers<'a>(&'a [sys::sc_header]);
// These pointers refer to immutable event-owned storage; constructing Headers
// is private/unsafe and its lifetime is tied to that owner by safe accessors.
unsafe impl Send for Headers<'_> {}
unsafe impl Sync for Headers<'_> {}
impl<'a> Headers<'a> {
    pub(crate) unsafe fn from_raw(data: *const sys::sc_header, len: usize) -> Self {
        Self(if len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(data, len) }
        })
    }
    pub fn len(self) -> usize {
        self.0.len()
    }
    pub fn is_empty(self) -> bool {
        self.0.is_empty()
    }
    pub fn iter(self) -> impl ExactSizeIterator<Item = (&'a [u8], &'a [u8])> {
        self.0
            .iter()
            .map(|h| unsafe { (borrowed_bytes(h.name), borrowed_bytes(h.value)) })
    }
    pub fn get(self, name: &str) -> Option<&'a [u8]> {
        self.iter()
            .find(|(key, _)| key.eq_ignore_ascii_case(name.as_bytes()))
            .map(|(_, value)| value)
    }
    /// Exact lookup for case-sensitive application attributes/route parameters.
    pub fn get_exact(self, name: &str) -> Option<&'a [u8]> {
        self.iter()
            .find(|(key, _)| *key == name.as_bytes())
            .map(|(_, value)| value)
    }
}

/// An advisory native send-capacity notification. Drop cancels the registration
/// and waits for native notification quiescence. Readiness is not a reservation.
pub struct CapacityWait {
    handle: NonNull<sys::sc_wait>,
    _readiness: reactor::Lease,
}
// Native wait state supports concurrent observation/cancellation; destruction
// requires exclusive Rust ownership and never races a borrowed method/future.
unsafe impl Send for CapacityWait {}
unsafe impl Sync for CapacityWait {}
impl CapacityWait {
    fn native_handle(&self) -> *mut sys::sc_wait {
        self.handle.as_ptr()
    }
    pub(crate) fn from_raw(value: *mut sys::sc_wait) -> Result<Self> {
        let handle = pointer(value)?;
        match reactor::Lease::new() {
            Ok(lease) => Ok(Self {
                handle,
                _readiness: lease,
            }),
            Err(error) => {
                unsafe { sys::sc_wait_destroy(handle.as_ptr()) };
                Err(error)
            }
        }
    }
    pub fn result(&self) -> Result<()> {
        check(unsafe { sys::sc_wait_result(self.handle.as_ptr()) })
    }
    pub fn wait_timeout(&self, timeout: Duration) -> Result<()> {
        check(unsafe { sys::sc_wait_wait(self.handle.as_ptr(), timeout_ms(timeout)?) })
    }
    pub fn cancel(&self) {
        unsafe { sys::sc_wait_cancel(self.handle.as_ptr()) };
    }
    pub async fn wait(&self) -> Result<()> {
        reactor::poll_fn(
            || self.result(),
            |notifier, key, out| unsafe {
                sys::sc_wait_subscribe(self.native_handle(), notifier, key, out)
            },
        )?
        .await
    }
}
impl Drop for CapacityWait {
    fn drop(&mut self) {
        unsafe { sys::sc_wait_destroy(self.handle.as_ptr()) };
    }
}

/// A small executor for command-line examples. Production applications can use
/// any existing executor instead. It parks the caller, never busy-spins.
pub fn block_on<F: std::future::Future>(future: F) -> F::Output {
    struct ThreadWake(std::thread::Thread);
    impl std::task::Wake for ThreadWake {
        fn wake(self: std::sync::Arc<Self>) {
            self.0.unpark();
        }
        fn wake_by_ref(self: &std::sync::Arc<Self>) {
            self.0.unpark();
        }
    }
    let waker = std::task::Waker::from(std::sync::Arc::new(ThreadWake(std::thread::current())));
    let mut context = std::task::Context::from_waker(&waker);
    let mut future = std::pin::pin!(future);
    loop {
        if let std::task::Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::park();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn timeout_rounding_does_not_turn_waits_into_polls() {
        assert_eq!(timeout_ms(Duration::ZERO), Ok(0));
        assert_eq!(timeout_ms(Duration::from_nanos(1)), Ok(1));
        assert_eq!(timeout_ms(Duration::from_micros(1001)), Ok(2));
        assert_eq!(
            timeout_ms(Duration::from_millis(u32::MAX as u64)),
            Err(Error::INVALID_ARGUMENT)
        );
    }
}
