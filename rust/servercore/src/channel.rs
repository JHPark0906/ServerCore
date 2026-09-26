//! Bounded immutable byte channels backed by native ServerCore queues.
//!
//! FIFO budgets count queued bytes only; receiving releases queue capacity even
//! while the returned [`ChannelValue`] stays alive. Latest-value budgets count
//! only the current snapshot. Consumer-owned snapshots have independent lifetimes.
//! Async methods use native readiness, not periodic polling. Dropping a pending
//! future removes its subscription and never consumes or publishes a value.
use crate::{borrowed_bytes, bytes, check, pointer, reactor, sys, verify_abi, Result};
use std::{mem::size_of, ptr::NonNull};

#[derive(Clone, Copy, Debug)]
pub struct ChannelOptions {
    pub max_messages: usize,
    pub max_retained_bytes: usize,
}
impl Default for ChannelOptions {
    fn default() -> Self {
        Self { max_messages: 128, max_retained_bytes: 4 * 1024 * 1024 }
    }
}

/// An immutable owned message/snapshot. Bytes remain valid after its channel is
/// closed or dropped; borrowed slices cannot outlive this owner.
pub struct ChannelValue {
    handle: NonNull<sys::sc_channel_value>,
    view: sys::sc_bytes,
    version: u64,
}
// Native values never mutate, and safe byte borrows require a live &self.
unsafe impl Send for ChannelValue {}
unsafe impl Sync for ChannelValue {}
impl ChannelValue {
    fn from_raw(raw: *mut sys::sc_channel_value) -> Result<Self> {
        let handle = pointer(raw)?;
        let mut view = sys::sc_bytes { data: std::ptr::null(), len: 0 };
        if let Err(error) = check(unsafe { sys::sc_channel_value_view(handle.as_ptr(), &mut view) }) {
            unsafe { sys::sc_channel_value_destroy(handle.as_ptr()) };
            return Err(error);
        }
        let version = unsafe { sys::sc_channel_value_version(handle.as_ptr()) };
        Ok(Self { handle, view, version })
    }
    pub fn bytes(&self) -> &[u8] { unsafe { borrowed_bytes(self.view) } }
    /// FIFO messages use zero; latest snapshots use increasing publication versions.
    pub fn version(&self) -> u64 { self.version }
}
impl Drop for ChannelValue {
    fn drop(&mut self) { unsafe { sys::sc_channel_value_destroy(self.handle.as_ptr()) }; }
}

/// Shared FIFO queue. `try_clone` retains the same native state. Close rejects
/// new sends but drains accepted messages. There is one pending async waiter in
/// each direction across all clones; a second receives `Error::ALREADY_EXISTS`.
pub struct ByteChannel {
    handle: NonNull<sys::sc_channel>,
    _readiness: reactor::Lease,
}
// Native operations synchronize internally. Borrowed methods/futures prevent
// destruction of their own handle; cloned handles have independent lifetimes.
unsafe impl Send for ByteChannel {}
unsafe impl Sync for ByteChannel {}
impl ByteChannel {
    fn native_handle(&self) -> *mut sys::sc_channel { self.handle.as_ptr() }
    pub fn new(options: ChannelOptions) -> Result<Self> {
        verify_abi()?;
        let options = sys::sc_channel_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_channel_options>() as u32,
            max_messages: options.max_messages,
            max_retained_bytes: options.max_retained_bytes,
        };
        let mut handle = std::ptr::null_mut();
        check(unsafe { sys::sc_channel_create(&options, &mut handle) })?;
        Self::from_raw(handle)
    }
    fn from_raw(raw: *mut sys::sc_channel) -> Result<Self> {
        let handle = pointer(raw)?;
        match reactor::Lease::new() {
            Ok(lease) => Ok(Self { handle, _readiness: lease }),
            Err(error) => {
                unsafe { sys::sc_channel_destroy(handle.as_ptr()) };
                Err(error)
            }
        }
    }
    pub fn try_clone(&self) -> Result<Self> {
        let mut handle = std::ptr::null_mut();
        check(unsafe { sys::sc_channel_retain(self.handle.as_ptr(), &mut handle) })?;
        Self::from_raw(handle)
    }
    /// Copies bytes before returning. Empty messages are allowed.
    pub fn try_send(&self, value: &[u8]) -> Result<()> {
        check(unsafe { sys::sc_channel_try_send(self.handle.as_ptr(), bytes(value)) })
    }
    pub fn try_receive(&self) -> Result<ChannelValue> {
        let mut value = std::ptr::null_mut();
        check(unsafe { sys::sc_channel_try_receive(self.handle.as_ptr(), &mut value) })?;
        ChannelValue::from_raw(value)
    }
    pub async fn send(&self, value: &[u8]) -> Result<()> {
        reactor::poll_fn(|| self.try_send(value), |notifier, key, out| unsafe {
            sys::sc_channel_subscribe_write(self.native_handle(), value.len(), notifier, key, out)
        })?.await
    }
    pub async fn receive(&self) -> Result<ChannelValue> {
        reactor::poll_fn(|| self.try_receive(), |notifier, key, out| unsafe {
            sys::sc_channel_subscribe_read(self.native_handle(), notifier, key, out)
        })?.await
    }
    pub fn len(&self) -> usize { unsafe { sys::sc_channel_size(self.handle.as_ptr()) } }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn retained_bytes(&self) -> usize { unsafe { sys::sc_channel_retained_bytes(self.handle.as_ptr()) } }
    pub fn close(&self) { unsafe { sys::sc_channel_close(self.handle.as_ptr()) }; }
}
impl Drop for ByteChannel {
    fn drop(&mut self) { unsafe { sys::sc_channel_destroy(self.handle.as_ptr()) }; }
}

