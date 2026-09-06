#include "TestHarness.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/IoContext.h"

#include "Net/AcceptorInternal.h"
#include "Net/ConnectionInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <WinSock2.h>

#include <WS2tcpip.h>

/// <summary>
/// 전송 층이 실제로 포트를 열고 바이트를 주고받는지 고정하는 검사들이다.
/// </summary>
/// <remarks>
/// 이 파일의 검사가 앞선 층들의 검사와 다른 점이 둘 있다.
///
/// 첫째, 스레드와 타이밍이 있다. 그래서 기다림에는 전부 시간 제한을 두고, 제한을 넘긴 것을
/// 통과로 세지 않는다. ctest 쪽에도 따로 제한을 건다. 매달리는 것이 이 층의 대표적 실패
/// 형태이고, 제한이 없으면 검증이 멈춘 채 남는다.
///
/// 둘째, "통과했다"가 두 가지를 뜻할 수 있다. 고친 것이 옳아서 통과한 것과, 그 경로가 아예
/// 안 돌아서 통과한 것이 같은 초록으로 보인다. 두 번 돌려 수가 같은 것으로는 그 둘이 갈리지
/// 않는다. 그래서 검사마다 "그 경로가 실제로 돌았다"를 스스로 단언한다. 관찰한 수신 호출
/// 수와 바이트 수가 0보다 크다는 것을 직접 확인하고, 큰 payload 검사는 수신 호출이 두 번
/// 이상 일어났다는 것까지 확인한다.
///
/// 포트는 검사 전용 블록에서만 쓴다. 검사마다 다른 포트를 쓰고 구성마다도 다르게 둔다. 값은
/// CMake가 SERVERCORE_TEST_PORT_BASE로 준다.
///
/// 계약 위반 경로는 여기 없다. 잘못된 endpoint는 Status로 오므로 검사할 수 있지만, 처리기
/// 없이 Start()하거나 SetObserver()를 두 번 부르는 것은 단언이 프로세스를 끊으므로 시험
/// 대상이 아니다.
/// </remarks>

#ifndef SERVERCORE_TEST_PORT_BASE
#error "SERVERCORE_TEST_PORT_BASE must be defined by the build"
#endif

namespace
{
constexpr std::uint16_t PortBase = static_cast<std::uint16_t>(SERVERCORE_TEST_PORT_BASE);

/// <summary>기다림의 기본 제한이다. 정상 경로는 이보다 훨씬 빨리 끝난다.</summary>
constexpr std::chrono::milliseconds WaitLimit{ 10000 };

/// <summary>검사 클라이언트가 수신에 거는 제한이다. 없으면 recv가 영원히 매달린다.</summary>
constexpr DWORD ClientReceiveTimeoutMilliseconds = 10000;

/// <summary>이 검사 파일이 도는 동안 Winsock이 살아 있게 한다.</summary>
class WinsockGuard
{
public:
    WinsockGuard()
    {
        WSADATA data{};
        mStartupResult = ::WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~WinsockGuard()
    {
        if (mStartupResult == 0)
        {
            ::WSACleanup();
        }
    }

    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
    WinsockGuard(WinsockGuard&&) = delete;
    WinsockGuard& operator=(WinsockGuard&&) = delete;

    [[nodiscard]] bool IsReady() const noexcept { return mStartupResult == 0; }

private:
    int mStartupResult = 0;
};

/// <summary>바이트 하나를 지정한 수만큼 늘어놓는다.</summary>
std::vector<std::byte> MakeFilledBytes(std::size_t count, unsigned char value)
{
    return std::vector<std::byte>(count, static_cast<std::byte>(value));
}

/// <summary>
/// 서버 쪽 연결에 붙어 무엇이 왔는지 세어 두는 관찰자다.
/// </summary>
/// <remarks>
/// 이 관찰자가 검사의 눈이다. 받은 바이트, 수신 호출 수, 끊김 통지 수를 전부 센다.
/// 특히 끊김 통지 수는 "정확히 한 번"을 검사가 확인하는 유일한 방법이다.
///
/// 스레드 안전성: 스레드 안전. 관찰자 호출은 I/O 스레드에서, 확인은 검사 스레드에서 온다.
/// </remarks>
class RecordingObserver final : public ServerCore::Net::IConnectionObserver
{
public:
    void OnBytesReceived(std::span<const std::byte> bytes) override
    {
        std::shared_ptr<ServerCore::Net::Connection> connection;
        bool shouldEcho = false;

        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mReceiveCallCount;
            mReceivedBytes.insert(mReceivedBytes.end(), bytes.begin(), bytes.end());
            shouldEcho = mEcho;
            connection = mConnection.lock();
        }

        mChanged.notify_all();

        if (shouldEcho && connection)
        {
            // 관찰자 안에서 Send를 부르는 것이 실제 쓰임새다. 연결이 잠금을 쥔 채 관찰자를
            // 부르면 여기서 그 잠금을 다시 잡게 되므로, 이 호출이 그 자리를 고정한다.
            const ServerCore::Core::Status sent = connection->Send(bytes);
            if (!sent.IsOk())
            {
                const std::lock_guard<std::mutex> guard(mMutex);
                ++mEchoFailureCount;
                mLastEchoFailure = sent.Message();
            }
        }
    }

    void OnDisconnected(ServerCore::Core::Status reason) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mDisconnectCallCount;
            mLastDisconnectCode = reason.Code();
        }

        mChanged.notify_all();
    }

    void SetEcho(bool echo)
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mEcho = echo;
    }

    void SetConnection(std::weak_ptr<ServerCore::Net::Connection> connection)
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mConnection = std::move(connection);
    }

    [[nodiscard]] std::size_t ReceiveCallCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mReceiveCallCount;
    }

    [[nodiscard]] std::size_t ReceivedByteCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mReceivedBytes.size();
    }

    [[nodiscard]] std::size_t DisconnectCallCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mDisconnectCallCount;
    }

    [[nodiscard]] ServerCore::Core::ErrorCode LastDisconnectCode() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mLastDisconnectCode;
    }

    [[nodiscard]] std::size_t EchoFailureCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mEchoFailureCount;
    }

    [[nodiscard]] std::string LastEchoFailure() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mLastEchoFailure;
    }

    /// <summary>끊김 통지가 올 때까지 기다린다.</summary>
    /// <returns>제한 안에 오면 참. 제한을 넘기면 거짓이며, 거짓은 통과가 아니다.</returns>
    [[nodiscard]] bool WaitForDisconnect(std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mDisconnectCallCount > 0; });
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;

    std::vector<std::byte> mReceivedBytes;
    std::size_t mReceiveCallCount = 0;
    std::size_t mDisconnectCallCount = 0;
    std::size_t mEchoFailureCount = 0;
    std::string mLastEchoFailure;
    ServerCore::Core::ErrorCode mLastDisconnectCode = ServerCore::Core::ErrorCode::Ok;

    bool mEcho = false;
    std::weak_ptr<ServerCore::Net::Connection> mConnection;
};

/// <summary>OnBytesReceived 안에 머물며 OnDisconnected와의 직렬 경계를 확인하는 관찰자다.</summary>
/// <remarks>
/// receive active 표시는 gate mutex와 분리한 원자로 둔다. 콜백이 gate를 붙든 채 기다리면,
/// 잘못 동시에 호출된 OnDisconnected가 같은 mutex를 기다려 검사가 그 경합을 못 볼 수 있기
/// 때문이다.
/// </remarks>
class BlockingReceiveObserver final : public ServerCore::Net::IConnectionObserver
{
public:
    void OnBytesReceived(std::span<const std::byte>) override
    {
        mReceiveActive.store(true, std::memory_order_release);
        {
            const std::lock_guard<std::mutex> guard(mGateMutex);
            mReceiveEntered = true;
        }
        mChanged.notify_all();

        std::unique_lock<std::mutex> guard(mGateMutex);
        mChanged.wait(guard, [this] { return mReleaseReceive; });
        guard.unlock();

        mReceiveActive.store(false, std::memory_order_release);
        mChanged.notify_all();
    }

    void OnDisconnected(ServerCore::Core::Status) override
    {
        if (mReceiveActive.load(std::memory_order_acquire))
        {
            mDisconnectedWhileReceiving.store(true, std::memory_order_release);
        }
        mDisconnectCount.fetch_add(1, std::memory_order_acq_rel);
        mChanged.notify_all();
    }

    [[nodiscard]] bool WaitForReceiveEntry(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mGateMutex);
        return mChanged.wait_for(guard, limit, [this] { return mReceiveEntered; });
    }

    [[nodiscard]] bool WaitForDisconnect(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mGateMutex);
        return mChanged.wait_for(
            guard, limit, [this] { return mDisconnectCount.load(std::memory_order_acquire) != 0; });
    }

    void ReleaseReceive()
    {
        {
            const std::lock_guard<std::mutex> guard(mGateMutex);
            mReleaseReceive = true;
        }
        mChanged.notify_all();
    }

    [[nodiscard]] bool DisconnectedWhileReceiving() const noexcept
    {
        return mDisconnectedWhileReceiving.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t DisconnectCount() const noexcept
    {
        return mDisconnectCount.load(std::memory_order_acquire);
    }

private:
    std::mutex mGateMutex;
    std::condition_variable mChanged;
    bool mReceiveEntered = false;
    bool mReleaseReceive = false;
    std::atomic<bool> mReceiveActive{ false };
    std::atomic<bool> mDisconnectedWhileReceiving{ false };
    std::atomic<std::size_t> mDisconnectCount{ 0 };
};

/// <summary>서로 다른 연결의 수신 콜백이 실제로 겹쳐 실행됐는지 보는 문이다.</summary>
/// <remarks>
/// 한 연결에서는 Connection이 수신 콜백을 직렬화하므로, 이 문에 두 callback이 함께 들어오면
/// 반드시 서로 다른 연결에서 온 것이다. 첫 callback을 여기서 멈춘 채 두 번째를 보내면,
/// I/O worker가 하나뿐인 구현은 두 번째 문턱을 넘을 수 없다.
/// </remarks>
class ConcurrentReceiveGate
{
public:
    void Enter()
    {
        std::unique_lock<std::mutex> guard(mMutex);
        ++mActiveCallbackCount;
        ++mEntryCount;
        if (mActiveCallbackCount >= 2)
        {
            mObservedOverlap = true;
        }
        mChanged.notify_all();

        mChanged.wait(guard, [this] { return mReleased; });

        --mActiveCallbackCount;
        mChanged.notify_all();
    }

    void RecordDisconnect()
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mDisconnectCount;
        }
        mChanged.notify_all();
    }

    [[nodiscard]] bool WaitForEntries(
        const std::size_t expectedCount, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(
            guard, limit, [this, expectedCount] { return mEntryCount >= expectedCount; });
    }

    [[nodiscard]] bool WaitForIdle(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mActiveCallbackCount == 0; });
    }

    [[nodiscard]] bool WaitForDisconnects(
        const std::size_t expectedCount, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(
            guard, limit, [this, expectedCount] { return mDisconnectCount >= expectedCount; });
    }

    void Release()
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mReleased = true;
        }
        mChanged.notify_all();
    }

    [[nodiscard]] bool ObservedOverlap() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mObservedOverlap;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mActiveCallbackCount = 0;
    std::size_t mEntryCount = 0;
    std::size_t mDisconnectCount = 0;
    bool mReleased = false;
    bool mObservedOverlap = false;
};

