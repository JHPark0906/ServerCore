use crate::{Error, Result};
use std::{
    future::Future,
    pin::Pin,
    sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock, Weak},
    task::{Context, Poll, Waker},
    thread::{self, JoinHandle},
    time::Duration,
};

const MAX_WAITERS: usize = 4096;
struct Slot(Mutex<Option<Waker>>);
struct State {
    stopping: bool,
    slots: Vec<Weak<Slot>>,
}
struct Shared {
    state: Mutex<State>,
    changed: Condvar,
}
struct Reactor {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}
/// Keeps the one shared worker alive across sequential awaits on a live owner.
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

impl Reactor {
    fn acquire() -> Result<Arc<Self>> {
        static CURRENT: OnceLock<Mutex<Weak<Reactor>>> = OnceLock::new();
        let mut current = lock(CURRENT.get_or_init(|| Mutex::new(Weak::new())));
        if let Some(reactor) = current.upgrade() {
            return Ok(reactor);
        }
        let shared = Arc::new(Shared {
            state: Mutex::new(State {
                stopping: false,
                slots: Vec::new(),
            }),
            changed: Condvar::new(),
        });
        let inner = shared.clone();
        let worker = thread::Builder::new()
            .name("servercore-readiness".into())
            .spawn(move || {
                loop {
                    let slots = {
                        let mut state = lock(&inner.state);
                        if state.stopping {
                            break;
                        }
                        state.slots.retain(|slot| slot.strong_count() != 0);
                        if state.slots.is_empty() {
                            state = inner.changed.wait(state).unwrap_or_else(|e| e.into_inner());
                        } else {
                            state = inner
                                .changed
                                .wait_timeout(state, Duration::from_millis(10))
                                .unwrap_or_else(|e| e.into_inner())
                                .0;
                        }
                        if state.stopping {
                            break;
                        }
                        state
                            .slots
                            .iter()
                            .filter_map(Weak::upgrade)
                            .collect::<Vec<_>>()
                    };
                    for slot in slots {
                        // Take instead of cloning: custom RawWaker clone/drop code
                        // must not run under the slot mutex. Poll installs a fresh
                        // waker if the operation is still pending after this wake.
                        let waker = lock(&slot.0).take();
                        if let Some(waker) = waker {
                            // A custom executor's Wake may panic; keep other waiters
                            // progressing and never hold a registry lock while waking.
                            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                                waker.wake()
                            }));
                        }
                    }
                }
            })
            .map_err(|_| Error::PLATFORM)?;
        let reactor = Arc::new(Self {
            shared,
            worker: Some(worker),
        });
        *current = Arc::downgrade(&reactor);
        Ok(reactor)
    }
    fn register(&self) -> Result<Arc<Slot>> {
        let mut state = lock(&self.shared.state);
        state.slots.retain(|slot| slot.strong_count() != 0);
        if state.slots.len() == MAX_WAITERS {
            return Err(Error::WOULD_BLOCK);
        }
        let slot = Arc::new(Slot(Mutex::new(None)));
        state.slots.push(Arc::downgrade(&slot));
        self.shared.changed.notify_one();
        Ok(slot)
    }
}
impl Drop for Reactor {
    fn drop(&mut self) {
        {
            let mut state = lock(&self.shared.state);
            state.stopping = true;
            self.shared.changed.notify_one();
        }
        if let Some(worker) = self.worker.take() {
            // A user Waker can drop its final Future on this very worker. The
            // thread owns only Shared, so detaching in that case remains safe;
            // it observes stopping after the current wake and exits.
            if worker.thread().id() != thread::current().id() {
                let _ = worker.join();
            }
        }
    }
}

pub(crate) struct PollFuture<F> {
    operation: F,
    slot: Arc<Slot>,
    _reactor: Arc<Reactor>,
    done: bool,
}
pub(crate) fn poll_fn<F, T>(operation: F) -> Result<PollFuture<F>>
where
    F: FnMut() -> Result<T>,
{
    let reactor = Reactor::acquire()?;
    let slot = reactor.register()?;
    Ok(PollFuture {
        operation,
        slot,
        _reactor: reactor,
        done: false,
    })
}
impl<F, T> Future for PollFuture<F>
where
    F: FnMut() -> Result<T> + Unpin,
{
    type Output = Result<T>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        assert!(!this.done, "completed ServerCore Future polled again");
        // Install before polling native state; readiness cannot be lost between
        // the unsuccessful poll and registration. The reactor never owns handles.
        let waker = cx.waker().clone();
        let previous = lock(&this.slot.0).replace(waker);
        drop(previous);
        match (this.operation)() {
            Err(error) if error == Error::WOULD_BLOCK => Poll::Pending,
            result => {
                let previous = lock(&this.slot.0).take();
                drop(previous);
                this.done = true;
                Poll::Ready(result)
            }
        }
    }
}
impl<F> Drop for PollFuture<F> {
    fn drop(&mut self) {
        let previous = lock(&self.slot.0).take();
        drop(previous);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn registrations_are_bounded_and_returned_on_drop() {
        // Isolate the capacity test from other Cargo tests using the global
        // reactor concurrently, and avoid a worker temporarily retaining slots.
        let reactor = Reactor {
            shared: Arc::new(Shared {
                state: Mutex::new(State {
                    stopping: false,
                    slots: Vec::new(),
                }),
                changed: Condvar::new(),
            }),
            worker: None,
        };
        let slots: Vec<_> = (0..MAX_WAITERS)
            .map(|_| reactor.register().unwrap())
            .collect();
        assert!(matches!(reactor.register(), Err(Error::WOULD_BLOCK)));
        drop(slots);
        assert!(reactor.register().is_ok());
    }
    #[test]
    fn wakes_until_operation_becomes_ready() {
        let state = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let ready = state.clone();
        let worker = thread::spawn(move || {
            thread::sleep(Duration::from_millis(20));
            ready.store(true, std::sync::atomic::Ordering::Release);
        });
        let future = poll_fn(|| {
            if state.load(std::sync::atomic::Ordering::Acquire) {
                Ok(42)
            } else {
                Err(Error::WOULD_BLOCK)
            }
        })
        .unwrap();
        assert_eq!(crate::block_on(future), Ok(42));
        worker.join().unwrap();
    }
}
