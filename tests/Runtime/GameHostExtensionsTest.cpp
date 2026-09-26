#include "ConfigTestSupport.h"
#include "Net/DatagramSocket.h"
#include "Runtime/ServerHostTestAccess.h"
#include "ServerCore/Core/Config.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Protocol/BinaryMessage.h"
#include "ServerCore/Runtime/ServerHost.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace ServerCore;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using namespace std::chrono_literals;

template <class Predicate> bool Wait(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    for (;;)
    {
        if (predicate())
            return true;
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(2ms);
    }
}

std::span<const std::byte> Bytes(std::string_view value)
{
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::uint16_t FreePort()
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == ServerCoreTest::InvalidSocket)
        return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ServerCoreTest::SocketLength size = sizeof(address);
    const bool success =
        ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    ServerCoreTest::CloseSocket(socket);
    return success ? ntohs(address.sin_port) : 0;
}

class Peer
{
public:
    ~Peer() { Close(); }
    void Close()
    {
        if (socket != ServerCoreTest::InvalidSocket)
            ServerCoreTest::CloseSocket(socket);
        socket = ServerCoreTest::InvalidSocket;
    }
    bool Connect(std::uint16_t port, int receiveBufferBytes = 0)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == ServerCoreTest::InvalidSocket ||
            !ServerCoreTest::SetSocketTimeouts(socket, 5000))
            return false;
        if (receiveBufferBytes > 0 && ::setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                                          reinterpret_cast<const char*>(&receiveBufferBytes),
                                          sizeof(receiveBufferBytes)) != 0)
            return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        return ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    bool Send(std::span<const std::byte> bytes)
    {
        while (!bytes.empty())
        {
            const int sent = ServerCoreTest::Send(socket,
                reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (sent <= 0)
                return false;
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
        }
        return true;
    }
    std::vector<std::byte> Receive(std::size_t size)
    {
        std::vector<std::byte> bytes(size);
        std::size_t offset = 0;
        while (offset < size)
        {
            const int got = ServerCoreTest::Receive(socket,
                reinterpret_cast<char*>(bytes.data() + offset), static_cast<int>(size - offset), 0);
            if (got <= 0)
            {
                bytes.resize(offset);
                return bytes;
            }
            offset += static_cast<std::size_t>(got);
        }
        return bytes;
    }

private:
    ServerCoreTest::Socket socket = ServerCoreTest::InvalidSocket;
};

struct Observer : Session::ISessionObserver
{
    std::mutex mutex;
    std::shared_ptr<Session::Session> session;
    std::atomic<int> opened{ 0 }, closed{ 0 };
    std::atomic<Core::ErrorCode> reason{ Core::ErrorCode::Ok };
    std::function<void(const std::shared_ptr<Session::Session>&)> onOpen;
    void OnSessionOpened(const std::shared_ptr<Session::Session>& value) override
    {
        {
            const std::lock_guard guard(mutex);
            session = value;
        }
        if (onOpen)
            onOpen(value);
        ++opened;
    }
    void OnSessionClosed(Session::SessionId, Core::Status status) override
    {
        reason.store(status.Code());
        ++closed;
    }
    std::shared_ptr<Session::Session> Get()
    {
        const std::lock_guard guard(mutex);
        return session;
    }
};

struct ReceiveCounter : Runtime::TestAccess::IBeforeSessionReceiveGate
{
    std::atomic<unsigned> calls{ 0 };
    void WaitBeforeSessionReceive() noexcept override { ++calls; }
};

struct BlockingState
{
    std::atomic<bool> entered{ false };
    void WaitForRelease()
    {
        entered = true;
        std::unique_lock guard(mutex);
        changed.wait(guard, [this] { return released; });
    }
    void Release()
    {
        {
            const std::lock_guard guard(mutex);
            released = true;
        }
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
};

struct ReleaseBlockingOnExit
{
    std::shared_ptr<BlockingState> state;
    ~ReleaseBlockingOnExit() { state->Release(); }
};

class ReceiveGateScope
{
public:
    explicit ReceiveGateScope(std::shared_ptr<ReceiveCounter> gate)
        : mGate(std::move(gate))
    {
        Runtime::TestAccess::InstallBeforeSessionReceiveGate(mGate);
    }
    ~ReceiveGateScope() { Runtime::TestAccess::ClearBeforeSessionReceiveGate(mGate); }
    ReceiveGateScope(const ReceiveGateScope&) = delete;
    ReceiveGateScope& operator=(const ReceiveGateScope&) = delete;

private:
    std::shared_ptr<ReceiveCounter> mGate;
};

bool Start(Runtime::ServerHost& host, Runtime::ServerHostOptions options,
    std::shared_ptr<Observer> observer)
{
    ExpectTrue(host.SetSessionObserver(observer).IsOk(), "observer is set before Start");
    if (!host.Configure(options).IsOk())
    {
        ExpectTrue(false, "host options accepted");
        return false;
    }
    const auto status = host.Start();
    ExpectTrue(status.IsOk(), "host starts");
    return status.IsOk();
}

void HostBinaryAndUdpLifetime()
{
    ServerCoreTest::SocketRuntime sockets;
    auto udp = std::make_shared<Runtime::DatagramTransport>();
    ExpectTrue(udp->Configure({ 1, Protocol::PayloadMode::Binary }).IsOk() &&
                   udp->Bind("127.0.0.1", 0).IsOk(),
        "bounded UDP binds");
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    options.payloadMode = Protocol::PayloadMode::Binary;
    auto observer = std::make_shared<Observer>();
    auto tokenVisible = std::make_shared<std::atomic<bool>>(false);
    observer->onOpen = [&host, tokenVisible](const auto& session)
    { *tokenVisible = host.GetDatagramToken(session->Id()).IsOk(); };
    ExpectTrue(
        host.AttachDatagramTransport(udp).IsOk(), "host attaches caller-owned UDP transport");
    ExpectTrue(host.SetBinaryHandler([](const auto& session, Protocol::BinaryMessageView message)
                       { return session->SendBinary(message.type, message.payload); })
                   .IsOk(),
        "binary handler registers");
    if (!Start(host, options, observer))
        return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "binary peer connects");
    if (!Wait([&] { return observer->opened.load() == 1; }))
    {
        ExpectTrue(false, "session opens");
        return;
    }
    ExpectTrue(tokenVisible->load() && udp->RegisteredSessionCount() == 1,
        "UDP token exists before opened callback");
    const std::array payload{ std::byte{ 0 }, std::byte{ 0xff }, std::byte{ 0x80 } };
    auto envelope = Protocol::EncodeBinaryMessage(7, payload, 64);
    auto frame = Protocol::EncodeFrame(envelope.Value());
    ExpectTrue(peer.Send(frame.Value()) && peer.Receive(frame.Value().size()) == frame.Value(),
        "binary TCP uses existing framing without JSON interpretation");
    auto retained = observer->Get();
    ExpectTrue(retained->GetCancellationToken().stop_possible(),
        "native session exposes a stoppable lifetime token");
    host.Stop();
    ExpectTrue(retained->GetCancellationToken().stop_requested() && observer->closed == 1,
        "stop cancels retained session exactly once");
    ExpectTrue(udp->RegisteredSessionCount() == 0 && udp->Port() != 0,
        "TCP lifetime unregisters UDP without closing caller socket");
}