/// <summary>수신은 공유 문에서 멈추고, 끊김만 공유 문에 기록하는 연결별 관찰자다.</summary>
class ConcurrentReceiveObserver final : public ServerCore::Net::IConnectionObserver
{
public:
    explicit ConcurrentReceiveObserver(std::shared_ptr<ConcurrentReceiveGate> gate)
        : mGate(std::move(gate))
    {
    }

    void OnBytesReceived(std::span<const std::byte>) override { mGate->Enter(); }

    void OnDisconnected(ServerCore::Core::Status) override { mGate->RecordDisconnect(); }

private:
    std::shared_ptr<ConcurrentReceiveGate> mGate;
};

/// <summary>검사가 상대편이 되어 주는 평범한 블로킹 TCP 클라이언트다.</summary>
/// <remarks>
/// 일부러 우리 코드를 쓰지 않는다. 양쪽이 같은 구현이면 두 쪽이 같이 틀렸을 때 검사가
/// 통과한다. 여기서는 표준 소켓 호출만 쓴다.
/// </remarks>
class TestClient
{
public:
    ~TestClient() { Close(); }

    TestClient() = default;
    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;
    TestClient(TestClient&&) = delete;
    TestClient& operator=(TestClient&&) = delete;

    [[nodiscard]] bool Connect(std::uint16_t port)
    {
        mSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (mSocket == INVALID_SOCKET)
        {
            return false;
        }

        DWORD timeout = ClientReceiveTimeoutMilliseconds;
        ::setsockopt(mSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
            sizeof(timeout));
        ::setsockopt(mSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
            sizeof(timeout));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = ::htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

        if (::connect(mSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR)
        {
            Close();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool SendAll(std::span<const std::byte> bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const int chunk = ::send(mSocket, reinterpret_cast<const char*>(bytes.data() + offset),
                static_cast<int>(bytes.size() - offset), 0);
            if (chunk <= 0)
            {
                return false;
            }
            offset += static_cast<std::size_t>(chunk);
        }
        return true;
    }

    /// <summary>정확히 지정한 수만큼 받을 때까지 읽는다.</summary>
    /// <returns>다 받으면 참. 상대가 먼저 닫거나 제한을 넘기면 거짓.</returns>
    [[nodiscard]] bool ReceiveExactly(std::size_t count, std::vector<std::byte>& received)
    {
        received.clear();
        received.reserve(count);

        std::vector<char> chunk(16 * 1024);
        while (received.size() < count)
        {
            const int read = ::recv(mSocket, chunk.data(),
                static_cast<int>(std::min<std::size_t>(chunk.size(), count - received.size())), 0);
            if (read <= 0)
            {
                return false;
            }

            for (int index = 0; index < read; ++index)
            {
                received.push_back(static_cast<std::byte>(chunk[static_cast<std::size_t>(index)]));
            }
        }
        return true;
    }

    /// <summary>남은 바이트 없이 peer가 정상적으로 보내기를 닫을 때까지 읽는다.</summary>
    [[nodiscard]] bool WaitForPeerClose()
    {
        char byte = 0;
        return ::recv(mSocket, &byte, 1, 0) == 0;
    }

    void Close()
    {
        if (mSocket != INVALID_SOCKET)
        {
            ::shutdown(mSocket, SD_BOTH);
            ::closesocket(mSocket);
            mSocket = INVALID_SOCKET;
        }
    }

private:
    SOCKET mSocket = INVALID_SOCKET;
};

/// <summary>완료 포트·수락기·관찰자를 한 벌로 세워 두는 자리다.</summary>
/// <remarks>
/// 종료 순서가 여기 박혀 있다. 수락기를 먼저 멈추고, 연결을 닫고, 마지막에 완료 포트를
/// 멈춘다. 뒤집으면 수락기가 기다리는 완료를 처리할 스레드가 없어 Stop()의 단언에 걸린다.
/// 그 순서를 검사마다 손으로 적으면 한 곳에서 틀린다.
/// </remarks>
class TestServer
{
public:
    TestServer()
        : mObserver(std::make_shared<RecordingObserver>())
    {
    }

    ~TestServer() { Shutdown(); }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;
    TestServer(TestServer&&) = delete;
    TestServer& operator=(TestServer&&) = delete;

    [[nodiscard]] RecordingObserver& Observer() const noexcept { return *mObserver; }

    /// <summary>완료 포트를 띄우고 포트를 열고 수락을 시작한다.</summary>
    void Start(std::uint16_t port)
    {
        const ServerCore::Core::Status ioStarted = mIo.Start(2);
        ServerCoreTest::ExpectTrue(ioStarted.IsOk(), "IoContext::Start() succeeded");

        mAcceptor.SetConnectionHandler(
            [this](std::shared_ptr<ServerCore::Net::Connection> connection)
            {
                // 관찰자를 여기서 건다. 처리기가 돌아온 뒤에 수신이 시작되므로 첫 바이트를
                // 놓치지 않는다.
                mObserver->SetConnection(connection);
                connection->SetObserver(mObserver);

                {
                    const std::lock_guard<std::mutex> guard(mMutex);
                    mConnection = std::move(connection);
                }
                mAccepted.notify_all();
            });

        const ServerCore::Core::Status listening = mAcceptor.Listen("127.0.0.1", port, 8);
        ServerCoreTest::ExpectTrue(listening.IsOk(),
            listening.IsOk() ? "Acceptor::Listen() succeeded"
                             : ("Acceptor::Listen() succeeded, message: " + listening.Message()));

        const ServerCore::Core::Status accepting = mAcceptor.Start(mIo);
        ServerCoreTest::ExpectTrue(accepting.IsOk(),
            accepting.IsOk() ? "Acceptor::Start() succeeded"
                             : ("Acceptor::Start() succeeded, message: " + accepting.Message()));
    }

    /// <summary>수락된 연결이 생길 때까지 기다린다.</summary>
    [[nodiscard]] std::shared_ptr<ServerCore::Net::Connection> WaitForConnection(
        std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        if (!mAccepted.wait_for(guard, limit, [this] { return mConnection != nullptr; }))
        {
            return nullptr;
        }
        return mConnection;
    }

    /// <summary>정해진 순서대로 전부 내린다. 여러 번 불러도 된다.</summary>
    void Shutdown()
    {
        mAcceptor.Stop();

        std::shared_ptr<ServerCore::Net::Connection> connection;
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            connection = std::move(mConnection);
            mConnection.reset();
        }

        if (connection)
        {
            connection->Close();

            // 끊김 통지는 걸려 있던 요청이 전부 끝난 뒤에 온다. 그것을 기다리고 나서 완료
            // 포트를 멈춰야, 아직 커널에 걸린 요청이 남은 채로 멈추는 일이 없다.
            (void)mObserver->WaitForDisconnect(WaitLimit);
            connection.reset();
        }

        mIo.Stop();
    }

private:
    ServerCore::Net::IoContext mIo;
    ServerCore::Net::Acceptor mAcceptor;
    std::shared_ptr<RecordingObserver> mObserver;

    std::mutex mMutex;
    std::condition_variable mAccepted;
    std::shared_ptr<ServerCore::Net::Connection> mConnection;
};

/// <summary>수락 처리기를 명시적으로 멈춰 두어 Stop()의 drain 경계를 보는 문이다.</summary>
/// <remarks>
/// 처리기가 여기까지 왔다는 것은 AcceptEx 완료가 pendingAccepts에서 빠지고, 수락 소켓이
/// IoContext에 붙은 뒤라는 뜻이다. 따라서 이 문이 잠겨 있는 동안 Stop()이 돌아오면 완료
/// 경로가 아직 처리기 인계를 쓰고 있는데도 Acceptor가 정리를 끝냈다는 뜻이 된다.
/// </remarks>
class BlockingHandoff final
{
public:
    void Handle(std::shared_ptr<ServerCore::Net::Connection> connection)
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mEnteredCount;
        }
        mChanged.notify_all();

        {
            std::unique_lock<std::mutex> guard(mMutex);
            mChanged.wait(guard, [this] { return mReleased; });
        }

        // 이 검사는 수락기만 내리는 경로이므로, 만들어진 연결도 여기서 닫아 I/O 종료 전에
        // 진행 중인 요청을 남기지 않는다.
        connection->Close();
    }

    [[nodiscard]] bool WaitForEntries(
        const std::size_t expectedCount, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(
            guard, limit, [this, expectedCount] { return mEnteredCount >= expectedCount; });
    }

    void Release()
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mReleased = true;
        }
        mChanged.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mEnteredCount = 0;
    bool mReleased = false;
};

/// <summary>Stop()이 리슨 소켓을 닫는 지점까지 갔는지 제한 안에 확인한다.</summary>
[[nodiscard]] bool WaitForClosedPort(
    const ServerCore::Net::Acceptor& acceptor, const std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (acceptor.Port() != 0)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// <summary>송신 완료가 누적 큐 바이트를 기대한 값까지 줄일 때까지 기다린다.</summary>
/// <remarks>
/// QueuedSendBytes()는 완료 처리와 별도 스레드에서 읽는 순간값이다. sleep 자체로 "충분히
/// 기다렸다"고 가정하지 않고, 완료가 실제로 counter를 바꿨다는 관측값으로 다음 단계를 연다.
/// </remarks>
[[nodiscard]] bool WaitForQueuedSendBytes(const ServerCore::Net::Connection& connection,
    const std::size_t expectedBytes, const std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (connection.QueuedSendBytes() != expectedBytes)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

[[nodiscard]] bool WaitForUsedSendBudget(const ServerCore::Net::SendBudget& budget,
    const std::size_t expectedBytes, const std::chrono::milliseconds limit)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (budget.UsedBytes() != expectedBytes)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 검사
// ---------------------------------------------------------------------------

void SharedSendBudgetSerializesReservations()
{
    constexpr std::size_t ReservationCount = 8;
    constexpr std::size_t BudgetLimit = 4;
    ServerCore::Net::SendBudget budget(BudgetLimit);
    std::array<bool, ReservationCount> reserved{};
    std::latch callersReady{ ReservationCount };
    std::latch start{ 1 };
    std::vector<std::thread> callers;
    callers.reserve(ReservationCount);

    for (std::size_t index = 0; index < ReservationCount; ++index)
    {
        callers.emplace_back(
            [&budget, &reserved, &callersReady, &start, index]()
            {
                callersReady.count_down();
                start.wait();
                reserved[index] = budget.TryReserve(1);
            });
    }

    callersReady.wait();
    start.count_down();
    for (std::thread& caller : callers)
    {
        caller.join();
    }

    const std::size_t successCount =
        static_cast<std::size_t>(std::count(reserved.begin(), reserved.end(), true));
    ServerCoreTest::ExpectEqual(
        BudgetLimit, successCount, "concurrent reservations admit exactly the shared byte limit");
    ServerCoreTest::ExpectEqual(
        BudgetLimit, budget.UsedBytes(), "the shared budget never exceeds its limit");

    for (const bool wasReserved : reserved)
    {
        if (wasReserved)
        {
            budget.Release(1);
        }
    }
    ServerCoreTest::ExpectEqual(
        std::size_t{ 0 }, budget.UsedBytes(), "releasing every winner restores the shared budget");
}

/// <summary>완료 포트가 서고 멈추는지, 그리고 그것을 스스로 아는지 본다.</summary>
void IoContextStartsAndStops()
{
    ServerCore::Net::IoContext io;

    ServerCoreTest::ExpectTrue(!io.IsRunning(), "IsRunning() before Start()");

    const ServerCore::Core::Status started = io.Start(2);
    ServerCoreTest::ExpectTrue(started.IsOk(), "Start() succeeded");
    ServerCoreTest::ExpectTrue(io.IsRunning(), "IsRunning() after Start()");

    ServerCoreTest::ExpectTrue(
        !io.IsCurrentThreadIoThread(), "IsCurrentThreadIoThread() on the test thread");

    const ServerCore::Core::Status again = io.Start(2);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::AlreadyExists),
        static_cast<int>(again.Code()), "the error code of a second Start()");

    io.Stop();
    ServerCoreTest::ExpectTrue(!io.IsRunning(), "IsRunning() after Stop()");

    io.Stop();
    ServerCoreTest::ExpectTrue(!io.IsRunning(), "IsRunning() after a second Stop()");
}

