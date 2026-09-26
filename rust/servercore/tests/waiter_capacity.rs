//! This file holds a single test so that it runs alone in its own process: it
//! fills the process-wide waiter table, which would make any concurrently
//! running test that awaits fail.
use servercore::{
    channel::{ByteChannel, ChannelOptions},
    Error,
};
use std::{
    future::Future,
    sync::Arc,
    task::{Context, Poll, Wake, Waker},
};

// The process-wide limit documented at the crate root.
const MAX_WAITERS: usize = 4096;

struct Ignore;
impl Wake for Ignore {
    fn wake(self: Arc<Self>) {}
}

fn channel() -> ByteChannel {
    ByteChannel::new(ChannelOptions {
        max_messages: 1,
        max_retained_bytes: 8,
    })
    .unwrap()
}

#[test]
fn exhausted_waiters_complete_ready_operations_and_never_report_would_block() {
    let waker = Waker::from(Arc::new(Ignore));
    let mut cx = Context::from_waker(&waker);
    let channels: Vec<_> = (0..MAX_WAITERS).map(|_| channel()).collect();
    let mut waiting = Vec::with_capacity(MAX_WAITERS);
    for channel in &channels {
        let mut receive = Box::pin(channel.receive());
        assert!(
            receive.as_mut().poll(&mut cx).is_pending(),
            "waiter {} is not pending",
            waiting.len()
        );
        waiting.push(receive);
    }

    let ready = channel();
    ready.try_send(b"ready").unwrap();
    let mut receive = Box::pin(ready.receive());
    match receive.as_mut().poll(&mut cx) {
        Poll::Ready(Ok(value)) => assert_eq!(value.bytes(), b"ready"),
        Poll::Ready(Err(error)) => panic!("ready receive beyond the limit failed: {error}"),
        Poll::Pending => panic!("ready receive beyond the limit is pending"),
    }
    drop(receive);

    let empty = channel();
    let mut receive = Box::pin(empty.receive());
    match receive.as_mut().poll(&mut cx) {
        Poll::Ready(result) => assert_eq!(result.err(), Some(Error::TOO_LARGE)),
        Poll::Pending => panic!("empty receive beyond the limit is pending"),
    }
    drop(receive);

    drop(waiting.pop());
    let mut receive = Box::pin(empty.receive());
    assert!(
        receive.as_mut().poll(&mut cx).is_pending(),
        "receive after a waiter was dropped is not pending"
    );
}
