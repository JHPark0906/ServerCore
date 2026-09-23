//! Bounded native log instances and fixed-cardinality HTTP metrics.
use crate::{borrowed_bytes, bytes, check, pointer, sys, verify_abi, Error, Result};
use std::{mem::size_of, ptr::NonNull};

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Protocol {
    Unknown,
    Http,
    WebSocket,
    Tcp,
    Udp,
    Task,
    Host,
    Logger,
}
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Lifecycle {
    Unknown,
    Created,
    Running,
    Draining,
    Stopped,
}
#[repr(usize)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventReason {
    Unknown,
    Normal,
    Cancelled,
    Timeout,
    ProtocolError,
    Capacity,
    Policy,
    TransportError,
}
#[repr(u64)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObservationField {
    Connections = 1,
    PendingWork = 2,
    ReceiveBytes = 4,
    SendBytes = 8,
    RetainedBytes = 16,
    DrainRemaining = 32,
    Closed = 64,
    Rejected = 128,
    TimedOut = 256,
}
/// One independently sampled view, never a health/readiness judgment. Missing
/// fields are unknown. Zero remaining work does not replace native stop/join.
#[derive(Clone, Copy, Debug)]
pub struct Observation(sys::sc_observation);
impl Observation {
    pub(crate) fn read(
        get: impl FnOnce(*mut sys::sc_observation) -> sys::sc_status,
    ) -> Result<Self> {
        let mut value = sys::sc_observation {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_observation>() as u32,
            ..Default::default()
        };
        check(get(&mut value))?;
        if value.protocol > 7 || value.lifecycle > 4 {
            return Err(Error::INVALID_FORMAT);
        }
        Ok(Self(value))
    }
    pub fn protocol(&self) -> Protocol {
        match self.0.protocol {
            1 => Protocol::Http,
            2 => Protocol::WebSocket,
            3 => Protocol::Tcp,
            4 => Protocol::Udp,
            5 => Protocol::Task,
            6 => Protocol::Host,
            7 => Protocol::Logger,
            _ => Protocol::Unknown,
        }
    }
    pub fn lifecycle(&self) -> Lifecycle {
        match self.0.lifecycle {
            1 => Lifecycle::Created,
            2 => Lifecycle::Running,
            3 => Lifecycle::Draining,
            4 => Lifecycle::Stopped,
            _ => Lifecycle::Unknown,
        }
    }
    pub fn has(&self, field: ObservationField) -> bool {
        self.0.available & field as u64 != 0
    }
    pub fn value(&self, field: ObservationField) -> Option<u64> {
        if !self.has(field) {
            return None;
        }
        Some(match field {
            ObservationField::Connections => self.0.connections,
            ObservationField::PendingWork => self.0.pending_work,
            ObservationField::ReceiveBytes => self.0.receive_bytes,
            ObservationField::SendBytes => self.0.send_bytes,
            ObservationField::RetainedBytes => self.0.retained_bytes,
            ObservationField::DrainRemaining => self.0.drain_remaining,
            ObservationField::Closed => {
                self.0.closed.iter().fold(0u64, |a, b| a.saturating_add(*b))
            }
            ObservationField::Rejected => self
                .0
                .rejected
                .iter()
                .fold(0u64, |a, b| a.saturating_add(*b)),
            ObservationField::TimedOut => self.0.timed_out,
        })
    }
    pub fn closed(&self, reason: EventReason) -> Option<u64> {
        self.has(ObservationField::Closed)
            .then_some(self.0.closed[reason as usize])
    }
    pub fn rejected(&self, reason: EventReason) -> Option<u64> {
        self.has(ObservationField::Rejected)
            .then_some(self.0.rejected[reason as usize])
    }
}
#[derive(Clone, Copy, Debug, Default)]
pub struct LogCorrelation {
    pub request_id: u64,
    pub session_id: u64,
    pub task_id: u64,
    pub connection_id: u64,
}
#[derive(Clone, Copy, Debug)]
pub struct LogField<'a> {
    pub name: &'a str,
    pub value: &'a str,
}

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
    /// Copies a bounded UTF-8 JSON record. Correlation is explicit and follows
    /// owned task/request data across threads; zero IDs are omitted.
    pub fn try_write_record(
        &self,
        level: LogLevel,
        message: &str,
        fields: &[LogField<'_>],
        correlation: LogCorrelation,
    ) -> Result<()> {
        if fields.len() > 32 {
            return Err(Error::INVALID_ARGUMENT);
        }
        let empty = sys::sc_log_field {
            name: bytes(&[]),
            value: bytes(&[]),
        };
        let mut raw_fields = [empty; 32];
        for (out, field) in raw_fields.iter_mut().zip(fields) {
            *out = sys::sc_log_field {
                name: bytes(field.name.as_bytes()),
                value: bytes(field.value.as_bytes()),
            };
        }
        let record = sys::sc_log_record {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_log_record>() as u32,
            level: level as u32,
            reserved: 0,
            message: bytes(message.as_bytes()),
            fields: raw_fields.as_ptr(),
            field_count: fields.len(),
            request_id: correlation.request_id,
            session_id: correlation.session_id,
            task_id: correlation.task_id,
            connection_id: correlation.connection_id,
        };
        check(unsafe { sys::sc_logger_try_write_record(self.handle.as_ptr(), &record) })
    }
    pub fn observation(&self) -> Result<Observation> {
        Observation::read(|out| unsafe {
            sys::sc_logger_get_observation(self.handle.as_ptr(), out)
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
impl crate::web::HttpServer {
    pub fn observation(&self) -> Result<Observation> {
        Observation::read(|out| unsafe {
            sys::sc_web_server_get_observation(self.native_handle(), out)
        })
    }
}
impl crate::net::TcpServer {
    pub fn observation(&self) -> Result<Observation> {
        Observation::read(|out| unsafe {
            sys::sc_tcp_server_get_observation(self.native_handle(), out)
        })
    }
}
impl crate::runtime::Executor {
    pub fn observation(&self) -> Result<Observation> {
        Observation::read(|out| unsafe {
            sys::sc_executor_get_observation(self.native_handle(), out)
        })
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        sync::{Arc, Condvar, Mutex},
        time::{Duration, SystemTime, UNIX_EPOCH},
    };

    #[test]
    fn structured_log_copies_context_and_drains() {
        let file = std::env::temp_dir().join(format!(
            "servercore-rust-structured-{}-{}.jsonl",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        struct Cleanup(std::path::PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = std::fs::remove_file(&self.0);
            }
        }
        let _cleanup = Cleanup(file.clone());
        let logger = Logger::new(&LoggerOptions {
            console: false,
            file: file.to_str().unwrap().into(),
            max_message_bytes: 512,
            ..Default::default()
        })
        .unwrap();
        let input = String::from("untrusted\n\"value");
        logger
            .try_write_record(
                LogLevel::Info,
                "completed",
                &[LogField {
                    name: "value",
                    value: &input,
                }],
                LogCorrelation {
                    request_id: 101,
                    session_id: 202,
                    task_id: 303,
                    connection_id: 404,
                },
            )
            .unwrap();
        drop(input);
        assert_eq!(
            logger.try_write_record(
                LogLevel::Info,
                "x",
                &[
                    LogField {
                        name: "key",
                        value: "a"
                    },
                    LogField {
                        name: "key",
                        value: "b"
                    }
                ],
                Default::default()
            ),
            Err(Error::INVALID_ARGUMENT)
        );
        assert_eq!(
            logger.try_write_record(LogLevel::Info, &"a".repeat(512), &[], Default::default()),
            Err(Error::TOO_LARGE)
        );
        assert_eq!(
            logger.observation().unwrap().lifecycle(),
            Lifecycle::Running
        );
        logger.request_stop();
        assert_eq!(
            logger.observation().unwrap().lifecycle(),
            Lifecycle::Draining
        );
        logger.stop().unwrap();
        let observed = logger.observation().unwrap();
        assert_eq!(observed.lifecycle(), Lifecycle::Stopped);
        assert_eq!(observed.value(ObservationField::PendingWork), Some(0));
        assert_eq!(observed.value(ObservationField::RetainedBytes), Some(0));
        assert_eq!(observed.value(ObservationField::Connections), None);
        assert_eq!(observed.rejected(EventReason::Unknown), Some(2));
        let output = std::fs::read_to_string(file).unwrap();
        assert_eq!(output.lines().count(), 1);
        assert!(output.starts_with("{\"timestamp_ms\":"));
        assert!(output.contains(
            "\"request_id\":101,\"session_id\":202,\"task_id\":303,\"connection_id\":404"
        ));
        assert!(output.contains("untrusted\\u000a\\\"value"));
    }

    #[test]
    fn executor_observation_preserves_shutdown_and_unknown_fields() {
        let executor = crate::runtime::Executor::new(&Default::default()).unwrap();
        let gate = Arc::new((Mutex::new((false, false)), Condvar::new()));
        struct Release(Arc<(Mutex<(bool, bool)>, Condvar)>);
        impl Drop for Release {
            fn drop(&mut self) {
                let (lock, wake) = &*self.0;
                lock.lock().unwrap_or_else(|error| error.into_inner()).1 = true;
                wake.notify_all();
            }
        }
        let _release = Release(gate.clone());
        let worker_gate = gate.clone();
        let task = executor
            .submit(&Default::default(), move |_| {
                let (lock, wake) = &*worker_gate;
                let mut state = lock.lock().unwrap();
                state.0 = true;
                wake.notify_all();
                while !state.1 {
                    state = wake.wait(state).unwrap();
                }
                Ok(())
            })
            .unwrap();
        {
            let (lock, wake) = &*gate;
            let (state, timed) = wake
                .wait_timeout_while(lock.lock().unwrap(), Duration::from_secs(5), |value| {
                    !value.0
                })
                .unwrap();
            assert!(!timed.timed_out() && state.0);
        }
        executor.request_stop();
        let observed = executor.observation().unwrap();
        assert_eq!(observed.protocol(), Protocol::Task);
        assert_eq!(observed.lifecycle(), Lifecycle::Draining);
        assert_eq!(observed.value(ObservationField::PendingWork), Some(1));
        assert_eq!(observed.value(ObservationField::Connections), None);
        {
            let (lock, wake) = &*gate;
            lock.lock().unwrap().1 = true;
            wake.notify_all();
        }
        executor.stop().unwrap();
        assert!(task.is_finished());
        assert_eq!(
            executor.observation().unwrap().lifecycle(),
            Lifecycle::Stopped
        );
    }
}
