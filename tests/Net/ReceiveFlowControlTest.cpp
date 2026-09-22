#include "TestHarness.h"
#include "SocketTestSupport.h"

#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/ConnectionFlowControl.h"
#include "ServerCore/Net/IoContext.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace
{
using namespace ServerCore;
constexpr auto WaitLimit = std::chrono::seconds(10);
constexpr auto QuietInterval = std::chrono::milliseconds(100);
constexpr auto PortBase = static_cast<std::uint16_t>(SERVERCORE_TEST_PORT_BASE);

std::vector<std::byte> Payload(const std::size_t size)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<std::byte>((index * 37 + index / 251) % 256);
    return bytes;
}

class Peer final
{
public:
    ~Peer()
    {
        if (mSocket != ServerCoreTest::InvalidSocket)
        {
            (void)::shutdown(mSocket, ServerCoreTest::ShutdownBoth);
            ServerCoreTest::CloseSocket(mSocket);
        }
    }

    bool Connect(const std::uint16_t port)
    {
        mSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (mSocket == ServerCoreTest::InvalidSocket) return false;
        if (!ServerCoreTest::SetSocketTimeouts(mSocket, 10000)) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) return false;
        return ::connect(mSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }

    bool SendAll(const std::span<const std::byte> bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const int sent = ServerCoreTest::Send(mSocket,
                reinterpret_cast<const char*>(bytes.data() + offset),
                static_cast<int>(bytes.size() - offset));
            if (sent <= 0) return false;
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool ReceiveExactly(const std::span<std::byte> bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const int received = ServerCoreTest::Receive(mSocket,
                reinterpret_cast<char*>(bytes.data() + offset),
                static_cast<int>(bytes.size() - offset));
            if (received <= 0) return false;
            offset += static_cast<std::size_t>(received);
        }
        return true;
    }

    bool ShutdownSend()
    {
#ifdef _WIN32
        return ::shutdown(mSocket, SD_SEND) == 0;
#else
        return ::shutdown(mSocket, SHUT_WR) == 0;
#endif
    }

private:
    ServerCoreTest::Socket mSocket = ServerCoreTest::InvalidSocket;
};

class FlowObserver final : public Net::IConnectionObserver
{
public:
    explicit FlowObserver(const bool cycleInCallback = false) : mCycleInCallback(cycleInCallback) {}

    void SetFlow(const std::shared_ptr<Net::ConnectionFlowControl>& flow) { mFlow = flow; }

    void OnBytesReceived(const std::span<const std::byte> bytes) override
    {
        if (mActive.fetch_add(1) != 0) mOverlap.store(true);
        const auto call = mCalls.fetch_add(1) + 1;
        const std::vector<std::byte> original(bytes.begin(), bytes.end());
        if (bytes.size() > 16 * 1024) mFailure.store(true);
        if (mCycleInCallback)
        {
            const auto flow = mFlow.lock();
            if (!flow || !flow->PauseReceive().IsOk()) mFailure.store(true);
            if (call == 1)
            {
                std::unique_lock lock(mMutex);
                mFirstPaused = true;
                mChanged.notify_all();
                if (!mChanged.wait_for(lock, WaitLimit, [this] { return mResumeFirst; }))
                    mFailure.store(true);
            }
            if (!flow || !flow->ResumeReceive().IsOk()) mFailure.store(true);
            if (call == 1)
            {
                std::unique_lock lock(mMutex);
                mFirstResumed = true;
                mChanged.notify_all();
                // Other workers have readable input while this callback still
                // owns the borrowed span. Resuming must not recycle its buffer.
                if (!mChanged.wait_for(lock, WaitLimit, [this] { return mFinishFirst; }))
                    mFailure.store(true);
            }
        }
        if (!std::equal(bytes.begin(), bytes.end(), original.begin())) mFailure.store(true);
        {
            const std::lock_guard lock(mMutex);
            mBytes.insert(mBytes.end(), original.begin(), original.end());
        }
        mActive.fetch_sub(1);
        mChanged.notify_all();
    }

    void OnDisconnected(Core::Status) override
    {
        if (mActive.load() != 0) mOverlap.store(true);
        {
            const std::lock_guard lock(mMutex);
            ++mDisconnects;
        }
        mChanged.notify_all();
    }