void SessionCapacityNotifications()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    auto observer = std::make_shared<Observer>();
    if (!Start(host, options, observer))
        return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "capacity peer connects");
    if (!Wait([&] { return observer->opened == 1; }))
    {
        ExpectTrue(false, "capacity session opens");
        return;
    }
    auto session = observer->Get();
    std::atomic<int> called = 0;
    auto ready = session->WaitForSendCapacity(32,
        [&](Core::Status status)
        {
            ExpectTrue(status.IsOk(), "empty session has capacity");
            (void)session->QueuedSendBytes();
            ++called;
        });
    ExpectTrue(ready.IsOk() && called == 1, "session forwards one-shot readiness");
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = session->WaitForSendCapacity(
        32,
        [&](Core::Status status)
        {
            ExpectTrue(status.Code() == Core::ErrorCode::Cancelled,
                "caller cancellation precedes ready notification");
            ++called;
        },
        stop.get_token());
    ExpectTrue(cancelled.IsOk() && called == 2, "session accepts cancelled notification once");
    auto tooLarge =
        session->WaitForSendCapacity(static_cast<std::size_t>(-1), [&](Core::Status) { ++called; });
    ExpectTrue(tooLarge.GetStatus().Code() == Core::ErrorCode::TooLarge && called == 2,
        "impossible wait rejects without callback");
    auto closing = session->WaitForSendCapacity(32,
        [&](Core::Status status)
        {
            ExpectTrue(status.IsOk(), "reentrant close begins from readiness");
            session->Disconnect(Core::Status::Ok());
        });
    ExpectTrue(closing.IsOk() && Wait([&] { return observer->closed == 1; }),
        "readiness callback can disconnect without locking itself");
    ExpectTrue(session->WaitForSendCapacity(32, [](Core::Status) {}).GetStatus().Code() ==
                   Core::ErrorCode::Closed,
        "closed session rejects new waits");
    host.Stop();
}

void DatagramRegistryAndBinary()
{
    Runtime::DatagramTransport transport;
    ExpectTrue(!transport.Configure({ 0 }).IsOk(), "zero UDP registry cap rejected");
    ExpectTrue(transport.Configure({ 1, Protocol::PayloadMode::Binary }).IsOk(),
        "binary UDP config accepted");
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk(), "UDP binds");
    const auto one = static_cast<Session::SessionId>(1), two = static_cast<Session::SessionId>(2);
    const auto token = transport.RegisterSession(one);
    if (!token.IsOk())
    {
        ExpectTrue(false, "first token registered");
        return;
    }
    ExpectTrue(transport.RegisterSession(two).GetStatus().Code() == Core::ErrorCode::WouldBlock,
        "registration cap is enforced before allocation");
    Net::DatagramSocket peer;
    ExpectTrue(peer.Bind("127.0.0.1", 0).IsOk(), "raw UDP peer binds");
    const auto endpoint = Core::IpEndpoint::Parse("127.0.0.1", transport.Port()).Value();
    auto binary = Protocol::EncodeBinaryMessage(9, Bytes(std::string_view("a\0b", 3)), 64);
    std::array<std::byte, Protocol::DatagramCodec::MaximumDatagramBytes> packet{};
    const auto size = Protocol::DatagramCodec::Encode(packet, token.Value(), 1, binary.Value());
    bool received = false;
    ExpectTrue(peer.Send(endpoint, std::span(packet).first(size)).IsOk(), "binary datagram sent");
    const auto admit = [](const Runtime::DatagramMessageView&, Protocol::BinaryMessageView)
    { return true; };
    ExpectTrue(
        Wait(
            [&]
            {
                transport.PollBinary(admit,
                    [&](Session::SessionId id, Protocol::BinaryMessageView message)
                    {
                        received = id == one && message.type == 9 && message.payload.size() == 3;
                        ExpectTrue(transport.SendBinary(id, message.type, message.payload).IsOk(),
                            "binary UDP reply uses learned endpoint");
                    });
                return received;
            }),
        "binary datagram decoded without JSON parsing");
    std::array<std::byte, 1200> reply{};
    bool replied = Wait([&] { return peer.Receive(reply).status.IsOk(); });
    ExpectTrue(replied, "raw peer receives binary reply");
    transport.UnregisterSession(one);
    ExpectTrue(transport.RegisterSession(two).IsOk(), "unregister returns registry capacity");
    ExpectTrue(!transport.GetToken(one).IsOk(), "old token no longer exposed");
}

void HostInputAdmission()
{
    ServerCoreTest::SocketRuntime sockets;
    for (int scenario = 0; scenario != 6; ++scenario)
    {
        Runtime::ServerHost host;
        Runtime::ServerHostOptions options;
        options.port = FreePort();
        options.idleSessionTimeout = 5s;
        if (scenario == 0)
            options.frameCompletionTimeout = 60ms;
        if (scenario == 1)
            options.authenticationTimeout = 60ms;
        if (scenario == 2)
        {
            options.maxBodySize = 4;
            options.maxInputBytesPerSecond = 8;
        }
        if (scenario == 3)
            options.maxInputFramesPerSecond = 1;
        if (scenario == 4)
            options.frameCompletionTimeout = 60ms;
        if (scenario == 5)
            options.maxPendingReceiveChunks = 1;
        auto observer = std::make_shared<Observer>();
        if (!Start(host, options, observer))
            return;
        Peer peer;
        ExpectTrue(peer.Connect(host.Port()), "limited peer connects");
        if (!Wait([&] { return observer->opened == 1; }))
        {
            ExpectTrue(false, "limited session opens");
            return;
        }
        if (scenario == 0)
        {
            const std::array partial{ std::byte{ 32 }, std::byte{ 0 } };
            ExpectTrue(peer.Send(partial), "partial frame starts absolute deadline");
            std::this_thread::sleep_for(30ms);
            ExpectTrue(
                peer.Send(std::span(partial).last(1)), "additional bytes do not restart deadline");
        }
        if (scenario == 2)
            ExpectTrue(peer.Send(Bytes("123456789")), "oversize rate window bytes reach server");
        if (scenario == 3)
        {
            auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"Ignored","body":{}})"));
            auto combined = frame.Value();
            combined.insert(combined.end(), frame.Value().begin(), frame.Value().end());
            ExpectTrue(peer.Send(combined), "two valid frames reach one admission window");
        }
        if (scenario >= 4)
        {
            auto receives = std::make_shared<ReceiveCounter>();
            const ReceiveGateScope receiveGate(receives);
            auto blocker = std::make_shared<BlockingState>();
            const ReleaseBlockingOnExit releaseOnExit{ blocker };
            // Releasing a callback is not a join. Its state must stay owned by
            // the queued/running job after this inner fixture scope disappears.
            ExpectTrue(host.GetJobRunner().Post([blocker] { blocker->WaitForRelease(); }).IsOk(),
                "runner blocked for queued-input regression");
            if (!Wait([&] { return blocker->entered.load(); }))
            {
                ExpectTrue(false, "blocking job enters");
                return;
            }
            auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"Ignored","body":{}})"));
            ExpectTrue(peer.Send(std::span(frame.Value()).first(2)),
                "first fragment queued behind runner");
            ExpectTrue(
                Wait([&] { return receives->calls >= 1; }), "first fragment reaches I/O boundary");
            std::this_thread::sleep_for(100ms);
            ExpectTrue(peer.Send(std::span(frame.Value()).subspan(2)),
                "second fragment reaches same pending aggregate");
            ExpectTrue(Wait([&] { return receives->calls >= 2; }),
                "second fragment has separate I/O timestamp");
            std::this_thread::sleep_for(10ms);
            blocker->Release();
        }
        ExpectTrue(
            Wait([&] { return observer->closed == 1; }), "input policy closes violating session");
        ExpectTrue(observer->reason == (scenario < 2 || scenario == 4 ? Core::ErrorCode::Timeout
                                                                      : Core::ErrorCode::TooLarge),
            "deadline and rate errors stay distinct");
        host.Stop();
    }
}

