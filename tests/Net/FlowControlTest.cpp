#include "TestHarness.h"
#include "Net/SendBudgetInternal.h"
#include "Net/SendQueueInternal.h"
#include "ServerCore/Net/ConnectionFlowControl.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
using namespace std::chrono_literals;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCore::Net::SendBudget;
using ServerCore::Net::SendCapacitySubscription;
using ServerCore::Net::SendNotificationScope;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;

std::span<const std::byte> Bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Mirrors the backend lock boundary, without sockets or scheduler timing.
// Capacity registration is independently synchronized and can invoke inline.
class Queue
{
public:
    explicit Queue(std::shared_ptr<SendBudget> budget) : mBudget(std::move(budget)), mQueue(mBudget) {}
    ~Queue() { Close(false); }

    Status Send(std::string_view text)
    {
        const SendNotificationScope notifications(mBudget);
        const std::lock_guard guard(mMutex);
        if (mClosed) return Status::FailWithoutMessage(ErrorCode::Closed);
        return mQueue.Enqueue(Bytes(text));
    }

    void Consume(std::size_t count)
    {
        const SendNotificationScope notifications(mBudget);
        const std::lock_guard guard(mMutex);
        mQueue.Consume(count);
    }

    void Clear()
    {
        const SendNotificationScope notifications(mBudget);
        const std::lock_guard guard(mMutex);
        mQueue.Clear();
    }

    void Close(bool drain)
    {
        const SendNotificationScope notifications(mBudget);
        const std::lock_guard guard(mMutex);
        mClosed = true;
        mQueue.CloseCapacityWaits();
        if (!drain) mQueue.Clear();
    }

    std::size_t RetainedBytes() const
    {
        const std::lock_guard guard(mMutex);
        return mQueue.RetainedBytes();
    }

    std::size_t QueuedBytes() const
    {
        const std::lock_guard guard(mMutex);
        return mQueue.QueuedBytes();
    }

    ServerCore::Core::Result<SendCapacitySubscription> Wait(std::size_t required,
        std::function<void(Status)> callback, std::stop_token cancellation = {})
    {
        return mQueue.WaitForCapacity(required, std::move(callback), cancellation);
    }

private:
    std::shared_ptr<SendBudget> mBudget;
    mutable std::mutex mMutex;
    ServerCore::Net::SendQueue mQueue;
    bool mClosed = false;
};

SendCapacitySubscription Accepted(Queue& queue, std::size_t bytes, std::function<void(Status)> callback,
    std::stop_token cancellation = {})
{
    auto result = queue.Wait(bytes, std::move(callback), cancellation);
    ExpectTrue(result.IsOk(), "capacity subscription is accepted");
    return result.IsOk() ? std::move(result.Value()) : SendCapacitySubscription{};
}

void ExpectCode(ErrorCode expected, const Status& actual, std::string_view description)
{
    ExpectTrue(expected == actual.Code(), description);
}

void SendCapacityRetainsPartialStorage()
{
    const auto budget = std::make_shared<SendBudget>(16, 8);
    Queue queue(budget);
    ExpectTrue(queue.Send("12345678").IsOk(), "payload fills local retained-byte limit");
    unsigned int calls = 0;
    auto wait = Accepted(queue, 1, [&](Status status) {
        ++calls;
        ExpectTrue(status.IsOk(), "complete payload release reports capacity");
    });
    ExpectTrue(wait.IsPending(), "full local queue leaves capacity pending");
    queue.Consume(7);
    ExpectEqual(std::size_t{1}, queue.QueuedBytes(), "logical bytes decrease after partial completion");
    ExpectEqual(std::size_t{8}, queue.RetainedBytes(), "sent prefix remains retained until full front release");
    ExpectEqual(std::size_t{8}, budget->UsedBytes(), "shared budget also retains the sent prefix");
    budget->MarkChanged();
    budget->Notify();
    ExpectEqual(0u, calls, "shared free space cannot bypass full local retained storage");
    ExpectCode(ErrorCode::WouldBlock, queue.Send("x"), "partially sent allocation still rejects new bytes");
    queue.Consume(1);
    ExpectEqual(1u, calls, "final front completion notifies exactly once");
    ExpectTrue(!wait.IsPending(), "capacity wait is one-shot");
    ExpectEqual(std::size_t{0}, queue.RetainedBytes(), "front storage releases on final completion");
    ExpectEqual(std::size_t{0}, budget->UsedBytes(), "shared reservation releases with storage");
    budget->MarkChanged();
    budget->Notify();
    ExpectEqual(1u, calls, "later budget notifications do not repeat completed wait");
    ExpectTrue(queue.Send("x").IsOk(), "caller retries Send after advisory notification");
}