/// <summary>잘못된 IPv4 endpoint를 거절하고 loopback endpoint는 여는지 본다.</summary>
void ListenRejectsInvalidEndpoint()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");

    ServerCore::Net::Acceptor acceptor;

    const ServerCore::Core::Status zeroPort = acceptor.Listen("127.0.0.1", 0, 8);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(zeroPort.Code()), "the error code for an unconfigured port");

    const ServerCore::Core::Status emptyAddress = acceptor.Listen("", PortBase, 8);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(emptyAddress.Code()), "the error code for an empty listen address");

    const ServerCore::Core::Status malformedAddress =
        acceptor.Listen("not-an-ipv4-address", PortBase, 8);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(malformedAddress.Code()), "the error code for a malformed listen address");

    // 거절이 전부를 거절하는 것이 아니라는 것을 같은 자리에서 고정한다. 이것이 없으면
    // Listen이 늘 실패하도록 망가져도 위의 두 단언은 통과한다.
    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 0);
    const ServerCore::Core::Status loopback = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(loopback.IsOk(),
        loopback.IsOk()
            ? "a valid loopback endpoint is accepted"
            : ("a valid loopback endpoint is accepted, message: " + loopback.Message()));

    ServerCoreTest::ExpectEqual(port, acceptor.Port(), "the port the acceptor reports");

    acceptor.Stop();
    ServerCoreTest::ExpectEqual(static_cast<std::uint16_t>(0), acceptor.Port(),
        "the port the acceptor reports after Stop()");
}

/// <summary>Stop() 뒤 같은 수락기가 새 endpoint를 실제로 다시 수락하는지 본다.</summary>
/// <remarks>
/// Port()가 0으로 돌아오는 것만으로는 다음 Listen()·Start()에 새 Winsock 범위, AcceptEx와
/// 처리기가 온전히 준비되는지 알 수 없다. 첫 연결을 완전히 닫은 뒤 같은 IoContext와 Acceptor로
/// 다른 포트를 열고, 두 번째 처리기가 실제 연결을 받는지까지 확인한다.
/// </remarks>
void AcceptorCanListenAgainAfterStop()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the relisten test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const ServerCore::Core::Status ioStarted = io.Start(1);
    ServerCoreTest::ExpectTrue(ioStarted.IsOk(), "IoContext started for the relisten test");
    if (!ioStarted.IsOk())
    {
        return;
    }

    std::mutex acceptedMutex;
    std::condition_variable acceptedChanged;
    auto configureHandler = [&acceptor, &acceptedMutex, &acceptedChanged](
                                const std::shared_ptr<RecordingObserver>& observer,
                                std::shared_ptr<ServerCore::Net::Connection>* acceptedConnection)
    {
        acceptor.SetConnectionHandler(
            [&acceptedMutex, &acceptedChanged, acceptedConnection, observer](
                std::shared_ptr<ServerCore::Net::Connection> connection)
            {
                connection->SetObserver(observer);
                {
                    const std::lock_guard<std::mutex> guard(acceptedMutex);
                    *acceptedConnection = std::move(connection);
                }
                acceptedChanged.notify_all();
            });
    };
    auto waitForConnection =
        [&acceptedMutex, &acceptedChanged](
            const std::shared_ptr<ServerCore::Net::Connection>* acceptedConnection)
        -> std::shared_ptr<ServerCore::Net::Connection>
    {
        std::unique_lock<std::mutex> guard(acceptedMutex);
        if (!acceptedChanged.wait_for(
                guard, WaitLimit, [acceptedConnection] { return *acceptedConnection != nullptr; }))
        {
            return nullptr;
        }
        return *acceptedConnection;
    };
    auto copyConnection =
        [&acceptedMutex](const std::shared_ptr<ServerCore::Net::Connection>* acceptedConnection)
        -> std::shared_ptr<ServerCore::Net::Connection>
    {
        const std::lock_guard<std::mutex> guard(acceptedMutex);
        return *acceptedConnection;
    };
    auto closeConnection = [](const std::shared_ptr<ServerCore::Net::Connection>& connection,
                               const std::shared_ptr<RecordingObserver>& observer)
    {
        if (!connection)
        {
            return true;
        }
        connection->Close();
        return observer->WaitForDisconnect(WaitLimit);
    };

    const std::uint16_t firstPort = static_cast<std::uint16_t>(PortBase + 33);
    std::shared_ptr<ServerCore::Net::Connection> firstAcceptedConnection;
    const std::shared_ptr<RecordingObserver> firstObserver = std::make_shared<RecordingObserver>();
    configureHandler(firstObserver, &firstAcceptedConnection);

    const ServerCore::Core::Status firstListening = acceptor.Listen("127.0.0.1", firstPort, 8);
    ServerCoreTest::ExpectTrue(firstListening.IsOk(),
        firstListening.IsOk()
            ? "Acceptor listened for its first lifecycle"
            : ("Acceptor listened for its first lifecycle, message: " + firstListening.Message()));
    if (!firstListening.IsOk())
    {
        io.Stop();
        return;
    }
    const ServerCore::Core::Status firstAccepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(firstAccepting.IsOk(),
        firstAccepting.IsOk()
            ? "Acceptor started for its first lifecycle"
            : ("Acceptor started for its first lifecycle, message: " + firstAccepting.Message()));
    if (!firstAccepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient firstClient;
    const bool firstConnected = firstClient.Connect(firstPort);
    ServerCoreTest::ExpectTrue(firstConnected, "the first lifecycle client connected");
    if (!firstConnected)
    {
        acceptor.Stop();
        (void)closeConnection(copyConnection(&firstAcceptedConnection), firstObserver);
        firstClient.Close();
        io.Stop();
        return;
    }
    const std::shared_ptr<ServerCore::Net::Connection> firstConnection =
        waitForConnection(&firstAcceptedConnection);
    ServerCoreTest::ExpectTrue(
        firstConnection != nullptr, "the first lifecycle handler accepted a connection");
    if (!firstConnection)
    {
        acceptor.Stop();
        (void)closeConnection(copyConnection(&firstAcceptedConnection), firstObserver);
        firstClient.Close();
        io.Stop();
        return;
    }

    acceptor.Stop();
    ServerCoreTest::ExpectEqual(static_cast<std::uint16_t>(0), acceptor.Port(),
        "Acceptor::Stop() clears the first lifecycle port before relistening");
    const bool firstDisconnected = closeConnection(firstConnection, firstObserver);
    ServerCoreTest::ExpectTrue(firstDisconnected,
        "the first lifecycle connection disconnected before the Acceptor was reused");
    firstClient.Close();
    if (!firstDisconnected)
    {
        io.Stop();
        return;
    }

    // Stop()은 handler도 비우므로, 두 번째 Start() 전에 새 cycle 전용 처리기를 다시 건다.
    const std::uint16_t secondPort = static_cast<std::uint16_t>(PortBase + 34);
    std::shared_ptr<ServerCore::Net::Connection> secondAcceptedConnection;
    const std::shared_ptr<RecordingObserver> secondObserver = std::make_shared<RecordingObserver>();
    configureHandler(secondObserver, &secondAcceptedConnection);

    const ServerCore::Core::Status secondListening = acceptor.Listen("127.0.0.1", secondPort, 8);
    ServerCoreTest::ExpectTrue(secondListening.IsOk(),
        secondListening.IsOk() ? "Acceptor listened for its second lifecycle"
                               : ("Acceptor listened for its second lifecycle, message: " +
                                     secondListening.Message()));
    if (!secondListening.IsOk())
    {
        io.Stop();
        return;
    }
    const ServerCore::Core::Status secondAccepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(secondAccepting.IsOk(),
        secondAccepting.IsOk()
            ? "Acceptor started for its second lifecycle"
            : ("Acceptor started for its second lifecycle, message: " + secondAccepting.Message()));
    if (!secondAccepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient secondClient;
    const bool secondConnected = secondClient.Connect(secondPort);
    ServerCoreTest::ExpectTrue(secondConnected, "the second lifecycle client connected");
    if (!secondConnected)
    {
        acceptor.Stop();
        (void)closeConnection(copyConnection(&secondAcceptedConnection), secondObserver);
        secondClient.Close();
        io.Stop();
        return;
    }
    const std::shared_ptr<ServerCore::Net::Connection> secondConnection =
        waitForConnection(&secondAcceptedConnection);
    ServerCoreTest::ExpectTrue(secondConnection != nullptr,
        "the second lifecycle handler accepted a connection after relistening");

    acceptor.Stop();
    ServerCoreTest::ExpectEqual(static_cast<std::uint16_t>(0), acceptor.Port(),
        "Acceptor::Stop() clears the second lifecycle port");
    const std::shared_ptr<ServerCore::Net::Connection> secondConnectionToClose =
        secondConnection != nullptr ? secondConnection : copyConnection(&secondAcceptedConnection);
    const bool secondDisconnected = closeConnection(secondConnectionToClose, secondObserver);
    ServerCoreTest::ExpectTrue(
        secondDisconnected, "the second lifecycle connection disconnected before shutdown");
    secondClient.Close();
    io.Stop();
}

/// <summary>보낸 바이트가 관찰자에게 오고 되돌아오는지 본다. 이 층의 본 경로다.</summary>
void EchoRoundTrip()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 1);

    TestServer server;
    server.Observer().SetEcho(true);
    server.Start(port);

    TestClient client;
    ServerCoreTest::ExpectTrue(client.Connect(port), "the client connected");

    const std::vector<std::byte> payload = MakeFilledBytes(5, 0xA5);
    ServerCoreTest::ExpectTrue(client.SendAll(payload), "the client sent its payload");

    std::vector<std::byte> echoed;
    ServerCoreTest::ExpectTrue(
        client.ReceiveExactly(payload.size(), echoed), "the client read the echoed payload back");
    ServerCoreTest::ExpectTrue(echoed == payload, "the echoed bytes equal the sent bytes");

    // 이 경로가 실제로 돌았다는 것을 검사가 스스로 확인한다. 위의 왕복만으로는 관찰자가
    // 불렸는지 알 수 없고, 관찰자가 한 번도 안 불려도 위 단언들이 통과할 수는 없지만,
    // 세어 둔 값을 직접 보는 편이 그 사실을 기록으로 남긴다.
    ServerCoreTest::ExpectTrue(
        server.Observer().ReceiveCallCount() > 0, "the observer was called at least once");
    ServerCoreTest::ExpectEqual(payload.size(), server.Observer().ReceivedByteCount(),
        "the number of bytes the observer received");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), server.Observer().EchoFailureCount(),
        server.Observer().EchoFailureCount() == 0
            ? "the number of failed echo sends"
            : ("the number of failed echo sends, last failure: " +
                  server.Observer().LastEchoFailure()));

    client.Close();
    server.Shutdown();
}