void HostGracefulDrain()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    options.payloadMode = Protocol::PayloadMode::Binary;
    auto observer = std::make_shared<Observer>();
    auto blocker = std::make_shared<BlockingState>();
    const ReleaseBlockingOnExit releaseOnExit{ blocker };
    ExpectTrue(host.SetBinaryHandler(
                       [blocker](const auto& session, Protocol::BinaryMessageView message)
                       {
                           blocker->WaitForRelease();
                           return session->SendBinary(message.type, message.payload);
                       })
                   .IsOk(),
        "drain handler registers");
    if (!Start(host, options, observer))
        return;
    auto peer = std::make_unique<Peer>();
    ExpectTrue(peer->Connect(host.Port()), "drain peer connects");
    auto body = Protocol::EncodeBinaryMessage(4, Bytes("response"), 64);
    auto frame = Protocol::EncodeFrame(body.Value());
    ExpectTrue(peer->Send(frame.Value()), "request queued before drain");
    if (!Wait([&] { return blocker->entered.load(); }))
    {
        ExpectTrue(false, "handler starts");
        return;
    }
    Core::Status result = Core::Status::Ok();
    std::jthread stop(
        [&host, &result] { result = host.StopGracefully(std::chrono::steady_clock::now() + 3s); });
    // On an early exit/unwind, unblock the handler before jthread joins the
    // graceful-stop thread. result and host are both still alive at that join.
    const ReleaseBlockingOnExit releaseBeforeJoin{ blocker };
    ExpectTrue(Wait([&] { return !host.IsRunning(); }), "drain closes listener admission promptly");
    blocker->Release();
    ExpectTrue(peer->Receive(frame.Value().size()) == frame.Value(),
        "already admitted handler response drains before EOF");
    ExpectTrue(peer->Receive(1).empty(), "the peer read EOF after the drained response");
    // A client closes its socket after EOF. The transport finishes its graceful close only then.
    peer.reset();
    stop.join();
    ExpectTrue(host.GetJobRunner().Post([] {}).Code() == Core::ErrorCode::Closed,
        "drain closes application job admission");
    ExpectTrue(result.IsOk() && observer->closed == 1 && host.DrainStatus().IsOk(),
        "graceful drain completes and stays idempotent");
    Runtime::ServerHost blocked;
    options.port = FreePort();
    options.payloadMode = Protocol::PayloadMode::Json;
    auto other = std::make_shared<Observer>();
    if (!Start(blocked, options, other))
        return;
    auto reservation = blocked.GetJobRunner().Reserve();
    ExpectTrue(reservation.IsOk(), "background continuation capacity reserved");
    const auto timeout = blocked.StopGracefully(std::chrono::steady_clock::now() + 30ms);
    ExpectTrue(timeout.Code() == Core::ErrorCode::Timeout && !blocked.IsRunning(),
        "deadline aborts a drain held by an unused reservation");
}

/// <summary>I/O worker를 수신 callback 입구에서 멈춰 송신 완료가 돌지 못하게 하는 gate다.</summary>
struct BlockingReceiveGate : Runtime::TestAccess::IBeforeSessionReceiveGate
{
    explicit BlockingReceiveGate(std::shared_ptr<BlockingState> value)
        : state(std::move(value))
    {
    }
    void WaitBeforeSessionReceive() noexcept override { state->WaitForRelease(); }
    std::shared_ptr<BlockingState> state;
};

class BlockingReceiveGateScope
{
public:
    explicit BlockingReceiveGateScope(std::shared_ptr<BlockingReceiveGate> gate)
        : mGate(std::move(gate))
    {
        Runtime::TestAccess::InstallBeforeSessionReceiveGate(mGate);
    }
    ~BlockingReceiveGateScope()
    {
        mGate->state->Release();
        Runtime::TestAccess::ClearBeforeSessionReceiveGate(mGate);
    }
    BlockingReceiveGateScope(const BlockingReceiveGateScope&) = delete;
    BlockingReceiveGateScope& operator=(const BlockingReceiveGateScope&) = delete;

private:
    std::shared_ptr<BlockingReceiveGate> mGate;
};

/// <summary>교착한 스레드는 join할 수 없으므로, 실패를 남기고 이 검사 프로세스를 끝낸다.</summary>
[[noreturn]] void AbandonDeadlockedCheck(std::string_view what)
{
    ExpectTrue(false, what);
    std::_Exit(1);
}

enum class CapacityClose
{
    RunnerDisconnect,
    RunnerSendAndDisconnect,
    IdleTimeout,
    Drain
};

/// <summary>대기 중인 송신 용량 callback이 세션 종료 경로에서 어떤 잠금 아래 불리는지 본다.</summary>
struct CapacityProbe
{
    std::atomic<int> calls{ 0 };
    std::atomic<Core::ErrorCode> code{ Core::ErrorCode::Ok };
    std::atomic<bool> otherThreadEntered{ false };
    std::atomic<bool> disconnectThrew{ false };
    std::atomic<bool> finished{ false };
    std::thread other;
};