    bool WaitForBytes(const std::size_t size)
    {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, WaitLimit, [this, size] { return mBytes.size() >= size; });
    }

    bool CallsStayAt(const std::size_t count)
    {
        std::unique_lock lock(mMutex);
        return !mChanged.wait_for(lock, QuietInterval, [this, count] { return mCalls.load() != count; });
    }

    bool WaitForFirstPause()
    {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, WaitLimit, [this] { return mFirstPaused; });
    }

    bool ResumeFirstAndWait()
    {
        std::unique_lock lock(mMutex);
        mResumeFirst = true;
        mChanged.notify_all();
        return mChanged.wait_for(lock, WaitLimit, [this] { return mFirstResumed; });
    }

    void ReleaseCallbacks()
    {
        {
            const std::lock_guard lock(mMutex);
            mResumeFirst = true;
            mFinishFirst = true;
        }
        mChanged.notify_all();
    }

    bool WaitForDisconnect()
    {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, WaitLimit, [this] { return mDisconnects != 0; });
    }

    bool DisconnectsStayAtZero()
    {
        std::unique_lock lock(mMutex);
        return !mChanged.wait_for(lock, QuietInterval, [this] { return mDisconnects != 0; });
    }

    std::size_t Disconnects() const
    {
        const std::lock_guard lock(mMutex);
        return mDisconnects;
    }

    std::vector<std::byte> Bytes() const
    {
        const std::lock_guard lock(mMutex);
        return mBytes;
    }

    std::size_t Calls() const { return mCalls.load(); }
    bool Healthy() const { return !mOverlap.load() && !mFailure.load(); }

private:
    const bool mCycleInCallback;
    std::weak_ptr<Net::ConnectionFlowControl> mFlow;
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::vector<std::byte> mBytes;
    std::size_t mDisconnects = 0;
    bool mFirstPaused = false;
    bool mFirstResumed = false;
    bool mResumeFirst = false;
    bool mFinishFirst = false;
    std::atomic<std::size_t> mCalls{0};
    std::atomic<unsigned> mActive{0};
    std::atomic<bool> mOverlap{false};
    std::atomic<bool> mFailure{false};
};

class FlowServer final
{
public:
    explicit FlowServer(const bool cycleInCallback = false)
        : observer(std::make_shared<FlowObserver>(cycleInCallback)) {}
    ~FlowServer() { Shutdown(); }

    Core::Status ConfigureLimits(const Net::SendQueueLimits& limits)
    {
        return mAcceptor.SetSendQueueLimits(limits);
    }

    bool Start(const std::uint16_t port, const bool pauseBeforeStart,
        const bool resumeBeforeStart = false, const bool holdHandler = false)
    {
        if (!mIo.Start(4).IsOk()) return false;
        mAcceptor.SetConnectionHandler([this, pauseBeforeStart, resumeBeforeStart, holdHandler]
            (std::shared_ptr<Net::Connection> accepted)
        {
            const auto control = Net::GetConnectionFlowControl(accepted);
            observer->SetFlow(control);
            accepted->SetObserver(observer);
            bool configured = control != nullptr;
            if (configured && pauseBeforeStart) configured = control->PauseReceive().IsOk();
            if (configured && resumeBeforeStart) configured = control->ResumeReceive().IsOk();
            std::unique_lock lock(mMutex);
            connection = std::move(accepted);
            flow = control;
            mConfigured = configured;
            mChanged.notify_all();
            if (holdHandler && !mChanged.wait_for(lock, WaitLimit, [this] { return mReleaseHandler; }))
                mHandlerTimedOut = true;
        });
        return mAcceptor.Listen("127.0.0.1", port, 8).IsOk() && mAcceptor.Start(mIo).IsOk();
    }

    bool WaitForConnection()
    {
        std::unique_lock lock(mMutex);
        return mChanged.wait_for(lock, WaitLimit, [this] { return connection != nullptr; }) && mConfigured;
    }

    void FinishAccept()
    {
        {
            const std::lock_guard lock(mMutex);
            mReleaseHandler = true;
        }
        mChanged.notify_all();
        // Stop drains the complete handoff, including Connection::Start after
        // the handler returns, without stopping this accepted connection.
        mAcceptor.Stop();
    }

    void Shutdown()
    {
        if (mStopped) return;
        observer->ReleaseCallbacks();
        FinishAccept();
        if (connection)
        {
            connection->Close();
            ServerCoreTest::ExpectTrue(observer->WaitForDisconnect(), "flow-control connection drains before IoContext stops");
        }
        mIo.Stop();
        ServerCoreTest::ExpectTrue(!mHandlerTimedOut, "accept handoff gate was released within its deadline");
        mStopped = true;
    }

