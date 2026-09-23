#include "Net/DatagramSocket.h"
#include "ServerCore/Runtime/DatagramTransport.h"
#include "TestHarness.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
namespace
{
using namespace ServerCore;
using ServerCoreTest::ExpectTrue;
using namespace std::chrono_literals;
namespace Codec = Protocol::DatagramCodec;
constexpr auto Id = static_cast<Session::SessionId>(1);
constexpr std::string_view Body = R"({"type":"Ping","body":{}})";
auto Bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}
std::vector<std::byte> Packet(
    const Codec::Token& token, std::uint64_t sequence, std::string_view payload = Body)
{
    std::vector<std::byte> result(Codec::HeaderBytes + payload.size());
    ExpectTrue(Codec::Encode(result, token, sequence, Bytes(payload)) == result.size(),
        "encode bounded wire packet");
    return result;
}
template <class F> bool Wait(F&& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ExpectTrue(false, "bounded asynchronous completion");
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
void Readiness()
{
    std::atomic<unsigned> ready = 0, cancelled = 0, closed = 0;
    Runtime::DatagramTransport transport;
    Net::DatagramSocket peer;
    if (!transport.Bind("127.0.0.1", 0).IsOk() || !peer.Bind("127.0.0.1", 0).IsOk())
    {
        ExpectTrue(false, "bind readiness fixture");
        return;
    }
    std::vector<Core::CompletionSubscription> observers;
    for (unsigned i = 0; i < 16; ++i)
    {
        auto s = transport.WaitForReadable(
            [&](Core::Status status)
            {
                if (status.Code() == Core::ErrorCode::Closed)
                    ++closed;
            });
        ExpectTrue(s.IsOk(), "bounded readiness slot");
        if (s.IsOk())
            observers.push_back(std::move(s.Value()));
    }
    auto full = transport.WaitForReadable([](Core::Status) {});
    ExpectTrue(
        full.GetStatus().Code() == Core::ErrorCode::WouldBlock, "seventeenth observer rejected");
    observers.clear();
    std::stop_source stop;
    auto stopped = transport.WaitForReadable(
        [&](Core::Status status)
        {
            if (status.Code() == Core::ErrorCode::Cancelled)
                ++cancelled;
        },
        stop.get_token());
    ExpectTrue(stopped.IsOk(), "cancel observer admitted");
    stop.request_stop();
    ExpectTrue(cancelled == 1, "cancellation exactly once");
    const auto token = transport.RegisterSession(Id);
    ExpectTrue(token.IsOk(), "register readiness peer");
    if (!token.IsOk())
        return;
    ExpectTrue(peer.Send(transport.LocalEndpoint(), Packet(token.Value(), 1)).IsOk(),
        "send before subscribe");
    auto available = transport.WaitForReadable(
        [&](Core::Status status)
        {
            if (status.IsOk())
                ++ready;
        });
    ExpectTrue(available.IsOk(), "ready observer admitted");
    if (!Wait([&] { return ready == 1; }))
        return;
    transport.PollDatagrams([](const auto&) { return true; }, [](const auto&) {});
    auto closing = transport.WaitForReadable(
        [&](Core::Status status)
        {
            if (status.Code() == Core::ErrorCode::Closed)
                ++closed;
        });
    ExpectTrue(closing.IsOk(), "close observer admitted");
    transport.Close();
    ExpectTrue(closed == 1, "close wakes exactly one remaining subscription");
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk(), "rebind after monitor close");
    std::atomic<bool> callbackClosed = false;
    auto reentrant = transport.WaitForReadable(
        [&](Core::Status status)
        {
            if (status.IsOk())
            {
                transport.Close();
                callbackClosed = true;
            }
        });
    ExpectTrue(
        reentrant.IsOk() && peer.Send(transport.LocalEndpoint(), Packet(token.Value(), 2)).IsOk(),
        "signal reentrant close observer");
    (void)Wait([&] { return callbackClosed.load(); });
    reentrant.Value().Reset();
}
void PumpAndPressure()
{
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    std::atomic<unsigned> calls = 0;
    std::atomic<bool> worker = false;
    std::atomic<bool> receiverEntered = false, observerClosing = false;
    std::atomic<bool> receiveClosed = false, observerClosed = false;
    std::atomic<Core::ErrorCode> selfStop = Core::ErrorCode::Ok;
    auto executor = std::make_shared<Runtime::TaskExecutor>();
    ExpectTrue(executor->Start({ 1, 1, 4096 }).IsOk(), "start bounded executor");
    Runtime::DatagramTransport transport;
    Net::DatagramSocket peer;
    // The scope guard runs before captured fixture locals are destroyed on every exit.
    struct Cleanup
    {
        std::shared_ptr<Runtime::TaskExecutor> executor;
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& release;
        ~Cleanup()
        {
            {
                std::lock_guard guard(mutex);
                release = true;
            }
            wake.notify_all();
            (void)executor->Stop();
        }
    } cleanup{ executor, mutex, wake, release };
    auto occupied = executor->Submit(
        [&](std::stop_token)
        {
            std::unique_lock guard(mutex);
            wake.wait(guard, [&] { return release; });
            return Core::Status::Ok();
        });
    ExpectTrue(occupied.IsOk(), "occupy only task capacity");
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk() && peer.Bind("127.0.0.1", 0).IsOk(),
        "bind pump sockets");
    auto token = transport.RegisterSession(Id);
    if (!token.IsOk())
        return;
    auto started = transport.StartReceiving(
        executor, [](const auto&) { return true; },
        [&](const auto& view)
        {
            worker = executor->IsCurrentThreadWorker();
            ExpectTrue(view.session == Id && view.payload.size() == Body.size(),
                "pump preserves session and payload");
            selfStop = transport.StopReceiving().Code();
            ++calls;
        });
    ExpectTrue(started.IsOk(), "start readiness executor bridge");
    ExpectTrue(peer.Send(transport.LocalEndpoint(), Packet(token.Value(), 1)).IsOk(),
        "send while executor saturated");
    if (!Wait([&] { return transport.SnapshotMetrics().pendingBatches == 1; }))
        return;
    ExpectTrue(calls == 0 && transport.SnapshotMetrics().pendingBatches == 1,
        "one pending batch does not bypass executor admission");
    {
        std::lock_guard guard(mutex);
        release = true;
    }
    wake.notify_all();
    if (!Wait([&] { return calls == 1; }))
        return;
    ExpectTrue(worker && selfStop == Core::ErrorCode::InvalidArgument,
        "receive callback uses executor and self stop rejects joining");
    ExpectTrue(transport.StopReceiving().IsOk(), "control thread joins pump");
    ExpectTrue(!transport.SnapshotMetrics().receiving, "stopped pump gauge");
    transport.Close();
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk(), "bind concurrent close fixture");
    auto closingToken = transport.RegisterSession(Id);
    if (!closingToken.IsOk())
        return;
    ExpectTrue(transport
                   .StartReceiving(
                       executor, [](const auto&) { return true; },
                       [&](const auto&)
                       {
                           receiverEntered = true;
                           if (Wait([&] { return observerClosing.load(); }))
                           {
                               transport.Close();
                               receiveClosed = true;
                           }
                       })
                   .IsOk(),
        "start concurrent close pump");
    auto observer = transport.WaitForReadable(
        [&](Core::Status status)
        {
            if (status.IsOk() && Wait([&] { return receiverEntered.load(); }))
            {
                observerClosing = true;
                transport.Close();
                observerClosed = true;
            }
        });
    ExpectTrue(observer.IsOk() &&
                   peer.Send(transport.LocalEndpoint(), Packet(closingToken.Value(), 1)).IsOk(),
        "signal receive and readiness callbacks concurrently");
    (void)Wait([&] { return receiveClosed.load() && observerClosed.load(); });
    if (observer.IsOk())
        observer.Value().Reset();
    ExpectTrue(
        transport.StopReceiving().IsOk(), "concurrent callback close remains externally joinable");
    ExpectTrue(executor->Stop().IsOk(), "executor stops after pump");
}
void SendLimits()
{
    Runtime::DatagramTransport transport;
    Runtime::DatagramTransportOptions options;
    options.totalSendRate = { 1200, 1200, 100ms };
    options.peerSendRate = { 1200, 1200, 100ms };
    ExpectTrue(transport.Configure(options).IsOk(), "configure combined token buckets");
    Net::DatagramSocket peer;
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk() && peer.Bind("127.0.0.1", 0).IsOk(),
        "bind rate fixture");
    auto token = transport.RegisterSession(Id);
    if (!token.IsOk())
        return;
    ExpectTrue(transport.SendSerialized(Id, Bytes(Body)).Code() == Core::ErrorCode::WouldBlock,
        "unbound peer sends no packet");
    ExpectTrue(peer.Send(transport.LocalEndpoint(), Packet(token.Value(), 1)).IsOk(),
        "authenticate endpoint");
    if (!Wait(
            [&]
            {
                transport.PollDatagrams([](const auto&) { return true; }, [](const auto&) {});
                return transport.IsReady(Id);
            }))
        return;
    std::string maximum(Codec::MaximumPayloadBytes, 'x');
    ExpectTrue(transport.SendSerialized(Id, Bytes(maximum)).IsOk(), "full burst accepted");
    ExpectTrue(transport.SendSerialized(Id, Bytes(maximum)).Code() == Core::ErrorCode::WouldBlock,
        "second burst limited atomically");
    auto metrics = transport.SnapshotMetrics();
    ExpectTrue(metrics.sentDatagrams == 1 && metrics.sendRateLimited == 1 &&
                   metrics.sendLimitedGlobal == 1 && metrics.sendLimitedPeer == 1 &&
                   metrics.sendNotReady == 1,
        "overlapping details retain unique limited count");
    std::array<std::byte, 1200> buffer{};
    auto first = peer.Receive(buffer);
    ExpectTrue(
        first.status.IsOk() && Codec::Decode(std::span(buffer).first(first.bytes))->sequence == 1,
        "accepted sequence starts at one");
    if (!Wait([&] { return transport.SendSerialized(Id, Bytes(maximum)).IsOk(); }))
        return;
    auto second = peer.Receive(buffer);
    ExpectTrue(
        second.status.IsOk() && Codec::Decode(std::span(buffer).first(second.bytes))->sequence == 2,
        "rejected sends consume no sequence and refill works");
    transport.Close();
    Runtime::DatagramTransport shared;
    options.totalSendRate = { 1, 2400, 1h };
    options.peerSendRate = { 1, 1200, 1h };
    ExpectTrue(shared.Configure(options).IsOk() && shared.Bind("127.0.0.1", 0).IsOk(),
        "bind independent peer budget fixture");
    const auto otherId = static_cast<Session::SessionId>(2);
    auto firstToken = shared.RegisterSession(Id);
    auto secondToken = shared.RegisterSession(otherId);
    if (!firstToken.IsOk() || !secondToken.IsOk())
        return;
    ExpectTrue(peer.Send(shared.LocalEndpoint(), Packet(firstToken.Value(), 1)).IsOk() &&
                   peer.Send(shared.LocalEndpoint(), Packet(secondToken.Value(), 1)).IsOk(),
        "bind both tokens");
    if (!Wait(
            [&]
            {
                shared.PollDatagrams([](const auto&) { return true; }, [](const auto&) {});
                return shared.IsReady(Id) && shared.IsReady(otherId);
            }))
        return;
    ExpectTrue(shared.SendSerialized(Id, Bytes(maximum)).IsOk(), "first peer consumes its burst");
    ExpectTrue(shared.SendSerialized(Id, Bytes(maximum)).Code() == Core::ErrorCode::WouldBlock,
        "peer-only exhaustion rejects before global consumption");
    ExpectTrue(shared.SendSerialized(otherId, Bytes(maximum)).IsOk(),
        "second peer retains full remaining global allowance after rejected send");
    const auto sharedMetrics = shared.SnapshotMetrics();
    ExpectTrue(sharedMetrics.sendRateLimited == 1 && sharedMetrics.sendLimitedPeer == 1 &&
                   sharedMetrics.sendLimitedGlobal == 0,
        "peer-only reason is distinct");
    Runtime::DatagramTransport invalid;
    options.peerSendRate.burstBytes = 1199;
    ExpectTrue(invalid.Configure(options).Code() == Core::ErrorCode::InvalidArgument,
        "burst must support one maximum packet");
}
const ServerCoreTest::CheckRegistration a{ "Runtime.DatagramReadinessAndCancellation", Readiness };
const ServerCoreTest::CheckRegistration b{ "Runtime.DatagramExecutorPressureAndStop",
    PumpAndPressure };
const ServerCoreTest::CheckRegistration c{ "Runtime.DatagramSendRateLimits", SendLimits };
}