/// <summary>끊김 통지가 정확히 한 번 오는지 본다.</summary>
/// <remarks>
/// 상대가 먼저 닫고, 그 뒤에 우리 쪽에서 Close()를 두 번 더 부른다. 통지 자리가 한 번만
/// 지나가는 것이 아니라면 여기서 수가 1보다 커진다.
/// </remarks>
void DisconnectIsNotifiedExactlyOnce()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 2);

    TestServer server;
    server.Start(port);

    TestClient client;
    ServerCoreTest::ExpectTrue(client.Connect(port), "the client connected");

    const std::shared_ptr<ServerCore::Net::Connection> connection =
        server.WaitForConnection(WaitLimit);
    ServerCoreTest::ExpectTrue(connection != nullptr, "the server accepted the connection");
    if (connection == nullptr)
    {
        return;
    }

    ServerCoreTest::ExpectTrue(connection->IsOpen(), "IsOpen() while the peer is still connected");

    client.Close();

    ServerCoreTest::ExpectTrue(server.Observer().WaitForDisconnect(WaitLimit),
        "the observer was notified that the connection ended");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1),
        server.Observer().DisconnectCallCount(), "the number of disconnect notifications");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(server.Observer().LastDisconnectCode()),
        "the error code carried by the disconnect notification");
    ServerCoreTest::ExpectTrue(!connection->IsOpen(), "IsOpen() after the peer closed");

    connection->Close();
    connection->Close();
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1),
        server.Observer().DisconnectCallCount(),
        "the number of disconnect notifications after two more Close() calls");

    server.Shutdown();
}

/// <summary>큰 payload가 여러 번의 수신으로 쪼개져 오고, 그래도 전부 오는지 본다.</summary>
/// <remarks>
/// 이 검사가 "메시지 경계를 약속하지 않는다"를 고정한다. 한 번 보낸 것이 한 번의 수신으로
/// 오지 않는다는 것을 수신 호출 수가 둘 이상이라는 단언으로 확인한다. 수신 버퍼가 payload
/// 보다 작으므로 이 단언은 우연이 아니라 구조가 보장한다.
///
/// 클라이언트가 보내기와 읽기를 따로 돌리는 이유: 서버가 되돌려 보내는 동안 클라이언트가
/// 읽지 않으면 양쪽 소켓 버퍼가 모두 차서 둘 다 멈춘다.
/// </remarks>
void LargePayloadCrossesReceiveBoundaries()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 3);
    constexpr std::size_t payloadSize = 256 * 1024;

    TestServer server;
    server.Observer().SetEcho(true);
    server.Start(port);

    TestClient client;
    ServerCoreTest::ExpectTrue(client.Connect(port), "the client connected");

    const std::vector<std::byte> payload = MakeFilledBytes(payloadSize, 0x3C);

    std::atomic<bool> sendSucceeded{ false };
    std::thread sender(
        [&client, &payload, &sendSucceeded] { sendSucceeded.store(client.SendAll(payload)); });

    std::vector<std::byte> echoed;
    const bool receiveSucceeded = client.ReceiveExactly(payloadSize, echoed);
    sender.join();

    ServerCoreTest::ExpectTrue(sendSucceeded.load(), "the client sent the whole payload");
    ServerCoreTest::ExpectTrue(receiveSucceeded, "the client read the whole payload back");
    ServerCoreTest::ExpectEqual(payloadSize, echoed.size(), "the number of bytes echoed back");
    ServerCoreTest::ExpectTrue(echoed == payload, "the echoed bytes equal the sent bytes");

    ServerCoreTest::ExpectEqual(payloadSize, server.Observer().ReceivedByteCount(),
        "the number of bytes the observer received");

    // 이 층이 메시지 경계를 모른다는 것이 여기서 드러난다. 한 번 보낸 것이 여러 번으로 왔다.
    ServerCoreTest::ExpectTrue(server.Observer().ReceiveCallCount() > 1,
        "the observer was called more than once for one send");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), server.Observer().EchoFailureCount(),
        server.Observer().EchoFailureCount() == 0
            ? "the number of failed echo sends"
            : ("the number of failed echo sends, last failure: " +
                  server.Observer().LastEchoFailure()));

    client.Close();
    server.Shutdown();
}

/// <summary>닫힌 연결에 보내면 거절되는지 본다.</summary>
void SendAfterCloseIsRefused()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 4);

    TestServer server;
    server.Start(port);

    TestClient client;
    ServerCoreTest::ExpectTrue(client.Connect(port), "the client connected");

    const std::shared_ptr<ServerCore::Net::Connection> connection =
        server.WaitForConnection(WaitLimit);
    ServerCoreTest::ExpectTrue(connection != nullptr, "the server accepted the connection");
    if (connection == nullptr)
    {
        return;
    }

    const std::vector<std::byte> payload = MakeFilledBytes(16, 0x11);

    const ServerCore::Core::Status beforeClose = connection->Send(payload);
    ServerCoreTest::ExpectTrue(beforeClose.IsOk(), "Send() on an open connection succeeded");

    connection->Close();
    ServerCoreTest::ExpectTrue(!connection->IsOpen(), "IsOpen() after Close()");

    const ServerCore::Core::Status afterClose = connection->Send(payload);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(afterClose.Code()), "the error code of Send() after Close()");

    const ServerCore::Core::Status emptyAfterClose = connection->Send(std::span<const std::byte>());
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(emptyAfterClose.Code()),
        "the error code of an empty Send() after Close()");

    ServerCoreTest::ExpectTrue(server.Observer().WaitForDisconnect(WaitLimit),
        "the observer was notified that the connection ended");

    client.Close();
    server.Shutdown();
}

/// <summary>Close()가 진행 중인 송신 뒤 큐와 공유 예산을 함께 반납하는지 본다.</summary>
/// <remarks>
/// WSASend는 큐 맨 앞 vector의 주소를 빌린다. 그러므로 Close() 직후 바로 큐를 지우면 use-after-free가
/// 된다. 유일한 I/O worker를 수신 callback 안에서 멈춘 뒤 두 송신을 넣으면, 첫 요청은 진행 중이고
/// 두 번째 요청은 반드시 큐 뒤에 남는다. Close() 직후에는 진행 중인 요청이 vector를 빌리고 있으므로
/// 공유 예산도 그대로 유지되어야 한다. worker를 풀고 끊김 통지를 받은 시점에는 큐와 공유 예산이
/// 모두 비었는지 보아, 버퍼 수명은 지키면서도 버린 payload의 예산을 붙잡지 않는지 함께 고정한다.
/// </remarks>
void CloseDiscardsQueuedSendBytes()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the queued-send discard test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const auto budget =
        std::make_shared<ServerCore::Net::SendBudget>(ServerCore::Net::SendQueueLimitBytes);
    ServerCore::Net::AcceptorAccess::SetSendBudget(acceptor, budget);
    const std::shared_ptr<BlockingReceiveObserver> observer =
        std::make_shared<BlockingReceiveObserver>();
    std::mutex acceptedMutex;
    std::condition_variable acceptedChanged;
    std::shared_ptr<ServerCore::Net::Connection> connection;

    const ServerCore::Core::Status ioStarted = io.Start(1);
    ServerCoreTest::ExpectTrue(
        ioStarted.IsOk(), "IoContext started one worker for the queued-send discard test");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&observer, &acceptedMutex, &acceptedChanged, &connection](
            std::shared_ptr<ServerCore::Net::Connection> accepted)
        {
            accepted->SetObserver(observer);
            {
                const std::lock_guard<std::mutex> guard(acceptedMutex);
                connection = std::move(accepted);
            }
            acceptedChanged.notify_all();
        });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 12);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(
        listening.IsOk(), "Acceptor listened for the queued-send discard test");
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }

    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(
        accepting.IsOk(), "Acceptor started for the queued-send discard test");
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient client;
    const auto cleanup = [&observer, &acceptor, &connection, &client, &io]()
    {
        // 실패 경로에서도 먼저 callback 문을 열어야 I/O worker를 join하는 Stop()이 매달리지 않는다.
        observer->ReleaseReceive();
        acceptor.Stop();
        if (connection)
        {
            connection->Close();
        }
        client.Close();
        connection.reset();
        io.Stop();
    };

    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the queued-send discard client connected");
    if (!connected)
    {
        cleanup();
        return;
    }

    {
        std::unique_lock<std::mutex> guard(acceptedMutex);
        const bool accepted = acceptedChanged.wait_for(
            guard, WaitLimit, [&connection] { return connection != nullptr; });
        ServerCoreTest::ExpectTrue(accepted, "the queued-send discard connection was accepted");
    }
    if (connection == nullptr)
    {
        cleanup();
        return;
    }

    const std::array<std::byte, 1> trigger{ static_cast<std::byte>(0x6A) };
    const bool triggerSent = client.SendAll(trigger);
    ServerCoreTest::ExpectTrue(triggerSent, "the queued-send discard client sent its trigger byte");
    const bool receiveEntered = triggerSent && observer->WaitForReceiveEntry(WaitLimit);
    ServerCoreTest::ExpectTrue(receiveEntered,
        "the only I/O worker entered the blocking callback before queued sends were closed");
    if (!receiveEntered)
    {
        cleanup();
        return;
    }

    constexpr std::size_t PayloadSize = ServerCore::Net::SendQueueLimitBytes / 2;
    const std::vector<std::byte> first = MakeFilledBytes(PayloadSize, 0x4A);
    const std::vector<std::byte> second = MakeFilledBytes(PayloadSize, 0xB7);
    const ServerCore::Core::Status firstQueued = connection->Send(first);
    const ServerCore::Core::Status secondQueued = connection->Send(second);
    ServerCoreTest::ExpectTrue(
        firstQueued.IsOk(), "the first queued send succeeded before Close()");
    ServerCoreTest::ExpectTrue(
        secondQueued.IsOk(), "the queued tail send succeeded before Close()");
    ServerCoreTest::ExpectEqual(ServerCore::Net::SendQueueLimitBytes, connection->QueuedSendBytes(),
        "two queued sends fill the send queue before Close()");
    ServerCoreTest::ExpectEqual(ServerCore::Net::SendQueueLimitBytes, budget->UsedBytes(),
        "the queued payloads fill the shared send budget before Close()");
    if (!firstQueued.IsOk() || !secondQueued.IsOk())
    {
        cleanup();
        return;
    }

    connection->Close();
    ServerCoreTest::ExpectTrue(
        !connection->IsOpen(), "Close() immediately closes the queued-send connection");
    ServerCoreTest::ExpectEqual(ServerCore::Net::SendQueueLimitBytes, budget->UsedBytes(),
        "Close() keeps shared budget reserved while WSASend still borrows the queue front");
    observer->ReleaseReceive();

    const bool disconnected = observer->WaitForDisconnect(WaitLimit);
    ServerCoreTest::ExpectTrue(disconnected,
        "the queued-send discard observer was notified after the in-flight send completed");
    const bool queueDiscarded =
        disconnected && WaitForQueuedSendBytes(*connection, std::size_t{ 0 }, WaitLimit);
    ServerCoreTest::ExpectTrue(queueDiscarded,
        "Close() discards every queued byte after the in-flight WSASend completion");
    const bool budgetReleased =
        disconnected && WaitForUsedSendBudget(*budget, std::size_t{ 0 }, WaitLimit);
    ServerCoreTest::ExpectTrue(budgetReleased,
        "Close() releases every discarded payload byte from the shared send budget");

    cleanup();
}