    std::shared_ptr<FlowObserver> observer;
    std::shared_ptr<Net::Connection> connection;
    std::shared_ptr<Net::ConnectionFlowControl> flow;

private:
    Net::IoContext mIo;
    Net::Acceptor mAcceptor;
    std::mutex mMutex;
    std::condition_variable mChanged;
    bool mConfigured = false;
    bool mReleaseHandler = false;
    bool mHandlerTimedOut = false;
    bool mStopped = false;
};

bool Connect(FlowServer& server, Peer& peer, const std::uint16_t offset,
    const bool paused, const bool resumeBeforeStart = false, const bool holdHandler = false)
{
    const auto port = static_cast<std::uint16_t>(PortBase + offset);
    const bool started = server.Start(port, paused, resumeBeforeStart, holdHandler);
    ServerCoreTest::ExpectTrue(started, "flow-control test listener starts");
    if (!started) return false;
    const bool connected = peer.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "raw TCP peer connects");
    if (!connected) return false;
    const bool accepted = server.WaitForConnection();
    ServerCoreTest::ExpectTrue(accepted, "accepted connection implements and configures flow control");
    return accepted;
}

void PauseReceiveBeforeStart()
{
    ServerCoreTest::SocketRuntime sockets;
    ServerCoreTest::ExpectTrue(sockets.IsReady(), "socket runtime starts");
    if (!sockets.IsReady()) return;
    FlowServer server;
    Peer peer;
    if (!Connect(server, peer, 41, true)) return;
    server.FinishAccept();
    ServerCoreTest::ExpectTrue(server.flow->IsReceivePaused(), "accept-handler pause survives Connection::Start");
    ServerCoreTest::ExpectTrue(server.flow->PauseReceive().IsOk(), "repeated pause succeeds");
    const auto bytes = Payload(4099);
    ServerCoreTest::ExpectTrue(peer.SendAll(bytes), "peer sends while no receive is admitted");
    ServerCoreTest::ExpectTrue(server.observer->CallsStayAt(0), "a connection started paused submits no initial receive");
    ServerCoreTest::ExpectTrue(server.flow->ResumeReceive().IsOk(), "resume admits buffered peer bytes");
    ServerCoreTest::ExpectTrue(server.flow->ResumeReceive().IsOk(), "repeated resume succeeds without duplicate I/O");
    ServerCoreTest::ExpectTrue(!server.flow->IsReceivePaused(), "resume clears paused state");
    ServerCoreTest::ExpectTrue(server.observer->WaitForBytes(bytes.size()), "resumed receive makes progress");
    server.Shutdown();
    ServerCoreTest::ExpectTrue(server.observer->Bytes() == bytes, "pre-start pause preserves every byte exactly once");
    ServerCoreTest::ExpectTrue(server.observer->Healthy(), "receive and disconnect callbacks remain serialized");
}

void ResumeReceiveBeforeStart()
{
    ServerCoreTest::SocketRuntime sockets;
    ServerCoreTest::ExpectTrue(sockets.IsReady(), "socket runtime starts");
    if (!sockets.IsReady()) return;
    FlowServer server;
    Peer peer;
    if (!Connect(server, peer, 42, true, true, true)) return;
    const auto bytes = Payload(8193);
    ServerCoreTest::ExpectTrue(!server.flow->IsReceivePaused(), "resume inside the accept handler clears paused state");
    ServerCoreTest::ExpectTrue(peer.SendAll(bytes), "peer queues input during the held accept handler");
    ServerCoreTest::ExpectTrue(server.observer->CallsStayAt(0), "pre-start resume cannot deliver input before accept handoff returns");
    server.FinishAccept();
    ServerCoreTest::ExpectTrue(server.observer->WaitForBytes(bytes.size()), "Connection::Start begins receiving after pre-start resume");
    server.Shutdown();
    ServerCoreTest::ExpectTrue(server.observer->Bytes() == bytes, "pre-start resume neither loses nor duplicates input");
    ServerCoreTest::ExpectTrue(server.observer->Healthy(), "pre-start resume preserves callback serialization");
}