void CapacityCallbackRunsOutsideSessionLocks(const CapacityClose scenario, const char* name)
{
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    options.ioWorkerThreadCount = 1;
    if (scenario == CapacityClose::IdleTimeout)
        options.idleSessionTimeout = 2s;
    auto observer = std::make_shared<Observer>();
    if (!Start(host, options, observer))
        return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port(), 1024), name);
    if (!Wait([&] { return observer->opened == 1; }))
    {
        ExpectTrue(false, name);
        return;
    }
    const auto session = observer->Get();

    auto blocker = std::make_shared<BlockingState>();
    const Protocol::JsonValue body(Protocol::JsonValue::Object{});
    std::optional<BlockingReceiveGateScope> gateScope;
    if (scenario != CapacityClose::Drain)
    {
        // 하나뿐인 I/O worker를 멈춰 두면 아래 Send의 완료가 돌지 못해 보유 바이트가 남는다.
        gateScope.emplace(std::make_shared<BlockingReceiveGate>(blocker));
        const std::array trigger{ std::byte{ 0 } };
        ExpectTrue(peer.Send(trigger), name);
        if (!Wait([&] { return blocker->entered.load(); }))
        {
            ExpectTrue(false, name);
            return;
        }
        ExpectTrue(
            session->Send("capacity.retained", body).IsOk() && session->QueuedSendBytes() != 0,
            "retained send bytes stay queued while the I/O worker is blocked");
    }
    else
    {
        // drain은 수락기를 멈추므로 I/O worker가 돌아야 한다. 대신 읽지 않는 상대로 송신 큐를 채운다.
        const Protocol::JsonValue payload(Protocol::JsonValue::Object{
            { "payload", Protocol::JsonValue(std::string(60000, 'x')) } });
        bool full = false;
        for (int round = 0; round < 40 && !full; ++round)
        {
            for (int attempt = 0; attempt < 100 && !full; ++attempt)
                full =
                    session->Send("capacity.fill", payload).Code() == Core::ErrorCode::WouldBlock;
            if (!full)
                std::this_thread::sleep_for(50ms);
        }
        ExpectTrue(full, "a non-reading peer fills the session send queue");
        if (!full)
            return;
    }

    // Reset한 대기는 세션당 하나인 등록 자리를 돌려줘야 한다. 그렇지 않으면 아래 등록이 AlreadyExists다.
    auto abandonedCalls = std::make_shared<std::atomic<int>>(0);
    auto abandoned = session->WaitForSendCapacity(
        Net::SendQueueLimitBytes, [abandonedCalls](Core::Status) { ++*abandonedCalls; });
    ExpectTrue(abandoned.IsOk() && abandoned.Value().IsPending(), "first capacity wait is pending");
    if (abandoned.IsOk())
        abandoned.Value().Reset();

    auto probe = std::make_shared<CapacityProbe>();
    auto subscription = session->WaitForSendCapacity(Net::SendQueueLimitBytes,
        [probe, session, &body](Core::Status status)
        {
            ++probe->calls;
            probe->code = status.Code();
            // 이 callback이 세션 잠금을 쥐고 있으면 다른 스레드의 Send는 callback이 끝날 때까지 막힌다.
            auto entered = std::make_shared<std::promise<void>>();
            auto enteredFuture = entered->get_future();
            probe->other = std::thread(
                [session, entered, &body]
                {
                    (void)session->Send("capacity.probe", body);
                    entered->set_value();
                });
            probe->otherThreadEntered = enteredFuture.wait_for(5s) == std::future_status::ready;
            // 흔한 사용 형태: 용량 대기가 실패하면 그 세션을 끊는다.
            try
            {
                session->Disconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::Timeout));
            }
            catch (...)
            {
                probe->disconnectThrew = true;
            }
            probe->finished = true;
        });
    ExpectTrue(subscription.IsOk() && probe->calls == 0,
        "capacity wait stays pending behind retained bytes");
    if (!subscription.IsOk())
        return;

    auto triggered = std::make_shared<std::atomic<bool>>(scenario == CapacityClose::IdleTimeout);
    std::thread drainer;
    if (scenario == CapacityClose::RunnerDisconnect ||
        scenario == CapacityClose::RunnerSendAndDisconnect)
    {
        ExpectTrue(host.GetJobRunner()
                       .Post(
                           [session, scenario, triggered, &body]
                           {
                               if (scenario == CapacityClose::RunnerDisconnect)
                                   session->Disconnect(
                                       Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
                               else
                                   (void)session->SendAndDisconnect(
                                       { "capacity.final", &body, nullptr, nullptr },
                                       Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
                               *triggered = true;
                           })
                       .IsOk(),
            name);
    }
    else if (scenario == CapacityClose::Drain)
    {
        drainer = std::thread(
            [&host, triggered]
            {
                (void)host.BeginDrain();
                *triggered = true;
            });
    }
    // 교착이면 callback이나 종료 호출이 끝나지 않는다. 멈춘 스레드는 되돌릴 수 없으므로 검사를 끝낸다.
    // callback 안의 탐침이 최대 5초를 쓰므로 그보다 넉넉히 기다린다.
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!(probe->finished.load() && triggered->load()))
    {
        if (std::chrono::steady_clock::now() >= deadline)
            AbandonDeadlockedCheck(name);
        std::this_thread::sleep_for(2ms);
    }
    if (drainer.joinable())
        drainer.join();
    if (probe->other.joinable())
        probe->other.join();

    ExpectEqual(1, probe->calls.load(), name);
    ExpectTrue(probe->code.load() != Core::ErrorCode::Ok,
        "pending capacity wait completes with a close status");
    ExpectTrue(probe->otherThreadEntered.load(),
        "another thread enters the same session while the capacity callback runs");
    ExpectTrue(
        !probe->disconnectThrew.load(), "Disconnect from the capacity callback returns normally");
    ExpectEqual(0, abandonedCalls->load(), "a reset capacity wait never calls back");
    blocker->Release();
    host.Stop();
}

void HostCapacityCallbackOutsideSessionLocks()
{
    ServerCoreTest::SocketRuntime sockets;
    CapacityCallbackRunsOutsideSessionLocks(
        CapacityClose::RunnerDisconnect, "runner Disconnect closes a pending capacity wait");
    CapacityCallbackRunsOutsideSessionLocks(CapacityClose::RunnerSendAndDisconnect,
        "runner SendAndDisconnect closes a pending capacity wait");
    CapacityCallbackRunsOutsideSessionLocks(
        CapacityClose::IdleTimeout, "idle timeout closes a pending capacity wait");
    CapacityCallbackRunsOutsideSessionLocks(
        CapacityClose::Drain, "drain closes a pending capacity wait");
}

/// <summary>세션 수명 통지가 어느 스레드에서 실행됐는지 기록한다.</summary>
struct ThreadRecordingObserver : Session::ISessionObserver
{
    std::mutex mutex;
    std::thread::id runner;
    std::atomic<int> opened{ 0 }, closed{ 0 }, closedOffRunner{ 0 };
    void OnSessionOpened(const std::shared_ptr<Session::Session>&) override
    {
        {
            const std::lock_guard guard(mutex);
            runner = std::this_thread::get_id();
        }
        ++opened;
    }
    void OnSessionClosed(Session::SessionId, Core::Status) override
    {
        bool onRunner = false;
        {
            const std::lock_guard guard(mutex);
            onRunner = runner == std::this_thread::get_id();
        }
        if (!onRunner)
            ++closedOffRunner;
        ++closed;
    }
};

void HostControlLaneFitsSessionWork()
{
    ServerCoreTest::SocketRuntime sockets;
    constexpr int Sessions = 8;
    constexpr std::size_t ConfiguredControlJobs = 2;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    options.ioWorkerThreadCount = 1;
    options.maxConcurrentSessions = Sessions;
    options.jobRunner.maxControlJobs = ConfiguredControlJobs;
    auto observer = std::make_shared<ThreadRecordingObserver>();
    auto dispatched = std::make_shared<std::atomic<int>>(0);
    ExpectTrue(host.GetDispatcher()
                   .Register("lane.count",
                       [dispatched](const auto&, const auto&)
                       {
                           ++*dispatched;
                           return Core::Status::Ok();
                       })
                   .IsOk(),
        "control-lane handler registers");
    ExpectTrue(host.SetSessionObserver(observer).IsOk(), "observer is set before Start");
    if (!host.Configure(options).IsOk() || !host.Start().IsOk())
    {
        ExpectTrue(false, "control-lane host starts");
        return;
    }

    std::array<Peer, Sessions> peers;
    for (int index = 0; index != Sessions; ++index)
    {
        // 한 번에 하나씩 열어, 열기 작업 자체가 설정한 control 용량을 넘지 않게 한다.
        ExpectTrue(peers[index].Connect(host.Port()), "control-lane peer connects");
        if (!Wait([&] { return observer->opened == index + 1; }))
        {
            ExpectTrue(false, "control-lane session opens");
            return;
        }
    }

    auto blocker = std::make_shared<BlockingState>();
    const ReleaseBlockingOnExit releaseOnExit{ blocker };
    ExpectTrue(host.GetJobRunner().Post([blocker] { blocker->WaitForRelease(); }).IsOk(),
        "runner blocked");
    if (!Wait([&] { return blocker->entered.load(); }))
    {
        ExpectTrue(false, "blocking job enters");
        return;
    }

    auto receives = std::make_shared<ReceiveCounter>();
    const ReceiveGateScope receiveGate(receives);
    auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"lane.count","body":{}})"));
    for (int index = 0; index != Sessions; ++index)
    {
        ExpectTrue(peers[index].Send(frame.Value()), "frame queued behind the blocked runner");
        if (!Wait([&] { return receives->calls >= static_cast<unsigned>(index + 1); }))
        {
            ExpectTrue(false, "frame reaches I/O");
            return;
        }
    }
    // I/O worker가 하나이므로, 이 수신 callback이 시작됐다면 앞의 모든 수신 callback은 끝났다.
    ExpectTrue(peers[0].Send(frame.Value()), "sentinel frame sent");
    if (!Wait([&] { return receives->calls >= Sessions + 1; }))
    {
        ExpectTrue(false, "sentinel reaches I/O");
        return;
    }
    ExpectEqual(0, observer->closed.load(),
        "no session closes while its input waits for the blocked runner");

    blocker->Release();
    ExpectTrue(Wait([&] { return dispatched->load() == Sessions + 1; }),
        "every queued frame dispatches although more receive jobs waited than the configured "
        "control capacity");
    for (auto& peer : peers)
        peer.Close();
    ExpectTrue(
        Wait([&] { return observer->closed == Sessions; }), "every session close is notified");
    host.Stop();
    ExpectEqual(Sessions, observer->closed.load(), "close notification count");
    ExpectEqual(0, observer->closedOffRunner.load(), "close notifications run on the Host runner");
}

