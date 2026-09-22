//! Bounded native log instances and fixed-cardinality HTTP metrics.
use crate::{borrowed_bytes, bytes, check, pointer, sys, verify_abi, Error, Result};
use std::{mem::size_of, ptr::NonNull};

pub use sys::{
    sc_logger_metrics as LoggerMetrics, sc_task_metrics as TaskMetrics,
    sc_web_metrics as WebMetrics,
};
/// Inclusive finite upper bounds. The twelfth snapshot bin is infinity.
pub const LATENCY_UPPER_BOUNDS_NS: [u64; 11] = [
    100_000,
    250_000,
    500_000,
    1_000_000,
    2_500_000,
    5_000_000,
    10_000_000,
    25_000_000,
    50_000_000,
    100_000_000,
    1_000_000_000,
];

#[repr(u32)]
#[derive(Clone, Copy, Debug)]
pub enum LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
}
#[derive(Clone, Debug)]
pub struct LoggerOptions {
    pub minimum_level: LogLevel,
    pub console: bool,
    pub file: String,
    pub max_queued_messages: usize,
    pub max_retained_bytes: usize,
    pub max_message_bytes: usize,
    pub max_file_bytes: u64,
    pub retained_files: u32,
}
impl Default for LoggerOptions {
    fn default() -> Self {
        Self {
            minimum_level: LogLevel::Info,
            console: true,
            file: String::new(),
            max_queued_messages: 1024,
            max_retained_bytes: 1024 * 1024,
            max_message_bytes: 4096,
            max_file_bytes: 10 * 1024 * 1024,
            retained_files: 3,
        }
    }
}
/// Servers attached to this instance retain it after the Rust Logger is dropped.
/// Explicit stop drains and joins its worker, including when a server retains it.
pub struct Logger {
    pub(crate) handle: NonNull<sys::sc_logger>,
}
// Native logger operations synchronize internally; Rust borrowing excludes
// destruction while methods run. Native server attachment takes an owning ref.
unsafe impl Send for Logger {}
unsafe impl Sync for Logger {}
impl Logger {
    pub fn new(options: &LoggerOptions) -> Result<Self> {
        verify_abi()?;
        let raw = sys::sc_logger_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_logger_options>() as u32,
            minimum_level: options.minimum_level as u32,
            console: options.console.into(),
            file: bytes(options.file.as_bytes()),
            max_queued_messages: options.max_queued_messages,
            max_retained_bytes: options.max_retained_bytes,
            max_message_bytes: options.max_message_bytes,
            max_file_bytes: options.max_file_bytes,
            retained_files: options.retained_files,
        };
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_logger_create(&raw, &mut out) })?;
        Ok(Self {
            handle: pointer(out)?,
        })
    }
    pub fn try_write(&self, level: LogLevel, message: &[u8]) -> Result<()> {
        check(unsafe {
            sys::sc_logger_try_write(self.handle.as_ptr(), level as u32, bytes(message))
        })
    }
    pub fn set_minimum_level(&self, level: LogLevel) -> Result<()> {
        check(unsafe { sys::sc_logger_set_minimum_level(self.handle.as_ptr(), level as u32) })
    }
    pub fn metrics(&self) -> Result<LoggerMetrics> {
        let mut value = LoggerMetrics {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<LoggerMetrics>() as u32,
            ..Default::default()
        };
        check(unsafe { sys::sc_logger_get_metrics(self.handle.as_ptr(), &mut value) })?;
        Ok(value)
    }
    pub fn request_stop(&self) {
        unsafe { sys::sc_logger_request_stop(self.handle.as_ptr()) };
    }
    /// Blocking control-thread operation. Filesystem/console stalls have no
    /// finite bound; this should not run on an async executor worker.
    pub fn stop(&self) -> Result<()> {
        check(unsafe { sys::sc_logger_stop(self.handle.as_ptr()) })
    }
}
impl Drop for Logger {
    fn drop(&mut self) {
        unsafe { sys::sc_logger_destroy(self.handle.as_ptr()) };
    }
}

pub struct MetricsText {
    handle: NonNull<sys::sc_owned_text>,
}
// Immutable native storage is owned until exclusive Drop.
unsafe impl Send for MetricsText {}
unsafe impl Sync for MetricsText {}
impl MetricsText {
    pub(crate) fn from_raw(raw: *mut sys::sc_owned_text) -> Result<Self> {
        Ok(Self {
            handle: pointer(raw)?,
        })
    }
    pub fn bytes(&self) -> &[u8] {
        unsafe { borrowed_bytes(sys::sc_owned_text_view(self.handle.as_ptr())) }
    }
    pub fn as_str(&self) -> Result<&str> {
        std::str::from_utf8(self.bytes()).map_err(|_| Error::INVALID_FORMAT)
    }
}
impl Drop for MetricsText {
    fn drop(&mut self) {
        unsafe { sys::sc_owned_text_destroy(self.handle.as_ptr()) };
    }
}
