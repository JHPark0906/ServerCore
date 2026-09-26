#include "Net/DatagramSocket.h"
#include "Runtime/TransportTestAccess.h"
#include "ServerCore/Runtime/DatagramTransport.h"
#include "TestHarness.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
namespace
{
using namespace ServerCore;
namespace TA = ServerCore::Runtime::TestAccess;
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
bool ExpectReceivedSequence(Net::DatagramSocket& peer, std::span<std::byte> buffer,
    std::uint64_t expected, std::string_view what)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    Net::DatagramReceiveResult received;
    do
    {
        received = peer.Receive(buffer);
        if (received.status.Code() != Core::ErrorCode::WouldBlock ||
            std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(1ms);
    } while (true);

    const auto decoded = received.status.IsOk() && received.bytes <= buffer.size()
                             ? Codec::Decode(buffer.first(received.bytes))
                             : std::nullopt;
    if (received.status.IsOk() && decoded && decoded->sequence == expected)
        return true;

    auto details = std::string(what) +
                   " (status_code=" + std::to_string(static_cast<int>(received.status.Code())) +
                   ", bytes=" + std::to_string(received.bytes) +
                   ", expected_sequence=" + std::to_string(expected) + ", received_sequence=" +
                   (decoded ? std::to_string(decoded->sequence) : "<none>") + ")";
    if (!received.status.Message().empty())
        details += ": " + received.status.Message();
    ExpectTrue(false, details);
    return false;
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
/// <summary>모니터 스레드를 readable 콜백 안에 세워 두고, 타이머 스레드의 재무장이 그 사이에 끝나게 한다.</summary>
struct PumpInterleavingGate final : TA::IReceivePumpGate
{
    std::mutex mutex;
    std::condition_variable wake;
    bool armed = false, monitorWaiting = false, rearmed = false, forced = false;
    std::thread::id monitor;
    void AfterCompletionSubscribed() noexcept override
    {
        std::unique_lock guard(mutex);
        if (!armed)
            return;
        armed = false;
        monitor = std::this_thread::get_id();
        monitorWaiting = true;
        wake.notify_all();
        forced = wake.wait_for(guard, 5s, [&] { return rearmed; });
        monitorWaiting = false;
        wake.notify_all();
    }
    void AfterRearmPublished() noexcept override
    {
        const std::lock_guard guard(mutex);
        if (monitorWaiting && std::this_thread::get_id() != monitor)
        {
            rearmed = true;
            wake.notify_all();
        }
    }
    bool AwaitMonitorWaiting()
    {
        std::unique_lock guard(mutex);
        return wake.wait_for(guard, 5s, [&] { return monitorWaiting; });
    }
    bool AwaitForced()
    {
        std::unique_lock guard(mutex);
        return wake.wait_for(guard, 5s, [&] { return forced; });
    }
};
/// <summary>교착은 소멸자까지 매달리게 하므로, 알린 뒤 프로세스를 끝내 제한 시간 안에 실패로 닫는다.</summary>
[[noreturn]] void FailDeadlocked(std::string_view what)
{
    ExpectTrue(false, what);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}
// EXEC-2: 모니터 스레드가 readable 콜백 안에서 완료 구독을 받은 직후, 타이머 스레드의 완료 콜백이
// 재무장을 끝내는 창. 재무장이 교체된 readable 구독의 콜백(모니터 스레드)을 기다리고, 모니터
// 스레드는 넘기지 못한 완료 구독의 콜백(타이머 스레드)을 기다리면 수신이 영구히 멈춘다.
void PumpRearmDoesNotWaitOnMonitor()
{
    auto gate = std::make_shared<PumpInterleavingGate>();
    TA::InstallReceivePumpGate(gate);
    auto executor = std::make_shared<Runtime::TaskExecutor>();
    ExpectTrue(executor->Start({ 1, 4, 4096 }).IsOk(), "start pump executor");
    Runtime::DatagramTransport transport;
    Net::DatagramSocket peer;
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk() && peer.Bind("127.0.0.1", 0).IsOk(),
        "bind interleaving sockets");
    auto token = transport.RegisterSession(Id);
    if (!token.IsOk())
        return;
    std::atomic<unsigned> calls = 0;
    auto started = transport.StartReceiving(
        executor, [](const auto&) { return true; },
        [&](const auto&)
        {
            // 첫 배치는 모니터 스레드가 gate에 들어선 뒤에 끝나야 완료 콜백이 타이머 스레드에서 돈다.
            if (calls.load() == 0)
                (void)gate->AwaitMonitorWaiting();
            ++calls;
        });
    ExpectTrue(started.IsOk(), "start interleaving pump");
    {
        const std::lock_guard guard(gate->mutex);
        gate->armed = true;
    }
    ExpectTrue(
        peer.Send(transport.LocalEndpoint(), Packet(token.Value(), 1)).IsOk(), "send first packet");
    ExpectTrue(
        gate->AwaitForced(), "timer-thread rearm completed while the monitor callback waited");
    ExpectTrue(peer.Send(transport.LocalEndpoint(), Packet(token.Value(), 2)).IsOk(),
        "send second packet");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (calls.load() < 2)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            FailDeadlocked("receive pump keeps delivering after the interleaving");
        std::this_thread::sleep_for(1ms);
    }
    ExpectTrue(transport.SnapshotMetrics().pumpBatches >= 2, "both batches ran on the pump");
    ExpectTrue(transport.StopReceiving().IsOk(), "pump stops after the interleaving");
    TA::ClearReceivePumpGate(gate);
    transport.Close();
    ExpectTrue(executor->Stop().IsOk(), "executor stops after interleaving pump");
}
// EXEC-7: 배치 타이머가 Ok가 아닌 결과로 끝나면(예: 선언한 캡처 바이트가 실행기 한도보다 커서 TooLarge)
// 펌프는 멈추면서도 pumpFailures를 올리지 않아, StartReceiving이 Ok를 돌려준 뒤 조용히 수신이 끊겼다.
void PumpCountsBatchFailures()
{
    auto executor = std::make_shared<Runtime::TaskExecutor>();
    ExpectTrue(executor->Start({ 1, 4, 64 }).IsOk(), "start small-budget executor");
    Runtime::DatagramTransport transport;
    Net::DatagramSocket peer;
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk() && peer.Bind("127.0.0.1", 0).IsOk(),
        "bind failure sockets");
    auto token = transport.RegisterSession(Id);
    if (!token.IsOk())
        return;
    // 정상 정지는 실패로 세지 않는다.
    Runtime::DatagramReceiveOptions fits;
    fits.retainedCallbackBytes = 16;
    ExpectTrue(transport
                   .StartReceiving(
                       executor, [](const auto&) { return true; }, [](const auto&) {}, fits)
                   .IsOk(),
        "start fitting pump");
    ExpectTrue(transport.StopReceiving().IsOk(), "stop fitting pump");
    ExpectTrue(transport.SnapshotMetrics().pumpFailures == 0, "normal stop is not a pump failure");

    Runtime::DatagramReceiveOptions oversized;
    oversized.retainedCallbackBytes = 128;
    std::atomic<unsigned> calls = 0;
    ExpectTrue(transport
                   .StartReceiving(
                       executor, [](const auto&) { return true; }, [&](const auto&) { ++calls; },
                       oversized)
                   .IsOk(),
        "pump starts although its batch cannot fit the executor");
    ExpectTrue(peer.Send(transport.LocalEndpoint(), Packet(token.Value(), 1)).IsOk(),
        "send packet to failing pump");
    (void)Wait([&] { return !transport.SnapshotMetrics().receiving; });
    const auto metrics = transport.SnapshotMetrics();
    ExpectTrue(calls == 0 && !metrics.receiving, "rejected batch stops the pump");
    ExpectTrue(metrics.pumpFailures == 1, "stopped pump reports its batch failure");
    (void)transport.StopReceiving();
    transport.Close();
    ExpectTrue(executor->Stop().IsOk(), "small-budget executor stops");
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
    if (!ExpectReceivedSequence(peer, buffer, 1, "accepted sequence starts at one"))
        return;
    if (!Wait([&] { return transport.SendSerialized(Id, Bytes(maximum)).IsOk(); }))
        return;
    if (!ExpectReceivedSequence(
            peer, buffer, 2, "rejected sends consume no sequence and refill works"))
        return;
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
const ServerCoreTest::CheckRegistration d{ "Runtime.DatagramPumpRearmDoesNotWaitOnMonitor",
    PumpRearmDoesNotWaitOnMonitor };
const ServerCoreTest::CheckRegistration e{ "Runtime.DatagramPumpCountsBatchFailures",
    PumpCountsBatchFailures };
}