void ReceiveFlowControlSerializesCallbacks()
{
    ServerCoreTest::SocketRuntime sockets;
    ServerCoreTest::ExpectTrue(sockets.IsReady(), "socket runtime starts");
    if (!sockets.IsReady()) return;
    FlowServer server(true);
    Peer peer;
    if (!Connect(server, peer, 43, false)) return;
    server.FinishAccept();
    const auto bytes = Payload(128 * 1024 + 23);
    ServerCoreTest::ExpectTrue(peer.SendAll(std::span(bytes).first(1024)), "peer starts the first receive callback");
    const bool paused = server.observer->WaitForFirstPause();
    ServerCoreTest::ExpectTrue(paused, "first receive callback pauses further input");
    if (!paused) return;
    ServerCoreTest::ExpectTrue(server.flow->IsReceivePaused(), "callback pause is visible to other threads");
    ServerCoreTest::ExpectTrue(peer.SendAll(std::span(bytes).subspan(1024)), "peer queues multiple receive buffers behind the active callback");
    ServerCoreTest::ExpectTrue(server.observer->ResumeFirstAndWait(), "first callback resumes while its borrowed span is live");
    ServerCoreTest::ExpectTrue(server.observer->CallsStayAt(1), "resume inside a callback cannot start a concurrent callback");
    server.observer->ReleaseCallbacks();
    ServerCoreTest::ExpectTrue(server.observer->WaitForBytes(bytes.size()), "repeated callback pause/resume consumes all buffered input");
    server.Shutdown();
    ServerCoreTest::ExpectTrue(server.observer->Calls() > 1, "payload exercised multiple receive callbacks");
    ServerCoreTest::ExpectTrue(server.observer->Bytes() == bytes, "callback pause/resume preserves payload order and contents");
    ServerCoreTest::ExpectTrue(server.observer->Healthy(), "no callback overlap, borrowed buffer corruption, or flow-control failure");
}

void SendWhileReceivePaused()
{
    ServerCoreTest::SocketRuntime sockets;
    ServerCoreTest::ExpectTrue(sockets.IsReady(), "socket runtime starts");
    if (!sockets.IsReady()) return;
    FlowServer server;
    Peer peer;
    if (!Connect(server, peer, 44, true)) return;
    server.FinishAccept();
    const auto inbound = Payload(513);
    const auto outbound = Payload(64 * 1024 + 7);
    ServerCoreTest::ExpectTrue(peer.SendAll(inbound), "paused connection has unread inbound bytes");
    const bool sent = server.connection->Send(outbound).IsOk();
    ServerCoreTest::ExpectTrue(sent, "receive pause does not close send admission");
    std::vector<std::byte> received(outbound.size());
    if (sent) ServerCoreTest::ExpectTrue(peer.ReceiveExactly(received), "outgoing writes progress while receive remains paused");
    ServerCoreTest::ExpectTrue(received == outbound, "paused connection sends the exact payload");
    ServerCoreTest::ExpectTrue(server.observer->CallsStayAt(0), "write readiness never bypasses receive pause");
    ServerCoreTest::ExpectTrue(server.flow->ResumeReceive().IsOk(), "input can resume after writes drain");
    ServerCoreTest::ExpectTrue(server.observer->WaitForBytes(inbound.size()), "buffered input survives paused outgoing writes");
    server.Shutdown();
    ServerCoreTest::ExpectTrue(server.observer->Bytes() == inbound, "full-duplex flow control preserves inbound bytes");
    ServerCoreTest::ExpectTrue(server.observer->Healthy(), "paused write callbacks preserve receive serialization");
}

void PausedPeerEofDisconnectsOnce()
{
    ServerCoreTest::SocketRuntime sockets;
    ServerCoreTest::ExpectTrue(sockets.IsReady(), "socket runtime starts");
    if (!sockets.IsReady()) return;
    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
        FlowServer server;
        Peer peer;
        if (!Connect(server, peer, static_cast<std::uint16_t>(45 + attempt), true)) return;
        server.FinishAccept();
        ServerCoreTest::ExpectTrue(peer.ShutdownSend(), "peer queues graceful EOF while receiving is paused");
        ServerCoreTest::ExpectTrue(server.observer->DisconnectsStayAtZero(), "paused connection has not submitted an EOF-consuming receive");
        if (attempt == 0)
            ServerCoreTest::ExpectTrue(server.flow->ResumeReceive().IsOk(), "resuming a paused connection admits pending EOF");
        else
            server.connection->Close();
        ServerCoreTest::ExpectTrue(server.observer->WaitForDisconnect(), "resume or explicit close completes paused EOF teardown");
        ServerCoreTest::ExpectTrue(server.flow->ResumeReceive().Code() == Core::ErrorCode::Closed,
            "resume after disconnect cannot revive the socket");
        ServerCoreTest::ExpectTrue(server.flow->PauseReceive().Code() == Core::ErrorCode::Closed,
            "pause after disconnect reports Closed");
        server.connection->Close();
        server.Shutdown();
        ServerCoreTest::ExpectEqual(std::size_t{1}, server.observer->Disconnects(), "paused EOF disconnects exactly once");
        ServerCoreTest::ExpectEqual(std::size_t{0}, server.observer->Calls(), "EOF is never delivered as an empty payload callback");
        ServerCoreTest::ExpectTrue(server.observer->Healthy(), "paused EOF teardown does not overlap receive callbacks");
    }
}