void SendCapacitySharesBudgetWithoutMissedWake()
{
    const auto budget = std::make_shared<SendBudget>(8, 8);
    Queue producer(budget);
    Queue first(budget);
    Queue second(budget);
    ExpectTrue(producer.Send("12345678").IsOk(), "producer fills shared budget");
    unsigned int firstCalls = 0;
    unsigned int secondCalls = 0;
    auto firstWait = Accepted(first, 8, [&](Status status) { ExpectTrue(status.IsOk(), "first peer sees released capacity"); ++firstCalls; });
    auto secondWait = Accepted(second, 8, [&](Status status) { ExpectTrue(status.IsOk(), "second peer sees released capacity"); ++secondCalls; });
    producer.Consume(4);
    ExpectEqual(0u, firstCalls + secondCalls, "partial release does not wake queues sharing retained storage");
    producer.Consume(4);
    ExpectEqual(1u, firstCalls, "one queue's release wakes another queue");
    ExpectEqual(1u, secondCalls, "advisory availability is not an exclusive reservation");
    ExpectEqual(std::size_t{0}, budget->UsedBytes(), "notifications themselves consume no byte capacity");
    ExpectTrue(!firstWait.IsPending() && !secondWait.IsPending(), "both advisory waits finish once");
    ExpectTrue(first.Send("12345678").IsOk(), "one notified producer can consume capacity");
    ExpectCode(ErrorCode::WouldBlock, second.Send("x"), "another producer must still retry admission normally");
    first.Clear();
    budget->Notify(); // The release has been fully dispatched before registration.
    unsigned int lateCalls = 0;
    auto late = Accepted(second, 8, [&](Status status) { ExpectTrue(status.IsOk(), "registration observes current capacity"); ++lateCalls; });
    ExpectEqual(1u, lateCalls, "release before registration cannot lose a wakeup");
    ExpectTrue(!late.IsPending(), "already available capacity may complete inline");
}

void SendCapacityValidatesAndCancels()
{
    const auto budget = std::make_shared<SendBudget>(8, 4);
    Queue queue(budget);
    ExpectTrue(queue.Send("1234").IsOk(), "validation queue is full");
    unsigned int rejectedCalls = 0;
    const auto rejected = [&](Status) { ++rejectedCalls; };
    ExpectCode(ErrorCode::InvalidArgument, queue.Wait(0, rejected).GetStatus(), "zero requested bytes are invalid");
    ExpectCode(ErrorCode::InvalidArgument, queue.Wait(1, {}).GetStatus(), "empty callback is invalid");
    ExpectCode(ErrorCode::TooLarge, queue.Wait(5, rejected).GetStatus(), "request larger than connection limit is TooLarge");
    {
        Queue smallShared(std::make_shared<SendBudget>(3, 8));
        ExpectCode(ErrorCode::TooLarge, smallShared.Wait(4, rejected).GetStatus(), "request larger than shared limit is TooLarge");
    }
    unsigned int cancelledCalls = 0;
    auto cancelled = Accepted(queue, 1, [&](Status status) {
        ExpectCode(ErrorCode::Cancelled, status, "explicit cancellation reports Cancelled");
        ++cancelledCalls;
    });
    ExpectCode(ErrorCode::AlreadyExists, queue.Wait(1, rejected).GetStatus(), "only one pending wait is allowed");
    ExpectCode(ErrorCode::InvalidArgument, queue.Wait(0, rejected).GetStatus(), "argument validation precedes duplicate state");
    ExpectTrue(cancelled.Cancel(), "first explicit cancellation wins");
    ExpectTrue(!cancelled.Cancel() && !cancelled.IsPending(), "late cancellation cannot repeat completion");
    ExpectEqual(1u, cancelledCalls, "cancel invokes callback exactly once");
    std::stop_source parent;
    unsigned int parentCalls = 0;
    auto parentWait = Accepted(queue, 1, [&](Status status) {
        ExpectCode(ErrorCode::Cancelled, status, "parent token cancellation reports Cancelled");
        ++parentCalls;
    }, parent.get_token());
    (void)parent.request_stop();
    ExpectEqual(1u, parentCalls, "parent token completes pending wait");
    ExpectTrue(!parentWait.IsPending(), "parent cancellation clears pending state");
    auto preCancelled = Accepted(queue, 1, [&](Status status) {
        ExpectCode(ErrorCode::Cancelled, status, "already-cancelled token completes during registration");
        ++parentCalls;
    }, parent.get_token());
    ExpectEqual(2u, parentCalls, "already requested parent cancellation is not missed");
    ExpectTrue(!preCancelled.IsPending(), "already-cancelled registration is terminal");
    auto capture = std::make_shared<int>(1);
    const std::weak_ptr<int> lifetime = capture;
    unsigned int resetCalls = 0;
    auto reset = Accepted(queue, 1, [value = std::move(capture), &resetCalls](Status) { (void)value; ++resetCalls; });
    reset.Reset();
    ExpectTrue(lifetime.expired(), "Reset releases callback captures");
    queue.Clear();
    ExpectEqual(0u, resetCalls, "Reset suppresses a pending callback");
    ExpectEqual(0u, rejectedCalls, "rejected registrations never invoke callbacks");
    unsigned int exceptionCalls = 0;
    auto throwing = Accepted(queue, 1, [&](Status) { ++exceptionCalls; throw std::runtime_error("callback failure"); });
    ExpectTrue(!throwing.IsPending(), "callback exception still completes registration");
    auto successor = Accepted(queue, 1, [&](Status status) { ExpectTrue(status.IsOk(), "registration recovers after callback exception"); ++exceptionCalls; });
    ExpectEqual(2u, exceptionCalls, "callback exception is contained and a successor can register");
    ExpectTrue(!successor.IsPending(), "successor completion is independent");
}