/// <summary>서로 다른 Connection의 보관 payload가 같은 Host 예산에서 경쟁하는지 본다.</summary>
/// <remarks>
/// 수락을 두 번 끝낸 뒤 유일한 I/O worker를 첫 연결의 수신 callback에서 멈춘다. 그러면 두
/// WSASend의 완료가 처리될 수 없어 첫 payload 예약이 결정적으로 유지된다. 둘째 연결의 같은
/// 크기 Send는 전체가 거절되어야 하고, worker를 푼 뒤에는 예산이 0으로 돌아와 재시도가 된다.
/// </remarks>
void SharedSendBudgetRejectsAcrossConnectionsAndRecovers()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the shared send-budget test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const auto budget =
        std::make_shared<ServerCore::Net::SendBudget>(ServerCore::Net::SendQueueLimitBytes);
    ServerCore::Net::AcceptorAccess::SetSendBudget(acceptor, budget);

    const auto blockingObserver = std::make_shared<BlockingReceiveObserver>();
    const auto secondObserver = std::make_shared<RecordingObserver>();
    std::mutex acceptedMutex;
    std::condition_variable acceptedChanged;
    std::vector<std::shared_ptr<ServerCore::Net::Connection>> connections;

    const ServerCore::Core::Status ioStarted = io.Start(1);
    ServerCoreTest::ExpectTrue(
        ioStarted.IsOk(), "one I/O worker started for the shared send-budget test");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&blockingObserver, &secondObserver, &acceptedMutex, &acceptedChanged, &connections](
            std::shared_ptr<ServerCore::Net::Connection> connection)
        {
            const std::lock_guard<std::mutex> guard(acceptedMutex);
            if (connections.empty())
            {
                connection->SetObserver(blockingObserver);
            }
            else
            {
                connection->SetObserver(secondObserver);
                secondObserver->SetConnection(connection);
            }
            connections.push_back(std::move(connection));
            acceptedChanged.notify_all();
        });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 22);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    const ServerCore::Core::Status accepting =
        listening.IsOk()
            ? acceptor.Start(io)
            : ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed);
    ServerCoreTest::ExpectTrue(listening.IsOk(), "the shared send-budget Acceptor listened");
    ServerCoreTest::ExpectTrue(accepting.IsOk(), "the shared send-budget Acceptor started");
    if (!listening.IsOk() || !accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient firstClient;
    TestClient secondClient;
    const auto waitForConnections = [&acceptedMutex, &acceptedChanged, &connections](
                                        const std::size_t expected)
    {
        std::unique_lock<std::mutex> guard(acceptedMutex);
        return acceptedChanged.wait_for(guard, WaitLimit,
            [&connections, expected]() { return connections.size() >= expected; });
    };
    const auto cleanup = [&]()
    {
        blockingObserver->ReleaseReceive();
        acceptor.Stop();
        firstClient.Close();
        secondClient.Close();
        std::vector<std::shared_ptr<ServerCore::Net::Connection>> connectionsToClose;
        {
            const std::lock_guard<std::mutex> guard(acceptedMutex);
            connectionsToClose.swap(connections);
        }
        for (const std::shared_ptr<ServerCore::Net::Connection>& connection : connectionsToClose)
        {
            connection->Close();
        }
        (void)blockingObserver->WaitForDisconnect(WaitLimit);
        (void)secondObserver->WaitForDisconnect(WaitLimit);
        connectionsToClose.clear();
        io.Stop();
    };

    const bool firstConnected = firstClient.Connect(port);
    const bool firstAccepted = firstConnected && waitForConnections(1);
    const bool secondConnected = firstAccepted && secondClient.Connect(port);
    const bool secondAccepted = secondConnected && waitForConnections(2);
    ServerCoreTest::ExpectTrue(firstAccepted, "the first shared-budget connection was accepted");
    ServerCoreTest::ExpectTrue(secondAccepted, "the second shared-budget connection was accepted");
    if (!firstAccepted || !secondAccepted)
    {
        cleanup();
        return;
    }

    std::shared_ptr<ServerCore::Net::Connection> firstConnection;
    std::shared_ptr<ServerCore::Net::Connection> secondConnection;
    {
        const std::lock_guard<std::mutex> guard(acceptedMutex);
        firstConnection = connections[0];
        secondConnection = connections[1];
    }

    const std::array<std::byte, 1> trigger{ static_cast<std::byte>(0x79) };
    const bool triggerSent = firstClient.SendAll(trigger);
    const bool receiveBlocked = triggerSent && blockingObserver->WaitForReceiveEntry(WaitLimit);
    ServerCoreTest::ExpectTrue(
        receiveBlocked, "the only I/O worker blocked before shared-budget sends");
    if (!receiveBlocked)
    {
        cleanup();
        return;
    }

    constexpr std::size_t PayloadSize = ServerCore::Net::SendQueueLimitBytes * 3 / 4;
    const std::vector<std::byte> firstPayload = MakeFilledBytes(PayloadSize, 0x2D);
    const std::vector<std::byte> secondPayload = MakeFilledBytes(PayloadSize, 0xA4);
    const ServerCore::Core::Status firstQueued = firstConnection->Send(firstPayload);
    const ServerCore::Core::Status secondRejected = secondConnection->Send(secondPayload);
    ServerCoreTest::ExpectTrue(firstQueued.IsOk(), "the first shared-budget payload was queued");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::WouldBlock),
        static_cast<int>(secondRejected.Code()),
        "the second Connection is rejected by the shared retained-payload budget");
    ServerCoreTest::ExpectEqual(PayloadSize, budget->UsedBytes(),
        "a rejected cross-Connection Send does not consume shared budget");
    if (!firstQueued.IsOk() || secondRejected.Code() != ServerCore::Core::ErrorCode::WouldBlock)
    {
        cleanup();
        return;
    }

    blockingObserver->ReleaseReceive();
    std::vector<std::byte> receivedFirst;
    const bool firstReceived = firstClient.ReceiveExactly(firstPayload.size(), receivedFirst);
    const bool firstBudgetReleased =
        firstReceived && WaitForUsedSendBudget(*budget, std::size_t{ 0 }, WaitLimit);
    ServerCoreTest::ExpectTrue(firstReceived && receivedFirst == firstPayload,
        "the admitted shared-budget payload arrived unchanged");
    ServerCoreTest::ExpectTrue(
        firstBudgetReleased, "a completed cross-Connection Send releases shared budget");
    if (!firstBudgetReleased)
    {
        cleanup();
        return;
    }

    const std::vector<std::byte> retry = MakeFilledBytes(37, 0x5C);
    const ServerCore::Core::Status retryQueued = secondConnection->Send(retry);
    std::vector<std::byte> receivedRetry;
    const bool retryReceived =
        retryQueued.IsOk() && secondClient.ReceiveExactly(retry.size(), receivedRetry);
    const bool retryBudgetReleased =
        retryReceived && WaitForUsedSendBudget(*budget, std::size_t{ 0 }, WaitLimit);
    ServerCoreTest::ExpectTrue(retryQueued.IsOk(),
        "the previously rejected Connection sends after shared-budget recovery");
    ServerCoreTest::ExpectTrue(retryReceived && receivedRetry == retry,
        "the shared-budget retry arrives without bytes from the rejected payload");
    ServerCoreTest::ExpectTrue(
        retryBudgetReleased, "the shared budget returns to zero after the retry");

    cleanup();
}

/// <summary>송신 큐 상한이 overflow를 통째로 거절하고, 완료 뒤 다시 수락하는지 본다.</summary>
/// <remarks>
/// 유일한 I/O worker를 수신 callback 안에서 멈추면 이미 게시한 WSASend의 완료가 처리되지
/// 않는다. 그래서 정확히 상한만큼 넣은 뒤 1바이트를 더 보낼 때의 QueuedSendBytes()를 시간
/// 경합 없이 고정할 수 있다. worker를 푼 뒤에는 상대가 실제 바이트를 읽고, counter가 0으로
/// 돌아온 다음의 재시도까지 본다. 재시도 수신값 비교는 거절된 1바이트가 몰래 큐에 남지
/// 않았다는 증거이기도 하다.
/// </remarks>
void SendQueueLimitRejectsWholeOverflowAndRecovers()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the send queue limit test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const std::shared_ptr<BlockingReceiveObserver> observer =
        std::make_shared<BlockingReceiveObserver>();
    std::mutex acceptedMutex;
    std::condition_variable acceptedChanged;
    std::shared_ptr<ServerCore::Net::Connection> connection;

    const ServerCore::Core::Status ioStarted = io.Start(1);
    ServerCoreTest::ExpectTrue(
        ioStarted.IsOk(), "IoContext started one worker for the send queue limit");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&observer, &acceptedMutex, &acceptedChanged, &connection](
            std::shared_ptr<ServerCore::Net::Connection> accepted)
        {
            accepted->SetObserver(observer);
            {
                const std::lock_guard<std::mutex> guard(acceptedMutex);
                connection = std::move(accepted);
            }
            acceptedChanged.notify_all();
        });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 11);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(listening.IsOk(), "Acceptor listened for the send queue limit");
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }

    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(accepting.IsOk(), "Acceptor started for the send queue limit");
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient client;
    const auto cleanup = [&observer, &acceptor, &connection, &client, &io]()
    {
        // I/O worker가 callback 안에 멈춘 채면 Stop()이 join을 기다린다. 모든 실패 경로도
        // 먼저 문을 열어 둬야 검증 실패가 다음 검증의 hang으로 번지지 않는다.
        observer->ReleaseReceive();
        acceptor.Stop();
        if (connection)
        {
            connection->Close();
        }
        client.Close();
        connection.reset();
        io.Stop();
    };

    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the send queue limit client connected");
    if (!connected)
    {
        cleanup();
        return;
    }

    {
        std::unique_lock<std::mutex> guard(acceptedMutex);
        const bool accepted = acceptedChanged.wait_for(
            guard, WaitLimit, [&connection] { return connection != nullptr; });
        ServerCoreTest::ExpectTrue(accepted, "the send queue limit connection was accepted");
    }
    if (connection == nullptr)
    {
        cleanup();
        return;
    }

    const std::array<std::byte, 1> trigger{ static_cast<std::byte>(0x6D) };
    const bool triggerSent = client.SendAll(trigger);
    ServerCoreTest::ExpectTrue(triggerSent, "the send queue limit client sent its trigger byte");
    const bool receiveEntered = triggerSent && observer->WaitForReceiveEntry(WaitLimit);
    ServerCoreTest::ExpectTrue(receiveEntered,
        "the only I/O worker entered the blocking callback before filling the send queue");
    if (!receiveEntered)
    {
        cleanup();
        return;
    }

    const std::vector<std::byte> atLimit =
        MakeFilledBytes(ServerCore::Net::SendQueueLimitBytes, 0x3A);
    const ServerCore::Core::Status atLimitQueued = connection->Send(atLimit);
    ServerCoreTest::ExpectTrue(
        atLimitQueued.IsOk(), "a send exactly at SendQueueLimitBytes succeeded");
    if (!atLimitQueued.IsOk())
    {
        cleanup();
        return;
    }

    ServerCoreTest::ExpectEqual(ServerCore::Net::SendQueueLimitBytes, connection->QueuedSendBytes(),
        "the queued byte count after filling the send queue limit");

    const std::array<std::byte, 1> overflow{ static_cast<std::byte>(0xEE) };
    const ServerCore::Core::Status overflowRejected = connection->Send(overflow);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::WouldBlock),
        static_cast<int>(overflowRejected.Code()),
        "the error code for a send beyond the queue limit");
    ServerCoreTest::ExpectEqual(ServerCore::Net::SendQueueLimitBytes, connection->QueuedSendBytes(),
        "the queued byte count after rejecting the overflowing send");
    if (overflowRejected.Code() != ServerCore::Core::ErrorCode::WouldBlock)
    {
        cleanup();
        return;
    }

    observer->ReleaseReceive();

    std::vector<std::byte> receivedAtLimit;
    const bool receivedLimit = client.ReceiveExactly(atLimit.size(), receivedAtLimit);
    ServerCoreTest::ExpectTrue(
        receivedLimit, "the client received the entire send queue limit payload");
    ServerCoreTest::ExpectTrue(
        receivedAtLimit == atLimit, "the bytes accepted before the queue limit arrived unchanged");
    if (!receivedLimit || receivedAtLimit != atLimit)
    {
        cleanup();
        return;
    }

    const bool firstDrainCompleted =
        WaitForQueuedSendBytes(*connection, std::size_t{ 0 }, WaitLimit);
    ServerCoreTest::ExpectTrue(firstDrainCompleted,
        "the queued byte count returned to zero after the limit payload completed");
    if (!firstDrainCompleted)
    {
        cleanup();
        return;
    }

    const std::vector<std::byte> retry = MakeFilledBytes(23, 0x4C);
    const ServerCore::Core::Status retryQueued = connection->Send(retry);
    ServerCoreTest::ExpectTrue(
        retryQueued.IsOk(), "a send after draining the queue limit succeeded");
    if (!retryQueued.IsOk())
    {
        cleanup();
        return;
    }

    std::vector<std::byte> receivedRetry;
    const bool receivedRetryPayload = client.ReceiveExactly(retry.size(), receivedRetry);
    ServerCoreTest::ExpectTrue(receivedRetryPayload, "the client received the retry payload");
    ServerCoreTest::ExpectTrue(receivedRetry == retry,
        "the retry payload contains no byte from the rejected overflowing send");

    const bool retryDrainCompleted =
        receivedRetryPayload && WaitForQueuedSendBytes(*connection, std::size_t{ 0 }, WaitLimit);
    ServerCoreTest::ExpectTrue(retryDrainCompleted,
        "the queued byte count returned to zero after the retry payload completed");

    cleanup();
}