void SendQueueLimitsConfiguration()
{
    ServerCoreTest::SocketRuntime sockets;
    ServerCoreTest::ExpectTrue(sockets.IsReady(), "socket runtime starts");
    if (!sockets.IsReady()) return;
    FlowServer server;
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({0, 16}).Code() == Core::ErrorCode::InvalidArgument,
        "zero per-connection send capacity is invalid");
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({8, 0}).Code() == Core::ErrorCode::InvalidArgument,
        "zero shared send capacity is invalid");
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({1024 * 1024 + 1, 16}).Code() == Core::ErrorCode::InvalidArgument,
        "per-connection send capacity above 1 MiB is invalid");
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({8, 512 * 1024 * 1024 + 1}).Code() == Core::ErrorCode::InvalidArgument,
        "shared send capacity above 512 MiB is invalid");
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({1024 * 1024, 512 * 1024 * 1024}).IsOk(),
        "documented maximum send capacities are accepted");
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({8, 16}).IsOk(), "small send limits can be configured before Start");
    Peer peer;
    if (!Connect(server, peer, 47, true)) return;
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({16, 32}).Code() == Core::ErrorCode::Closed,
        "an active acceptor cannot replace its send budget");
    server.FinishAccept();
    const auto tooLarge = Payload(9);
    ServerCoreTest::ExpectTrue(server.connection->Send(tooLarge).Code() == Core::ErrorCode::WouldBlock,
        "accepted connection enforces the configured per-connection send limit");
    ServerCoreTest::ExpectEqual(std::size_t{0}, server.flow->RetainedSendBytes(),
        "rejected oversized send retains no payload storage");
    std::atomic<unsigned> callbacks{0};
    auto wait = server.flow->WaitForSendCapacity(9, [&callbacks](Core::Status) { callbacks.fetch_add(1); });
    ServerCoreTest::ExpectTrue(!wait.IsOk() && wait.GetStatus().Code() == Core::ErrorCode::TooLarge,
        "capacity wait above the configured connection limit cannot succeed");
    if (wait.IsOk()) wait.Value().Reset();
    const auto bytes = Payload(8);
    const bool sent = server.connection->Send(bytes).IsOk();
    ServerCoreTest::ExpectTrue(sent, "payload exactly at configured capacity is admitted");
    std::vector<std::byte> received(bytes.size());
    if (sent) ServerCoreTest::ExpectTrue(peer.ReceiveExactly(received), "configured capacity still permits real TCP sends");
    ServerCoreTest::ExpectTrue(received == bytes, "accepted boundary payload arrives intact");
    server.Shutdown();
    ServerCoreTest::ExpectEqual(0U, callbacks.load(), "rejected capacity wait never invokes its callback");
    ServerCoreTest::ExpectEqual(std::size_t{0}, server.flow->RetainedSendBytes(),
        "disconnect releases all retained bytes under custom limits");
    ServerCoreTest::ExpectTrue(server.ConfigureLimits({16, 32}).IsOk(), "a stopped acceptor permits a new budget configuration");
}

const ServerCoreTest::CheckRegistration pauseBeforeStart{
    "Transport.PauseReceiveBeforeStart", &PauseReceiveBeforeStart};
const ServerCoreTest::CheckRegistration resumeBeforeStart{
    "Transport.ResumeReceiveBeforeStart", &ResumeReceiveBeforeStart};
const ServerCoreTest::CheckRegistration serializedCallbacks{
    "Transport.ReceiveFlowControlSerializesCallbacks", &ReceiveFlowControlSerializesCallbacks};
const ServerCoreTest::CheckRegistration sendWhilePaused{
    "Transport.SendWhileReceivePaused", &SendWhileReceivePaused};
const ServerCoreTest::CheckRegistration pausedEof{
    "Transport.PausedPeerEofDisconnectsOnce", &PausedPeerEofDisconnectsOnce};
const ServerCoreTest::CheckRegistration sendQueueLimits{
    "Transport.SendQueueLimitsConfiguration", &SendQueueLimitsConfiguration};
}
