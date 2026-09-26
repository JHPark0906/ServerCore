//! Bounded native work executors, keyed serialization, timers and child groups.
//!
//! Callbacks are `Send + 'static` and run on native workers. Cancellation is
//! cooperative; callbacks must finish promptly. Work panics become PLATFORM
//! errors; work and capture-destructor panics never unwind through C. Declared retained bytes must include
//! dynamically owned captures. Async waits use native readiness notifications.
//! Last-owner Drop requests shutdown through a bounded native cleanup worker;
//! use `stop` outside callbacks when a completed join is required. Task handles
//! only observe work: dropping a task handle does not cancel its job.
use crate::{
    callback_status, check, pointer, reactor, sys, timeout_ms, verify_abi, Error, Result,
};
use std::{
    ffi::c_void,
    marker::PhantomData,
    mem::{size_of, MaybeUninit},
    panic::{catch_unwind, AssertUnwindSafe},
    ptr::NonNull,
    sync::{Arc, Mutex},
    time::Duration,
};

fn defaults<T>(init: unsafe extern "C" fn(*mut T, usize) -> sys::sc_status) -> T {
    let mut value = MaybeUninit::<T>::uninit();
    assert_eq!(
        unsafe { init(value.as_mut_ptr(), size_of::<T>()) },
        sys::SC_OK
    );
    unsafe { value.assume_init() }
}
fn deadline(value: Option<Duration>) -> Result<u32> {
    value
        .map(timeout_ms)
        .transpose()
        .map(|value| value.unwrap_or(u32::MAX))
}
fn contain<T>(body: impl FnOnce() -> T) -> std::result::Result<T, ()> {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => Ok(value),
        // Release ordinary payloads. A pathological payload destructor may
        // itself panic; only that secondary payload is deliberately leaked.
        Err(payload) => {
            if let Err(secondary) = catch_unwind(AssertUnwindSafe(|| drop(payload))) {
                std::mem::forget(secondary);
            }
            Err(())
        }
    }
}
/// Borrowed only for the current work invocation. It cannot be retained by a
/// 'static closure or used after the native callback returns.
pub struct CancellationToken<'a> {
    handle: *const sys::sc_runtime_token,
    _borrow: PhantomData<&'a sys::sc_runtime_token>,
}
impl CancellationToken<'_> {
    pub fn is_cancelled(&self) -> bool {
        unsafe { sys::sc_runtime_token_requested(self.handle) != 0 }
    }
}
struct Once<F>(Option<F>);
unsafe extern "C" fn run_once<F>(
    context: *mut c_void,
    token: *const sys::sc_runtime_token,
) -> sys::sc_status
where
    F: for<'a> FnOnce(CancellationToken<'a>) -> Result<()> + Send + 'static,
{
    match contain(|| {
        // Native invokes once, retains context through invocation, then releases.
        let work = unsafe { &mut *context.cast::<Once<F>>() }
            .0
            .take()
            .ok_or(Error::CLOSED)?;
        work(CancellationToken {
            handle: token,
            _borrow: PhantomData,
        })
    }) {
        Ok(result) => callback_status(result),
        Err(()) => sys::SC_PLATFORM_ERROR,
    }
}
unsafe extern "C" fn run_repeat<F>(
    context: *mut c_void,
    token: *const sys::sc_runtime_token,
) -> sys::sc_status
where
    F: for<'a> FnMut(CancellationToken<'a>) -> Result<()> + Send + 'static,
{
    match contain(|| {
        let work = unsafe { &*context.cast::<Mutex<F>>() };
        let mut work = work.lock().unwrap_or_else(|error| error.into_inner());
        work(CancellationToken {
            handle: token,
            _borrow: PhantomData,
        })
    }) {
        Ok(result) => callback_status(result),
        Err(()) => sys::SC_PLATFORM_ERROR,
    }
}
unsafe extern "C" fn release<T>(context: *mut c_void) {
    let _ = contain(|| {
        drop(unsafe { Box::from_raw(context.cast::<T>()) });
    });
}
fn once<F>(work: F) -> sys::sc_runtime_work
where
    F: for<'a> FnOnce(CancellationToken<'a>) -> Result<()> + Send + 'static,
{
    sys::sc_runtime_work {
        context: Box::into_raw(Box::new(Once(Some(work)))).cast(),
        work: Some(run_once::<F>),
        release: Some(release::<Once<F>>),
    }
}
fn repeat<F>(work: F) -> sys::sc_runtime_work
where
    F: for<'a> FnMut(CancellationToken<'a>) -> Result<()> + Send + 'static,
{
    sys::sc_runtime_work {
        context: Box::into_raw(Box::new(Mutex::new(work))).cast(),
        work: Some(run_repeat::<F>),
        release: Some(release::<Mutex<F>>),
    }
}
struct CancellationInner(NonNull<sys::sc_cancellation>);
unsafe impl Send for CancellationInner {}
unsafe impl Sync for CancellationInner {}
impl Drop for CancellationInner {
    fn drop(&mut self) {
        unsafe { sys::sc_cancellation_destroy(self.0.as_ptr()) };
    }
}
#[derive(Clone)]
pub struct Cancellation(Arc<CancellationInner>);
impl Cancellation {
    pub fn new() -> Result<Self> {
        verify_abi()?;
        let mut value = std::ptr::null_mut();
        check(unsafe { sys::sc_cancellation_create(&mut value) })?;
        Ok(Self(Arc::new(CancellationInner(pointer(value)?))))
    }
    pub fn request(&self) {
        unsafe { sys::sc_cancellation_request(self.0 .0.as_ptr()) };
    }
    pub fn is_cancelled(&self) -> bool {
        unsafe { sys::sc_cancellation_requested(self.0 .0.as_ptr()) != 0 }
    }
}
fn parent(value: &Option<Cancellation>) -> *const sys::sc_cancellation {
    value
        .as_ref()
        .map_or(std::ptr::null(), |value| value.0 .0.as_ptr())
}
#[derive(Clone, Debug)]
pub struct ExecutorOptions {
    pub worker_count: usize,
    pub max_pending_tasks: usize,
    pub max_retained_bytes: usize,
}
impl Default for ExecutorOptions {
    fn default() -> Self {
        let value = defaults(sys::sc_executor_options_init);
        Self {
            worker_count: value.worker_count,
            max_pending_tasks: value.max_pending_tasks,
            max_retained_bytes: value.max_retained_bytes,
        }
    }
}
#[derive(Clone, Debug)]
pub struct KeyedExecutorOptions {
    pub worker_count: usize,
    pub max_keys: usize,
    pub max_outstanding_tasks: usize,
    pub max_retained_bytes: usize,
    pub max_outstanding_tasks_per_key: usize,
    pub max_retained_bytes_per_key: usize,
}
impl Default for KeyedExecutorOptions {
    fn default() -> Self {
        let value = defaults(sys::sc_keyed_executor_options_init);
        Self {
            worker_count: value.worker_count,
            max_keys: value.max_keys,
            max_outstanding_tasks: value.max_outstanding_tasks,
            max_retained_bytes: value.max_retained_bytes,
            max_outstanding_tasks_per_key: value.max_outstanding_tasks_per_key,
            max_retained_bytes_per_key: value.max_retained_bytes_per_key,
        }
    }
}
#[derive(Clone, Default)]
pub struct TaskOptions {
    pub retained_bytes: usize,
    pub parent: Option<Cancellation>,
    /// Relative deadline from submission; None has no deadline.
    pub deadline: Option<Duration>,
}
impl TaskOptions {
    fn raw(&self) -> Result<sys::sc_task_options> {
        Ok(sys::sc_task_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_task_options>() as u32,
            retained_bytes: self.retained_bytes,
            parent: parent(&self.parent),
            deadline_ms: deadline(self.deadline)?,
        })
    }
}
#[derive(Clone, Debug)]
pub struct TimerSchedulerOptions {
    pub max_timers: usize,
    pub max_retained_bytes: usize,
}
impl Default for TimerSchedulerOptions {
    fn default() -> Self {
        let value = defaults(sys::sc_timer_scheduler_options_init);
        Self {
            max_timers: value.max_timers,
            max_retained_bytes: value.max_retained_bytes,
        }
    }
}
#[derive(Clone, Default)]
pub struct TimerOptions {
    pub delay: Duration,
    pub retained_bytes: usize,
    pub parent: Option<Cancellation>,
}
impl TimerOptions {
    fn raw(&self, interval: Duration) -> Result<sys::sc_timer_options> {
        Ok(sys::sc_timer_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_timer_options>() as u32,
            retained_bytes: self.retained_bytes,
            parent: parent(&self.parent),
            delay_ms: timeout_ms(self.delay)?,
            repeat_ms: timeout_ms(interval)?,
        })
    }
}
#[derive(Clone)]
pub struct TaskGroupOptions {
    pub max_children: usize,
    pub max_retained_bytes: usize,
    pub parent: Option<Cancellation>,
    pub deadline: Option<Duration>,
}
impl Default for TaskGroupOptions {
    fn default() -> Self {
        let value = defaults(sys::sc_task_group_options_init);
        Self {
            max_children: value.max_children,
            max_retained_bytes: value.max_retained_bytes,
            parent: None,
            deadline: None,
        }
    }
}
// C owner cleanup is deferred and bounded, so final Arc drop from that owner's
// own callback is safe. Explicit stop remains the quiescent control-thread API.
macro_rules! owned {
    ($name:ident, $inner:ident, $native:ident, $destroy:ident) => {
        struct $inner {
            handle: NonNull<sys::$native>,
            _readiness: reactor::Lease,
        }
        unsafe impl Send for $inner {}
        unsafe impl Sync for $inner {}
        impl Drop for $inner {
            fn drop(&mut self) {
                unsafe { sys::$destroy(self.handle.as_ptr()) };
            }
        }
        #[derive(Clone)]
        pub struct $name(Arc<$inner>);
        impl $name {
            fn from_raw(value: *mut sys::$native, readiness: reactor::Lease) -> Result<Self> {
                Ok(Self(Arc::new($inner {
                    handle: pointer(value)?,
                    _readiness: readiness,
                })))
            }
            pub(crate) fn native_handle(&self) -> *mut sys::$native {
                self.0.handle.as_ptr()
            }
        }
    };
}
owned!(Executor, ExecutorInner, sc_executor, sc_executor_destroy);
owned!(
    KeyedExecutor,
    KeyedExecutorInner,
    sc_keyed_executor,
    sc_keyed_executor_destroy
);
owned!(
    TimerScheduler,
    TimerInner,
    sc_timer_scheduler,
    sc_timer_scheduler_destroy
);
owned!(TaskGroup, GroupInner, sc_task_group, sc_task_group_destroy);
owned!(Task, TaskInner, sc_runtime_task, sc_runtime_task_destroy);
impl Executor {
    pub fn new(options: &ExecutorOptions) -> Result<Self> {
        verify_abi()?;
        let lease = reactor::Lease::new()?;
        let options = sys::sc_executor_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_executor_options>() as u32,
            worker_count: options.worker_count,
            max_pending_tasks: options.max_pending_tasks,
            max_retained_bytes: options.max_retained_bytes,
        };
        let mut value = std::ptr::null_mut();
        check(unsafe { sys::sc_executor_create(&options, &mut value) })?;
        Self::from_raw(value, lease)
    }
    pub fn submit<F>(&self, options: &TaskOptions, work: F) -> Result<Task>
    where
        F: for<'a> FnOnce(CancellationToken<'a>) -> Result<()> + Send + 'static,
    {
        let options = options.raw()?;
        let mut task = std::ptr::null_mut();
        check(unsafe {
            sys::sc_executor_submit(self.native_handle(), once(work), &options, &mut task)
        })?;
        Task::from_raw(task, self.0._readiness.clone())
    }
    pub fn request_stop(&self) {
        unsafe { sys::sc_executor_request_stop(self.native_handle()) };
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { sys::sc_executor_stop(self.native_handle()) })
    }
}
impl KeyedExecutor {
    pub fn new(options: &KeyedExecutorOptions) -> Result<Self> {
        verify_abi()?;
        let lease = reactor::Lease::new()?;
        let options = sys::sc_keyed_executor_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_keyed_executor_options>() as u32,
            worker_count: options.worker_count,
            max_keys: options.max_keys,
            max_outstanding_tasks: options.max_outstanding_tasks,
            max_retained_bytes: options.max_retained_bytes,
            max_outstanding_tasks_per_key: options.max_outstanding_tasks_per_key,
            max_retained_bytes_per_key: options.max_retained_bytes_per_key,
        };
        let mut value = std::ptr::null_mut();
        check(unsafe { sys::sc_keyed_executor_create(&options, &mut value) })?;
        Self::from_raw(value, lease)
    }
    pub fn submit<F>(&self, key: u64, options: &TaskOptions, work: F) -> Result<Task>
    where
        F: for<'a> FnOnce(CancellationToken<'a>) -> Result<()> + Send + 'static,
    {
        let options = options.raw()?;
        let mut task = std::ptr::null_mut();
        check(unsafe {
            sys::sc_keyed_executor_submit(
                self.native_handle(),
                key,
                once(work),
                &options,
                &mut task,
            )
        })?;
        Task::from_raw(task, self.0._readiness.clone())
    }
    pub fn request_stop(&self) {
        unsafe { sys::sc_keyed_executor_request_stop(self.native_handle()) };
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { sys::sc_keyed_executor_stop(self.native_handle()) })
    }
}
impl TimerScheduler {
    pub fn new(executor: &Executor, options: &TimerSchedulerOptions) -> Result<Self> {
        let lease = reactor::Lease::new()?;
        let options = sys::sc_timer_scheduler_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_timer_scheduler_options>() as u32,
            max_timers: options.max_timers,
            max_retained_bytes: options.max_retained_bytes,
        };
        let mut value = std::ptr::null_mut();
        check(unsafe {
            sys::sc_timer_scheduler_create(executor.native_handle(), &options, &mut value)
        })?;
        Self::from_raw(value, lease)
    }
    pub fn schedule<F>(&self, options: &TimerOptions, work: F) -> Result<Task>
    where
        F: for<'a> FnOnce(CancellationToken<'a>) -> Result<()> + Send + 'static,
    {
        let options = options.raw(Duration::ZERO)?;
        self.schedule_raw(options, once(work))
    }
    /// Fixed delay after each completion; invocations never overlap. Returning
    /// an error (including a panic) terminates repetition.
    pub fn repeat<F>(&self, options: &TimerOptions, interval: Duration, work: F) -> Result<Task>
    where
        F: for<'a> FnMut(CancellationToken<'a>) -> Result<()> + Send + 'static,
    {
        if interval.is_zero() {
            return Err(Error::INVALID_ARGUMENT);
        }
        let options = options.raw(interval)?;
        self.schedule_raw(options, repeat(work))
    }
    fn schedule_raw(
        &self,
        options: sys::sc_timer_options,
        work: sys::sc_runtime_work,
    ) -> Result<Task> {
        let mut task = std::ptr::null_mut();
        check(unsafe {
            sys::sc_timer_scheduler_schedule(self.native_handle(), work, &options, &mut task)
        })?;
        Task::from_raw(task, self.0._readiness.clone())
    }
    pub fn request_stop(&self) {
        unsafe { sys::sc_timer_scheduler_request_stop(self.native_handle()) };
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { sys::sc_timer_scheduler_stop(self.native_handle()) })
    }
}
pub struct GroupTask {
    pub id: u64,
    pub task: Task,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Completion {
    pub id: u64,
    pub status: Result<()>,
}
struct CompletionList(NonNull<sys::sc_group_completions>);
impl Drop for CompletionList {
    fn drop(&mut self) {
        unsafe { sys::sc_group_completions_destroy(self.0.as_ptr()) };
    }
}
impl TaskGroup {
    pub fn new(executor: &Executor, options: &TaskGroupOptions) -> Result<Self> {
        let lease = reactor::Lease::new()?;
        let options = sys::sc_task_group_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_task_group_options>() as u32,
            max_children: options.max_children,
            max_retained_bytes: options.max_retained_bytes,
            parent: parent(&options.parent),
            deadline_ms: deadline(options.deadline)?,
        };
        let mut value = std::ptr::null_mut();
        check(unsafe {
            sys::sc_task_group_create(executor.native_handle(), &options, &mut value)
        })?;
        Self::from_raw(value, lease)
    }
    pub fn submit<F>(&self, options: &TaskOptions, work: F) -> Result<GroupTask>
    where
        F: for<'a> FnOnce(CancellationToken<'a>) -> Result<()> + Send + 'static,
    {
        let options = options.raw()?;
        let mut task = std::ptr::null_mut();
        let mut id = 0;
        check(unsafe {
            sys::sc_task_group_submit(
                self.native_handle(),
                once(work),
                &options,
                &mut id,
                &mut task,
            )
        })?;
        Ok(GroupTask {
            id,
            task: Task::from_raw(task, self.0._readiness.clone())?,
        })
    }
    pub fn close(&self) {
        unsafe { sys::sc_task_group_close(self.native_handle()) };
    }
    pub fn cancel(&self) {
        unsafe { sys::sc_task_group_cancel(self.native_handle()) };
    }
    pub fn result(&self) -> Result<()> {
        check(unsafe { sys::sc_task_group_result(self.native_handle()) })
    }
    pub fn wait_timeout(&self, timeout: Duration) -> Result<()> {
        check(unsafe { sys::sc_task_group_wait(self.native_handle(), timeout_ms(timeout)?) })
    }
    /// Closes admission and awaits all children; inspect child results separately.
    pub async fn wait(&self) -> Result<()> {
        reactor::poll_fn(
            || self.result(),
            |notifier, key, out| unsafe {
                sys::sc_task_group_subscribe(self.native_handle(), notifier, key, out)
            },
        )?
        .await
    }
    pub fn take_completions(&self) -> Result<Vec<Completion>> {
        let mut value = std::ptr::null_mut();
        check(unsafe { sys::sc_task_group_take_completions(self.native_handle(), &mut value) })?;
        let list = CompletionList(pointer(value)?);
        let mut count = 0;
        let data = unsafe { sys::sc_group_completions_data(list.0.as_ptr(), &mut count) };
        let records = if count == 0 {
            &[]
        } else {
            if data.is_null() {
                return Err(Error::INVALID_FORMAT);
            }
            unsafe { std::slice::from_raw_parts(data, count) }
        };
        Ok(records
            .iter()
            .map(|value| Completion {
                id: value.id,
                status: check(value.status),
            })
            .collect())
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { sys::sc_task_group_stop(self.native_handle()) })
    }
}
impl Task {
    pub fn cancel(&self) {
        unsafe { sys::sc_runtime_task_cancel(self.native_handle()) };
    }
    /// Distinguishes pending work from a callback's terminal WouldBlock error.
    pub fn is_finished(&self) -> bool {
        unsafe { sys::sc_runtime_task_finished(self.native_handle()) != 0 }
    }
    pub fn result(&self) -> Result<()> {
        check(unsafe { sys::sc_runtime_task_result(self.native_handle()) })
    }
    pub fn wait_timeout(&self, timeout: Duration) -> Result<()> {
        check(unsafe { sys::sc_runtime_task_wait(self.native_handle(), timeout_ms(timeout)?) })
    }
    /// Dropping this future suppresses only observation, not the underlying task.
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
                sys::sc_runtime_task_subscribe(self.native_handle(), notifier, key, out)
            },
        )?
        .await?
    }
    /// Only pending timer tasks can be rescheduled; running timers return WouldBlock.
    pub fn reschedule(&self, delay: Duration) -> Result<()> {
        check(unsafe { sys::sc_runtime_task_reschedule(self.native_handle(), timeout_ms(delay)?) })
    }
}