/// <summary>빈 큐의 CloseAfterSend()가 기다리지 않고 정상 종료를 시작하는지 본다.</summary>
void CloseAfterSendOnEmptyConnectionCloses()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 9);

    TestServer server;
    server.Start(port);

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the client connected");
    if (!connected)
    {
        server.Shutdown();
        return;
    }

    const std::shared_ptr<ServerCore::Net::Connection> connection =
        server.WaitForConnection(WaitLimit);
    ServerCoreTest::ExpectTrue(connection != nullptr, "the server accepted the connection");
    if (connection == nullptr)
    {
        client.Close();
        server.Shutdown();
        return;
    }

    connection->CloseAfterSend();
    connection->CloseAfterSend();

    const std::vector<std::byte> payload = MakeFilledBytes(16, 0x4D);
    const ServerCore::Core::Status afterCloseAfterSend = connection->Send(payload);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(afterCloseAfterSend.Code()),
        "the error code of Send() after an empty CloseAfterSend()");
    const ServerCore::Core::Status emptyAfterCloseAfterSend =
        connection->Send(std::span<const std::byte>());
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(emptyAfterCloseAfterSend.Code()),
        "the error code of an empty Send() after an empty CloseAfterSend()");
    ServerCoreTest::ExpectTrue(client.WaitForPeerClose(),
        "the client observed the peer close from an empty CloseAfterSend()");

    ServerCoreTest::ExpectTrue(server.Observer().WaitForDisconnect(WaitLimit),
        "the observer was notified after an empty CloseAfterSend()");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1),
        server.Observer().DisconnectCallCount(),
        "the number of disconnect notifications after an empty CloseAfterSend()");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), connection->QueuedSendBytes(),
        "the queued byte count after an empty CloseAfterSend()");
    ServerCoreTest::ExpectTrue(
        !connection->IsOpen(), "the connection is closed after an empty CloseAfterSend()");

    client.Close();
    server.Shutdown();
}

/// <summary>CloseAfterSend()가 이미 받은 바이트를 모두 내보낸 뒤 끊는지 본다.</summary>
/// <remarks>
/// 수신 callback 하나를 유일한 I/O worker에서 멈춘다. 그 동안 WSASend 완료가 처리되지 못하므로,
/// CloseAfterSend()가 실제로 비어 있지 않은 큐를 drain하는 상태에서 호출됐음을 시간 경합 없이
/// 고정할 수 있다.
/// </remarks>
void CloseAfterSendDrainsQueuedBytes()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const std::shared_ptr<BlockingReceiveObserver> observer =
        std::make_shared<BlockingReceiveObserver>();
    std::mutex acceptedMutex;
    std::condition_variable acceptedChanged;
    std::shared_ptr<ServerCore::Net::Connection> connection;

    const ServerCore::Core::Status ioStarted = io.Start(1);
    ServerCoreTest::ExpectTrue(ioStarted.IsOk(), "IoContext started for CloseAfterSend()");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&observer, &acceptedMutex, &acceptedChanged, &connection](
            std::shared_ptr<ServerCore::Net::Connection> accepted)
        {
            accepted->SetObserver(observer);
            {
                const std::lock_guard<std::mutex> guard(acceptedMutex);
                connection = std::move(accepted);
            }
            acceptedChanged.notify_all();
        });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 8);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(listening.IsOk(), "Acceptor listened for CloseAfterSend()");
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }

    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(accepting.IsOk(), "Acceptor started for CloseAfterSend()");
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the CloseAfterSend client connected");
    if (!connected)
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    {
        std::unique_lock<std::mutex> guard(acceptedMutex);
        const bool accepted = acceptedChanged.wait_for(
            guard, WaitLimit, [&connection] { return connection != nullptr; });
        ServerCoreTest::ExpectTrue(accepted, "the CloseAfterSend connection was accepted");
    }
    if (connection == nullptr)
    {
        client.Close();
        acceptor.Stop();
        io.Stop();
        return;
    }

    const std::array<std::byte, 1> trigger{ static_cast<std::byte>(0x52) };
    const bool triggerSent = client.SendAll(trigger);
    ServerCoreTest::ExpectTrue(triggerSent, "the CloseAfterSend client sent its trigger byte");
    const bool receiveEntered = triggerSent && observer->WaitForReceiveEntry(WaitLimit);
    ServerCoreTest::ExpectTrue(receiveEntered,
        "the only I/O worker entered the blocking receive callback before queuing sends");
    if (!receiveEntered)
    {
        observer->ReleaseReceive();
        connection->Close();
        client.Close();
        acceptor.Stop();
        connection.reset();
        io.Stop();
        return;
    }

    const std::vector<std::byte> first = MakeFilledBytes(16 * 1024, 0x3A);
    const std::vector<std::byte> second = MakeFilledBytes(32 * 1024, 0xC7);
    std::vector<std::byte> expected;
    expected.reserve(first.size() + second.size());
    expected.insert(expected.end(), first.begin(), first.end());
    expected.insert(expected.end(), second.begin(), second.end());

    const ServerCore::Core::Status firstQueued = connection->Send(first);
    ServerCoreTest::ExpectTrue(firstQueued.IsOk(), "the first queued send succeeded");
    const ServerCore::Core::Status secondQueued = connection->Send(second);
    ServerCoreTest::ExpectTrue(secondQueued.IsOk(), "the second queued send succeeded");
    if (!firstQueued.IsOk() || !secondQueued.IsOk())
    {
        observer->ReleaseReceive();
        connection->Close();
        client.Close();
        acceptor.Stop();
        connection.reset();
        io.Stop();
        return;
    }

    connection->CloseAfterSend();

    const ServerCore::Core::Status afterCloseAfterSend = connection->Send(first);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(afterCloseAfterSend.Code()),
        "the error code of Send() after CloseAfterSend()");
    const ServerCore::Core::Status emptyAfterCloseAfterSend =
        connection->Send(std::span<const std::byte>());
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(emptyAfterCloseAfterSend.Code()),
        "the error code of an empty Send() after CloseAfterSend()");

    observer->ReleaseReceive();

    std::vector<std::byte> received;
    ServerCoreTest::ExpectTrue(client.ReceiveExactly(expected.size(), received),
        "the client read every byte queued before CloseAfterSend()");
    ServerCoreTest::ExpectTrue(
        received == expected, "the queued bytes arrived in their original send order before close");
    ServerCoreTest::ExpectTrue(client.WaitForPeerClose(),
        "the client observed the peer close after every queued byte arrived");

    ServerCoreTest::ExpectTrue(observer->WaitForDisconnect(WaitLimit),
        "the observer was notified after CloseAfterSend() completed");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, observer->DisconnectCount(),
        "the number of disconnect notifications after CloseAfterSend()");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), connection->QueuedSendBytes(),
        "the queued byte count after CloseAfterSend() drains");
    ServerCoreTest::ExpectTrue(
        !connection->IsOpen(), "the connection is closed after CloseAfterSend() drains");

    client.Close();
    acceptor.Stop();
    connection->Close();
    connection.reset();
    io.Stop();
}

/// <summary>여러 스레드가 동시에 보내도 각 Send의 바이트가 서로 섞이지 않는지 본다.</summary>
/// <remarks>
/// 스레드마다 자기 번호로 채운 블록을 보낸다. 블록 경계가 정렬되어 있으므로, 받은 흐름을
/// 블록 크기로 잘랐을 때 각 조각이 한 가지 값으로만 되어 있어야 한다. 한 Send의 바이트
/// 사이에 다른 Send의 바이트가 끼어들면 그 조각에 두 값이 섞인다.
///
/// 보내는 순서는 약속된 것이 아니므로 조각의 순서는 보지 않는다. 조각 안이 섞이지 않는
/// 것만 본다. 그것이 이 층이 실제로 약속하는 것이다.
/// </remarks>
void ConcurrentSendsDoNotInterleave()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 5);

    constexpr std::size_t senderCount = 4;
    constexpr std::size_t blocksPerSender = 64;
    constexpr std::size_t blockSize = 256;
    constexpr std::size_t totalBytes = senderCount * blocksPerSender * blockSize;

    TestServer server;
    server.Start(port);

    TestClient client;
    ServerCoreTest::ExpectTrue(client.Connect(port), "the client connected");

    const std::shared_ptr<ServerCore::Net::Connection> connection =
        server.WaitForConnection(WaitLimit);
    ServerCoreTest::ExpectTrue(connection != nullptr, "the server accepted the connection");
    if (connection == nullptr)
    {
        return;
    }

    std::atomic<std::size_t> failedSendCount{ 0 };
    std::vector<std::thread> senders;
    senders.reserve(senderCount);

    for (std::size_t senderIndex = 0; senderIndex < senderCount; ++senderIndex)
    {
        senders.emplace_back(
            [&connection, &failedSendCount, senderIndex]
            {
                const std::vector<std::byte> block =
                    MakeFilledBytes(blockSize, static_cast<unsigned char>(0x40 + senderIndex));
                for (std::size_t blockIndex = 0; blockIndex < blocksPerSender; ++blockIndex)
                {
                    if (!connection->Send(block).IsOk())
                    {
                        failedSendCount.fetch_add(1);
                    }
                }
            });
    }

    std::vector<std::byte> received;
    const bool receiveSucceeded = client.ReceiveExactly(totalBytes, received);

    for (std::thread& sender : senders)
    {
        sender.join();
    }

    ServerCoreTest::ExpectEqual(
        static_cast<std::size_t>(0), failedSendCount.load(), "the number of failed sends");
    ServerCoreTest::ExpectTrue(receiveSucceeded, "the client read every byte that was sent");
    ServerCoreTest::ExpectEqual(totalBytes, received.size(), "the number of bytes received");

    std::size_t mixedBlockCount = 0;
    for (std::size_t offset = 0; offset + blockSize <= received.size(); offset += blockSize)
    {
        const std::byte first = received[offset];
        for (std::size_t index = 1; index < blockSize; ++index)
        {
            if (received[offset + index] != first)
            {
                ++mixedBlockCount;
                break;
            }
        }
    }

    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), mixedBlockCount,
        "the number of blocks that hold bytes from more than one send");

    client.Close();
    server.Shutdown();
}