/// <summary>drain이 이미 받은 요청의 게임 후속 작업(Post·Reserve·관측)을 막지 않는지 본다.</summary>
void HostDrainAcceptsGameContinuations()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    auto observer = std::make_shared<Observer>();
    auto blocker = std::make_shared<BlockingState>();
    const ReleaseBlockingOnExit releaseOnExit{ blocker };
    auto reserved = std::make_shared<std::atomic<bool>>(false);
    auto posted = std::make_shared<std::atomic<bool>>(false);
    const Protocol::JsonValue reply(Protocol::JsonValue::Object{});
    const auto runner = host.GetJobRunner();
    ExpectTrue(host.GetDispatcher()
                   .Register("drain.request",
                       [blocker, reserved, posted, runner, &reply](
                           const std::shared_ptr<Session::Session>& session, const auto&)
                       {
                           blocker->WaitForRelease();
                           auto reservation = runner.Reserve();
                           *reserved = reservation.IsOk() && reservation.Value().Post([] {}).IsOk();
                           *posted = runner
                                         .Post([session, &reply]
                                             { (void)session->Send("drain.reply", reply); })
                                         .IsOk();
                           return Core::Status::Ok();
                       })
                   .IsOk(),
        "drain handler registers");
    if (!Start(host, options, observer))
        return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "drain peer connects");
    const auto request = Protocol::EncodeFrame(Bytes(R"({"type":"drain.request","body":{}})"));
    ExpectTrue(peer.Send(request.Value()), "request admitted before drain");
    if (!Wait([&] { return blocker->entered.load(); }))
    {
        ExpectTrue(false, "drain handler starts");
        return;
    }

    ExpectTrue(host.BeginDrain().IsOk(), "drain begins while the admitted request runs");
    auto observed = std::make_shared<std::atomic<int>>(-1);
    const auto snapshotPosted = runner.Post(
        [&host, observed]
        {
            const auto metrics = host.SnapshotMetrics();
            *observed = metrics.IsOk() ? static_cast<int>(metrics.Value().lifecycle) : -2;
        });
    ExpectTrue(
        snapshotPosted.IsOk(), "drain progress can still be observed through the game runner");
    blocker->Release();

    const auto header = peer.Receive(Protocol::HeaderSize);
    ExpectTrue(
        header.size() == Protocol::HeaderSize, "handler continuation reply arrives before EOF");
    std::uint32_t bodySize = 0;
    for (std::size_t index = 0; index != header.size(); ++index)
        bodySize |= std::to_integer<std::uint32_t>(header[index]) << (8u * index);
    ExpectEqual(static_cast<std::size_t>(bodySize), peer.Receive(bodySize).size(),
        "handler continuation reply body arrives before EOF");
    // 서버는 보내기를 닫은 뒤 상대가 닫을 때까지 기다린다. 보통 클라이언트처럼 EOF를 읽고 닫는다.
    ExpectTrue(peer.Receive(1).empty(), "the peer reads EOF after the drained reply");
    peer.Close();
    ExpectTrue(reserved->load(), "admitted handler reserves a completion during drain");
    ExpectTrue(posted->load(), "admitted handler posts a continuation during drain");
    ExpectTrue(Wait([&] { return observed->load() != -1; }), "posted observation runs");
    ExpectEqual(static_cast<int>(Observability::Lifecycle::Draining), observed->load(),
        "observation sees Draining");
    ExpectTrue(
        Wait([&] { return host.DrainStatus().IsOk(); }), "drain completes after the continuation");
    ExpectTrue(runner.Post([] {}).Code() == Core::ErrorCode::Closed,
        "game admission closes once drain sends start");
    host.Stop();
}

/// <summary>Binary Host에서 잘못된 봉투 종류의 종료 요청과 송신 실패가 어떻게 남는지 본다.</summary>
void HostBinaryGracefulClose()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    options.payloadMode = Protocol::PayloadMode::Binary;
    auto observer = std::make_shared<Observer>();
    struct Result
    {
        Core::ErrorCode jsonClose = Core::ErrorCode::Ok;
        Session::SessionState stateAfterJsonClose = Session::SessionState::Closed;
        std::uint64_t errorsBefore = 0, errorsAfter = 0;
    };
    auto result = std::make_shared<Result>();
    auto handled = std::make_shared<std::atomic<bool>>(false);
    const auto payload = Bytes("bye");
    ExpectTrue(host.SetBinaryHandler(
                       [&host, result, handled, payload](
                           const auto& session, Protocol::BinaryMessageView message)
                       {
                           if (message.type == 6)
                               return session->SendBinaryAndDisconnect(10, payload,
                                   Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
                           const Protocol::JsonValue body(Protocol::JsonValue::Object{});
                           const auto before = host.SnapshotMetrics();
                           result->errorsBefore = before.IsOk() ? before.Value().errorCount : 99;
                           result->jsonClose =
                               session
                                   ->SendAndDisconnect({ "json.close", &body, nullptr, nullptr },
                                       Core::Status::FailWithoutMessage(Core::ErrorCode::Closed))
                                   .Code();
                           result->stateAfterJsonClose = session->State();
                           (void)session->SendBinary(0, payload);
                           const auto after = host.SnapshotMetrics();
                           result->errorsAfter = after.IsOk() ? after.Value().errorCount : 0;
                           *handled = true;
                           return session->SendBinary(9, payload);
                       })
                   .IsOk(),
        "binary close handler registers");
    if (!Start(host, options, observer))
        return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "binary close peer connects");
    auto request = Protocol::EncodeFrame(Protocol::EncodeBinaryMessage(5, payload, 64).Value());
    ExpectTrue(peer.Send(request.Value()), "binary close request sent");
    const auto expected =
        Protocol::EncodeFrame(Protocol::EncodeBinaryMessage(9, payload, 64).Value());
    ExpectTrue(peer.Receive(expected.Value().size()) == expected.Value(),
        "binary reply after rejected JSON close");
    ExpectTrue(handled->load(), "binary close handler ran");
    ExpectEqual(static_cast<int>(Core::ErrorCode::InvalidArgument),
        static_cast<int>(result->jsonClose), "JSON SendAndDisconnect is rejected on a binary Host");
    ExpectEqual(static_cast<int>(Session::SessionState::Connected),
        static_cast<int>(result->stateAfterJsonClose),
        "rejected JSON SendAndDisconnect leaves the binary session open");
    ExpectEqual(result->errorsBefore + 2, result->errorsAfter,
        "rejected JSON close and invalid SendBinary are both counted as errors");

    auto closeRequest =
        Protocol::EncodeFrame(Protocol::EncodeBinaryMessage(6, payload, 64).Value());
    ExpectTrue(peer.Send(closeRequest.Value()), "binary graceful close request sent");
    const auto last = Protocol::EncodeFrame(Protocol::EncodeBinaryMessage(10, payload, 64).Value());
    ExpectTrue(peer.Receive(last.Value().size()) == last.Value(), "final binary frame arrives");
    ExpectTrue(peer.Receive(1).empty(), "binary session closes after its final frame");
    // 서버는 보내기를 닫은 뒤 상대가 닫을 때까지 기다린다. 보통 클라이언트처럼 EOF를 읽고 닫는다.
    peer.Close();
    ExpectTrue(Wait([&] { return observer->closed == 1; }), "binary graceful close is notified");
    ExpectEqual(static_cast<int>(Core::ErrorCode::Closed),
        static_cast<int>(observer->reason.load()), "binary graceful close keeps its local reason");
    host.Stop();
}

