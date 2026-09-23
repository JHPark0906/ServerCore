use servercore::{
    block_on,
    channel::{ByteChannel, ChannelOptions},
    runtime::{Executor, ExecutorOptions, TaskGroup, TaskGroupOptions, TaskOptions},
    Result,
};

fn main() -> Result<()> {
    let executor = Executor::new(&ExecutorOptions::default())?;
    let group = TaskGroup::new(&executor, &TaskGroupOptions::default())?;
    let messages = ByteChannel::new(ChannelOptions {
        max_messages: 1,
        max_retained_bytes: 1024,
    })?;
    let sender = messages.try_clone()?;
    let task = group.submit(
        &TaskOptions {
            retained_bytes: 128,
            ..TaskOptions::default()
        },
        move |cancellation| {
            if cancellation.is_cancelled() {
                return Err(servercore::Error::CANCELLED);
            }
            sender.try_send(b"ready")
        },
    )?;
    block_on(task.task.wait())?;
    let message = block_on(messages.receive())?;
    println!("{}", String::from_utf8_lossy(message.bytes()));
    block_on(group.wait())?;
    for completion in group.take_completions()? {
        completion.status?;
    }
    group.stop()?;
    executor.stop()?;
    Ok(())
}