void SendCapacityCloseAndReentrancy()
{
    const auto budget = std::make_shared<SendBudget>(8, 8);
    Queue drain(budget);
    ExpectTrue(drain.Send("12345678").IsOk(), "draining queue holds payload");
    unsigned int closedCalls = 0;
    auto pending = Accepted(drain, 1, [&](Status status) {
        ExpectCode(ErrorCode::Closed, status, "closing admission completes pending capacity wait with Closed");
        ++closedCalls;
    });
    drain.Close(true);
    ExpectEqual(1u, closedCalls, "drain initiation notifies without waiting for queue completion");
    ExpectEqual(std::size_t{8}, drain.RetainedBytes(), "drain retains outstanding payload storage");
    unsigned int rejectedCalls = 0;
    ExpectCode(ErrorCode::Closed, drain.Wait(1, [&](Status) { ++rejectedCalls; }).GetStatus(), "new closed wait rejects synchronously");
    ExpectCode(ErrorCode::InvalidArgument, drain.Wait(0, [&](Status) { ++rejectedCalls; }).GetStatus(), "invalid bytes precede closed state");
    ExpectCode(ErrorCode::TooLarge, drain.Wait(9, [&](Status) { ++rejectedCalls; }).GetStatus(), "impossible capacity precedes closed state");
    drain.Consume(8);
    drain.Close(false);
    ExpectEqual(1u, closedCalls, "drain completion and repeated close cannot repeat notification");
    ExpectEqual(0u, rejectedCalls, "closed registration failure never invokes callback");
    ExpectTrue(!pending.IsPending(), "closed wait is terminal");
    SendCapacitySubscription ownerless;
    {
        Queue destroyed(std::make_shared<SendBudget>(8, 8));
        ExpectTrue(destroyed.Send("12345678").IsOk(), "destruction queue is full");
        ownerless = Accepted(destroyed, 1, [&](Status status) {
            ExpectCode(ErrorCode::Closed, status, "queue destruction completes an existing wait");
            ++closedCalls;
        });
    }
    ExpectEqual(2u, closedCalls, "queue destruction issues one terminal completion");
    ExpectTrue(!ownerless.IsPending(), "subscription may outlive its queue");
    std::stop_source alreadyCancelled;
    (void)alreadyCancelled.request_stop();
    auto releasedDuringRegistration = std::make_shared<Queue>(std::make_shared<SendBudget>(8, 8));
    const std::weak_ptr<Queue> releasedLifetime = releasedDuringRegistration;
    unsigned int destructionCalls = 0;
    auto released = releasedDuringRegistration->Wait(1, [&](Status status) {
        ExpectCode(ErrorCode::Cancelled, status, "pre-cancelled registration invokes its cancellation callback");
        releasedDuringRegistration.reset();
        ++destructionCalls;
    }, alreadyCancelled.get_token());
    ExpectTrue(released.IsOk(), "registration remains valid when callback destroys its queue");
    ExpectTrue(releasedLifetime.expired(), "inline callback released the last queue owner");
    ExpectEqual(1u, destructionCalls, "registration survives recursive queue destruction without repeating completion");
    {
        const auto sharedBudget = std::make_shared<SendBudget>(8, 8);
        Queue triggering(sharedBudget);
        auto destroyedDuringNotify = std::make_unique<Queue>(sharedBudget);
        ExpectTrue(triggering.Send("12345678").IsOk(), "shared budget starts full before recursive destruction");
        unsigned int triggeringCalls = 0;
        unsigned int destroyedCalls = 0;
        // Register A before B so the budget dispatcher invokes A first.
        auto triggeringWait = Accepted(triggering, 1, [&](Status status) {
            ExpectTrue(status.IsOk(), "first registered queue observes released shared capacity");
            ++triggeringCalls;
            destroyedDuringNotify.reset();
        });
        auto destroyedWait = Accepted(*destroyedDuringNotify, 1, [&](Status status) {
            ExpectCode(ErrorCode::Closed, status, "queue destroyed during shared dispatch still reports Closed");
            ++destroyedCalls;
        });
        triggering.Consume(8);
        ExpectEqual(1u, triggeringCalls, "shared budget dispatcher enters the first callback once");
        ExpectEqual(1u, destroyedCalls, "recursive destruction cannot lose a terminal callback");
        ExpectTrue(!triggeringWait.IsPending() && !destroyedWait.IsPending(),
            "both subscriptions finish even when a queue unregisters during dispatch");
        sharedBudget->MarkChanged();
        sharedBudget->Notify();
        ExpectEqual(1u, destroyedCalls, "later dispatch cannot repeat destruction completion");
    }

    Queue reentrant(std::make_shared<SendBudget>(8, 8));
    ExpectTrue(reentrant.Send("12345678").IsOk(), "reentrant queue starts full");
    unsigned int firstCalls = 0;
    unsigned int successorCalls = 0;
    SendCapacitySubscription first;
    SendCapacitySubscription successor;
    first = Accepted(reentrant, 8, [&](Status status) {
        ++firstCalls;
        ExpectTrue(status.IsOk(), "reentrant callback observes capacity");
        first.Reset(); // Must not wait for itself.
        ExpectTrue(reentrant.Send("abcdefgh").IsOk(), "capacity callback may reenter Send outside transport lock");
        successor = Accepted(reentrant, 1, [&](Status next) {
            ++successorCalls;
            ExpectCode(ErrorCode::Closed, next, "callback can register a successor that observes close");
        });
        reentrant.Close(false);
    });
    reentrant.Consume(8);
    ExpectEqual(1u, firstCalls, "reentrant sender's initial completion happens once");
    ExpectEqual(1u, successorCalls, "reentrant close dispatches successor exactly once");
    ExpectTrue(!first.IsPending() && !successor.IsPending(), "self-reset and successor close both finish safely");
    ExpectEqual(std::size_t{0}, reentrant.RetainedBytes(), "reentrant close releases newly enqueued bytes");
}

