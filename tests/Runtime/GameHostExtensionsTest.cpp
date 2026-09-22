#include "ServerCore/Runtime/ServerHost.h"
#include "ServerCore/Protocol/BinaryMessage.h"
#include "Net/DatagramSocket.h"
#include "Runtime/ServerHostTestAccess.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
using namespace ServerCore;
using ServerCoreTest::ExpectTrue;
using namespace std::chrono_literals;

template<class Predicate> bool Wait(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    for (;;)
    {
        if (predicate()) return true;
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(2ms);
    }
}

std::span<const std::byte> Bytes(std::string_view value)
{ return std::as_bytes(std::span(value.data(), value.size())); }

std::uint16_t FreePort()
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == ServerCoreTest::InvalidSocket) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ServerCoreTest::SocketLength size = sizeof(address);
    const bool success = ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    ServerCoreTest::CloseSocket(socket);
    return success ? ntohs(address.sin_port) : 0;
}

class Peer
{
public:
    ~Peer() { if (socket != ServerCoreTest::InvalidSocket) ServerCoreTest::CloseSocket(socket); }
    bool Connect(std::uint16_t port)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == ServerCoreTest::InvalidSocket || !ServerCoreTest::SetSocketTimeouts(socket, 5000)) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        return ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    bool Send(std::span<const std::byte> bytes)
    {
        while (!bytes.empty())
        {
            const int sent = ServerCoreTest::Send(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (sent <= 0) return false;
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
            const int got = ServerCoreTest::Receive(socket, reinterpret_cast<char*>(bytes.data() + offset), static_cast<int>(size - offset), 0);
            if (got <= 0) { bytes.resize(offset); return bytes; }
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
    std::atomic<int> opened{0}, closed{0};
    std::atomic<Core::ErrorCode> reason{Core::ErrorCode::Ok};
    std::function<void(const std::shared_ptr<Session::Session>&)> onOpen;
    void OnSessionOpened(const std::shared_ptr<Session::Session>& value) override
    {
        { const std::lock_guard guard(mutex); session = value; }
        if (onOpen) onOpen(value);
        ++opened;
    }
    void OnSessionClosed(Session::SessionId, Core::Status status) override
    { reason.store(status.Code()); ++closed; }
    std::shared_ptr<Session::Session> Get()
    { const std::lock_guard guard(mutex); return session; }
};

struct ReceiveCounter : Runtime::TestAccess::IBeforeSessionReceiveGate
{
    std::atomic<unsigned> calls{0};
    void WaitBeforeSessionReceive() noexcept override { ++calls; }
};

struct BlockingState
{
    std::atomic<bool> entered{false};
    void WaitForRelease()
    {
        entered = true;
        std::unique_lock guard(mutex);
        changed.wait(guard, [this] { return released; });
    }
    void Release()
    {
        { const std::lock_guard guard(mutex); released = true; }
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
    explicit ReceiveGateScope(std::shared_ptr<ReceiveCounter> gate) : mGate(std::move(gate))
    { Runtime::TestAccess::InstallBeforeSessionReceiveGate(mGate); }
    ~ReceiveGateScope() { Runtime::TestAccess::ClearBeforeSessionReceiveGate(mGate); }
    ReceiveGateScope(const ReceiveGateScope&) = delete;
    ReceiveGateScope& operator=(const ReceiveGateScope&) = delete;
private:
    std::shared_ptr<ReceiveCounter> mGate;
};

bool Start(Runtime::ServerHost& host, Runtime::ServerHostOptions options, std::shared_ptr<Observer> observer)
{
    host.SetSessionObserver(observer);
    if (!host.Configure(options).IsOk()) { ExpectTrue(false, "host options accepted"); return false; }
    const auto status = host.Start();
    ExpectTrue(status.IsOk(), "host starts");
    return status.IsOk();
}

void HostBinaryAndUdpLifetime()
{
    ServerCoreTest::SocketRuntime sockets;
    auto udp = std::make_shared<Runtime::DatagramTransport>();
    ExpectTrue(udp->Configure({1, Protocol::PayloadMode::Binary}).IsOk() && udp->Bind("127.0.0.1", 0).IsOk(), "bounded UDP binds");
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options; options.port = FreePort(); options.payloadMode = Protocol::PayloadMode::Binary;
    auto observer = std::make_shared<Observer>();
    auto tokenVisible = std::make_shared<std::atomic<bool>>(false);
    observer->onOpen = [&host, tokenVisible](const auto& session) { *tokenVisible = host.GetDatagramToken(session->Id()).IsOk(); };
    ExpectTrue(host.AttachDatagramTransport(udp).IsOk(), "host attaches caller-owned UDP transport");
    ExpectTrue(host.SetBinaryHandler([](const auto& session, Protocol::BinaryMessageView message)
        { return session->SendBinary(message.type, message.payload); }).IsOk(), "binary handler registers");
    if (!Start(host, options, observer)) return;
    Peer peer;
    ExpectTrue(peer.Connect(host.Port()), "binary peer connects");
    if (!Wait([&] { return observer->opened.load() == 1; })) { ExpectTrue(false, "session opens"); return; }
    ExpectTrue(tokenVisible->load() && udp->RegisteredSessionCount() == 1, "UDP token exists before opened callback");
    const std::array payload{std::byte{0}, std::byte{0xff}, std::byte{0x80}};
    auto envelope = Protocol::EncodeBinaryMessage(7, payload, 64);
    auto frame = Protocol::EncodeFrame(envelope.Value());
    ExpectTrue(peer.Send(frame.Value()) && peer.Receive(frame.Value().size()) == frame.Value(), "binary TCP uses existing framing without JSON interpretation");
    auto retained = observer->Get();
    ExpectTrue(retained->GetCancellationToken().stop_possible(), "native session exposes a stoppable lifetime token");
    host.Stop();
    ExpectTrue(retained->GetCancellationToken().stop_requested() && observer->closed == 1, "stop cancels retained session exactly once");
    ExpectTrue(udp->RegisteredSessionCount() == 0 && udp->Port() != 0, "TCP lifetime unregisters UDP without closing caller socket");
}

void DatagramRegistryAndBinary()
{
    Runtime::DatagramTransport transport;
    ExpectTrue(!transport.Configure({0}).IsOk(), "zero UDP registry cap rejected");
    ExpectTrue(transport.Configure({1, Protocol::PayloadMode::Binary}).IsOk(), "binary UDP config accepted");
    ExpectTrue(transport.Bind("127.0.0.1", 0).IsOk(), "UDP binds");
    const auto one = static_cast<Session::SessionId>(1), two = static_cast<Session::SessionId>(2);
    const auto token = transport.RegisterSession(one);
    if (!token.IsOk()) { ExpectTrue(false, "first token registered"); return; }
    ExpectTrue(transport.RegisterSession(two).GetStatus().Code() == Core::ErrorCode::WouldBlock, "registration cap is enforced before allocation");
    Net::DatagramSocket peer;
    ExpectTrue(peer.Bind("127.0.0.1", 0).IsOk(), "raw UDP peer binds");
    sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port = htons(transport.Port()); endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto binary = Protocol::EncodeBinaryMessage(9, Bytes(std::string_view("a\0b", 3)), 64);
    std::array<std::byte, Protocol::DatagramCodec::MaximumDatagramBytes> packet{};
    const auto size = Protocol::DatagramCodec::Encode(packet, token.Value(), 1, binary.Value());
    bool received = false;
    ExpectTrue(peer.Send(endpoint, std::span(packet).first(size)).IsOk(), "binary datagram sent");
    const auto admit = [](Session::SessionId, Protocol::BinaryMessageView) { return true; };
    ExpectTrue(Wait([&]
        {
            transport.PollBinary(admit, [&](Session::SessionId id, Protocol::BinaryMessageView message)
                { received = id == one && message.type == 9 && message.payload.size() == 3;
                  ExpectTrue(transport.SendBinary(id, message.type, message.payload).IsOk(), "binary UDP reply uses learned endpoint"); });
            return received;
        }), "binary datagram decoded without JSON parsing");
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
        Runtime::ServerHostOptions options; options.port = FreePort(); options.idleSessionTimeout = 5s;
        if (scenario == 0) options.frameCompletionTimeout = 60ms;
        if (scenario == 1) options.authenticationTimeout = 60ms;
        if (scenario == 2) options.maxInputBytesPerSecond = 8;
        if (scenario == 3) options.maxInputFramesPerSecond = 1;
        if (scenario == 4) options.frameCompletionTimeout = 60ms;
        if (scenario == 5) options.maxPendingReceiveChunks = 1;
        auto observer = std::make_shared<Observer>();
        if (!Start(host, options, observer)) return;
        Peer peer; ExpectTrue(peer.Connect(host.Port()), "limited peer connects");
        if (!Wait([&] { return observer->opened == 1; })) { ExpectTrue(false, "limited session opens"); return; }
        if (scenario == 0)
        {
            const std::array partial{std::byte{32}, std::byte{0}};
            ExpectTrue(peer.Send(partial), "partial frame starts absolute deadline");
            std::this_thread::sleep_for(30ms);
            ExpectTrue(peer.Send(std::span(partial).last(1)), "additional bytes do not restart deadline");
        }
        if (scenario == 2) ExpectTrue(peer.Send(Bytes("123456789")), "oversize rate window bytes reach server");
        if (scenario == 3)
        {
            auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"Ignored","body":{}})"));
            auto combined = frame.Value(); combined.insert(combined.end(), frame.Value().begin(), frame.Value().end());
            ExpectTrue(peer.Send(combined), "two valid frames reach one admission window");
        }
        if (scenario >= 4)
        {
            auto receives = std::make_shared<ReceiveCounter>();
            const ReceiveGateScope receiveGate(receives);
            auto blocker = std::make_shared<BlockingState>();
            const ReleaseBlockingOnExit releaseOnExit{blocker};
            // Releasing a callback is not a join. Its state must stay owned by
            // the queued/running job after this inner fixture scope disappears.
            ExpectTrue(host.GetJobRunner().Post([blocker]
                { blocker->WaitForRelease(); }).IsOk(), "runner blocked for queued-input regression");
            if (!Wait([&] { return blocker->entered.load(); })) { ExpectTrue(false, "blocking job enters"); return; }
            auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"Ignored","body":{}})"));
            ExpectTrue(peer.Send(std::span(frame.Value()).first(2)), "first fragment queued behind runner");
            ExpectTrue(Wait([&] { return receives->calls >= 1; }), "first fragment reaches I/O boundary");
            std::this_thread::sleep_for(100ms);
            ExpectTrue(peer.Send(std::span(frame.Value()).subspan(2)), "second fragment reaches same pending aggregate");
            ExpectTrue(Wait([&] { return receives->calls >= 2; }), "second fragment has separate I/O timestamp");
            std::this_thread::sleep_for(10ms);
            blocker->Release();
        }
        ExpectTrue(Wait([&] { return observer->closed == 1; }), "input policy closes violating session");
        ExpectTrue(observer->reason == (scenario < 2 || scenario == 4 ? Core::ErrorCode::Timeout : Core::ErrorCode::TooLarge), "deadline and rate errors stay distinct");
        host.Stop();
    }
}

