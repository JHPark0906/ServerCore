#pragma once

#include "ServerCore/Core/CompletionSubscription.h"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

namespace ServerCore::Runtime
{
struct ChannelOptions
{
    std::size_t maxMessages = 128;
    std::size_t maxRetainedBytes = 4 * 1024 * 1024;
};

// Copyable shared handle; the last handle closes its state. Payloads are
// immutable owned snapshots. Declared bytes bound values retained BY THE QUEUE,
// not aliases retained by producers/consumers. Close rejects sends but drains
// accepted values. Operations never destroy user values while holding a lock.
// One pending readiness subscription per direction (AlreadyExists otherwise).
// Notifications are advisory: competing readers/writers must retry.
template<class T> class BoundedChannel
{
    using Value = std::shared_ptr<const T>;
    struct Entry { Value value; std::size_t bytes; };
    struct State
    {
        explicit State(ChannelOptions limits) : options(limits) {}
        ~State() { read.Complete(Core::ErrorCode::Closed); write.Complete(Core::ErrorCode::Closed); }
        ChannelOptions options;
        std::mutex mutex;
        std::condition_variable_any changed;
        std::list<Entry> queue;
        std::size_t bytes = 0, writeBytes = 0;
        bool closed = false;
        Core::CompletionSource read, write;
        bool CanSend(std::size_t amount) const noexcept
        { return queue.size() < options.maxMessages && amount <= options.maxRetainedBytes - bytes; }
    };
public:
    BoundedChannel() noexcept = default;
    static Core::Result<BoundedChannel> Create(ChannelOptions options = {})
    {
        if (options.maxMessages == 0 || options.maxRetainedBytes == 0)
            return Core::Result<BoundedChannel>::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
        try { return Core::Result<BoundedChannel>::FromValue(BoundedChannel(std::make_shared<State>(options))); }
        catch (...) { return Core::Result<BoundedChannel>::FromStatus(Core::Status::AllocationFailure()); }
    }
    Core::Status TrySend(Value value, std::size_t retainedBytes)
    {
        const auto state = mState;
        if (!state || !value) return Fail(Core::ErrorCode::InvalidArgument);
        if (retainedBytes > state->options.maxRetainedBytes) return Fail(Core::ErrorCode::TooLarge);
        std::unique_ptr<std::list<Entry>> prepared;
        try
        {
            prepared = std::make_unique<std::list<Entry>>();
            prepared->push_back({std::move(value), retainedBytes});
        }
        catch (...) { return Core::Status::AllocationFailure(); }
        Core::CompletionSource ready;
        {
            const std::lock_guard guard(state->mutex);
            if (state->closed) return Fail(Core::ErrorCode::Closed);
            if (!state->CanSend(retainedBytes)) return Fail(Core::ErrorCode::WouldBlock);
            state->queue.splice(state->queue.end(), *prepared);
            state->bytes += retainedBytes;
            ready = std::exchange(state->read, {});
        }
        state->changed.notify_all();
        ready.Complete();
        return Core::Status::Ok();
    }
    Core::Result<Value> TryReceive() { return ReceiveImpl(false, {}, {}); }
    // Blocking waits are for application worker threads, not I/O callbacks.
    Core::Result<Value> Receive(std::stop_token cancellation = {},
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max())
    { return ReceiveImpl(true, cancellation, deadline); }

    Core::Result<Core::CompletionSubscription> WaitForReadReady(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {})
    { return Subscribe(false, 0, std::move(callback), cancellation); }
    Core::Result<Core::CompletionSubscription> WaitForWriteReady(std::size_t retainedBytes,
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {})
    { return Subscribe(true, retainedBytes, std::move(callback), cancellation); }
    void Close() const noexcept
    {
        const auto state = mState;
        if (!state) return;
        Core::CompletionSource read, write;
        bool empty;
        {
            const std::lock_guard guard(state->mutex);
            state->closed = true;
            empty = state->queue.empty();
            read = std::exchange(state->read, {});
            write = std::exchange(state->write, {});
        }
        state->changed.notify_all();
        read.Complete(empty ? Core::ErrorCode::Closed : Core::ErrorCode::Ok);
        write.Complete(Core::ErrorCode::Closed);
    }
    [[nodiscard]] std::size_t Size() const noexcept
    { if (!mState) return 0; const std::lock_guard guard(mState->mutex); return mState->queue.size(); }
    [[nodiscard]] std::size_t RetainedBytes() const noexcept
    { if (!mState) return 0; const std::lock_guard guard(mState->mutex); return mState->bytes; }
private:
    explicit BoundedChannel(std::shared_ptr<State> state) : mState(std::move(state)) {}
    static Core::Status Fail(Core::ErrorCode code) { return Core::Status::FailWithoutMessage(code); }
    Core::Result<Value> ReceiveImpl(bool wait, std::stop_token cancellation,
        std::chrono::steady_clock::time_point deadline)
    {
        using Result = Core::Result<Value>;
        const auto state = mState;
        if (!state) return Result::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
        Core::CompletionSource ready;
        Value received;
        {
            std::unique_lock guard(state->mutex);
            if (wait && !state->changed.wait_until(guard, cancellation, deadline,
                    [&] { return state->closed || !state->queue.empty(); }))
                return Result::FromStatus(Fail(cancellation.stop_requested() ? Core::ErrorCode::Cancelled : Core::ErrorCode::Timeout));
            if (cancellation.stop_requested()) return Result::FromStatus(Fail(Core::ErrorCode::Cancelled));
            if (state->queue.empty()) return Result::FromStatus(Fail(state->closed ? Core::ErrorCode::Closed : Core::ErrorCode::WouldBlock));
            state->bytes -= state->queue.front().bytes;
            received = std::move(state->queue.front().value);
            state->queue.pop_front();
            if (state->CanSend(state->writeBytes)) ready = std::exchange(state->write, {});
        }
        state->changed.notify_all();
        ready.Complete();
        return Result::FromValue(std::move(received));
    }
    Core::Result<Core::CompletionSubscription> Subscribe(bool writing, std::size_t bytes,
        std::function<void(Core::Status)> callback, std::stop_token cancellation)
    {
        using Result = Core::Result<Core::CompletionSubscription>;
        const auto state = mState;
        if (!state) return Result::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
        if (writing && bytes > state->options.maxRetainedBytes) return Result::FromStatus(Fail(Core::ErrorCode::TooLarge));
        auto result = Core::CompletionSubscription::Create(std::move(callback));
        if (!result.IsOk()) return result;
        auto source = result.Value().GetSource();
        Core::ErrorCode status = Core::ErrorCode::WouldBlock;
        {
            const std::lock_guard guard(state->mutex);
            auto& pending = writing ? state->write : state->read;
            if (pending.IsPending()) return Result::FromStatus(Fail(Core::ErrorCode::AlreadyExists));
            if (writing)
            {
                if (state->closed) status = Core::ErrorCode::Closed;
                else if (state->CanSend(bytes)) status = Core::ErrorCode::Ok;
                state->writeBytes = bytes;
            }
            else if (!state->queue.empty()) status = Core::ErrorCode::Ok;
            else if (state->closed) status = Core::ErrorCode::Closed;
            if (status == Core::ErrorCode::WouldBlock) pending = source;
        }
        result.Value().BindCancellation(cancellation);
        if (status != Core::ErrorCode::WouldBlock) source.Complete(status);
        return result;
    }
    std::shared_ptr<State> mState;
};

struct LatestValueOptions
{
    std::size_t maxRetainedBytes = 4 * 1024 * 1024;
    std::size_t maxSubscriptions = 64;
};

// Versioned immutable latest value. Readers keep their own generation; reads
// never consume the value. Bounded asynchronous change subscriptions support
// independent observers (WouldBlock at the subscription cap). Close preserves the last
// unseen snapshot; once observed, reads return Closed. Generations never wrap.
template<class T> class LatestValue
{
public:
    struct Snapshot { std::uint64_t version; std::shared_ptr<const T> value; };
private:
    struct State
    {
        explicit State(LatestValueOptions limits)
            : options(limits), watchers(std::make_unique<std::list<Core::CompletionSource>>()) {}
        ~State() { if (watchers) for (const auto& source : *watchers) source.Complete(Core::ErrorCode::Closed); }
        LatestValueOptions options;
        std::size_t bytes = 0;
        std::mutex mutex;
        std::condition_variable_any changed;
        std::shared_ptr<const T> value;
        std::uint64_t version = 0;
        bool closed = false;
        std::unique_ptr<std::list<Core::CompletionSource>> watchers;
    };
public:
    LatestValue() noexcept = default;
    static Core::Result<LatestValue> Create(std::size_t maxRetainedBytes = 4 * 1024 * 1024)
    { return Create(LatestValueOptions{maxRetainedBytes, 64}); }
    static Core::Result<LatestValue> Create(LatestValueOptions options)
    {
        if (options.maxRetainedBytes == 0 || options.maxSubscriptions == 0) return Core::Result<LatestValue>::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
        try { return Core::Result<LatestValue>::FromValue(LatestValue(std::make_shared<State>(options))); }
        catch (...) { return Core::Result<LatestValue>::FromStatus(Core::Status::AllocationFailure()); }
    }
    Core::Status Publish(std::shared_ptr<const T> value, std::size_t retainedBytes)
    {
        const auto state = mState;
        if (!state || !value) return Fail(Core::ErrorCode::InvalidArgument);
        if (retainedBytes > state->options.maxRetainedBytes) return Fail(Core::ErrorCode::TooLarge);
        std::unique_ptr<std::list<Core::CompletionSource>> ready;
        try { ready = std::make_unique<std::list<Core::CompletionSource>>(); }
        catch (...) { return Core::Status::AllocationFailure(); }
        {
            const std::lock_guard guard(state->mutex);
            if (state->closed) return Fail(Core::ErrorCode::Closed);
            if (state->version == std::numeric_limits<std::uint64_t>::max()) return Fail(Core::ErrorCode::TooLarge);
            state->value.swap(value);
            state->bytes = retainedBytes;
            ++state->version;
            ready.swap(state->watchers);
        }
        state->changed.notify_all();
        for (const auto& source : *ready) source.Complete();
        return Core::Status::Ok();
    }
    Core::Result<Snapshot> ReadAfter(std::uint64_t version) const { return ReadImpl(version, false, {}, {}); }
    Core::Result<Snapshot> WaitAfter(std::uint64_t version, std::stop_token cancellation = {},
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) const
    { return ReadImpl(version, true, cancellation, deadline); }
    Core::Result<Core::CompletionSubscription> WaitForChange(std::uint64_t version,
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) const
    {
        using Result = Core::Result<Core::CompletionSubscription>;
        const auto state = mState;
        if (!state) return Result::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
        auto result = Core::CompletionSubscription::Create(std::move(callback));
        if (!result.IsOk()) return result;
        const auto source = result.Value().GetSource();
        std::unique_ptr<std::list<Core::CompletionSource>> prepared;
        try
        {
            prepared = std::make_unique<std::list<Core::CompletionSource>>();
            prepared->push_back(source);
        }
        catch (...) { return Result::FromStatus(Core::Status::AllocationFailure()); }
        Core::ErrorCode code;
        {
            const std::lock_guard guard(state->mutex);
            if (version > state->version) return Result::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
            code = state->version != version ? Core::ErrorCode::Ok : state->closed ? Core::ErrorCode::Closed : Core::ErrorCode::WouldBlock;
            if (code == Core::ErrorCode::WouldBlock)
            {
                state->watchers->remove_if([](const auto& item) { return !item.IsPending(); });
                if (state->watchers->size() >= state->options.maxSubscriptions)
                    return Result::FromStatus(Fail(Core::ErrorCode::WouldBlock));
                state->watchers->splice(state->watchers->end(), *prepared);
            }
        }
        result.Value().BindCancellation(cancellation);
        if (code != Core::ErrorCode::WouldBlock) source.Complete(code);
        return result;
    }
    void Close() const noexcept
    {
        const auto state = mState;
        if (!state) return;
        std::unique_ptr<std::list<Core::CompletionSource>> ready;
        {
            const std::lock_guard guard(state->mutex);
            state->closed = true;
            ready = std::move(state->watchers);
        }
        state->changed.notify_all();
        if (ready) for (const auto& source : *ready) source.Complete(Core::ErrorCode::Closed);
    }
    [[nodiscard]] std::size_t RetainedBytes() const noexcept
    { if (!mState) return 0; const std::lock_guard guard(mState->mutex); return mState->bytes; }
private:
    explicit LatestValue(std::shared_ptr<State> state) : mState(std::move(state)) {}
    static Core::Status Fail(Core::ErrorCode code) { return Core::Status::FailWithoutMessage(code); }
    Core::Result<Snapshot> ReadImpl(std::uint64_t version, bool wait, std::stop_token cancellation,
        std::chrono::steady_clock::time_point deadline) const
    {
        using Result = Core::Result<Snapshot>;
        const auto state = mState;
        if (!state) return Result::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
        std::unique_lock guard(state->mutex);
        if (version > state->version) return Result::FromStatus(Fail(Core::ErrorCode::InvalidArgument));
        if (wait && !state->changed.wait_until(guard, cancellation, deadline,
                [&] { return state->closed || state->version != version; }))
            return Result::FromStatus(Fail(cancellation.stop_requested() ? Core::ErrorCode::Cancelled : Core::ErrorCode::Timeout));
        if (cancellation.stop_requested()) return Result::FromStatus(Fail(Core::ErrorCode::Cancelled));
        if (state->version == version) return Result::FromStatus(Fail(state->closed ? Core::ErrorCode::Closed : Core::ErrorCode::WouldBlock));
        return Result::FromValue({state->version, state->value});
    }
    std::shared_ptr<State> mState;
};
}