/// <summary>한 세션의 큰 수신 batch가 runner를 끝까지 독점하지 않고 다른 작업과 번갈아 도는지 본다.</summary>
void HostBoundsFramesPerReceiveJob()
{
    ServerCoreTest::SocketRuntime sockets;
    constexpr int Frames = 201;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    options.ioWorkerThreadCount = 1;
    auto observer = std::make_shared<Observer>();
    auto handled = std::make_shared<std::atomic<int>>(0);
    ExpectTrue(host.GetDispatcher()
                   .Register("tiny",
                       [handled](const auto&, const auto&)
                       {
                           ++*handled;
                           return Core::Status::Ok();
                       })
                   .IsOk(),
        "tiny handler registers");
    if (!Start(host, options, observer))
        return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "burst peer connects");
    if (!Wait([&] { return observer->opened == 1; }))
    {
        ExpectTrue(false, "burst session opens");
        return;
    }

    auto blocker = std::make_shared<BlockingState>();
    const ReleaseBlockingOnExit releaseOnExit{ blocker };
    const auto runner = host.GetJobRunner();
    ExpectTrue(runner.Post([blocker] { blocker->WaitForRelease(); }).IsOk(), "runner blocked");
    if (!Wait([&] { return blocker->entered.load(); }))
    {
        ExpectTrue(false, "blocking job enters");
        return;
    }

    auto receives = std::make_shared<ReceiveCounter>();
    const ReceiveGateScope receiveGate(receives);
    const auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"tiny","body":{}})"));
    std::vector<std::byte> burst;
    for (int index = 0; index != Frames - 1; ++index)
        burst.insert(burst.end(), frame.Value().begin(), frame.Value().end());
    ExpectTrue(peer.Send(burst), "burst sent while the runner is blocked");
    if (!Wait([&] { return receives->calls >= 1; }))
    {
        ExpectTrue(false, "burst reaches I/O");
        return;
    }
    // 같은 연결의 다음 수신 callback이 시작됐다면 첫 callback이 수신 작업을 이미 넣었다.
    ExpectTrue(peer.Send(frame.Value()), "sentinel frame sent");
    if (!Wait([&] { return receives->calls >= 2; }))
    {
        ExpectTrue(false, "sentinel reaches I/O");
        return;
    }

    auto atMarker = std::make_shared<std::atomic<int>>(-1);
    ExpectTrue(runner.Post([handled, atMarker] { *atMarker = handled->load(); }).IsOk(),
        "marker job queued behind the receive job");
    blocker->Release();
    ExpectTrue(Wait([&] { return handled->load() == Frames; }), "every burst frame dispatches");
    ExpectTrue(Wait([&] { return atMarker->load() != -1; }), "marker job runs");
    ExpectTrue(atMarker->load() > 0, "the receive job dispatched frames before the marker");
    ExpectTrue(atMarker->load() < Frames,
        "a later runner job interleaves before one batch dispatches all its frames");
    host.Stop();
}

/// <summary>최대 크기 frame 하나도 받을 수 없는 초당 입력 상한을 설정에서 거절하는지 본다.</summary>
void HostRejectsUnsendableInputRate()
{
    Runtime::ServerHostOptions options;
    options.port = 1;
    options.maxBodySize = 1024;
    options.maxInputBytesPerSecond = options.maxBodySize;
    Runtime::ServerHost tooSmall;
    ExpectEqual(static_cast<int>(Core::ErrorCode::InvalidArgument),
        static_cast<int>(tooSmall.Configure(options).Code()),
        "input byte rate below one maximum frame is rejected");
    options.maxInputBytesPerSecond =
        options.maxBodySize + static_cast<std::uint32_t>(Protocol::HeaderSize);
    Runtime::ServerHost exact;
    ExpectTrue(
        exact.Configure(options).IsOk(), "input byte rate that fits one maximum frame is accepted");
}

struct Log : Core::ILogger
{
    std::atomic<unsigned> count{ 0 };
    void Write(Core::LogLevel, std::string_view) noexcept override { ++count; }
};

void HostConcurrentIsolation()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost first, second;
    auto firstLog = std::make_shared<Log>(), secondLog = std::make_shared<Log>();
    ExpectTrue(first.SetLogger(firstLog).IsOk() && second.SetLogger(secondLog).IsOk(),
        "loggers are set before Start");
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    if (!Start(first, options, std::make_shared<Observer>()))
        return;
    options.port = FreePort();
    if (!Start(second, options, std::make_shared<Observer>()))
        return;
    Peer one, two;
    ExpectTrue(one.Connect(first.Port()) && two.Connect(second.Port()),
        "independent hosts accept simultaneously");
    auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"Unregistered","body":{}})"));
    ExpectTrue(one.Send(frame.Value()), "first host receives unknown message");
    ExpectTrue(Wait([&] { return firstLog->count != 0; }) && secondLog->count == 0,
        "first host diagnostics reach only first logger");
    first.Stop();
    ExpectTrue(two.Send(frame.Value()) && Wait([&] { return secondLog->count != 0; }),
        "second host survives first host stop");
    second.Stop();
}

const ServerCoreTest::CheckRegistration BinaryUdp(
    "Runtime.HostBinaryAndUdpLifetime", HostBinaryAndUdpLifetime);
const ServerCoreTest::CheckRegistration Capacity(
    "Runtime.SessionCapacityNotifications", SessionCapacityNotifications);
const ServerCoreTest::CheckRegistration DatagramBinary(
    "Runtime.DatagramRegistryAndBinary", DatagramRegistryAndBinary);
const ServerCoreTest::CheckRegistration Input("Runtime.HostInputAdmission", HostInputAdmission);
const ServerCoreTest::CheckRegistration Drain("Runtime.HostGracefulDrain", HostGracefulDrain);
/// <summary>마지막 기록 한 줄을 보관한다.</summary>
struct LineLog : Core::ILogger
{
    std::mutex mutex;
    std::string last;
    std::atomic<unsigned> count{ 0 };
    void Write(Core::LogLevel, std::string_view line) noexcept override
    {
        try
        {
            const std::lock_guard guard(mutex);
            last.assign(line);
        }
        catch (...)
        {
        }
        ++count;
    }
};