/// <summary>I/O worker 둘이 서로 다른 연결의 수신 callback을 동시에 처리하는지 본다.</summary>
/// <remarks>
/// 시간 자체를 재지 않는다. 첫 연결의 callback을 문에서 멈춘 뒤 두 번째 연결에 바이트를 보낸다.
/// worker가 하나면 첫 callback이 돌아오기 전에는 두 번째가 문에 들어올 수 없다. worker가 둘이면
/// 두 callback이 문 안에서 함께 대기하며 overlap 표시가 선다.
/// </remarks>
void MultiWorkerDeliversDifferentConnectionsConcurrently()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the multi-worker delivery test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const std::shared_ptr<ConcurrentReceiveGate> gate = std::make_shared<ConcurrentReceiveGate>();
    std::mutex acceptedMutex;
    std::condition_variable acceptedChanged;
    std::vector<std::shared_ptr<ServerCore::Net::Connection>> acceptedConnections;
    std::vector<std::shared_ptr<ConcurrentReceiveObserver>> observers;

    const ServerCore::Core::Status ioStarted = io.Start(2);
    ServerCoreTest::ExpectTrue(
        ioStarted.IsOk(), "IoContext started two workers for concurrent delivery");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&acceptedMutex, &acceptedChanged, &acceptedConnections, &gate, &observers](
            std::shared_ptr<ServerCore::Net::Connection> connection)
        {
            const std::shared_ptr<ConcurrentReceiveObserver> observer =
                std::make_shared<ConcurrentReceiveObserver>(gate);
            connection->SetObserver(observer);
            {
                const std::lock_guard<std::mutex> guard(acceptedMutex);
                acceptedConnections.emplace_back(std::move(connection));
                observers.emplace_back(observer);
            }
            acceptedChanged.notify_all();
        });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 10);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(listening.IsOk(), "Acceptor listened for multi-worker delivery");
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }

    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(accepting.IsOk(), "Acceptor started for multi-worker delivery");
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient firstClient;
    TestClient secondClient;
    const bool firstConnected = firstClient.Connect(port);
    const bool secondConnected = secondClient.Connect(port);
    ServerCoreTest::ExpectTrue(firstConnected, "the first multi-worker client connected");
    ServerCoreTest::ExpectTrue(secondConnected, "the second multi-worker client connected");

    bool bothAccepted = false;
    if (firstConnected && secondConnected)
    {
        std::unique_lock<std::mutex> guard(acceptedMutex);
        bothAccepted = acceptedChanged.wait_for(
            guard, WaitLimit, [&acceptedConnections] { return acceptedConnections.size() >= 2; });
    }
    ServerCoreTest::ExpectTrue(
        bothAccepted, "both multi-worker clients were accepted before receiving");

    const std::vector<std::byte> payload = MakeFilledBytes(1, 0x7B);
    bool firstSent = false;
    bool firstEntered = false;
    bool secondSent = false;
    bool bothEntered = false;
    if (bothAccepted)
    {
        firstSent = firstClient.SendAll(payload);
        ServerCoreTest::ExpectTrue(firstSent, "the first multi-worker client sent its byte");

        if (firstSent)
        {
            firstEntered = gate->WaitForEntries(1, WaitLimit);
            ServerCoreTest::ExpectTrue(
                firstEntered, "the first receive callback entered the shared gate");
        }

        if (firstEntered)
        {
            secondSent = secondClient.SendAll(payload);
            ServerCoreTest::ExpectTrue(secondSent, "the second multi-worker client sent its byte");
        }

        if (secondSent)
        {
            bothEntered = gate->WaitForEntries(2, WaitLimit);
            ServerCoreTest::ExpectTrue(bothEntered,
                "the second receive callback entered while the first remained blocked");
            if (bothEntered)
            {
                ServerCoreTest::ExpectTrue(gate->ObservedOverlap(),
                    "two different connections had active receive callbacks together");
            }
        }
    }

    // 실패한 경우에도 첫 callback이 문 안에 남으면 이후 Stop()이 I/O worker를 기다릴 수 있다.
    // 그래서 검증 결과와 무관하게 먼저 문을 열고 callback들이 빠져나오게 한다.
    gate->Release();
    const bool callbacksReleased = gate->WaitForIdle(WaitLimit);
    ServerCoreTest::ExpectTrue(
        callbacksReleased, "all gated receive callbacks returned before shutdown");

    // 전송 계층의 종료 순서는 수락기 → 연결 → 완료 포트다. Acceptor::Stop() 뒤에는 더 이상
    // handler가 벡터에 연결을 넣지 않으므로, 그 다음에 복사해 닫으면 수명 경계가 단순해진다.
    acceptor.Stop();
    std::vector<std::shared_ptr<ServerCore::Net::Connection>> connectionsToClose;
    {
        const std::lock_guard<std::mutex> guard(acceptedMutex);
        connectionsToClose = acceptedConnections;
    }
    for (const std::shared_ptr<ServerCore::Net::Connection>& connection : connectionsToClose)
    {
        connection->Close();
    }

    if (!connectionsToClose.empty())
    {
        const bool disconnected = gate->WaitForDisconnects(connectionsToClose.size(), WaitLimit);
        ServerCoreTest::ExpectTrue(
            disconnected, "all accepted multi-worker connections reported disconnect");
    }

    firstClient.Close();
    secondClient.Close();
    connectionsToClose.clear();
    {
        const std::lock_guard<std::mutex> guard(acceptedMutex);
        acceptedConnections.clear();
        observers.clear();
    }
    io.Stop();
}

/// <summary>수신 관찰자가 돌아오는 동안 Close가 끊김 통지를 겹치게 하지 않는지 본다.</summary>
void DisconnectWaitsForReceiveCallback()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for callback serialization succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const std::shared_ptr<BlockingReceiveObserver> observer =
        std::make_shared<BlockingReceiveObserver>();
    std::mutex acceptedMutex;
    std::condition_variable acceptedChanged;
    std::shared_ptr<ServerCore::Net::Connection> connection;

    const ServerCore::Core::Status ioStarted = io.Start(2);
    ServerCoreTest::ExpectTrue(ioStarted.IsOk(), "IoContext started for callback serialization");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&observer, &acceptedMutex, &acceptedChanged, &connection](
            std::shared_ptr<ServerCore::Net::Connection> accepted)
        {
            accepted->SetObserver(observer);
            {
                const std::lock_guard<std::mutex> guard(acceptedMutex);
                connection = std::move(accepted);
            }
            acceptedChanged.notify_all();
        });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 6);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(listening.IsOk(), "Acceptor listened for callback serialization");
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }
    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(accepting.IsOk(), "Acceptor started for callback serialization");
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the callback serialization client connected");
    if (!connected)
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    {
        std::unique_lock<std::mutex> guard(acceptedMutex);
        const bool accepted = acceptedChanged.wait_for(
            guard, WaitLimit, [&connection] { return connection != nullptr; });
        ServerCoreTest::ExpectTrue(accepted, "the callback serialization connection was accepted");
    }
    if (connection == nullptr)
    {
        client.Close();
        acceptor.Stop();
        io.Stop();
        return;
    }

    const std::array<std::byte, 1> byte{ static_cast<std::byte>(0x5A) };
    const bool sent = client.SendAll(byte);
    ServerCoreTest::ExpectTrue(sent, "the callback serialization client sent one byte");
    const bool receiveEntered = sent && observer->WaitForReceiveEntry(WaitLimit);
    if (!receiveEntered)
    {
        ServerCoreTest::ExpectTrue(
            receiveEntered, "the callback serialization receive entry was reachable");
        observer->ReleaseReceive();
        connection->Close();
        client.Close();
        acceptor.Stop();
        io.Stop();
        return;
    }

    std::thread closer([connection] { connection->Close(); });
    closer.join();

    const bool disconnectedBeforeRelease =
        observer->WaitForDisconnect(std::chrono::milliseconds(300));
    ServerCoreTest::ExpectTrue(!disconnectedBeforeRelease,
        "OnDisconnected waits until the active OnBytesReceived callback returns");

    observer->ReleaseReceive();
    const bool disconnected = observer->WaitForDisconnect(WaitLimit);
    ServerCoreTest::ExpectTrue(
        disconnected, "the delayed disconnect notification eventually arrives");
    ServerCoreTest::ExpectTrue(!observer->DisconnectedWhileReceiving(),
        "OnDisconnected never overlaps the observer's receive callback");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, observer->DisconnectCount(),
        "the delayed disconnect still arrives exactly once");

    client.Close();
    acceptor.Stop();
    connection.reset();
    io.Stop();
}

/// <summary>관찰자 등록 전 Close가 나도 뒤의 SetObserver가 끊김을 받는지 본다.</summary>
void LateObserverReceivesPriorDisconnect()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for late observer registration succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    const std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();

    const ServerCore::Core::Status ioStarted = io.Start(1);
    ServerCoreTest::ExpectTrue(
        ioStarted.IsOk(), "IoContext started for late observer registration");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [observer](std::shared_ptr<ServerCore::Net::Connection> connection)
        {
            // Acceptor가 handler가 돌아온 뒤 Start()를 부르므로 이 경로는 진행 중인 I/O 없이
            // 닫힌다. 그 뒤 observer를 건 경우에 통지를 보류했다가 전달해야 한다.
            connection->Close();
            connection->SetObserver(observer);
        });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 6);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(
        listening.IsOk(), "Acceptor listened for late observer registration");
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }
    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(accepting.IsOk(), "Acceptor started for late observer registration");
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the late observer registration client connected");
    if (connected)
    {
        const bool disconnected = observer->WaitForDisconnect(std::chrono::milliseconds(1000));
        ServerCoreTest::ExpectTrue(disconnected,
            "SetObserver receives a disconnect that occurred before observer assignment");
        ServerCoreTest::ExpectEqual(std::size_t{ 1 }, observer->DisconnectCallCount(),
            "a late observer receives the prior disconnect exactly once");
    }

    client.Close();
    acceptor.Stop();
    io.Stop();
}