void HostGracefulDrain()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost host;
    Runtime::ServerHostOptions options; options.port = FreePort(); options.payloadMode = Protocol::PayloadMode::Binary;
    auto observer = std::make_shared<Observer>();
    auto blocker = std::make_shared<BlockingState>();
    const ReleaseBlockingOnExit releaseOnExit{blocker};
    ExpectTrue(host.SetBinaryHandler([blocker](const auto& session, Protocol::BinaryMessageView message)
        { blocker->WaitForRelease(); return session->SendBinary(message.type, message.payload); }).IsOk(), "drain handler registers");
    if (!Start(host, options, observer)) return;
    Peer peer; ExpectTrue(peer.Connect(host.Port()), "drain peer connects");
    auto body = Protocol::EncodeBinaryMessage(4, Bytes("response"), 64);
    auto frame = Protocol::EncodeFrame(body.Value());
    ExpectTrue(peer.Send(frame.Value()), "request queued before drain");
    if (!Wait([&] { return blocker->entered.load(); })) { ExpectTrue(false, "handler starts"); return; }
    Core::Status result = Core::Status::Ok();
    std::jthread stop([&host, &result] { result = host.StopGracefully(std::chrono::steady_clock::now() + 3s); });
    // On an early exit/unwind, unblock the handler before jthread joins the
    // graceful-stop thread. result and host are both still alive at that join.
    const ReleaseBlockingOnExit releaseBeforeJoin{blocker};
    ExpectTrue(Wait([&] { return !host.IsRunning(); }), "drain closes listener admission promptly");
    ExpectTrue(host.GetJobRunner().Post([] {}).Code() == Core::ErrorCode::Closed, "drain closes application job admission");
    blocker->Release();
    ExpectTrue(peer.Receive(frame.Value().size()) == frame.Value(), "already admitted handler response drains before EOF");
    stop.join();
    ExpectTrue(result.IsOk() && observer->closed == 1 && host.DrainStatus().IsOk(), "graceful drain completes and stays idempotent");
    Runtime::ServerHost blocked;
    options.port = FreePort(); options.payloadMode = Protocol::PayloadMode::Json;
    auto other = std::make_shared<Observer>();
    if (!Start(blocked, options, other)) return;
    auto reservation = blocked.GetJobRunner().Reserve();
    ExpectTrue(reservation.IsOk(), "background continuation capacity reserved");
    const auto timeout = blocked.StopGracefully(std::chrono::steady_clock::now() + 30ms);
    ExpectTrue(timeout.Code() == Core::ErrorCode::Timeout && !blocked.IsRunning(), "deadline aborts a drain held by an unused reservation");
}

