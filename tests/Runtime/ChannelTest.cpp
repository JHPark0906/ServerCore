#include "ServerCore/Runtime/Channel.h"
#include "TestHarness.h"
#include <atomic>
#include <future>
#include <string>
#include <thread>

namespace
{
using namespace ServerCore;
using namespace std::chrono_literals;
using Core::ErrorCode;
using Core::Status;
using ServerCoreTest::ExpectTrue;

void ChannelLimitsAndDrain()
{
    auto created = Runtime::BoundedChannel<std::string>::Create({2, 5});
    ExpectTrue(created.IsOk(), "bounded channel creates");
    if (!created.IsOk()) return;
    auto channel = std::move(created).Value();
    auto first = std::make_shared<const std::string>("abc");
    ExpectTrue(channel.TrySend(first, 3).IsOk(), "first value admitted");
    ExpectTrue(channel.TrySend(first, 3).Code() == ErrorCode::WouldBlock, "byte pressure rejects atomically");
    ExpectTrue(channel.TrySend(first, 6).Code() == ErrorCode::TooLarge, "oversized value distinct from pressure");
    ExpectTrue(channel.TrySend(std::make_shared<const std::string>("de"), 2).IsOk(), "exact byte cap admitted");
    ExpectTrue(channel.TrySend(first, 0).Code() == ErrorCode::WouldBlock, "count cap bounds zero-byte declarations");
    channel.Close();
    ExpectTrue(channel.TrySend(first, 0).Code() == ErrorCode::Closed, "close stops admission");
    auto one = channel.TryReceive(), two = channel.TryReceive();
    ExpectTrue(one.IsOk() && two.IsOk() && *one.Value() == "abc" && *two.Value() == "de", "close drains FIFO values");
    ExpectTrue(channel.TryReceive().GetStatus().Code() == ErrorCode::Closed && channel.RetainedBytes() == 0, "drained close is terminal");
}

void ChannelNotificationsAndCancellation()
{
    auto channel = std::move(Runtime::BoundedChannel<int>::Create({1, 8})).Value();
    std::atomic<int> reads = 0, writes = 0;
    auto subscription = channel.WaitForReadReady([&](Status status)
    { ExpectTrue(status.IsOk() && channel.Size() == 1, "read notification runs outside lock"); ++reads; });
    ExpectTrue(subscription.IsOk(), "subscribe before data");
    ExpectTrue(channel.WaitForReadReady([](Status) {}).GetStatus().Code() == ErrorCode::AlreadyExists, "read subscription count bounded");
    ExpectTrue(channel.TrySend(std::make_shared<const int>(42), 8).IsOk() && reads == 1, "publish wakes exactly once");
    auto write = channel.WaitForWriteReady(8, [&](Status status)
    { ExpectTrue(status.IsOk() && channel.RetainedBytes() == 0, "write callback outside lock"); ++writes; });
    ExpectTrue(write.IsOk() && writes == 0, "full queue waits for release");
    ExpectTrue(channel.TryReceive().IsOk() && writes == 1, "dequeue signals capacity");
    std::stop_source stop;
    std::atomic<ErrorCode> code = ErrorCode::Ok;
    auto cancelled = channel.WaitForReadReady([&](Status status) { code = status.Code(); }, stop.get_token());
    (void)stop.request_stop();
    ExpectTrue(cancelled.IsOk() && code == ErrorCode::Cancelled, "token cancels subscription");
    auto future = std::async(std::launch::async, [&] { return channel.Receive(stop.get_token()).GetStatus().Code(); });
    ExpectTrue(future.get() == ErrorCode::Cancelled, "already cancelled receive returns promptly");
    ExpectTrue(channel.Receive({}, std::chrono::steady_clock::now() + 1ms).GetStatus().Code() == ErrorCode::Timeout, "receive deadline bounds empty wait");
    auto abandoned = channel.WaitForReadReady([&](Status) { ++reads; });
    abandoned.Value().Reset();
    channel.Close();
    ExpectTrue(reads == 1, "reset suppresses notification");
}

void LatestValueVersionsAndRetirement()
{
    auto latest = std::move(Runtime::LatestValue<int>::Create(4)).Value();
    ExpectTrue(latest.ReadAfter(0).GetStatus().Code() == ErrorCode::WouldBlock, "empty latest value is not a default T");
    std::atomic<int> changed = 0;
    auto wait = latest.WaitForChange(0, [&](Status s) { ExpectTrue(s.IsOk() && latest.ReadAfter(0).IsOk(), "snapshot is visible to callback"); ++changed; });
    ExpectTrue(latest.Publish(std::make_shared<const int>(1), 4).IsOk() && changed == 1, "first generation notifies");
    auto old = latest.ReadAfter(0);
    ExpectTrue(latest.Publish(std::make_shared<const int>(2), 4).IsOk(), "replacement allowed at byte cap");
    auto current = latest.ReadAfter(old.Value().version);
    ExpectTrue(current.IsOk() && current.Value().version == 2 && *current.Value().value == 2 && *old.Value().value == 1, "readers own stable snapshots");
    ExpectTrue(latest.ReadAfter(3).GetStatus().Code() == ErrorCode::InvalidArgument, "future generation rejected");
    ExpectTrue(latest.Publish(std::make_shared<const int>(3), 5).Code() == ErrorCode::TooLarge, "latest byte limit enforced");
    latest.Close();
    ExpectTrue(latest.ReadAfter(1).IsOk() && latest.ReadAfter(2).GetStatus().Code() == ErrorCode::Closed, "close preserves unseen snapshot then terminal");

    auto fanout = std::move(Runtime::LatestValue<int>::Create(Runtime::LatestValueOptions{4, 2})).Value();
    std::atomic<int> observers = 0;
    auto first = fanout.WaitForChange(0, [&](Status s) { if (s.IsOk()) ++observers; });
    auto second = fanout.WaitForChange(0, [&](Status s) { if (s.IsOk()) ++observers; });
    ExpectTrue(fanout.WaitForChange(0, [](Status) {}).GetStatus().Code() == ErrorCode::WouldBlock, "observer table is bounded");
    first.Value().Reset();
    auto replacementObserver = fanout.WaitForChange(0, [&](Status s) { if (s.IsOk()) ++observers; });
    ExpectTrue(second.IsOk() && replacementObserver.IsOk() && fanout.Publish(std::make_shared<const int>(8), 4).IsOk() && observers == 2,
        "reset frees observer capacity and publication fans out once");

    struct Reentrant
    {
        std::function<void()> release;
        ~Reentrant() { release(); }
    };
    auto retirement = std::move(Runtime::LatestValue<Reentrant>::Create(8)).Value();
    std::atomic<int> destroyed = 0;
    auto value = std::make_shared<Reentrant>();
    value->release = [&] { (void)retirement.RetainedBytes(); ++destroyed; };
    ExpectTrue(retirement.Publish(value, 8).IsOk(), "reentrant capture retained");
    value.reset();
    auto replacement = std::make_shared<Reentrant>(); replacement->release = [] {};
    // Publish's by-value parameter owns the retired value. Its destruction may
    // occur at the end of the caller's full expression, rather than on return.
    const auto published = retirement.Publish(replacement, 8);
    ExpectTrue(published.IsOk() && destroyed == 1, "old value destroyed outside lock");
}

void CompletionResetQuiesces()
{
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto subscription = Core::CompletionSubscription::Create([&](Status)
    { entered.set_value(); gate.wait(); });
    auto source = subscription.Value().GetSource();
    std::thread producer([&] { source.Complete(); });
    entered.get_future().wait();
    auto resetting = std::async(std::launch::async, [&] { subscription.Value().Reset(); });
    ExpectTrue(resetting.wait_for(10ms) == std::future_status::timeout, "reset waits active callback");
    release.set_value(); producer.join(); resetting.get();
    ExpectTrue(!source.IsPending() && !source.Complete(), "weak source cannot re-fire reset registration");
    std::stop_source first, second;
    std::atomic<int> calls = 0;
    auto linked = Core::CompletionSubscription::Create([&](Status s) { ExpectTrue(s.Code() == ErrorCode::Cancelled, "linked cancellation reason"); ++calls; });
    linked.Value().BindCancellation(first.get_token(), second.get_token());
    std::thread one([&] { first.request_stop(); }), two([&] { second.request_stop(); });
    one.join(); two.join();
    ExpectTrue(calls == 1, "concurrent parent cancellation completes once");
}

const ServerCoreTest::CheckRegistration a("Runtime.ChannelLimitsAndDrain", ChannelLimitsAndDrain);
const ServerCoreTest::CheckRegistration b("Runtime.ChannelNotificationsAndCancellation", ChannelNotificationsAndCancellation);
const ServerCoreTest::CheckRegistration c("Runtime.LatestValueVersionsAndRetirement", LatestValueVersionsAndRetirement);
const ServerCoreTest::CheckRegistration d("Core.CompletionResetQuiesces", CompletionResetQuiesces);
}