/// Versioned latest byte snapshot. Independent readers retain their last seen
/// version. Multiple bounded async observers may wait for the same next version.
pub struct LatestBytes {
    handle: NonNull<sys::sc_latest_value>,
    _readiness: reactor::Lease,
}
// Same native synchronization and independently owned handle rules as ByteChannel.
unsafe impl Send for LatestBytes {}
unsafe impl Sync for LatestBytes {}
impl LatestBytes {
    fn native_handle(&self) -> *mut sys::sc_latest_value { self.handle.as_ptr() }
    pub fn new(max_retained_bytes: usize) -> Result<Self> {
        verify_abi()?;
        let mut handle = std::ptr::null_mut();
        check(unsafe { sys::sc_latest_value_create(max_retained_bytes, &mut handle) })?;
        Self::from_raw(handle)
    }
    fn from_raw(raw: *mut sys::sc_latest_value) -> Result<Self> {
        let handle = pointer(raw)?;
        match reactor::Lease::new() {
            Ok(lease) => Ok(Self { handle, _readiness: lease }),
            Err(error) => {
                unsafe { sys::sc_latest_value_destroy(handle.as_ptr()) };
                Err(error)
            }
        }
    }
    pub fn try_clone(&self) -> Result<Self> {
        let mut handle = std::ptr::null_mut();
        check(unsafe { sys::sc_latest_value_retain(self.handle.as_ptr(), &mut handle) })?;
        Self::from_raw(handle)
    }
    pub fn publish(&self, value: &[u8]) -> Result<()> {
        check(unsafe { sys::sc_latest_value_publish(self.handle.as_ptr(), bytes(value)) })
    }
    pub fn read_after(&self, version: u64) -> Result<ChannelValue> {
        let mut value = std::ptr::null_mut();
        check(unsafe { sys::sc_latest_value_read_after(self.handle.as_ptr(), version, &mut value) })?;
        ChannelValue::from_raw(value)
    }
    pub async fn wait_after(&self, version: u64) -> Result<ChannelValue> {
        reactor::poll_fn(|| self.read_after(version), |notifier, key, out| unsafe {
            sys::sc_latest_value_subscribe(self.native_handle(), version, notifier, key, out)
        })?.await
    }
    pub fn retained_bytes(&self) -> usize { unsafe { sys::sc_latest_value_retained_bytes(self.handle.as_ptr()) } }
    pub fn close(&self) { unsafe { sys::sc_latest_value_close(self.handle.as_ptr()) }; }
}
impl Drop for LatestBytes {
    fn drop(&mut self) { unsafe { sys::sc_latest_value_destroy(self.handle.as_ptr()) }; }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{block_on, Error};
    use std::{future::Future, sync::{Arc, Condvar, Mutex}, task::{Context, Poll, Wake, Waker}, time::Duration};