struct Log : Core::ILogger
{
    std::atomic<unsigned> count{0};
    void Write(Core::LogLevel, std::string_view) noexcept override { ++count; }
};

void HostConcurrentIsolation()
{
    ServerCoreTest::SocketRuntime sockets;
    Runtime::ServerHost first, second;
    auto firstLog = std::make_shared<Log>(), secondLog = std::make_shared<Log>();
    first.SetLogger(firstLog); second.SetLogger(secondLog);
    Runtime::ServerHostOptions options; options.port = FreePort();
    if (!Start(first, options, std::make_shared<Observer>())) return;
    options.port = FreePort();
    if (!Start(second, options, std::make_shared<Observer>())) return;
    Peer one, two;
    ExpectTrue(one.Connect(first.Port()) && two.Connect(second.Port()), "independent hosts accept simultaneously");
    auto frame = Protocol::EncodeFrame(Bytes(R"({"type":"Unregistered","body":{}})"));
    ExpectTrue(one.Send(frame.Value()), "first host receives unknown message");
    ExpectTrue(Wait([&] { return firstLog->count != 0; }) && secondLog->count == 0, "first host diagnostics reach only first logger");
    first.Stop();
    ExpectTrue(two.Send(frame.Value()) && Wait([&] { return secondLog->count != 0; }), "second host survives first host stop");
    second.Stop();
}

const ServerCoreTest::CheckRegistration BinaryUdp("Runtime.HostBinaryAndUdpLifetime", HostBinaryAndUdpLifetime);
const ServerCoreTest::CheckRegistration DatagramBinary("Runtime.DatagramRegistryAndBinary", DatagramRegistryAndBinary);
const ServerCoreTest::CheckRegistration Input("Runtime.HostInputAdmission", HostInputAdmission);
const ServerCoreTest::CheckRegistration Drain("Runtime.HostGracefulDrain", HostGracefulDrain);
const ServerCoreTest::CheckRegistration Isolated("Runtime.HostConcurrentIsolation", HostConcurrentIsolation);
}