/// <summary>Stop()이 완료 뒤 수락 처리기 인계가 끝날 때까지 기다리는지 본다.</summary>
/// <remarks>
/// Acceptor는 네 개의 AcceptEx를 동시에 건다. 네 처리기를 전부 문에서 멈추면
/// pendingAccepts는 0이지만, 네 완료 경로는 아직 AssociateSocket() 뒤 처리기를 실행 중이다.
/// 처리기를 풀기 전 Stop()이 끝나지 않는지 직접 고정한다.
/// </remarks>
void AcceptorStopWaitsForActiveCompletionHandoff()
{
    constexpr std::size_t HandoffCount = 4;
    constexpr std::chrono::milliseconds EarlyReturnWindow{ 500 };

    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    BlockingHandoff handoff;

    const ServerCore::Core::Status ioStarted = io.Start(static_cast<int>(HandoffCount));
    ServerCoreTest::ExpectTrue(ioStarted.IsOk(), "IoContext::Start() for handoff drain succeeded");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&handoff](std::shared_ptr<ServerCore::Net::Connection> connection)
        { handoff.Handle(std::move(connection)); });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 7);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(listening.IsOk(),
        listening.IsOk()
            ? "Acceptor::Listen() for handoff drain succeeded"
            : ("Acceptor::Listen() for handoff drain succeeded, message: " + listening.Message()));
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }

    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(accepting.IsOk(),
        accepting.IsOk()
            ? "Acceptor::Start() for handoff drain succeeded"
            : ("Acceptor::Start() for handoff drain succeeded, message: " + accepting.Message()));
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    std::array<TestClient, HandoffCount> clients;
    bool everyClientConnected = true;
    for (TestClient& client : clients)
    {
        everyClientConnected = client.Connect(port) && everyClientConnected;
    }
    ServerCoreTest::ExpectTrue(
        everyClientConnected, "every client connected to occupy the outstanding accept slots");
    if (!everyClientConnected)
    {
        handoff.Release();
        acceptor.Stop();
        io.Stop();
        return;
    }

    const bool everyHandoffEntered = handoff.WaitForEntries(HandoffCount, WaitLimit);
    ServerCoreTest::ExpectTrue(
        everyHandoffEntered, "every outstanding accept reached the blocking handoff handler");
    if (!everyHandoffEntered)
    {
        handoff.Release();
        acceptor.Stop();
        io.Stop();
        return;
    }

    std::mutex stopMutex;
    std::condition_variable stopChanged;
    bool stopReturned = false;
    std::thread stopper(
        [&acceptor, &stopMutex, &stopChanged, &stopReturned]
        {
            acceptor.Stop();
            {
                const std::lock_guard<std::mutex> guard(stopMutex);
                stopReturned = true;
            }
            stopChanged.notify_all();
        });

    const bool portClosed = WaitForClosedPort(acceptor, WaitLimit);
    ServerCoreTest::ExpectTrue(
        portClosed, "Stop() closed the listening port before it drains handoffs");

    bool returnedBeforeRelease = false;
    if (portClosed)
    {
        std::unique_lock<std::mutex> guard(stopMutex);
        returnedBeforeRelease = stopChanged.wait_for(
            guard, EarlyReturnWindow, [&stopReturned] { return stopReturned; });
    }
    ServerCoreTest::ExpectTrue(!returnedBeforeRelease,
        "Acceptor::Stop() waits while accepted connection handoffs are still running");

    handoff.Release();
    stopper.join();

    {
        const std::lock_guard<std::mutex> guard(stopMutex);
        ServerCoreTest::ExpectTrue(
            stopReturned, "Acceptor::Stop() returns after every active handoff completes");
    }

    io.Stop();
}

/// <summary>오래 실행되는 정상 수락 처리기가 Stop()의 취소 제한에 걸리지 않는지 본다.</summary>
/// <remarks>
/// I/O 스레드를 하나만 두고 처리기를 10초 넘게 막는다. 그 동안 나머지 AcceptEx 취소 완료도
/// 같은 스레드를 쓸 수 없다. 처리기는 제한 없이 기다리고 취소 완료에만 별도 제한을 적용해야
/// 정상 처리기의 실행 시간을 종료 순서 위반으로 오판하지 않는다.
/// </remarks>
void AcceptorStopDoesNotTimeOutActiveCompletionHandoff()
{
    constexpr std::chrono::seconds FormerAcceptDrainTimeout{ 10 };
    constexpr std::chrono::seconds TimeoutOverrun{ 1 };

    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    ServerCore::Net::IoContext io;
    ServerCore::Net::Acceptor acceptor;
    BlockingHandoff handoff;

    const ServerCore::Core::Status ioStarted = io.Start(1);
    ServerCoreTest::ExpectTrue(
        ioStarted.IsOk(), "IoContext::Start() for long handoff drain succeeded");
    if (!ioStarted.IsOk())
    {
        return;
    }

    acceptor.SetConnectionHandler(
        [&handoff](std::shared_ptr<ServerCore::Net::Connection> connection)
        { handoff.Handle(std::move(connection)); });

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 21);
    const ServerCore::Core::Status listening = acceptor.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(listening.IsOk(),
        listening.IsOk() ? "Acceptor::Listen() for long handoff drain succeeded"
                         : ("Acceptor::Listen() for long handoff drain succeeded, message: " +
                               listening.Message()));
    if (!listening.IsOk())
    {
        io.Stop();
        return;
    }

    const ServerCore::Core::Status accepting = acceptor.Start(io);
    ServerCoreTest::ExpectTrue(accepting.IsOk(),
        accepting.IsOk() ? "Acceptor::Start() for long handoff drain succeeded"
                         : ("Acceptor::Start() for long handoff drain succeeded, message: " +
                               accepting.Message()));
    if (!accepting.IsOk())
    {
        acceptor.Stop();
        io.Stop();
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the long handoff client connected");
    if (!connected)
    {
        handoff.Release();
        acceptor.Stop();
        io.Stop();
        return;
    }

    const bool handoffEntered = handoff.WaitForEntries(1, WaitLimit);
    ServerCoreTest::ExpectTrue(handoffEntered, "the long handoff reached the blocking handler");
    if (!handoffEntered)
    {
        handoff.Release();
        client.Close();
        acceptor.Stop();
        io.Stop();
        return;
    }

    std::mutex stopMutex;
    bool stopReturned = false;
    std::thread stopper(
        [&acceptor, &stopMutex, &stopReturned]
        {
            acceptor.Stop();
            {
                const std::lock_guard<std::mutex> guard(stopMutex);
                stopReturned = true;
            }
        });

    const bool portClosed = WaitForClosedPort(acceptor, WaitLimit);
    ServerCoreTest::ExpectTrue(
        portClosed, "Stop() closed the listening port before the long handoff completed");
    if (!portClosed)
    {
        handoff.Release();
        stopper.join();
        client.Close();
        io.Stop();
        return;
    }

    std::this_thread::sleep_for(FormerAcceptDrainTimeout + TimeoutOverrun);

    {
        const std::lock_guard<std::mutex> guard(stopMutex);
        ServerCoreTest::ExpectTrue(
            !stopReturned, "Acceptor::Stop() did not return while the long handoff was active");
    }

    handoff.Release();
    stopper.join();

    {
        const std::lock_guard<std::mutex> guard(stopMutex);
        ServerCoreTest::ExpectTrue(
            stopReturned, "Acceptor::Stop() returned after the long handoff completed");
    }

    client.Close();
    io.Stop();
}

const ServerCoreTest::CheckRegistration gIoContextStartsAndStops{
    "Transport.IoContextStartsAndStops", &IoContextStartsAndStops
};
const ServerCoreTest::CheckRegistration gSharedSendBudgetSerializesReservations{
    "Transport.SharedSendBudgetSerializesReservations", &SharedSendBudgetSerializesReservations
};
const ServerCoreTest::CheckRegistration gListenRejectsInvalidEndpoint{
    "Transport.ListenRejectsInvalidEndpoint", &ListenRejectsInvalidEndpoint
};
const ServerCoreTest::CheckRegistration gAcceptorCanListenAgainAfterStop{
    "Transport.AcceptorCanListenAgainAfterStop", &AcceptorCanListenAgainAfterStop
};
const ServerCoreTest::CheckRegistration gEchoRoundTrip{ "Transport.EchoRoundTrip", &EchoRoundTrip };
const ServerCoreTest::CheckRegistration gDisconnectIsNotifiedExactlyOnce{
    "Transport.DisconnectIsNotifiedExactlyOnce", &DisconnectIsNotifiedExactlyOnce
};
const ServerCoreTest::CheckRegistration gLargePayloadCrossesReceiveBoundaries{
    "Transport.LargePayloadCrossesReceiveBoundaries", &LargePayloadCrossesReceiveBoundaries
};
const ServerCoreTest::CheckRegistration gSendAfterCloseIsRefused{
    "Transport.SendAfterCloseIsRefused", &SendAfterCloseIsRefused
};
const ServerCoreTest::CheckRegistration gCloseDiscardsQueuedSendBytes{
    "Transport.CloseDiscardsQueuedSendBytes", &CloseDiscardsQueuedSendBytes
};
const ServerCoreTest::CheckRegistration gSendQueueLimitRejectsWholeOverflowAndRecovers{
    "Transport.SendQueueLimitRejectsWholeOverflowAndRecovers",
    &SendQueueLimitRejectsWholeOverflowAndRecovers
};
const ServerCoreTest::CheckRegistration gSharedSendBudgetRejectsAcrossConnectionsAndRecovers{
    "Transport.SharedSendBudgetRejectsAcrossConnectionsAndRecovers",
    &SharedSendBudgetRejectsAcrossConnectionsAndRecovers
};
const ServerCoreTest::CheckRegistration gCloseAfterSendDrainsQueuedBytes{
    "Transport.CloseAfterSendDrainsQueuedBytes", &CloseAfterSendDrainsQueuedBytes
};
const ServerCoreTest::CheckRegistration gCloseAfterSendOnEmptyConnectionCloses{
    "Transport.CloseAfterSendOnEmptyConnectionCloses", &CloseAfterSendOnEmptyConnectionCloses
};
const ServerCoreTest::CheckRegistration gConcurrentSendsDoNotInterleave{
    "Transport.ConcurrentSendsDoNotInterleave", &ConcurrentSendsDoNotInterleave
};
const ServerCoreTest::CheckRegistration gMultiWorkerDeliversDifferentConnectionsConcurrently{
    "Transport.MultiWorkerDeliversDifferentConnectionsConcurrently",
    &MultiWorkerDeliversDifferentConnectionsConcurrently
};
const ServerCoreTest::CheckRegistration gDisconnectWaitsForReceiveCallback{
    "Transport.DisconnectWaitsForReceiveCallback", &DisconnectWaitsForReceiveCallback
};
const ServerCoreTest::CheckRegistration gLateObserverReceivesPriorDisconnect{
    "Transport.LateObserverReceivesPriorDisconnect", &LateObserverReceivesPriorDisconnect
};
const ServerCoreTest::CheckRegistration gAcceptorStopWaitsForActiveCompletionHandoff{
    "Transport.AcceptorStopWaitsForActiveCompletionHandoff",
    &AcceptorStopWaitsForActiveCompletionHandoff
};
const ServerCoreTest::CheckRegistration gAcceptorStopDoesNotTimeOutActiveCompletionHandoff{
    "Transport.AcceptorStopDoesNotTimeOutActiveCompletionHandoff",
    &AcceptorStopDoesNotTimeOutActiveCompletionHandoff
};
}
