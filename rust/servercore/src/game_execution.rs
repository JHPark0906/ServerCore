//! Logical ticks and bounded outbound staging. Native scheduler/capacity events
//! drive progress; these wrappers add no polling worker or simulation ownership.
use crate::{bytes, check, pointer, reactor, sys, Error, Result};
use std::{
    ffi::c_void,
    marker::PhantomData,
    mem::size_of,
    panic::{catch_unwind, AssertUnwindSafe},
    ptr::NonNull,
    sync::{Arc, Mutex},
    time::Duration,
};
#[derive(Clone, Copy, Debug)]
pub enum LagPolicy {
    Skip,
    CatchUp,
}
#[derive(Clone, Copy, Debug)]
pub struct TickOptions {
    pub interval: Duration,
    pub lag_policy: LagPolicy,
    pub max_catch_up: usize,
    pub retained_bytes: usize,
}
impl Default for TickOptions {
    fn default() -> Self {
        Self {
            interval: Duration::from_millis(16),
            lag_policy: LagPolicy::Skip,
            max_catch_up: 4,
            retained_bytes: 0,
        }
    }
}
#[derive(Clone, Copy, Debug)]
pub struct TickInfo {
    pub index: u64,
    pub lateness: Duration,
    pub skipped: u64,
}
#[derive(Clone, Copy, Debug)]
pub struct TickMetrics {
    pub executed: u64,
    pub skipped: u64,
    pub last_lateness: Duration,
    pub max_lateness: Duration,
}
/// Callback-scoped token; the higher-ranked callback cannot retain this borrow.
pub struct TickToken<'a> {
    raw: *const sys::sc_tick_token,
    _borrow: PhantomData<&'a ()>,
}
impl TickToken<'_> {
    pub fn requested(&self) -> bool {
        unsafe { sys::sc_tick_token_requested(self.raw) != 0 }
    }
}
fn contain<F: FnOnce()>(f: F) {
    if let Err(payload) = catch_unwind(AssertUnwindSafe(f)) {
        if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
            std::mem::forget(secondary);
        }
    }
}
struct Callback<F>(Mutex<F>);
unsafe extern "C" fn invoke<F>(
    context: *mut c_void,
    info: *const sys::sc_tick_info,
    token: *const sys::sc_tick_token,
) -> i32
where
    F: for<'a> FnMut(TickInfo, TickToken<'a>) -> Result<()> + Send + 'static,
{
    let callback = unsafe { &*context.cast::<Callback<F>>() };
    let raw = unsafe { *info };
    let result = catch_unwind(AssertUnwindSafe(|| {
        let mut callback = callback.0.lock().map_err(|_| Error::PLATFORM)?;
        callback(
            TickInfo {
                index: raw.index,
                lateness: Duration::from_nanos(raw.lateness_ns),
                skipped: raw.skipped,
            },
            TickToken {
                raw: token,
                _borrow: PhantomData,
            },
        )
    }));
    match result {
        Ok(Ok(())) => sys::SC_OK,
        Ok(Err(error)) => error.0,
        Err(payload) => {
            contain(|| drop(payload));
            sys::SC_PLATFORM_ERROR
        }
    }
}
unsafe extern "C" fn release<F>(context: *mut c_void) {
    contain(|| drop(unsafe { Box::from_raw(context.cast::<Callback<F>>()) }));
}
struct TickOwner {
    handle: NonNull<sys::sc_tick>,
    _readiness: reactor::Lease,
}
unsafe impl Send for TickOwner {}
unsafe impl Sync for TickOwner {}
impl Drop for TickOwner {
    fn drop(&mut self) {
        unsafe { sys::sc_tick_destroy(self.handle.as_ptr()) };
    }
}
/// Last-owner Drop requests cancellation. Explicit wait observes capture cleanup.
#[derive(Clone)]
pub struct Ticks(Arc<TickOwner>);
impl Ticks {
    fn native_handle(&self) -> *mut sys::sc_tick {
        self.0.handle.as_ptr()
    }
    pub fn start<F>(
        scheduler: &crate::runtime::TimerScheduler,
        options: TickOptions,
        work: F,
    ) -> Result<Self>
    where
        F: for<'a> FnMut(TickInfo, TickToken<'a>) -> Result<()> + Send + 'static,
    {
        let interval = crate::timeout_ms(options.interval)?;
        let native = sys::sc_tick_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_tick_options>() as u32,
            interval_ms: interval,
            lag_policy: match options.lag_policy {
                LagPolicy::Skip => 0,
                LagPolicy::CatchUp => 1,
            },
            max_catch_up: options.max_catch_up,
            retained_bytes: options
                .retained_bytes
                .checked_add(size_of::<Callback<F>>())
                .ok_or(Error::TOO_LARGE)?,
        };
        let readiness = reactor::Lease::new()?;
        let context = Box::into_raw(Box::new(Callback(Mutex::new(work)))).cast();
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            sys::sc_tick_start(
                scheduler.native_handle(),
                &native,
                sys::sc_tick_work {
                    context,
                    work: Some(invoke::<F>),
                    release: Some(release::<F>),
                },
                &mut raw,
            )
        })?;
        Ok(Self(Arc::new(TickOwner {
            handle: pointer(raw)?,
            _readiness: readiness,
        })))
    }
    pub fn cancel(&self) {
        unsafe { sys::sc_tick_cancel(self.native_handle()) };
    }
    pub fn is_finished(&self) -> bool {
        unsafe { sys::sc_tick_finished(self.native_handle()) != 0 }
    }
    pub fn result(&self) -> Result<()> {
        check(unsafe { sys::sc_tick_result(self.native_handle()) })
    }
    pub fn wait_timeout(&self, timeout: Duration) -> Result<()> {
        check(unsafe { sys::sc_tick_wait(self.native_handle(), crate::timeout_ms(timeout)?) })
    }
    pub async fn wait(&self) -> Result<()> {
        reactor::poll_fn(
            || {
                if self.is_finished() {
                    Ok(self.result())
                } else {
                    Err(Error::WOULD_BLOCK)
                }
            },
            |notifier, key, out| unsafe {
                sys::sc_tick_subscribe(self.native_handle(), notifier, key, out)
            },
        )?
        .await?
    }
    pub fn metrics(&self) -> Result<TickMetrics> {
        let mut m = sys::sc_tick_metrics::default();
        check(unsafe { sys::sc_tick_get_metrics(self.native_handle(), &mut m) })?;
        Ok(TickMetrics {
            executed: m.executed,
            skipped: m.skipped,
            last_lateness: Duration::from_nanos(m.last_lateness_ns),
            max_lateness: Duration::from_nanos(m.max_lateness_ns),
        })
    }
}
#[derive(Clone, Copy, Debug)]
pub struct OutboundOptions {
    pub max_messages: usize,
    pub max_retained_bytes: usize,
    pub max_pump_messages: usize,
}
impl Default for OutboundOptions {
    fn default() -> Self {
        Self {
            max_messages: 128,
            max_retained_bytes: 4 * 1024 * 1024,
            max_pump_messages: 64,
        }
    }
}
#[derive(Clone, Copy, Debug, Default)]
pub struct ItemOptions {
    pub latest_key: Option<u64>,
    pub expires_after: Option<Duration>,
}
impl ItemOptions {
    fn raw(self) -> Result<sys::sc_outbound_item_options> {
        Ok(sys::sc_outbound_item_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_outbound_item_options>() as u32,
            has_latest_key: self.latest_key.is_some() as u32,
            latest_key: self.latest_key.unwrap_or(0),
            expiry_ms: self
                .expires_after
                .map(crate::timeout_ms)
                .transpose()?
                .unwrap_or(u32::MAX),
        })
    }
}
#[derive(Clone, Copy, Debug)]
pub struct OutboundMetrics {
    pub pending: usize,
    pub retained_bytes: usize,
    pub enqueued: u64,
    pub sent: u64,
    pub replaced: u64,
    pub expired: u64,
    pub discarded: u64,
}
struct QueueOwner {
    handle: NonNull<sys::sc_outbound_queue>,
    _readiness: reactor::Lease,
}
unsafe impl Send for QueueOwner {}
unsafe impl Sync for QueueOwner {}
impl Drop for QueueOwner {
    fn drop(&mut self) {
        unsafe { sys::sc_outbound_destroy(self.handle.as_ptr()) };
    }
}
/// A TCP staging queue. Clones share ordering/budgets. Last Drop discards staged
/// items; accepted transport data is unaffected. Success is not delivery.
#[derive(Clone)]
pub struct OutboundQueue(Arc<QueueOwner>);
impl OutboundQueue {
    fn native_handle(&self) -> *mut sys::sc_outbound_queue {
        self.0.handle.as_ptr()
    }
    pub fn new(
        scheduler: &crate::runtime::TimerScheduler,
        connection: &crate::net::Connection,
        options: OutboundOptions,
    ) -> Result<Self> {
        let readiness = reactor::Lease::new()?;
        let native = sys::sc_outbound_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_outbound_options>() as u32,
            max_messages: options.max_messages,
            max_retained_bytes: options.max_retained_bytes,
            max_pump_messages: options.max_pump_messages,
        };
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            sys::sc_outbound_create(
                scheduler.native_handle(),
                connection.native_handle(),
                &native,
                &mut raw,
            )
        })?;
        Ok(Self(Arc::new(QueueOwner {
            handle: pointer(raw)?,
            _readiness: readiness,
        })))
    }
    pub fn enqueue(&self, payload: &[u8], options: ItemOptions) -> Result<()> {
        let native = options.raw()?;
        check(unsafe { sys::sc_outbound_enqueue(self.native_handle(), bytes(payload), &native) })
    }
    pub fn batch(
        recipients: &[&Self],
        payload: &[u8],
        options: ItemOptions,
    ) -> Result<Vec<Result<()>>> {
        if recipients.len() > 4096 {
            return Err(Error::TOO_LARGE);
        }
        let native = options.raw()?;
        let targets: Vec<_> = recipients
            .iter()
            .map(|value| value.native_handle())
            .collect();
        let mut results = vec![sys::SC_OK; targets.len()];
        check(unsafe {
            sys::sc_outbound_batch(
                targets.as_ptr(),
                targets.len(),
                bytes(payload),
                &native,
                results.as_mut_ptr(),
                results.len(),
            )
        })?;
        Ok(results.into_iter().map(check).collect())
    }
    pub fn begin_drain(&self) {
        unsafe { sys::sc_outbound_begin_drain(self.native_handle()) };
    }
    pub fn close(&self) {
        unsafe { sys::sc_outbound_close(self.native_handle()) };
    }
    pub fn is_finished(&self) -> bool {
        unsafe { sys::sc_outbound_finished(self.native_handle()) != 0 }
    }
    pub fn result(&self) -> Result<()> {
        check(unsafe { sys::sc_outbound_result(self.native_handle()) })
    }
    pub async fn drain(&self) -> Result<()> {
        self.begin_drain();
        reactor::poll_fn(
            || {
                if self.is_finished() {
                    Ok(self.result())
                } else {
                    Err(Error::WOULD_BLOCK)
                }
            },
            |notifier, key, out| unsafe {
                sys::sc_outbound_subscribe(self.native_handle(), notifier, key, out)
            },
        )?
        .await?
    }
    pub fn metrics(&self) -> Result<OutboundMetrics> {
        let mut m = sys::sc_outbound_metrics::default();
        check(unsafe { sys::sc_outbound_get_metrics(self.native_handle(), &mut m) })?;
        Ok(OutboundMetrics {
            pending: m.pending,
            retained_bytes: m.retained_bytes,
            enqueued: m.enqueued,
            sent: m.sent,
            replaced: m.replaced,
            expired: m.expired,
            discarded: m.discarded,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        block_on,
        net::{Options, TcpServer},
        runtime::{Executor, ExecutorOptions, TimerScheduler, TimerSchedulerOptions},
    };
    use std::{
        io::Read,
        net::{TcpListener, TcpStream},
        sync::atomic::{AtomicUsize, Ordering},
    };
    fn assert_send<T: Send>(_: &T) {}
    #[test]
    fn logical_ticks_keep_callback_identity_and_native_owners() {
        let executor = Executor::new(&ExecutorOptions::default()).unwrap();
        let scheduler = TimerScheduler::new(&executor, &TimerSchedulerOptions::default()).unwrap();
        let calls = Arc::new(AtomicUsize::new(0));
        let captured = Arc::new(());
        let weak = Arc::downgrade(&captured);
        let counter = calls.clone();
        let ticks = Ticks::start(
            &scheduler,
            TickOptions {
                interval: Duration::from_millis(2),
                ..TickOptions::default()
            },
            move |tick, token| {
                let _keep = &captured;
                assert!(tick.index > 0 && !token.requested());
                if counter.fetch_add(1, Ordering::SeqCst) == 2 {
                    Err(Error::CLOSED)
                } else {
                    Ok(())
                }
            },
        )
        .unwrap();
        drop(executor);
        drop(scheduler);
        let future = ticks.wait();
        assert_send(&future);
        assert_eq!(block_on(future), Err(Error::CLOSED));
        assert_eq!(calls.load(Ordering::SeqCst), 3);
        assert!(weak.upgrade().is_none());
        assert_eq!(ticks.metrics().unwrap().executed, 3);
        // A terminal user WouldBlock is a completed error, not another wait.
        let executor = Executor::new(&ExecutorOptions::default()).unwrap();
        let scheduler = TimerScheduler::new(&executor, &TimerSchedulerOptions::default()).unwrap();
        let failed = Ticks::start(
            &scheduler,
            TickOptions {
                interval: Duration::from_millis(1),
                ..TickOptions::default()
            },
            |_, _| Err(Error::WOULD_BLOCK),
        )
        .unwrap();
        assert_eq!(block_on(failed.wait()), Err(Error::WOULD_BLOCK));
        assert!(failed.is_finished());
        scheduler.stop().unwrap();
        executor.stop().unwrap();
    }
    #[test]
    fn outbound_tcp_owner_copy_batch_and_drain() {
        let executor = Executor::new(&ExecutorOptions::default()).unwrap();
        let scheduler = TimerScheduler::new(&executor, &TimerSchedulerOptions::default()).unwrap();
        let reserved = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = reserved.local_addr().unwrap().port();
        drop(reserved);
        let mut server = TcpServer::new(&Options {
            listen_address: "127.0.0.1".into(),
            port,
            ..Options::default()
        })
        .unwrap();
        server.start().unwrap();
        let mut peer = TcpStream::connect(("127.0.0.1", server.port())).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
        let connection = server.accept_timeout(Duration::from_secs(3)).unwrap();
        let queue = OutboundQueue::new(
            &scheduler,
            &connection,
            OutboundOptions {
                max_retained_bytes: 8,
                ..OutboundOptions::default()
            },
        )
        .unwrap();
        drop(connection);
        assert_eq!(
            queue.enqueue(b"123456789", ItemOptions::default()),
            Err(Error::TOO_LARGE)
        );
        let mut data = b"copy".to_vec();
        queue.enqueue(&data, ItemOptions::default()).unwrap();
        data.fill(b'x');
        let mut received = [0u8; 4];
        peer.read_exact(&mut received).unwrap();
        assert_eq!(&received, b"copy");
        assert_eq!(
            OutboundQueue::batch(&[&queue, &queue], b"z", ItemOptions::default()).unwrap(),
            vec![Ok(()), Ok(())]
        );
        let mut pair = [0; 2];
        peer.read_exact(&mut pair).unwrap();
        assert_eq!(&pair, b"zz");
        let future = queue.drain();
        assert_send(&future);
        block_on(future).unwrap();
        assert!(queue.is_finished());
        assert_eq!(queue.metrics().unwrap().sent, 3);
        assert_eq!(
            queue.enqueue(b"x", ItemOptions::default()),
            Err(Error::CLOSED)
        );
        drop(queue);
        server.stop().unwrap();
        scheduler.stop().unwrap();
        executor.stop().unwrap();
    }
}