/// Permanently closes native runtime admission and waits for deferred owner
/// cleanup. Drop every runtime owner first. A timeout permits retry; this must
/// run on a control thread, never inside work or capture destructors. Normally
/// applications use owner `stop` methods; shutdown is for unloading the DLL.
pub fn shutdown(timeout: Duration) -> Result<()> {
    check(unsafe { sys::sc_runtime_shutdown(timeout_ms(timeout)?) })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        future::Future,
        sync::{
            atomic::{AtomicUsize, Ordering},
            mpsc,
        },
        task::{Context, Poll, Wake, Waker},
        thread,
        time::Instant,
    };

    const LIMIT: Duration = Duration::from_secs(5);
    struct ThreadWake(thread::Thread);
    impl Wake for ThreadWake {
        fn wake(self: Arc<Self>) {
            self.0.unpark();
        }
        fn wake_by_ref(self: &Arc<Self>) {
            self.0.unpark();
        }
    }
    // Bounded executor for tests: a lost native wake fails instead of hanging.
    fn await_ready<F: Future + Send>(future: F) -> F::Output {
        let waker = Waker::from(Arc::new(ThreadWake(thread::current())));
        let mut context = Context::from_waker(&waker);
        let mut future = Box::pin(future);
        let deadline = Instant::now() + LIMIT;
        loop {
            assert!(
                Instant::now() < deadline,
                "native readiness did not wake the future"
            );
            match future.as_mut().poll(&mut context) {
                Poll::Ready(value) => return value,
                Poll::Pending => {
                    thread::park_timeout(deadline.saturating_duration_since(Instant::now()))
                }
            }
        }
    }
    struct CountDrop(Arc<AtomicUsize>);
    impl Drop for CountDrop {
        fn drop(&mut self) {
            self.0.fetch_add(1, Ordering::SeqCst);
        }
    }
    fn executor() -> Executor {
        Executor::new(&ExecutorOptions {
            worker_count: 1,
            max_pending_tasks: 1,
            max_retained_bytes: 1024,
        })
        .unwrap()
    }
    #[test]
    fn executor_rejection_cancellation_and_terminal_errors_release_captures() {
        let executor = executor();
        let (started, entered) = mpsc::channel();
        let (release, resume) = mpsc::channel();
        let first = executor
            .submit(&TaskOptions::default(), move |_| {
                started.send(()).unwrap();
                resume.recv_timeout(LIMIT).unwrap();
                Ok(())
            })
            .unwrap();
        entered.recv_timeout(LIMIT).unwrap();
        let drops = Arc::new(AtomicUsize::new(0));
        let capture = CountDrop(drops.clone());
        let cancelled = executor
            .submit(&TaskOptions::default(), move |_| {
                let _capture = capture;
                panic!("cancelled queued job must never run");
            })
            .unwrap();
        let capture = CountDrop(drops.clone());
        assert!(matches!(
            executor.submit(&TaskOptions::default(), move |_| {
                let _capture = capture;
                Ok(())
            }),
            Err(Error::WOULD_BLOCK)
        ));
        assert_eq!(
            drops.load(Ordering::SeqCst),
            1,
            "rejected capture released before submit returns"
        );
        cancelled.cancel();
        assert_eq!(await_ready(cancelled.wait()), Err(Error::CANCELLED));
        assert_eq!(
            drops.load(Ordering::SeqCst),
            2,
            "cancelled capture released before terminal readiness"
        );
        release.send(()).unwrap();
        await_ready(first.wait()).unwrap();
        let terminal = executor
            .submit(&TaskOptions::default(), |_| Err(Error::WOULD_BLOCK))
            .unwrap();
        assert_eq!(await_ready(terminal.wait()), Err(Error::WOULD_BLOCK));
        assert!(terminal.is_finished());
        executor.stop().unwrap();
    }
    #[test]
    fn callback_error_carrying_the_ok_status_is_not_success() {
        let executor = executor();
        let task = executor
            .submit(&TaskOptions::default(), |_| Err(Error(sys::SC_OK)))
            .unwrap();
        assert_eq!(await_ready(task.wait()), Err(Error::INVALID_ARGUMENT));
        executor.stop().unwrap();
    }
    #[test]
    fn callback_panics_and_panicking_capture_drops_stay_inside_ffi() {
        let executor = executor();
        let payload_drops = Arc::new(AtomicUsize::new(0));
        let payload = CountDrop(payload_drops.clone());
        let panic = executor
            .submit(&TaskOptions::default(), move |_| {
                std::panic::panic_any(payload)
            })
            .unwrap();
        assert_eq!(await_ready(panic.wait()), Err(Error::PLATFORM));
        assert_eq!(
            payload_drops.load(Ordering::SeqCst),
            1,
            "ordinary panic payload is destroyed"
        );
        executor.request_stop();
        struct PanicDrop(Arc<AtomicUsize>);
        impl Drop for PanicDrop {
            fn drop(&mut self) {
                self.0.fetch_add(1, Ordering::SeqCst);
                panic!("capture destructor panic is contained");
            }
        }
        let drops = Arc::new(AtomicUsize::new(0));
        let capture = PanicDrop(drops.clone());
        assert!(matches!(
            executor.submit(&TaskOptions::default(), move |_| {
                let _capture = capture;
                Ok(())
            }),
            Err(Error::CLOSED)
        ));
        assert_eq!(drops.load(Ordering::SeqCst), 1);
        executor.stop().unwrap();
    }
    #[test]
    fn last_executor_owner_can_drop_inside_its_own_callback() {
        let executor = executor();
        let captured_owner = executor.clone();
        let capture = Arc::new(());
        let weak = Arc::downgrade(&capture);
        let (started, entered) = mpsc::channel();
        let (release, resume) = mpsc::channel();
        let task = executor
            .submit(&TaskOptions::default(), move |_| {
                let _capture = capture;
                started.send(()).unwrap();
                resume.recv_timeout(LIMIT).unwrap();
                assert_eq!(captured_owner.stop(), Err(Error::INVALID_ARGUMENT));
                drop(captured_owner);
                Ok(())
            })
            .unwrap();
        entered.recv_timeout(LIMIT).unwrap();
        drop(executor);
        release.send(()).unwrap();
        let result = await_ready(task.wait());
        assert!(matches!(result, Ok(()) | Err(Error::CANCELLED)));
        assert!(weak.upgrade().is_none());
    }
    #[test]
    fn keyed_fifo_and_independent_keys_progress() {
        let executor = KeyedExecutor::new(&KeyedExecutorOptions {
            worker_count: 2,
            ..KeyedExecutorOptions::default()
        })
        .unwrap();
        let order = Arc::new(Mutex::new(Vec::new()));
        let (started, entered) = mpsc::channel();
        let (release, resume) = mpsc::channel();
        let values = order.clone();
        let first = executor
            .submit(7, &TaskOptions::default(), move |_| {
                started.send(()).unwrap();
                resume.recv_timeout(LIMIT).unwrap();
                values.lock().unwrap().push(1);
                Ok(())
            })
            .unwrap();
        entered.recv_timeout(LIMIT).unwrap();
        let values = order.clone();
        let second = executor
            .submit(7, &TaskOptions::default(), move |_| {
                values.lock().unwrap().push(2);
                Ok(())
            })
            .unwrap();
        let independent = executor
            .submit(8, &TaskOptions::default(), |_| Ok(()))
            .unwrap();
        await_ready(independent.wait()).unwrap();
        assert!(order.lock().unwrap().is_empty());
        release.send(()).unwrap();
        await_ready(first.wait()).unwrap();
        await_ready(second.wait()).unwrap();
        assert_eq!(*order.lock().unwrap(), vec![1, 2]);
        executor.stop().unwrap();
    }
    #[test]
    fn groups_collect_results_and_keep_their_executor_alive() {
        let executor = Executor::new(&ExecutorOptions::default()).unwrap();
        let group = TaskGroup::new(&executor, &TaskGroupOptions::default()).unwrap();
        drop(executor);
        let first = group.submit(&TaskOptions::default(), |_| Ok(())).unwrap();
        let second = group
            .submit(&TaskOptions::default(), |_| Err(Error::INVALID_ARGUMENT))
            .unwrap();
        await_ready(group.wait()).unwrap();
        assert_eq!(await_ready(first.task.wait()), Ok(()));
        assert_eq!(
            await_ready(second.task.wait()),
            Err(Error::INVALID_ARGUMENT)
        );
        let mut completions = group.take_completions().unwrap();
        completions.sort_by_key(|record| record.id);
        assert_eq!(
            completions,
            vec![
                Completion {
                    id: first.id,
                    status: Ok(())
                },
                Completion {
                    id: second.id,
                    status: Err(Error::INVALID_ARGUMENT)
                }
            ]
        );
        assert!(group.take_completions().unwrap().is_empty());
        let drops = Arc::new(AtomicUsize::new(0));
        let capture = CountDrop(drops.clone());
        assert!(matches!(
            group.submit(&TaskOptions::default(), move |_| {
                let _capture = capture;
                Ok(())
            }),
            Err(Error::CLOSED)
        ));
        assert_eq!(drops.load(Ordering::SeqCst), 1);
        group.stop().unwrap();
    }
    #[test]
    fn timer_reschedule_repeat_and_parent_cancellation_are_notified() {
        let executor = Executor::new(&ExecutorOptions::default()).unwrap();
        let timers = TimerScheduler::new(&executor, &TimerSchedulerOptions::default()).unwrap();
        drop(executor);
        let pending = TimerOptions {
            delay: Duration::from_secs(30),
            ..TimerOptions::default()
        };
        let hits = Arc::new(AtomicUsize::new(0));
        let record = hits.clone();
        let once = timers
            .schedule(&pending, move |_| {
                record.fetch_add(1, Ordering::SeqCst);
                Ok(())
            })
            .unwrap();
        once.reschedule(Duration::ZERO).unwrap();
        await_ready(once.wait()).unwrap();
        assert_eq!(hits.load(Ordering::SeqCst), 1);
        let (send, receive) = mpsc::channel();
        let mut count = 0;
        let repeated = timers
            .repeat(
                &TimerOptions::default(),
                Duration::from_millis(1),
                move |_| {
                    count += 1;
                    send.send(count).unwrap();
                    if count == 3 {
                        Err(Error::WOULD_BLOCK)
                    } else {
                        Ok(())
                    }
                },
            )
            .unwrap();
        assert_eq!(await_ready(repeated.wait()), Err(Error::WOULD_BLOCK));
        assert_eq!(receive.into_iter().collect::<Vec<_>>(), vec![1, 2, 3]);
        let parent = Cancellation::new().unwrap();
        let drops = Arc::new(AtomicUsize::new(0));
        let capture = CountDrop(drops.clone());
        let cancelled = timers
            .schedule(
                &TimerOptions {
                    parent: Some(parent.clone()),
                    ..pending
                },
                move |_| {
                    let _capture = capture;
                    panic!("cancelled delayed callback must not run");
                },
            )
            .unwrap();
        parent.request();
        assert!(parent.is_cancelled());
        assert_eq!(await_ready(cancelled.wait()), Err(Error::CANCELLED));
        assert_eq!(drops.load(Ordering::SeqCst), 1);
        timers.stop().unwrap();
    }
}
