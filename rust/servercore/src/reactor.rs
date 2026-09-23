use crate::{check, pointer, sys, Error, Result};
use std::{
    future::Future,
    pin::Pin,
    ptr::NonNull,
    sync::{Arc, Mutex, MutexGuard, OnceLock, Weak},
    task::{Context, Poll, Waker},
    thread::{self, JoinHandle},
    time::Instant,
};

const MAX_WAITERS: usize = 4096;
struct SlotState {
    waker: Option<Waker>,
    deadline: Option<Instant>,
}
struct Slot(Mutex<SlotState>);
struct State {
    next: u64,
    slots: Vec<(u64, Weak<Slot>)>,
}
struct Native(NonNull<sys::sc_notifier>);
// Shared retains Native until the bridge returns; wait/interrupt/subscribe may
// be concurrent, while destruction never overlaps a native call.
unsafe impl Send for Native {}
unsafe impl Sync for Native {}
impl Drop for Native {
    fn drop(&mut self) {
        unsafe { sys::sc_notifier_destroy(self.0.as_ptr()) };
    }
}
struct Shared {
    state: Mutex<State>,
    native: Native,
}
struct Reactor {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}
#[derive(Clone)]
pub(crate) struct Lease {
    _reactor: Arc<Reactor>,
}
impl Lease {
    pub(crate) fn new() -> Result<Self> {
        Ok(Self {
            _reactor: Reactor::acquire()?,
        })
    }
}
fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|e| e.into_inner())
}
fn wake(slot: &Slot) {
    let value = lock(&slot.0).waker.take();
    if let Some(waker) = value {
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| waker.wake()));
    }
}
impl Reactor {
    fn create(start_worker: bool) -> Result<Self> {
        let mut native = std::ptr::null_mut();
        check(unsafe { sys::sc_notifier_create(MAX_WAITERS, &mut native) })?;
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                next: 1,
                slots: Vec::new(),
            }),
            native: Native(pointer(native)?),
        });
        let worker = if start_worker {
            let inner = shared.clone();
            Some(
                thread::Builder::new()
                    .name("servercore-readiness".into())
                    .spawn(move || {
                        loop {
                            let slots: Vec<_> = {
                                let mut state = lock(&inner.state);
                                state.slots.retain(|(_, slot)| slot.strong_count() != 0);
                                state
                                    .slots
                                    .iter()
                                    .filter_map(|(_, slot)| slot.upgrade())
                                    .collect()
                            };
                            let mut deadline = None;
                            for slot in &slots {
                                if let Some(value) = lock(&slot.0).deadline {
                                    deadline = Some(
                                        deadline
                                            .map_or(value, |current: Instant| current.min(value)),
                                    );
                                }
                            }
                            // Only explicit operation deadlines bound this native wait.
                            // No periodic readiness polling or fallback timer is used.
                            let timeout = deadline.map_or(u32::MAX, |value| {
                                value
                                    .saturating_duration_since(Instant::now())
                                    .as_millis()
                                    .saturating_add(1)
                                    .min((u32::MAX - 1) as u128)
                                    as u32
                            });
                            drop(slots);
                            let mut key = 0;
                            let status = unsafe {
                                sys::sc_notifier_next(inner.native.0.as_ptr(), timeout, &mut key)
                            };
                            if status == sys::SC_CLOSED {
                                break;
                            }
                            if status == sys::SC_OK {
                                let slot = {
                                    let state = lock(&inner.state);
                                    state
                                        .slots
                                        .iter()
                                        .find(|(id, _)| *id == key)
                                        .and_then(|(_, slot)| slot.upgrade())
                                };
                                if let Some(slot) = slot {
                                    wake(&slot);
                                }
                            }
                            let candidates: Vec<_> = {
                                let state = lock(&inner.state);
                                state
                                    .slots
                                    .iter()
                                    .filter_map(|(_, slot)| slot.upgrade())
                                    .collect()
                            };
                            let now = Instant::now();
                            for slot in candidates {
                                let expired = {
                                    let mut value = lock(&slot.0);
                                    if value.deadline.is_some_and(|deadline| deadline <= now) {
                                        value.deadline = None;
                                        true
                                    } else {
                                        false
                                    }
                                };
                                if expired {
                                    wake(&slot);
                                }
                            }
                        }
                    })
                    .map_err(|_| Error::PLATFORM)?,
            )
        } else {
            None
        };
        Ok(Self { shared, worker })
    }
    fn acquire() -> Result<Arc<Self>> {
        static CURRENT: OnceLock<Mutex<Weak<Reactor>>> = OnceLock::new();
        let mut current = lock(CURRENT.get_or_init(|| Mutex::new(Weak::new())));
        if let Some(reactor) = current.upgrade() {
            return Ok(reactor);
        }
        let reactor = Arc::new(Self::create(true)?);
        *current = Arc::downgrade(&reactor);
        Ok(reactor)
    }
    fn register(&self, deadline: Option<Instant>) -> Result<(u64, Arc<Slot>)> {
        let mut state = lock(&self.shared.state);
        state.slots.retain(|(_, slot)| slot.strong_count() != 0);
        if state.slots.len() == MAX_WAITERS {
            return Err(Error::WOULD_BLOCK);
        }
        let id = state.next;
        state.next = state.next.checked_add(1).ok_or(Error::TOO_LARGE)?;
        let slot = Arc::new(Slot(Mutex::new(SlotState {
            waker: None,
            deadline,
        })));
        state.slots.push((id, Arc::downgrade(&slot)));
        drop(state);
        if deadline.is_some() {
            unsafe { sys::sc_notifier_interrupt(self.shared.native.0.as_ptr()) };
        }
        Ok((id, slot))
    }
}
impl Drop for Reactor {
    fn drop(&mut self) {
        unsafe { sys::sc_notifier_close(self.shared.native.0.as_ptr()) };
        if let Some(worker) = self.worker.take() {
            // A Waker can drop the final owner on this bridge. Shared remains
            // alive until this worker sees the closed mailbox and returns.
            if worker.thread().id() != thread::current().id() {
                let _ = worker.join();
            }
        }
    }
}
struct Subscription(NonNull<sys::sc_subscription>);
unsafe impl Send for Subscription {}
unsafe impl Sync for Subscription {}
impl Drop for Subscription {
    fn drop(&mut self) {
        unsafe { sys::sc_subscription_destroy(self.0.as_ptr()) };
    }
}
pub(crate) struct PollFuture<F, S> {
    operation: F,
    subscribe: S,
    slot: Arc<Slot>,
    reactor: Arc<Reactor>,
    subscription: Option<Subscription>,
    key: u64,
    done: bool,
}
pub(crate) fn poll_fn<F, S, T>(operation: F, subscribe: S) -> Result<PollFuture<F, S>>
where
    F: FnMut() -> Result<T>,
    S: FnMut(*mut sys::sc_notifier, u64, *mut *mut sys::sc_subscription) -> sys::sc_status,
{
    poll_fn_deadline(operation, subscribe, None)
}