/// <summary>관측·기록·Run 반환값이 Host 수명의 경계에서 쓸 수 있는 값을 주는지 본다.</summary>
void HostObservationAndLogBoundaries()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost idle;
    ExpectEqual(1, idle.Run(), "Run before Start is not a normal shutdown");

    Runtime::ServerHost host;
    auto log = std::make_shared<LineLog>();
    ExpectTrue(host.SetLogger(log).IsOk(), "logger is set before Start");
    ExpectTrue(host.GetDispatcher()
                   .Register("fail", [](const auto&, const auto&)
                       { return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidFormat); })
                   .IsOk(),
        "failing handler registers");
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    if (!Start(host, options, std::make_shared<Observer>()))
        return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "log peer connects");
    const auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"fail","body":{}})"));
    ExpectTrue(peer.Send(frame.Value()), "failing request sent");
    ExpectTrue(Wait([&] { return log->count != 0; }), "handler failure is logged");
    {
        const std::lock_guard guard(log->mutex);
        ExpectTrue(log->last.find(R"("error_code":"2")") != std::string::npos,
            "a message-less failure log names its error code");
    }

    auto sampled = std::make_shared<std::atomic<int>>(0);
    ExpectTrue(host.GetJobRunner()
                   .Post(
                       [&host, sampled]
                       {
                           const auto metrics = host.SnapshotMetrics();
                           // 같은 GetMetrics 안의 두 값이다. 예약이 없으면 실행 중인 관측 작업만큼만 달라질 수 있다.
                           *sampled = !metrics.IsOk() ? -1
                                      : metrics.Value().jobs.outstandingJobs ==
                                              metrics.Value().jobs.pendingJobs
                                          ? 1
                                          : 2;
                       })
                   .IsOk(),
        "observation job posted");
    ExpectTrue(Wait([&] { return sampled->load() != 0; }), "observation job runs");
    ExpectEqual(
        1, sampled->load(), "a runner observation does not count its own job as outstanding work");

    host.Stop();
    const auto stopped = Observability::Observe(host);
    ExpectTrue(stopped.IsOk(), "a stopped Host can be observed from any thread");
    if (stopped.IsOk())
    {
        ExpectEqual(static_cast<int>(Observability::Lifecycle::Stopped),
            static_cast<int>(stopped.Value().lifecycle),
            "stopped Host observation reports Stopped");
        ExpectEqual(std::uint64_t{ 0 }, stopped.Value().drainRemaining,
            "stopped Host has no drain work left");
    }
    ExpectEqual(0, host.Run(), "Run after a completed Stop reports a normal shutdown");
}

/// <summary>모든 기록 줄을 보관한다.</summary>
struct AllLinesLog : Core::ILogger
{
    std::mutex mutex;
    std::vector<std::string> lines;
    void Write(Core::LogLevel, std::string_view line) noexcept override
    {
        try
        {
            const std::lock_guard guard(mutex);
            lines.emplace_back(line);
        }
        catch (...)
        {
        }
    }
};

struct FailureLogCount
{
    std::size_t written = 0;
    std::uint64_t suppressed = 0;
};

/// <summary>실패 메시지와 알 수 없는 타입을 섞은 burst를 보내고, 쓴 줄과 요약한 수를 센다.</summary>
FailureLogCount RunFailureLogBurst(const std::uint32_t limit, const int messages)
{
    FailureLogCount count;
    Runtime::ServerHost host;
    auto log = std::make_shared<AllLinesLog>();
    ExpectTrue(host.SetLogger(log).IsOk(), "logger is set before Start");
    auto handled = std::make_shared<std::atomic<int>>(0);
    ExpectTrue(host.GetDispatcher()
                   .Register("fail",
                       [handled](const auto&, const auto&)
                       {
                           ++*handled;
                           return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidFormat);
                       })
                   .IsOk(),
        "failing handler registers");
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    options.maxMessageFailureLogsPerSecond = limit;
    if (!Start(host, options, std::make_shared<Observer>()))
        return count;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "log-burst peer connects");
    const auto failing = Protocol::EncodeFrame(Bytes(R"({"type":"fail","body":{}})"));
    const auto unknown = Protocol::EncodeFrame(Bytes(R"({"type":"unknown.type","body":{}})"));
    std::vector<std::byte> burst;
    for (int index = 0; index != messages; ++index)
    {
        const auto& frame = index % 2 == 0 ? failing.Value() : unknown.Value();
        burst.insert(burst.end(), frame.begin(), frame.end());
    }
    // 뒤따르는 알 수 없는 타입까지 처리됐음을 보이려고 마지막에 실패 frame 하나를 더 붙인다.
    burst.insert(burst.end(), failing.Value().begin(), failing.Value().end());
    ExpectTrue(peer.Send(burst), "log burst sent");
    ExpectTrue(Wait([&] { return handled->load() == messages / 2 + 1; }),
        "every failing message is handled");
    host.Stop();

    const std::lock_guard guard(log->mutex);
    for (const auto& line : log->lines)
    {
        const auto field = line.find(R"("suppressed":")");
        if (field == std::string::npos)
            ++count.written;
        else
            count.suppressed += std::stoull(line.substr(field + 14));
    }
    return count;
}

/// <summary>원격 입력마다 생기는 Warn 기록이 Host 전체에서 속도 제한되고, 버린 수가 요약되는지 본다.</summary>
void HostLimitsMessageFailureLogs()
{
    ServerCoreTest::SocketRuntime sockets;
    constexpr int Messages = 30;
    const auto limited =
        RunFailureLogBurst(Runtime::ServerHostOptions{}.maxMessageFailureLogsPerSecond, Messages);
    ExpectTrue(limited.suppressed != 0, "a burst of invalid messages suppresses some Warn records");
    ExpectEqual(static_cast<std::uint64_t>(Messages + 1), limited.written + limited.suppressed,
        "every per-message Warn record is either written or counted in a summary");

    const auto unlimited = RunFailureLogBurst(0, Messages);
    ExpectEqual(std::uint64_t{ 0 }, unlimited.suppressed, "a zero limit suppresses nothing");
    ExpectEqual(static_cast<std::size_t>(Messages + 1), unlimited.written,
        "a zero limit writes every record");

    Runtime::ServerHost configured;
    const ServerCoreTest::ScopedConfigFile file(
        "servercore.host.port = 1\nservercore.host.max-message-failure-logs-per-second = -1\n");
    const auto config = Core::Config::LoadFromFile(file.Path());
    ExpectTrue(config.IsOk() &&
                   configured.Configure(config.Value()).Code() == Core::ErrorCode::InvalidArgument,
        "the Config adapter reads the per-message log limit key");
}