    #[derive(Default)]
    struct Wakes { count: Mutex<usize>, changed: Condvar }
    impl Wake for Wakes {
        fn wake(self: Arc<Self>) { self.wake_by_ref(); }
        fn wake_by_ref(self: &Arc<Self>) {
            *self.count.lock().unwrap() += 1;
            self.changed.notify_all();
        }
    }
    impl Wakes {
        fn wait(&self, minimum: usize) -> bool {
            let count = self.count.lock().unwrap();
            let (count, _) = self.changed.wait_timeout_while(count, Duration::from_secs(3),
                |count| *count < minimum).unwrap();
            *count >= minimum
        }
    }
    fn assert_send<T: Send>(value: T) -> T { value }
    #[test]
    fn fifo_copies_input_and_values_survive_channel_owners() {
        let channel = ByteChannel::new(ChannelOptions { max_messages: 1, max_retained_bytes: 4 }).unwrap();
        let retained = channel.try_clone().unwrap();
        let mut input = [1, 0, 255, 4];
        channel.try_send(&input).unwrap();
        input.fill(9);
        assert_eq!(channel.try_send(&[5]), Err(Error::WOULD_BLOCK));
        assert_eq!(channel.try_send(&[0; 5]), Err(Error::TOO_LARGE));
        let value = channel.try_receive().unwrap();
        assert_eq!(value.bytes(), &[1, 0, 255, 4]);
        assert_eq!(value.version(), 0);
        assert_eq!(channel.retained_bytes(), 0);
        drop(channel);
        retained.try_send(&[]).unwrap();
        retained.close();
        assert_eq!(retained.try_send(&[]), Err(Error::CLOSED));
        assert!(retained.try_receive().unwrap().bytes().is_empty());
        assert_eq!(retained.try_receive().err(), Some(Error::CLOSED));
        drop(retained);
        assert_eq!(value.bytes(), &[1, 0, 255, 4]);
    }
    #[test]
    fn fifo_readiness_backpressure_and_dropped_futures() {
        let channel = ByteChannel::new(ChannelOptions { max_messages: 1, max_retained_bytes: 4 }).unwrap();
        let wakes = Arc::new(Wakes::default());
        let waker = Waker::from(wakes.clone());
        let mut cx = Context::from_waker(&waker);
        let mut abandoned = Box::pin(channel.receive());
        assert!(abandoned.as_mut().poll(&mut cx).is_pending());
        drop(abandoned);
        let mut receiver = Box::pin(assert_send(channel.receive()));
        assert!(receiver.as_mut().poll(&mut cx).is_pending());
        channel.try_send(b"one").unwrap();
        assert!(wakes.wait(1));
        assert_eq!(block_on(receiver).unwrap().bytes(), b"one");

        channel.try_send(b"full").unwrap();
        let mut abandoned_send = Box::pin(channel.send(b"lost"));
        assert!(abandoned_send.as_mut().poll(&mut cx).is_pending());
        drop(abandoned_send);
        let mut sender = Box::pin(assert_send(channel.send(b"next")));
        assert!(sender.as_mut().poll(&mut cx).is_pending());
        assert_eq!(channel.try_receive().unwrap().bytes(), b"full");
        block_on(sender).unwrap();
        assert_eq!(channel.try_receive().unwrap().bytes(), b"next");
        assert_eq!(channel.try_receive().err(), Some(Error::WOULD_BLOCK));
        let mut closed = Box::pin(channel.receive());
        assert!(closed.as_mut().poll(&mut cx).is_pending());
        channel.close();
        assert_eq!(block_on(closed).err(), Some(Error::CLOSED));
    }
    #[test]
    fn latest_snapshots_have_independent_versions_and_observers() {
        let latest = LatestBytes::new(8).unwrap();
        let clone = latest.try_clone().unwrap();
        let wakes = Arc::new(Wakes::default());
        let waker = Waker::from(wakes.clone());
        let mut cx = Context::from_waker(&waker);
        let mut first = Box::pin(assert_send(latest.wait_after(0)));
        let mut second = Box::pin(clone.wait_after(0));
        assert!(first.as_mut().poll(&mut cx).is_pending());
        assert!(second.as_mut().poll(&mut cx).is_pending());
        latest.publish(b"first").unwrap();
        assert!(wakes.wait(2));
        let snapshot = block_on(first).unwrap();
        assert_eq!(snapshot.version(), 1);
        assert_eq!(block_on(second).unwrap().version(), 1);
        latest.publish(b"second").unwrap();
        let newer = latest.read_after(snapshot.version()).unwrap();
        assert_eq!(newer.version(), 2);
        assert_eq!(snapshot.bytes(), b"first");
        assert_eq!(latest.read_after(3).err(), Some(Error::INVALID_ARGUMENT));
        assert_eq!(latest.read_after(2).err(), Some(Error::WOULD_BLOCK));
        latest.close();
        assert_eq!(clone.read_after(1).unwrap().bytes(), b"second");
        assert_eq!(clone.read_after(2).err(), Some(Error::CLOSED));
        drop(latest);
        drop(clone);
        assert_eq!(newer.bytes(), b"second");
    }
    #[test]
    fn latest_observer_cap_is_not_reported_as_would_block() {
        let latest = LatestBytes::new(8).unwrap();
        let waker = Waker::from(Arc::new(Wakes::default()));
        let mut cx = Context::from_waker(&waker);
        // Channel.h caps pending change subscriptions at 64 per state.
        let mut observers = Vec::new();
        for _ in 0..64 {
            let mut observer = Box::pin(latest.wait_after(0));
            assert!(observer.as_mut().poll(&mut cx).is_pending());
            observers.push(observer);
        }
        let mut refused = Box::pin(latest.wait_after(0));
        match refused.as_mut().poll(&mut cx) {
            Poll::Ready(result) => assert_eq!(result.err(), Some(Error::TOO_LARGE)),
            Poll::Pending => panic!("observer beyond the native cap is pending"),
        }
    }
}