pub(crate) fn poll_fn_deadline<F, S, T>(
    operation: F,
    subscribe: S,
    deadline: Option<Instant>,
) -> Result<PollFuture<F, S>>
where
    F: FnMut() -> Result<T>,
    S: FnMut(*mut sys::sc_notifier, u64, *mut *mut sys::sc_subscription) -> sys::sc_status,
{
    let reactor = Reactor::acquire()?;
    let (key, slot) = reactor.register(deadline)?;
    Ok(PollFuture {
        operation,
        subscribe,
        slot,
        reactor,
        subscription: None,
        key,
        done: false,
    })
}
impl<F, S, T> Future for PollFuture<F, S>
where
    F: FnMut() -> Result<T> + Unpin,
    S: FnMut(*mut sys::sc_notifier, u64, *mut *mut sys::sc_subscription) -> sys::sc_status + Unpin,
{
    type Output = Result<T>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        assert!(!this.done, "completed ServerCore Future polled again");
        let waker = cx.waker().clone();
        let previous = lock(&this.slot.0).waker.replace(waker);
        drop(previous);
        // Re-arm before checking native state. Registration also checks state
        // after publication; neither transition window can lose a notification.
        this.subscription.take();
        let mut subscription = std::ptr::null_mut();
        let registered = check((this.subscribe)(
            this.reactor.shared.native.0.as_ptr(),
            this.key,
            &mut subscription,
        ))
        .and_then(|()| pointer(subscription));
        let result = match registered {
            Ok(value) => {
                this.subscription = Some(Subscription(value));
                (this.operation)()
            }
            Err(error) => Err(error),
        };
        match result {
            Err(Error::WOULD_BLOCK) if this.subscription.is_some() => Poll::Pending,
            result => {
                this.subscription.take();
                let previous = lock(&this.slot.0).waker.take();
                drop(previous);
                this.done = true;
                Poll::Ready(result)
            }
        }
    }
}
impl<F, S> Drop for PollFuture<F, S> {
    fn drop(&mut self) {
        let previous = lock(&self.slot.0).waker.take();
        drop(previous);
        {
            let mut state = lock(&self.reactor.shared.state);
            state.slots.retain(|(id, _)| *id != self.key);
        }
        self.subscription.take();
        unsafe { sys::sc_notifier_interrupt(self.reactor.shared.native.0.as_ptr()) };
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        sync::{
            atomic::{AtomicBool, AtomicUsize, Ordering},
            Condvar,
        },
        task::Wake,
        time::Duration,
    };
    struct TestSource {
        handle: NonNull<sys::sc_tcp_server>,
        stopped: AtomicBool,
    }
    unsafe impl Send for TestSource {}
    unsafe impl Sync for TestSource {}
    impl TestSource {
        fn new() -> Self {
            let mut options = std::mem::MaybeUninit::<sys::sc_tcp_options>::uninit();
            assert_eq!(
                unsafe {
                    sys::sc_tcp_options_init(
                        options.as_mut_ptr(),
                        std::mem::size_of::<sys::sc_tcp_options>(),
                    )
                },
                sys::SC_OK
            );
            let options = unsafe { options.assume_init() };
            let mut handle = std::ptr::null_mut();
            assert_eq!(
                unsafe { sys::sc_tcp_server_create(&options, &mut handle) },
                sys::SC_OK
            );
            Self {
                handle: pointer(handle).unwrap(),
                stopped: AtomicBool::new(false),
            }
        }
        fn stop(&self) {
            self.stopped.store(true, Ordering::Release);
            assert_eq!(
                unsafe { sys::sc_tcp_server_stop(self.handle.as_ptr()) },
                sys::SC_OK
            );
        }
        fn subscribe(
            &self,
            notifier: *mut sys::sc_notifier,
            key: u64,
            out: *mut *mut sys::sc_subscription,
        ) -> sys::sc_status {
            unsafe { sys::sc_tcp_server_subscribe(self.handle.as_ptr(), notifier, key, out) }
        }
        fn result(&self) -> Result<()> {
            if self.stopped.load(Ordering::Acquire) {
                Err(Error::CLOSED)
            } else {
                Err(Error::WOULD_BLOCK)
            }
        }
    }
    impl Drop for TestSource {
        fn drop(&mut self) {
            unsafe { sys::sc_tcp_server_destroy(self.handle.as_ptr()) };
        }
    }
    struct WakeCount {
        count: Mutex<usize>,
        changed: Condvar,
    }
    impl Wake for WakeCount {
        fn wake(self: Arc<Self>) {
            self.wake_by_ref();
        }
        fn wake_by_ref(self: &Arc<Self>) {
            *lock(&self.count) += 1;
            self.changed.notify_all();
        }
    }
    #[test]
    fn registrations_are_bounded_and_returned_on_drop() {
        let reactor = Reactor::create(false).unwrap();
        let slots: Vec<_> = (0..MAX_WAITERS)
            .map(|_| reactor.register(None).unwrap())
            .collect();
        assert!(matches!(reactor.register(None), Err(Error::WOULD_BLOCK)));
        drop(slots);
        assert!(reactor.register(None).is_ok());
    }
    #[test]
    fn native_readiness_is_idle_until_signalled_and_drop_unregisters() {
        let source = TestSource::new();
        let count = Arc::new(WakeCount {
            count: Mutex::new(0),
            changed: Condvar::new(),
        });
        let waker = Waker::from(count.clone());
        let mut context = Context::from_waker(&waker);
        let polls = AtomicUsize::new(0);
        let mut future = Box::pin(
            poll_fn(
                || {
                    polls.fetch_add(1, Ordering::Relaxed);
                    source.result()
                },
                |notifier, key, out| source.subscribe(notifier, key, out),
            )
            .unwrap(),
        );
        assert!(future.as_mut().poll(&mut context).is_pending());
        let (observed, timeout) = count
            .changed
            .wait_timeout_while(lock(&count.count), Duration::from_millis(50), |count| {
                *count == 0
            })
            .unwrap();
        assert!(timeout.timed_out());
        assert_eq!(
            *observed, 0,
            "idle native operation must never be polled periodically"
        );
        drop(observed);
        assert_eq!(polls.load(Ordering::Relaxed), 1);
        drop(future);
        source.stop();
        let (observed, timeout) = count
            .changed
            .wait_timeout_while(lock(&count.count), Duration::from_millis(30), |count| {
                *count == 0
            })
            .unwrap();
        assert!(timeout.timed_out());
        assert_eq!(
            *observed, 0,
            "dropped future no longer receives native readiness"
        );
        drop(observed);
        let future = poll_fn(
            || source.result(),
            |notifier, key, out| source.subscribe(notifier, key, out),
        )
        .unwrap();
        assert_eq!(crate::block_on(future), Err(Error::CLOSED));
    }
    #[test]
    fn notification_between_registration_and_state_recheck_is_not_lost() {
        let source = TestSource::new();
        let mut first = true;
        let future = poll_fn(
            || {
                if std::mem::take(&mut first) {
                    source.stop();
                    // Deliberately model a stale state read after notification.
                    Err(Error::WOULD_BLOCK)
                } else {
                    source.result()
                }
            },
            |notifier, key, out| source.subscribe(notifier, key, out),
        )
        .unwrap();
        assert_eq!(crate::block_on(future), Err(Error::CLOSED));
    }
}