void SendCapacityResetWaitsForCallback()
{
    const auto budget = std::make_shared<SendBudget>(8, 8);
    Queue queue(budget);
    ExpectTrue(queue.Send("12345678").IsOk(), "reset synchronization queue is full");
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false;
    bool release = false;
    std::atomic<unsigned int> calls{0};
    auto capture = std::make_shared<int>(7);
    const std::weak_ptr<int> lifetime = capture;
    auto subscription = Accepted(queue, 1, [value = std::move(capture), &mutex, &wake, &entered, &release, &calls](Status status) {
        (void)value;
        ExpectTrue(status.IsOk(), "active callback receives capacity");
        ++calls;
        std::unique_lock guard(mutex);
        entered = true;
        wake.notify_all();
        (void)wake.wait_for(guard, 5s, [&release] { return release; });
    });
    std::thread notifying([&queue] { queue.Consume(8); });
    {
        std::unique_lock guard(mutex);
        ExpectTrue(wake.wait_for(guard, 2s, [&entered] { return entered; }), "callback entered on notifying thread");
    }
    std::promise<void> resetStarted;
    std::promise<void> resetFinished;
    auto started = resetStarted.get_future();
    auto finished = resetFinished.get_future();
    std::thread resetting([&] {
        resetStarted.set_value();
        subscription.Reset();
        resetFinished.set_value();
    });
    ExpectTrue(started.wait_for(2s) == std::future_status::ready, "reset thread starts");
    ExpectTrue(finished.wait_for(30ms) == std::future_status::timeout, "Reset waits while callback remains active");
    {
        const std::lock_guard guard(mutex);
        release = true;
    }
    wake.notify_all();
    resetting.join();
    notifying.join();
    ExpectTrue(finished.wait_for(0ms) == std::future_status::ready, "Reset returns after callback completes");
    ExpectTrue(lifetime.expired(), "Reset also waits for active callback captures to be released");
    budget->MarkChanged();
    budget->Notify();
    ExpectEqual(1u, calls.load(), "reset after active completion cannot cause another callback");
}

const ServerCoreTest::CheckRegistration partial("Transport.SendCapacityRetainsPartialStorage", SendCapacityRetainsPartialStorage);
const ServerCoreTest::CheckRegistration shared("Transport.SendCapacitySharesBudgetWithoutMissedWake", SendCapacitySharesBudgetWithoutMissedWake);
const ServerCoreTest::CheckRegistration validation("Transport.SendCapacityValidatesAndCancels", SendCapacityValidatesAndCancels);
const ServerCoreTest::CheckRegistration close("Transport.SendCapacityCloseAndReentrancy", SendCapacityCloseAndReentrancy);
const ServerCoreTest::CheckRegistration reset("Transport.SendCapacityResetWaitsForCallback", SendCapacityResetWaitsForCallback);
}