/// <summary>정상 수신 한 번도 받을 수 없는 대기 수신 예산을 설정에서 거절하는지 본다.</summary>
void HostRejectsReceiveBudgetBelowOneReceive()
{
    const auto chunk = static_cast<std::uint32_t>(Net::MaximumReceiveChunkBytes);
    const auto configure = [](const std::uint32_t perSession, const std::uint32_t total)
    {
        Runtime::ServerHostOptions options;
        options.port = 1;
        options.maxPendingReceiveBytes = perSession;
        options.maxTotalPendingReceiveBytes = total;
        Runtime::ServerHost host;
        return host.Configure(options).Code();
    };
    ExpectEqual(static_cast<int>(Core::ErrorCode::InvalidArgument),
        static_cast<int>(configure(chunk - 1, chunk)),
        "a per-session receive budget below one receive is rejected");
    ExpectEqual(static_cast<int>(Core::ErrorCode::InvalidArgument),
        static_cast<int>(configure(chunk, chunk - 1)),
        "a total receive budget below one receive is rejected");
    ExpectEqual(static_cast<int>(Core::ErrorCode::Ok), static_cast<int>(configure(chunk, chunk)),
        "receive budgets of exactly one receive are accepted");
}

/// <summary>값이 매우 많은 JSON 본문 하나가 받을 수 있는 크기여도 값 수 상한에서 끊기는지 본다.</summary>
/// <returns>처리기가 불린 횟수와 종료 사유다.</returns>
std::pair<int, Core::ErrorCode> SendDenseJson(
    Runtime::ServerHostOptions options, const std::size_t zeros)
{
    Runtime::ServerHost host;
    auto handled = std::make_shared<std::atomic<int>>(0);
    ExpectTrue(host.GetDispatcher()
                   .Register("json.values",
                       [handled](const auto&, const auto&)
                       {
                           ++*handled;
                           return Core::Status::Ok();
                       })
                   .IsOk(),
        "dense JSON handler registers");
    options.port = FreePort();
    auto observer = std::make_shared<Observer>();
    if (!Start(host, options, observer))
        return { -1, Core::ErrorCode::Ok };
    std::string json = R"({"type":"json.values","body":{"a":[0)";
    for (std::size_t index = 1; index < zeros; ++index)
        json += ",0";
    json += "]}}";
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "dense JSON peer connects");
    const auto frame = Protocol::EncodeFrame(Bytes(json));
    ExpectTrue(frame.IsOk() && peer.Send(frame.Value()),
        "dense JSON frame fits the body limit and is sent");
    ExpectTrue(Wait([&] { return handled->load() != 0 || observer->closed != 0; }),
        "dense JSON is handled or rejected");
    const std::pair result{ handled->load(), observer->reason.load() };
    host.Stop();
    return result;
}

void HostLimitsJsonValuesPerMessage()
{
    ServerCoreTest::SocketRuntime sockets;
    // 기본 64 KiB body 안에 들어가는 20,000개짜리 배열이다.
    const auto rejected = SendDenseJson(Runtime::ServerHostOptions{}, 20000);
    ExpectEqual(
        0, rejected.first, "a dense JSON body over the value limit never reaches its handler");
    ExpectEqual(static_cast<int>(Core::ErrorCode::TooLarge), static_cast<int>(rejected.second),
        "a dense JSON body over the value limit closes its session as TooLarge");

    Runtime::ServerHostOptions workers;
    workers.parseWorkerThreadCount = 1;
    const auto workerRejected = SendDenseJson(workers, 20000);
    ExpectEqual(0, workerRejected.first, "parse workers apply the same value limit");
    ExpectEqual(static_cast<int>(Core::ErrorCode::TooLarge),
        static_cast<int>(workerRejected.second),
        "a parse-worker value limit rejection closes the session as TooLarge");

    Runtime::ServerHostOptions raised;
    raised.maxJsonValuesPerMessage = 30000;
    const auto accepted = SendDenseJson(raised, 20000);
    ExpectEqual(
        1, accepted.first, "an explicit value limit above the body's values lets it through");

    Runtime::ServerHost configured;
    const ServerCoreTest::ScopedConfigFile file(
        "servercore.host.port = 1\nservercore.host.max-json-values-per-message = -1\n");
    const auto config = Core::Config::LoadFromFile(file.Path());
    ExpectTrue(config.IsOk() &&
                   configured.Configure(config.Value()).Code() == Core::ErrorCode::InvalidArgument,
        "the Config adapter reads the JSON value limit key");
}

/// <summary>Start 뒤의 부팅 설정 호출이 프로세스를 끝내지 않고 같은 방식으로 거절되는지 본다.</summary>
void HostRejectsLateSetupUniformly()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options;
    options.port = FreePort();
    const auto observerAtStart = std::make_shared<Observer>();
    if (!Start(host, options, observerAtStart))
        return;
    const auto lateObserver = std::make_shared<Observer>();
    ExpectEqual(static_cast<int>(Core::ErrorCode::Closed),
        static_cast<int>(host.SetLogger(std::make_shared<Log>()).Code()),
        "late SetLogger is Closed");
    ExpectEqual(static_cast<int>(Core::ErrorCode::Closed),
        static_cast<int>(host.SetSessionObserver(lateObserver).Code()),
        "late SetSessionObserver is Closed");
    ExpectEqual(static_cast<int>(Core::ErrorCode::Closed),
        static_cast<int>(
            host.SetBinaryHandler(
                    [](const auto&, Protocol::BinaryMessageView) { return Core::Status::Ok(); })
                .Code()),
        "late SetBinaryHandler is Closed");
    // 거절된 관찰자는 걸리지 않았으므로 뒤에 열린 세션을 보지 못한다.
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "late-setup peer connects");
    ExpectTrue(Wait([&] { return observerAtStart->opened == 1; }),
        "the observer set before Start sees the session");
    ExpectEqual(0, lateObserver->opened.load(), "a rejected late observer sees nothing");
    host.Stop();
}

const ServerCoreTest::CheckRegistration Isolated(
    "Runtime.HostConcurrentIsolation", HostConcurrentIsolation);
const ServerCoreTest::CheckRegistration ControlLane(
    "Runtime.HostControlLaneFitsSessionWork", HostControlLaneFitsSessionWork);
const ServerCoreTest::CheckRegistration DrainContinuations(
    "Runtime.HostDrainAcceptsGameContinuations", HostDrainAcceptsGameContinuations);
const ServerCoreTest::CheckRegistration BinaryClose(
    "Runtime.HostBinaryGracefulClose", HostBinaryGracefulClose);
const ServerCoreTest::CheckRegistration FramesPerJob(
    "Runtime.HostBoundsFramesPerReceiveJob", HostBoundsFramesPerReceiveJob);
const ServerCoreTest::CheckRegistration InputRate(
    "Runtime.HostRejectsUnsendableInputRate", HostRejectsUnsendableInputRate);
const ServerCoreTest::CheckRegistration ObservationBounds(
    "Runtime.HostObservationAndLogBoundaries", HostObservationAndLogBoundaries);
const ServerCoreTest::CheckRegistration LateSetup(
    "Runtime.HostRejectsLateSetupUniformly", HostRejectsLateSetupUniformly);
const ServerCoreTest::CheckRegistration FailureLogs(
    "Runtime.HostLimitsMessageFailureLogs", HostLimitsMessageFailureLogs);
const ServerCoreTest::CheckRegistration ReceiveBudgetFloor(
    "Runtime.HostRejectsReceiveBudgetBelowOneReceive", HostRejectsReceiveBudgetBelowOneReceive);
const ServerCoreTest::CheckRegistration JsonValues(
    "Runtime.HostLimitsJsonValuesPerMessage", HostLimitsJsonValuesPerMessage);
const ServerCoreTest::CheckRegistration CapacityLocks(
    "Runtime.HostCapacityCallbackOutsideSessionLocks", HostCapacityCallbackOutsideSessionLocks);
}
