#include "ConfigTestSupport.h"

#include "TestHarness.h"

#include "ServerCore/Core/Config.h"
#include "ServerCore/Core/Error.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Protocol/Json.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Runtime/ServerHost.h"
#include "ServerCore/Session/Session.h"

#include "Runtime/ServerHostTestAccess.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <WinSock2.h>

#include <WS2tcpip.h>

// 이 파일은 ServerHost가 각 층을 실제로 조립하는 경로를 검사한다. 전송 층의 단위 검사와
// 달리, 여기서는 평범한 TCP 클라이언트가 프레임을 조각 내어 보내고 Host가 세션·디스패치·종료를
// 순서대로 잇는지만 본다. 처리기와 관찰자는 특정 게임의 개념을 넣지 않은 범용 경계다.

#ifndef SERVERCORE_TEST_PORT_BASE
#error "SERVERCORE_TEST_PORT_BASE must be defined by the build"
#endif

namespace
{
static_assert(
    std::is_same_v<decltype(std::declval<const ServerCore::Runtime::ServerHost&>().GetSessions()),
        const ServerCore::Session::SessionRegistry&>);

constexpr std::uint16_t PortBase = static_cast<std::uint16_t>(SERVERCORE_TEST_PORT_BASE);
constexpr std::chrono::milliseconds WaitLimit{ 10000 };
constexpr DWORD ClientReceiveTimeoutMilliseconds = 10000;

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

    [[nodiscard]] bool IsReady() const noexcept { return mStartupResult == 0; }

private:
    int mStartupResult = 0;
};

/// <summary>테스트가 서버 바깥에서 프레임 하나를 만드는 작은 독립 구현이다.</summary>
std::vector<std::byte> MakeFrame(const std::string_view json)
{
    const std::uint32_t length = static_cast<std::uint32_t>(json.size());
    std::vector<std::byte> frame(ServerCore::Protocol::HeaderSize + json.size());
    frame[0] = static_cast<std::byte>(length & 0xFFu);
    frame[1] = static_cast<std::byte>((length >> 8u) & 0xFFu);
    frame[2] = static_cast<std::byte>((length >> 16u) & 0xFFu);
    frame[3] = static_cast<std::byte>((length >> 24u) & 0xFFu);
    std::memcpy(frame.data() + ServerCore::Protocol::HeaderSize, json.data(), json.size());
    return frame;
}

/// <summary>우리 구현을 쓰지 않는, 검사용 블로킹 TCP 클라이언트다.</summary>
class TestClient
{
public:
    ~TestClient() { Close(); }

    TestClient() = default;
    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    [[nodiscard]] bool Connect(const std::uint16_t port, const int receiveBufferBytes = 0)
    {
        mSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (mSocket == INVALID_SOCKET)
        {
            return false;
        }

        if (receiveBufferBytes > 0 &&
            ::setsockopt(mSocket, SOL_SOCKET, SO_RCVBUF,
                reinterpret_cast<const char*>(&receiveBufferBytes), sizeof(receiveBufferBytes)) ==
                SOCKET_ERROR)
        {
            Close();
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
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
        {
            Close();
            return false;
        }

        if (::connect(mSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
            SOCKET_ERROR)
        {
            Close();
            return false;
        }

        return true;
    }

    [[nodiscard]] bool SendAll(const std::span<const std::byte> bytes)
    {
        if (mSocket == INVALID_SOCKET)
        {
            return false;
        }

        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const int sent = ::send(mSocket, reinterpret_cast<const char*>(bytes.data() + offset),
                static_cast<int>(bytes.size() - offset), 0);
            if (sent <= 0)
            {
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    /// <summary>정확히 지정한 수만큼 받을 때까지 읽는다.</summary>
    /// <returns>다 받으면 참. 상대가 먼저 닫거나 제한을 넘기면 거짓.</returns>
    [[nodiscard]] bool ReceiveExactly(const std::size_t count, std::vector<std::byte>& received)
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

    /// <summary>머리를 독립적으로 해석해 프레임 본문 하나를 받는다.</summary>
    [[nodiscard]] bool ReceiveFrame(std::vector<std::byte>& body)
    {
        std::vector<std::byte> header;
        if (!ReceiveExactly(ServerCore::Protocol::HeaderSize, header))
        {
            return false;
        }

        const std::uint32_t bodySize = std::to_integer<std::uint32_t>(header[0]) |
                                       (std::to_integer<std::uint32_t>(header[1]) << 8u) |
                                       (std::to_integer<std::uint32_t>(header[2]) << 16u) |
                                       (std::to_integer<std::uint32_t>(header[3]) << 24u);
        if (bodySize > ServerCore::Protocol::DefaultMaxBodySize)
        {
            return false;
        }

        return ReceiveExactly(static_cast<std::size_t>(bodySize), body);
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

/// <summary>Host가 세션 수명 훅을 실행했는지 안전하게 관찰한다.</summary>
class RecordingSessionObserver final : public ServerCore::Session::ISessionObserver
{
public:
    void OnSessionOpened(const std::shared_ptr<ServerCore::Session::Session>& session) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mOpenedCount;
            mOpenedId = session->Id();
        }
        mChanged.notify_all();
    }

    void OnSessionAuthenticated(
        const std::shared_ptr<ServerCore::Session::Session>& session) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mAuthenticatedCount;
            mAuthenticatedId = session->Id();
        }
        mChanged.notify_all();
    }

    void OnSessionClosed(
        const ServerCore::Session::SessionId id, ServerCore::Core::Status reason) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mClosedCount;
            mClosedId = id;
            mCloseCode = reason.Code();
        }
        mChanged.notify_all();
    }

    [[nodiscard]] bool WaitForOpened(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mOpenedCount != 0; });
    }

    [[nodiscard]] bool WaitForOpenedCount(
        const std::size_t count, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this, count] { return mOpenedCount >= count; });
    }

    [[nodiscard]] bool WaitForAuthenticated(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mAuthenticatedCount != 0; });
    }

    [[nodiscard]] bool WaitForClosed(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mClosedCount != 0; });
    }

    [[nodiscard]] std::size_t OpenedCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mOpenedCount;
    }

    [[nodiscard]] std::size_t AuthenticatedCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mAuthenticatedCount;
    }

    [[nodiscard]] std::size_t ClosedCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mClosedCount;
    }

    [[nodiscard]] ServerCore::Session::SessionId OpenedId() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mOpenedId;
    }

    [[nodiscard]] ServerCore::Session::SessionId AuthenticatedId() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mAuthenticatedId;
    }

    [[nodiscard]] ServerCore::Session::SessionId ClosedId() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mClosedId;
    }

    [[nodiscard]] ServerCore::Core::ErrorCode CloseCode() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mCloseCode;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mOpenedCount = 0;
    std::size_t mAuthenticatedCount = 0;
    std::size_t mClosedCount = 0;
    ServerCore::Session::SessionId mOpenedId = ServerCore::Session::SessionId::Invalid;
    ServerCore::Session::SessionId mAuthenticatedId = ServerCore::Session::SessionId::Invalid;
    ServerCore::Session::SessionId mClosedId = ServerCore::Session::SessionId::Invalid;
    ServerCore::Core::ErrorCode mCloseCode = ServerCore::Core::ErrorCode::Ok;
};

/// <summary>닫힌 뒤에도 Session shared_ptr를 보관하는 게임 백엔드의 합법적인 수명을 재현한다.</summary>
class RetainingSessionObserver final : public ServerCore::Session::ISessionObserver
{
public:
    void OnSessionOpened(const std::shared_ptr<ServerCore::Session::Session>& session) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mSessions.push_back(session);
        }
        mChanged.notify_all();
    }

    void OnSessionClosed(ServerCore::Session::SessionId, ServerCore::Core::Status) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mClosedCount;
        }
        mChanged.notify_all();
    }

    [[nodiscard]] bool WaitForOpenedCount(
        const std::size_t count, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this, count] { return mSessions.size() >= count; });
    }

    [[nodiscard]] bool WaitForClosed(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mClosedCount != 0; });
    }

    [[nodiscard]] std::shared_ptr<ServerCore::Session::Session> SessionAt(
        const std::size_t index) const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return index < mSessions.size() ? mSessions[index] : nullptr;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::vector<std::shared_ptr<ServerCore::Session::Session>> mSessions;
    std::size_t mClosedCount = 0;
};

/// <summary>OnSessionClosed 안에서 다른 스레드가 같은 Session을 다시 닫는 호출을 재현한다.</summary>
class ReentrantCloseObserver final : public ServerCore::Session::ISessionObserver
{
public:
    ReentrantCloseObserver()
        : mWorker([this]() { RunWorker(); })
    {
    }

    ~ReentrantCloseObserver() override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mStopWorker = true;
        }
        mChanged.notify_all();
        mWorker.join();
    }

    ReentrantCloseObserver(const ReentrantCloseObserver&) = delete;
    ReentrantCloseObserver& operator=(const ReentrantCloseObserver&) = delete;

    void OnSessionOpened(const std::shared_ptr<ServerCore::Session::Session>& session) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mSession = session;
        }
        mChanged.notify_all();
    }

    void OnSessionClosed(ServerCore::Session::SessionId, ServerCore::Core::Status) override
    {
        std::unique_lock<std::mutex> guard(mMutex);
        mReentryRequested = true;
        mChanged.notify_all();
        mReenteredBeforeCloseReturned =
            mChanged.wait_for(guard, std::chrono::seconds(2), [this] { return mReentryCompleted; });
        mClosed = true;
        guard.unlock();
        mChanged.notify_all();
    }

    [[nodiscard]] bool WaitForOpened(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mSession != nullptr; });
    }

    [[nodiscard]] bool WaitForClosed(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mClosed; });
    }

    [[nodiscard]] std::shared_ptr<ServerCore::Session::Session> Session() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mSession;
    }

    [[nodiscard]] bool ReenteredBeforeCloseReturned() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mReenteredBeforeCloseReturned;
    }

private:
    void RunWorker()
    {
        std::shared_ptr<ServerCore::Session::Session> session;
        {
            std::unique_lock<std::mutex> guard(mMutex);
            mChanged.wait(guard, [this] { return mStopWorker || mReentryRequested; });
            if (mStopWorker)
            {
                return;
            }
            session = mSession;
        }

        if (session != nullptr)
        {
            session->Disconnect(
                ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed));
        }

        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mReentryCompleted = true;
        }
        mChanged.notify_all();
    }

    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::thread mWorker;
    std::shared_ptr<ServerCore::Session::Session> mSession;
    bool mStopWorker = false;
    bool mReentryRequested = false;
    bool mReentryCompleted = false;
    bool mReenteredBeforeCloseReturned = false;
    bool mClosed = false;
};

/// <summary>처리기가 실제 JobRunner 문맥에서 호출됐는지를 관찰한다.</summary>
class RecordingHandler
{
public:
    [[nodiscard]] ServerCore::Core::Status Handle(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message)
    {
        const ServerCore::Protocol::JsonValue* const body = message.Body();
        const ServerCore::Protocol::JsonValue* const value =
            body == nullptr ? nullptr : body->Find("value");
        const std::string* const text = value == nullptr ? nullptr : value->TryString();
        const bool isExpected = text != nullptr && *text == "split";
        const ServerCore::Protocol::JsonValue* const sequence = message.Sequence();
        const ServerCore::Protocol::JsonValue* const request =
            sequence == nullptr ? nullptr : sequence->Find("request");
        const std::string* const requestText = request == nullptr ? nullptr : request->TryString();
        const bool sequenceIsExpected = requestText != nullptr && *requestText == "split-request";

        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mCallCount;
            mSessionId = session->Id();
            mBodyWasExpected = isExpected;
            mSequenceWasExpected = sequenceIsExpected;
        }
        mChanged.notify_all();

        if (!isExpected)
        {
            return ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::InvalidFormat,
                "the test handler received an unexpected body");
        }

        // 신원 정책은 여기서 만들지 않는다. 다만 Host가 제공하는 범용 상태 전이가 처리기 문맥에서
        // 가능한지는 확인한다. 관찰자가 그 전이를 보게 되는 것도 세션 수명 경계의 일부다.
        return session->MarkAuthenticated();
    }

    [[nodiscard]] bool WaitForCall(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mCallCount != 0; });
    }

    [[nodiscard]] std::size_t CallCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mCallCount;
    }

    [[nodiscard]] bool BodyWasExpected() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mBodyWasExpected;
    }

    [[nodiscard]] bool SequenceWasExpected() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mSequenceWasExpected;
    }

    [[nodiscard]] ServerCore::Session::SessionId SessionId() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mSessionId;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mCallCount = 0;
    bool mBodyWasExpected = false;
    bool mSequenceWasExpected = false;
    ServerCore::Session::SessionId mSessionId = ServerCore::Session::SessionId::Invalid;
};

/// <summary>느린 게임 처리를 흉내 내어 Host 수신 역압이 실제로 걸리는지 본다.</summary>
class BlockingReceiveHandler
{
public:
    [[nodiscard]] ServerCore::Core::Status Handle(
        const std::shared_ptr<ServerCore::Session::Session>&,
        const ServerCore::Protocol::JsonValue&)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        ++mCallCount;
        mEntered = true;
        mChanged.notify_all();
        mChanged.wait(guard, [this] { return mReleased; });
        return ServerCore::Core::Status::Ok();
    }

    [[nodiscard]] bool WaitUntilEntered(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mEntered; });
    }

    [[nodiscard]] bool WaitForCallCount(
        const std::size_t count, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this, count] { return mCallCount >= count; });
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
    std::size_t mCallCount = 0;
    bool mEntered = false;
    bool mReleased = false;
};

/// <summary>송신 프레임 계수와 수신 계수를 함께 보는 간단한 응답 처리기다.</summary>
class MetricsResponseHandler
{
public:
    [[nodiscard]] ServerCore::Core::Status Handle(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::JsonValue& body)
    {
        const ServerCore::Core::Status sent = session->Send("runtime.metrics.reply", body);
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mCallCount;
        }
        mChanged.notify_all();
        return sent;
    }

    [[nodiscard]] bool WaitForCall(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mCallCount != 0; });
    }

private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mCallCount = 0;
};

/// <summary>마지막 error 봉투를 남기고 세션을 닫는 처리기다.</summary>
class FinalResponseHandler
{
public:
    [[nodiscard]] ServerCore::Core::Status Handle(
        const std::shared_ptr<ServerCore::Session::Session>& session,
        const ServerCore::Protocol::Message& message)
    {
        const ServerCore::Protocol::JsonValue* const body = message.Body();
        if (body == nullptr)
        {
            return ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::InvalidFormat,
                "the final-response request had no body");
        }

        ServerCore::Protocol::JsonValue::Object errorFields;
        errorFields.emplace("code", ServerCore::Protocol::JsonValue(std::string("Rejected")));
        errorFields.emplace("message",
            ServerCore::Protocol::JsonValue(std::string("the test rejected the request")));
        const ServerCore::Protocol::JsonValue error(std::move(errorFields));
        const ServerCore::Core::Status final =
            session->SendAndDisconnect(ServerCore::Protocol::MessageFields{ "runtime.final.reply",
                                           nullptr, message.Sequence(), &error },
                ServerCore::Core::Status::Fail(
                    ServerCore::Core::ErrorCode::InvalidArgument, "the test rejected the request"));

        // 마지막 봉투 뒤의 평범한 Send는 수락되지 않아야 한다. 이 실패는 처리기 자체의 실패가
        // 아니므로 Dispatcher에 돌려주지 않고 검사에서 따로 본다.
        const ServerCore::Core::Status afterFinal = session->Send("runtime.final.after", *body);
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mCallCount;
            mFinalCode = final.Code();
            mAfterFinalCode = afterFinal.Code();
        }
        mChanged.notify_all();
        return final;
    }

    [[nodiscard]] bool WaitForCall(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mCallCount != 0; });
    }

    [[nodiscard]] ServerCore::Core::ErrorCode FinalCode() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mFinalCode;
    }

    [[nodiscard]] ServerCore::Core::ErrorCode AfterFinalCode() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mAfterFinalCode;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mCallCount = 0;
    ServerCore::Core::ErrorCode mFinalCode = ServerCore::Core::ErrorCode::Ok;
    ServerCore::Core::ErrorCode mAfterFinalCode = ServerCore::Core::ErrorCode::Ok;
};

/// <summary>
/// 파싱 worker 경로가 결과 봉투를 JobRunner로 돌려보내고 같은 세션의 처리 순서를 지키는지 본다.
/// </summary>
/// <remarks>
/// 첫 처리기 안에서 지표를 읽고 잠시 멈춘다. ParseReservation은 처리기 반환 전까지 살아 있으므로,
/// 이 시점의 pending parse 지표는 TCP 수신 분할 방식과 무관하게 적어도 현재 프레임 하나를 보여야
/// 한다.
/// </remarks>
class BlockingOrderedParseHandler
{
public:
    explicit BlockingOrderedParseHandler(ServerCore::Runtime::ServerHost& host) noexcept
        : mHost(host)
    {
    }

    [[nodiscard]] ServerCore::Core::Status Handle(
        const std::shared_ptr<ServerCore::Session::Session>&,
        const ServerCore::Protocol::Message& message)
    {
        const ServerCore::Protocol::JsonValue* const body = message.Body();
        const ServerCore::Protocol::JsonValue* const sequence = message.Sequence();
        const std::string* const text = sequence == nullptr ? nullptr : sequence->TryString();
        const bool bodyIsExpected = body != nullptr && body->IsObject();
        bool firstCall = false;
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mCallCount;
            firstCall = mCallCount == 1;
            if (!bodyIsExpected || text == nullptr)
            {
                mMessagesWereValid = false;
            }
            else
            {
                mSequences.emplace_back(*text);
            }
        }

        if (firstCall)
        {
            ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> snapshot =
                mHost.SnapshotMetrics();
            {
                const std::lock_guard<std::mutex> guard(mMutex);
                mFirstSnapshot.emplace(std::move(snapshot));
            }
            mChanged.notify_all();

            std::unique_lock<std::mutex> guard(mMutex);
            mChanged.wait(guard, [this] { return mReleased; });
        }
        else
        {
            mChanged.notify_all();
        }

        if (!bodyIsExpected || text == nullptr)
        {
            return ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::InvalidFormat,
                "the ordered parse handler received an unexpected envelope");
        }
        return ServerCore::Core::Status::Ok();
    }

    [[nodiscard]] bool WaitUntilFirstSnapshot(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mFirstSnapshot.has_value(); });
    }

    [[nodiscard]] bool WaitForCallCount(
        const std::size_t count, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this, count] { return mCallCount >= count; });
    }

    void Release()
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mReleased = true;
        }
        mChanged.notify_all();
    }

    [[nodiscard]] ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot>
    FirstSnapshot() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        if (!mFirstSnapshot.has_value())
        {
            return ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot>::FromStatus(
                ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::PlatformError,
                    "the ordered parse handler did not capture its first metrics snapshot"));
        }
        return *mFirstSnapshot;
    }

    [[nodiscard]] bool HasExpectedOrder(const std::size_t count) const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        if (!mMessagesWereValid || mSequences.size() != count)
        {
            return false;
        }

        for (std::size_t index = 0; index < count; ++index)
        {
            if (mSequences[index] != std::to_string(index))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t CallCount() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mCallCount;
    }

private:
    ServerCore::Runtime::ServerHost& mHost;
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mCallCount = 0;
    bool mMessagesWereValid = true;
    bool mReleased = false;
    std::vector<std::string> mSequences;
    std::optional<ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot>>
        mFirstSnapshot;
};

/// <summary>parse worker 완료가 실제 Dispatcher까지 도달했는지 횟수로 관찰한다.</summary>
class CountingParseHandler
{
public:
    [[nodiscard]] ServerCore::Core::Status Handle(
        const std::shared_ptr<ServerCore::Session::Session>&,
        const ServerCore::Protocol::Message& message)
    {
        const ServerCore::Protocol::JsonValue* const body = message.Body();
        const bool bodyIsExpected = body != nullptr && body->IsObject();
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            ++mCallCount;
            mBodiesWereExpected = mBodiesWereExpected && bodyIsExpected;
        }
        mChanged.notify_all();

        if (!bodyIsExpected)
        {
            return ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::InvalidFormat,
                "the parse budget handler received an unexpected envelope");
        }
        return ServerCore::Core::Status::Ok();
    }

    [[nodiscard]] bool WaitForCallCount(
        const std::size_t count, const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this, count] { return mCallCount >= count; });
    }

    [[nodiscard]] bool BodiesWereExpected() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mBodiesWereExpected;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::size_t mCallCount = 0;
    bool mBodiesWereExpected = true;
};

/// <summary>예약을 잡은 parse worker를 JSON 해석 직전에 멈추는 시험 gate다.</summary>
class BlockingParseWorkerGate final : public ServerCore::Runtime::TestAccess::IBeforeParseGate
{
public:
    void WaitBeforeParse() noexcept override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mEntered = true;
        }
        mChanged.notify_all();

        std::unique_lock<std::mutex> guard(mMutex);
        mChanged.wait(guard, [this] { return mReleased; });
    }

    [[nodiscard]] bool WaitUntilEntered(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mEntered; });
    }

    void Release() noexcept
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
    bool mEntered = false;
    bool mReleased = false;
};

/// <summary>gate를 지우고 반드시 먼저 깨워 Host::Stop()의 worker join 교착을 막는다.</summary>
class ScopedBeforeParseGate final
{
public:
    explicit ScopedBeforeParseGate(std::shared_ptr<BlockingParseWorkerGate> gate)
        : mGate(std::move(gate))
    {
        ServerCore::Runtime::TestAccess::InstallBeforeParseGate(mGate);
    }

    ~ScopedBeforeParseGate()
    {
        Release();
        ServerCore::Runtime::TestAccess::ClearBeforeParseGate(mGate);
    }

    ScopedBeforeParseGate(const ScopedBeforeParseGate&) = delete;
    ScopedBeforeParseGate& operator=(const ScopedBeforeParseGate&) = delete;

    void Release() const noexcept { mGate->Release(); }

private:
    std::shared_ptr<BlockingParseWorkerGate> mGate;
};

/// <summary>NetworkSession 수신 callback 안에서 I/O worker를 결정론적으로 멈춘다.</summary>
class BlockingSessionReceiveGate final
    : public ServerCore::Runtime::TestAccess::IBeforeSessionReceiveGate
{
public:
    void WaitBeforeSessionReceive() noexcept override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mEntered = true;
        }
        mChanged.notify_all();

        std::unique_lock<std::mutex> guard(mMutex);
        mChanged.wait(guard, [this] { return mReleased; });
    }

    [[nodiscard]] bool WaitUntilEntered(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mEntered; });
    }

    void Release() noexcept
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
    bool mEntered = false;
    bool mReleased = false;
};

class ScopedBeforeSessionReceiveGate final
{
public:
    explicit ScopedBeforeSessionReceiveGate(std::shared_ptr<BlockingSessionReceiveGate> gate)
        : mGate(std::move(gate))
    {
        ServerCore::Runtime::TestAccess::InstallBeforeSessionReceiveGate(mGate);
    }

    ~ScopedBeforeSessionReceiveGate()
    {
        Release();
        ServerCore::Runtime::TestAccess::ClearBeforeSessionReceiveGate(mGate);
    }

    ScopedBeforeSessionReceiveGate(const ScopedBeforeSessionReceiveGate&) = delete;
    ScopedBeforeSessionReceiveGate& operator=(const ScopedBeforeSessionReceiveGate&) = delete;

    void Release() const noexcept { mGate->Release(); }

private:
    std::shared_ptr<BlockingSessionReceiveGate> mGate;
};

/// <summary>수락된 Connection에 첫 overlapped receive가 걸리기 직전을 고정한다.</summary>
class BlockingConnectionStartGate final
    : public ServerCore::Runtime::TestAccess::IBeforeConnectionStartGate
{
public:
    void WaitBeforeConnectionStart() noexcept override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mEntered = true;
        }
        mChanged.notify_all();

        std::unique_lock<std::mutex> guard(mMutex);
        mChanged.wait(guard, [this] { return mReleased; });
    }

    [[nodiscard]] bool WaitUntilEntered(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mEntered; });
    }

    void Release() noexcept
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
    bool mEntered = false;
    bool mReleased = false;
};

class ScopedBeforeConnectionStartGate final
{
public:
    explicit ScopedBeforeConnectionStartGate(std::shared_ptr<BlockingConnectionStartGate> gate)
        : mGate(std::move(gate))
    {
        ServerCore::Runtime::TestAccess::InstallBeforeConnectionStartGate(mGate);
    }

    ~ScopedBeforeConnectionStartGate()
    {
        Release();
        ServerCore::Runtime::TestAccess::ClearBeforeConnectionStartGate(mGate);
    }

    ScopedBeforeConnectionStartGate(const ScopedBeforeConnectionStartGate&) = delete;
    ScopedBeforeConnectionStartGate& operator=(const ScopedBeforeConnectionStartGate&) = delete;

    void Release() const noexcept { mGate->Release(); }

private:
    std::shared_ptr<BlockingConnectionStartGate> mGate;
};

/// <summary>수락기가 시작됐지만 Host Lifecycle이 아직 Starting인 경계를 고정한다.</summary>
class BlockingHostRunningGate final : public ServerCore::Runtime::TestAccess::IBeforeHostRunningGate
{
public:
    void WaitBeforeHostRunning() noexcept override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mEntered = true;
        }
        mChanged.notify_all();

        std::unique_lock<std::mutex> guard(mMutex);
        mChanged.wait(guard, [this] { return mReleased; });
    }

    [[nodiscard]] bool WaitUntilEntered(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mEntered; });
    }

    void Release() noexcept
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
    bool mEntered = false;
    bool mReleased = false;
};

class ScopedBeforeHostRunningGate final
{
public:
    explicit ScopedBeforeHostRunningGate(std::shared_ptr<BlockingHostRunningGate> gate)
        : mGate(std::move(gate))
    {
        ServerCore::Runtime::TestAccess::InstallBeforeHostRunningGate(mGate);
    }

    ~ScopedBeforeHostRunningGate()
    {
        Release();
        ServerCore::Runtime::TestAccess::ClearBeforeHostRunningGate(mGate);
    }

    ScopedBeforeHostRunningGate(const ScopedBeforeHostRunningGate&) = delete;
    ScopedBeforeHostRunningGate& operator=(const ScopedBeforeHostRunningGate&) = delete;

    void Release() const noexcept { mGate->Release(); }

private:
    std::shared_ptr<BlockingHostRunningGate> mGate;
};

/// <summary>실패한 Start의 파기를 붙잡고 다른 Stop의 소유자 접근 여부를 관찰한다.</summary>
class FailedStartOwnerGate final : public ServerCore::Runtime::TestAccess::IFailedStartOwnerGate
{
public:
    void WaitBeforeFailedStartOwnerReset() noexcept override
    {
        std::unique_lock<std::mutex> guard(mMutex);
        mResetEntered = true;
        mChanged.notify_all();
        mChanged.wait(guard, [this] { return mReleased; });
    }

    void OnStopOwnerRead() noexcept override
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mStopOwnerRead = true;
        mChanged.notify_all();
    }

    void MarkStopCallerStarted()
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mStopCallerStarted = true;
        mChanged.notify_all();
    }

    [[nodiscard]] bool WaitForReset(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mResetEntered; });
    }

    [[nodiscard]] bool WaitForStopCaller(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mStopCallerStarted; });
    }

    [[nodiscard]] bool WaitForStopOwnerRead(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mStopOwnerRead; });
    }

    void Release()
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mReleased = true;
        mChanged.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    bool mResetEntered = false;
    bool mStopCallerStarted = false;
    bool mStopOwnerRead = false;
    bool mReleased = false;
};

/// <summary>fallback OnSessionClosed에서 Connection 시작 문을 열고 Host를 동기 종료한다.</summary>
class StopFromCloseObserver final : public ServerCore::Session::ISessionObserver
{
public:
    StopFromCloseObserver(ServerCore::Runtime::ServerHost& host,
        std::shared_ptr<BlockingConnectionStartGate> connectionStartGate)
        : mHost(host)
        , mConnectionStartGate(std::move(connectionStartGate))
    {
    }

    void OnSessionOpened(const std::shared_ptr<ServerCore::Session::Session>& session) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mSession = session;
        }
        mChanged.notify_all();
    }

    void OnSessionClosed(ServerCore::Session::SessionId, ServerCore::Core::Status) override
    {
        // Acceptor::Stop()은 이 callback을 시작한 수락 handler가 돌아오기를 기다린다. handler가
        // Connection::Start()로 넘어가도록 먼저 문을 열면, 이미 닫힌 Connection은 새 I/O를 만들지
        // 않고 handoff 계수만 안전하게 반납한다.
        mConnectionStartGate->Release();
        mHost.Stop();

        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mStopReturned = true;
        }
        mChanged.notify_all();
    }

    [[nodiscard]] std::shared_ptr<ServerCore::Session::Session> WaitForSession(
        const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        (void)mChanged.wait_for(guard, limit, [this] { return mSession != nullptr; });
        return mSession;
    }

    [[nodiscard]] bool StopReturned() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mStopReturned;
    }

    [[nodiscard]] bool WaitForStopReturned(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mStopReturned; });
    }

private:
    ServerCore::Runtime::ServerHost& mHost;
    std::shared_ptr<BlockingConnectionStartGate> mConnectionStartGate;
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::shared_ptr<ServerCore::Session::Session> mSession;
    bool mStopReturned = false;
};

/// <summary>OnSessionClosed를 붙잡아 외부 Stop의 callback drain 장벽을 관찰한다.</summary>
class BlockingCloseObserver final : public ServerCore::Session::ISessionObserver
{
public:
    void OnSessionOpened(const std::shared_ptr<ServerCore::Session::Session>&) override
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mOpened = true;
        }
        mChanged.notify_all();
    }

    void OnSessionClosed(ServerCore::Session::SessionId, ServerCore::Core::Status) override
    {
        std::unique_lock<std::mutex> guard(mMutex);
        mCloseEntered = true;
        mChanged.notify_all();
        mChanged.wait(guard, [this] { return mReleased; });
        mCloseReturned = true;
        guard.unlock();
        mChanged.notify_all();
    }

    [[nodiscard]] bool WaitForOpened(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mOpened; });
    }

    [[nodiscard]] bool WaitForCloseEntered(const std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> guard(mMutex);
        return mChanged.wait_for(guard, limit, [this] { return mCloseEntered; });
    }

    void Release() noexcept
    {
        {
            const std::lock_guard<std::mutex> guard(mMutex);
            mReleased = true;
        }
        mChanged.notify_all();
    }

    [[nodiscard]] bool CloseReturned() const
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        return mCloseReturned;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    bool mOpened = false;
    bool mCloseEntered = false;
    bool mReleased = false;
    bool mCloseReturned = false;
};

class ScopedFinalizePostFailure final
{
public:
    ScopedFinalizePostFailure() noexcept
    {
        ServerCore::Runtime::TestAccess::FailNextFinalizePost();
    }

    ~ScopedFinalizePostFailure() { ServerCore::Runtime::TestAccess::ClearFinalizePostFailure(); }

    ScopedFinalizePostFailure(const ScopedFinalizePostFailure&) = delete;
    ScopedFinalizePostFailure& operator=(const ScopedFinalizePostFailure&) = delete;
};

/// <summary>외부 검사 스레드가 Host의 JobRunner 문맥에서 스냅숏을 한 번 가져오는 보조물이다.</summary>
struct MetricsCaptureState
{
    std::mutex mutex;
    std::condition_variable changed;
    bool completed = false;
    std::optional<ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot>> result;
};

[[nodiscard]] ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> CaptureMetrics(
    ServerCore::Runtime::ServerHost& host)
{
    const std::shared_ptr<MetricsCaptureState> state = std::make_shared<MetricsCaptureState>();
    const ServerCore::Core::Status posted = host.GetJobRunner().Post(
        [&host, state]()
        {
            ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> snapshot =
                host.SnapshotMetrics();
            {
                const std::lock_guard<std::mutex> guard(state->mutex);
                state->result.emplace(std::move(snapshot));
                state->completed = true;
            }
            state->changed.notify_all();
        });
    if (!posted.IsOk())
    {
        return ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot>::FromStatus(
            posted);
    }

    std::unique_lock<std::mutex> guard(state->mutex);
    if (!state->changed.wait_for(guard, WaitLimit, [state] { return state->completed; }))
    {
        return ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot>::FromStatus(
            ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::PlatformError,
                "timed out waiting for a ServerHost metrics snapshot"));
    }
    if (!state->result.has_value())
    {
        return ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot>::FromStatus(
            ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::PlatformError,
                "ServerHost metrics capture did not return a result"));
    }
    return std::move(*state->result);
}

[[nodiscard]] ServerCore::Core::Result<ServerCore::Core::Config> LoadConfigSnapshotForHost(
    const std::string_view text)
{
    const ServerCoreTest::ScopedConfigFile file(text);
    return ServerCore::Core::Config::LoadFromFile(file.Path());
}

void ServerHostRoutesSplitFramesAndStops()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() in the test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 6);
    auto observer = std::make_shared<RecordingSessionObserver>();
    auto handler = std::make_shared<RecordingHandler>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(),
        configured.IsOk()
            ? "ServerHost accepted valid options"
            : ("ServerHost accepted valid options, message: " + configured.Message()));
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register(
        "runtime.probe",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, message); },
        1024);
    ServerCoreTest::ExpectTrue(registered.IsOk(),
        registered.IsOk()
            ? "the generic handler registered before Start()"
            : ("the generic handler registered before Start(), message: " + registered.Message()));
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(
        started.IsOk(), started.IsOk() ? "ServerHost started"
                                       : ("ServerHost started, message: " + started.Message()));
    if (!started.IsOk())
    {
        return;
    }

    ServerCoreTest::ExpectTrue(host.IsRunning(), "ServerHost reports running after Start()");
    ServerCoreTest::ExpectEqual(port, host.Port(), "ServerHost reports its configured port");
    ServerCoreTest::ExpectTrue(host.GetDispatcher().IsFrozen(),
        "ServerHost froze dispatcher registration before accepting connections");

    const ServerCore::Core::Status lateRegistration = host.GetDispatcher().Register("runtime.late",
        [](const std::shared_ptr<ServerCore::Session::Session>&,
            const ServerCore::Protocol::Message&) { return ServerCore::Core::Status::Ok(); });
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(lateRegistration.Code()),
        "Register() after ServerHost::Start() is rejected");

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the outside TCP client connected to ServerHost");
    if (!connected)
    {
        host.Stop();
        return;
    }

    const std::vector<std::byte> frame =
        MakeFrame("{\"type\":\"runtime.probe\",\"body\":{\"value\":\"split\"},"
                  "\"seq\":{\"request\":\"split-request\"}}");
    constexpr std::size_t FirstWriteSize = 2;
    const bool firstSent = client.SendAll(std::span<const std::byte>(frame.data(), FirstWriteSize));
    ServerCoreTest::ExpectTrue(firstSent, "the client sent the first partial frame write");
    const bool secondSent =
        firstSent && client.SendAll(std::span<const std::byte>(
                         frame.data() + FirstWriteSize, frame.size() - FirstWriteSize));
    ServerCoreTest::ExpectTrue(secondSent, "the client sent the remaining partial frame write");
    if (!secondSent)
    {
        host.Stop();
        return;
    }

    const bool opened = observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the ServerHost session-open observer ran");
    if (!opened)
    {
        host.Stop();
        return;
    }

    const bool handled = handler->WaitForCall(WaitLimit);
    ServerCoreTest::ExpectTrue(handled, "the registered generic handler ran after split input");
    if (!handled)
    {
        host.Stop();
        return;
    }

    const bool authenticated = observer->WaitForAuthenticated(WaitLimit);
    ServerCoreTest::ExpectTrue(
        authenticated, "the ServerHost authentication transition observer ran");
    if (!authenticated)
    {
        host.Stop();
        return;
    }

    ServerCoreTest::ExpectTrue(
        handler->BodyWasExpected(), "the handler received the complete JSON body");
    ServerCoreTest::ExpectTrue(
        handler->SequenceWasExpected(), "the handler received the complete envelope sequence");
    ServerCoreTest::ExpectEqual(
        static_cast<std::size_t>(1), handler->CallCount(), "the split frame ran its handler once");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->OpenedCount(),
        "the observer saw one opened session");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->AuthenticatedCount(),
        "the observer saw one authenticated session");
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(observer->OpenedId()),
        static_cast<std::uint64_t>(handler->SessionId()), "opened and handled session ids match");
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(observer->OpenedId()),
        static_cast<std::uint64_t>(observer->AuthenticatedId()),
        "opened and authenticated session ids match");

    host.Stop();
    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(closed, "Stop() closed the live Host session");
    ServerCoreTest::ExpectTrue(!host.IsRunning(), "ServerHost reports stopped after Stop()");
    ServerCoreTest::ExpectEqual(static_cast<std::uint16_t>(0), host.Port(),
        "ServerHost no longer reports a listening port after Stop()");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->ClosedCount(),
        "Stop() cleaned up the one observed session exactly once");
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(observer->OpenedId()),
        static_cast<std::uint64_t>(observer->ClosedId()), "opened and closed session ids match");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(observer->CloseCode()), "Stop() supplies a closed-session reason");

    client.Close();
    host.Stop();
}

/// <summary>한 번 정상 종료한 Host는 다시 부팅하지 않는 lifecycle 경계를 고정한다.</summary>
/// <remarks>
/// 실행 중 중복 Start()는 이미 도는 Host를 건드리지 않고 AlreadyExists를 돌려야 한다. Stop()까지
/// 끝난 뒤에는 dispatcher·registry·worker를 새로 조립하는 재시작을 지원하지 않으므로 Closed를
/// 돌려야 한다. 둘을 함께 확인해 호출자가 상태를 추측하지 않게 한다.
/// </remarks>
void ServerHostRejectsRestartAfterStop()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the Host lifecycle test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 20);
    ServerCore::Runtime::ServerHost host;
    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "ServerHost accepted lifecycle test options");
    if (!configured.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started for its lifecycle test");
    if (!started.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status duplicateStart = host.Start();
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::AlreadyExists),
        static_cast<int>(duplicateStart.Code()),
        "a running ServerHost rejects a duplicate Start()");
    ServerCoreTest::ExpectTrue(host.IsRunning(), "a duplicate Start() leaves ServerHost running");
    ServerCoreTest::ExpectEqual(
        port, host.Port(), "a duplicate Start() preserves the listening port");

    host.Stop();
    ServerCoreTest::ExpectTrue(
        !host.IsRunning(), "ServerHost reports stopped after its lifecycle Stop()");
    ServerCoreTest::ExpectEqual(
        std::uint16_t{ 0 }, host.Port(), "ServerHost clears its port after its lifecycle Stop()");

    const ServerCore::Core::Status restarted = host.Start();
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(restarted.Code()), "a stopped ServerHost rejects a restart");
    ServerCoreTest::ExpectTrue(!host.IsRunning(), "a rejected restart leaves ServerHost stopped");
    ServerCoreTest::ExpectEqual(
        std::uint16_t{ 0 }, host.Port(), "a rejected restart does not expose a listening port");

    const ServerCore::Core::Status reconfigured = host.Configure(options);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(reconfigured.Code()), "a stopped ServerHost rejects reconfiguration");
    host.Stop();
}

void ServerHostRejectsInvalidOptions()
{
    ServerCore::Runtime::ServerHost missingPortHost;
    ServerCore::Runtime::ServerHostOptions missingPortOptions;
    const ServerCore::Core::Status missingPort = missingPortHost.Configure(missingPortOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(missingPort.Code()), "ServerHost rejects an unconfigured port");

    ServerCore::Runtime::ServerHost projectSelectedPortHost;
    ServerCore::Runtime::ServerHostOptions projectSelectedPortOptions;
    projectSelectedPortOptions.port = 17100;
    const ServerCore::Core::Status projectSelectedPort =
        projectSelectedPortHost.Configure(projectSelectedPortOptions);
    ServerCoreTest::ExpectTrue(projectSelectedPort.IsOk(),
        projectSelectedPort.IsOk()
            ? "ServerHost leaves project port policy to its caller"
            : ("ServerHost leaves project port policy to its caller, message: " +
                  projectSelectedPort.Message()));

    ServerCore::Runtime::ServerHost host;
    ServerCore::Runtime::ServerHostOptions options;
    options.port = static_cast<std::uint16_t>(PortBase + 6);
    options.ioWorkerThreadCount = 0;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(configured.Code()), "ServerHost rejects a non-positive I/O worker count");
    ServerCoreTest::ExpectTrue(
        !host.IsRunning(), "a rejected configuration does not start ServerHost");
    ServerCoreTest::ExpectEqual(static_cast<std::uint16_t>(0), host.Port(),
        "a rejected configuration does not expose a port");

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(started.Code()), "Start() requires a successful ServerHost configuration");

    ServerCore::Runtime::ServerHost negativeIdleTimeoutHost;
    ServerCore::Runtime::ServerHostOptions negativeIdleTimeoutOptions;
    negativeIdleTimeoutOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    negativeIdleTimeoutOptions.idleSessionTimeout = std::chrono::milliseconds(-1);
    const ServerCore::Core::Status negativeIdleTimeout =
        negativeIdleTimeoutHost.Configure(negativeIdleTimeoutOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(negativeIdleTimeout.Code()),
        "ServerHost rejects a negative idle session timeout");

    for (const int milliseconds : { -1, 0 })
    {
        ServerCore::Runtime::ServerHost invalidGracefulTimeoutHost;
        ServerCore::Runtime::ServerHostOptions invalidGracefulTimeoutOptions;
        invalidGracefulTimeoutOptions.port = static_cast<std::uint16_t>(PortBase + 6);
        invalidGracefulTimeoutOptions.gracefulCloseTimeout = std::chrono::milliseconds(milliseconds);
        const auto invalidTimeout = invalidGracefulTimeoutHost.Configure(invalidGracefulTimeoutOptions);
        ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
            static_cast<int>(invalidTimeout.Code()),
            "ServerHost rejects disabled or negative graceful close timeouts");
    }

    ServerCore::Runtime::ServerHost excessiveIoWorkerHost;
    ServerCore::Runtime::ServerHostOptions excessiveIoWorkerOptions;
    excessiveIoWorkerOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    excessiveIoWorkerOptions.ioWorkerThreadCount =
        ServerCore::Runtime::MaximumServerHostWorkerThreadCount + 1;
    const ServerCore::Core::Status excessiveIoWorkers =
        excessiveIoWorkerHost.Configure(excessiveIoWorkerOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(excessiveIoWorkers.Code()),
        "ServerHost rejects an I/O worker count above its safety limit");

    ServerCore::Runtime::ServerHost excessiveParseWorkerHost;
    ServerCore::Runtime::ServerHostOptions excessiveParseWorkerOptions;
    excessiveParseWorkerOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    excessiveParseWorkerOptions.parseWorkerThreadCount =
        ServerCore::Runtime::MaximumServerHostWorkerThreadCount + 1;
    const ServerCore::Core::Status excessiveParseWorkers =
        excessiveParseWorkerHost.Configure(excessiveParseWorkerOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(excessiveParseWorkers.Code()),
        "ServerHost rejects a parse worker count above its safety limit");

    ServerCore::Runtime::ServerHost maximumWorkerHost;
    ServerCore::Runtime::ServerHostOptions maximumWorkerOptions;
    maximumWorkerOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    maximumWorkerOptions.ioWorkerThreadCount =
        ServerCore::Runtime::MaximumServerHostWorkerThreadCount;
    maximumWorkerOptions.parseWorkerThreadCount =
        ServerCore::Runtime::MaximumServerHostWorkerThreadCount;
    const ServerCore::Core::Status maximumWorkers =
        maximumWorkerHost.Configure(maximumWorkerOptions);
    ServerCoreTest::ExpectTrue(
        maximumWorkers.IsOk(), "ServerHost accepts worker counts exactly at its safety limit");

    ServerCore::Runtime::ServerHost missingSendCapacityHost;
    ServerCore::Runtime::ServerHostOptions missingSendCapacityOptions;
    missingSendCapacityOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    missingSendCapacityOptions.maxTotalSendQueueCapacityBytes = 0;
    const ServerCore::Core::Status missingSendCapacity =
        missingSendCapacityHost.Configure(missingSendCapacityOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(missingSendCapacity.Code()),
        "ServerHost rejects a zero aggregate send queue capacity");

    ServerCore::Runtime::ServerHost excessiveSendCapacityHost;
    ServerCore::Runtime::ServerHostOptions excessiveSendCapacityOptions;
    excessiveSendCapacityOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    excessiveSendCapacityOptions.maxTotalSendQueueCapacityBytes = 512u * 1024u * 1024u + 1u;
    const ServerCore::Core::Status excessiveSendCapacity =
        excessiveSendCapacityHost.Configure(excessiveSendCapacityOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(excessiveSendCapacity.Code()),
        "ServerHost rejects aggregate send queue capacity above its safety limit");

    ServerCore::Runtime::ServerHost undersizedSendCapacityHost;
    ServerCore::Runtime::ServerHostOptions undersizedSendCapacityOptions;
    undersizedSendCapacityOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    undersizedSendCapacityOptions.maxTotalSendQueueCapacityBytes =
        static_cast<std::uint32_t>(ServerCore::Net::SendQueueLimitBytes - 1);
    const ServerCore::Core::Status undersizedSendCapacity =
        undersizedSendCapacityHost.Configure(undersizedSendCapacityOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(undersizedSendCapacity.Code()),
        "ServerHost rejects aggregate send capacity below one session queue");

    ServerCore::Runtime::ServerHost minimumSendCapacityHost;
    ServerCore::Runtime::ServerHostOptions minimumSendCapacityOptions;
    minimumSendCapacityOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    minimumSendCapacityOptions.maxConcurrentSessions = 2;
    minimumSendCapacityOptions.maxTotalSendQueueCapacityBytes =
        static_cast<std::uint32_t>(ServerCore::Net::SendQueueLimitBytes);
    const ServerCore::Core::Status minimumSendCapacity =
        minimumSendCapacityHost.Configure(minimumSendCapacityOptions);
    ServerCoreTest::ExpectTrue(minimumSendCapacity.IsOk(),
        "a shared send budget may be smaller than all per-session queue capacities combined");

    ServerCore::Runtime::ServerHost legacySessionCountHost;
    ServerCore::Runtime::ServerHostOptions legacySessionCountOptions;
    legacySessionCountOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    legacySessionCountOptions.maxConcurrentSessions = 300;
    const ServerCore::Core::Status legacySessionCount =
        legacySessionCountHost.Configure(legacySessionCountOptions);
    ServerCoreTest::ExpectTrue(legacySessionCount.IsOk(),
        "the default shared send budget preserves an existing 300-session configuration");

    ServerCore::Runtime::ServerHost maximumSessionCountHost;
    ServerCore::Runtime::ServerHostOptions maximumSessionCountOptions;
    maximumSessionCountOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    maximumSessionCountOptions.maxConcurrentSessions = 65536;
    maximumSessionCountOptions.maxBodySize = 8192;
    const ServerCore::Core::Status maximumSessionCount =
        maximumSessionCountHost.Configure(maximumSessionCountOptions);
    ServerCoreTest::ExpectTrue(maximumSessionCount.IsOk(),
        "ServerHost accepts 65,536 sessions with 8 KiB bodies and the default shared send budget");

    ServerCore::Runtime::ServerHost excessiveSessionCountHost;
    ServerCore::Runtime::ServerHostOptions excessiveSessionCountOptions = maximumSessionCountOptions;
    excessiveSessionCountOptions.maxConcurrentSessions = 65537;
    const ServerCore::Core::Status excessiveSessionCount =
        excessiveSessionCountHost.Configure(excessiveSessionCountOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(excessiveSessionCount.Code()),
        "ServerHost rejects a session count above the 65,536 safety limit");

    ServerCore::Runtime::ServerHost maximumReaderStorageHost;
    ServerCore::Runtime::ServerHostOptions maximumReaderStorageOptions = maximumSessionCountOptions;
    maximumReaderStorageOptions.maxBodySize =
        static_cast<std::uint32_t>(16384u - ServerCore::Protocol::HeaderSize);
    const ServerCore::Core::Status maximumReaderStorage =
        maximumReaderStorageHost.Configure(maximumReaderStorageOptions);
    ServerCoreTest::ExpectTrue(maximumReaderStorage.IsOk(),
        "65,536 FrameReaders may occupy exactly 1 GiB including their length headers");

    ServerCore::Runtime::ServerHost headerOverflowHost;
    ++maximumReaderStorageOptions.maxBodySize;
    const ServerCore::Core::Status headerOverflow =
        headerOverflowHost.Configure(maximumReaderStorageOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(headerOverflow.Code()),
        "one extra body byte per session cannot bypass the aggregate FrameReader limit");

    ServerCore::Runtime::ServerHost maximumSendCapacityHost;
    ServerCore::Runtime::ServerHostOptions maximumSendCapacityOptions;
    maximumSendCapacityOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    maximumSendCapacityOptions.maxTotalSendQueueCapacityBytes = 512u * 1024u * 1024u;
    const ServerCore::Core::Status maximumSendCapacity =
        maximumSendCapacityHost.Configure(maximumSendCapacityOptions);
    ServerCoreTest::ExpectTrue(maximumSendCapacity.IsOk(),
        "ServerHost accepts aggregate send capacity exactly at its safety limit");

    ServerCore::Runtime::ServerHost oversizedFrameHost;
    ServerCore::Runtime::ServerHostOptions oversizedFrameOptions;
    oversizedFrameOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    oversizedFrameOptions.maxBodySize = static_cast<std::uint32_t>(
        ServerCore::Net::SendQueueLimitBytes - ServerCore::Protocol::HeaderSize + 1);
    const ServerCore::Core::Status oversizedFrame =
        oversizedFrameHost.Configure(oversizedFrameOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(oversizedFrame.Code()),
        "ServerHost rejects a body limit that cannot be sent through one Connection queue entry");

    ServerCore::Runtime::ServerHost oversizedReaderHost;
    ServerCore::Runtime::ServerHostOptions oversizedReaderOptions;
    oversizedReaderOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    oversizedReaderOptions.maxBodySize = static_cast<std::uint32_t>(
        ServerCore::Net::SendQueueLimitBytes - ServerCore::Protocol::HeaderSize);
    oversizedReaderOptions.maxConcurrentSessions = 1025;
    const ServerCore::Core::Status oversizedReader =
        oversizedReaderHost.Configure(oversizedReaderOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(oversizedReader.Code()),
        "ServerHost rejects an aggregate FrameReader allocation that exceeds its host safety "
        "limit");

    ServerCore::Runtime::ServerHost negativeParseWorkerHost;
    ServerCore::Runtime::ServerHostOptions negativeParseWorkerOptions;
    negativeParseWorkerOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    negativeParseWorkerOptions.parseWorkerThreadCount = -1;
    const ServerCore::Core::Status negativeParseWorker =
        negativeParseWorkerHost.Configure(negativeParseWorkerOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(negativeParseWorker.Code()),
        "ServerHost rejects a negative parse worker count");

    ServerCore::Runtime::ServerHost undersizedParseBudgetHost;
    ServerCore::Runtime::ServerHostOptions undersizedParseBudgetOptions;
    undersizedParseBudgetOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    undersizedParseBudgetOptions.parseWorkerThreadCount = 1;
    undersizedParseBudgetOptions.maxPendingParseBytes =
        undersizedParseBudgetOptions.maxBodySize - 1;
    const ServerCore::Core::Status undersizedParseBudget =
        undersizedParseBudgetHost.Configure(undersizedParseBudgetOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(undersizedParseBudget.Code()),
        "ServerHost rejects a parse byte budget smaller than one frame body");

    ServerCore::Runtime::ServerHost invertedParseByteBudgetHost;
    ServerCore::Runtime::ServerHostOptions invertedParseByteBudgetOptions;
    invertedParseByteBudgetOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    invertedParseByteBudgetOptions.parseWorkerThreadCount = 1;
    invertedParseByteBudgetOptions.maxPendingParseBytes =
        invertedParseByteBudgetOptions.maxBodySize + 1;
    invertedParseByteBudgetOptions.maxTotalPendingParseBytes =
        invertedParseByteBudgetOptions.maxBodySize;
    const ServerCore::Core::Status invertedParseByteBudget =
        invertedParseByteBudgetHost.Configure(invertedParseByteBudgetOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(invertedParseByteBudget.Code()),
        "ServerHost rejects a total parse byte budget below the per-session budget");

    ServerCore::Runtime::ServerHost invertedParseTaskBudgetHost;
    ServerCore::Runtime::ServerHostOptions invertedParseTaskBudgetOptions;
    invertedParseTaskBudgetOptions.port = static_cast<std::uint16_t>(PortBase + 6);
    invertedParseTaskBudgetOptions.parseWorkerThreadCount = 1;
    invertedParseTaskBudgetOptions.maxPendingParseTasks = 2;
    invertedParseTaskBudgetOptions.maxTotalPendingParseTasks = 1;
    const ServerCore::Core::Status invertedParseTaskBudget =
        invertedParseTaskBudgetHost.Configure(invertedParseTaskBudgetOptions);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(invertedParseTaskBudget.Code()),
        "ServerHost rejects a total parse task budget below the per-session budget");
}

void ServerHostSharesConfiguredSendBudgetAcrossSessions()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the Host shared-send-budget test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    constexpr std::size_t PayloadBytes = 600u * 1024u;
    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 33);
    auto observer = std::make_shared<RetainingSessionObserver>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    options.maxConcurrentSessions = 2;
    options.maxBodySize = 700u * 1024u;
    options.maxTotalSendQueueCapacityBytes =
        static_cast<std::uint32_t>(ServerCore::Net::SendQueueLimitBytes);

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted its minimum aggregate send budget");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started with a shared send budget");
    if (!started.IsOk())
    {
        return;
    }

    TestClient firstClient;
    TestClient secondClient;
    const bool firstConnected = firstClient.Connect(port);
    const bool secondConnected = secondClient.Connect(port);
    ServerCoreTest::ExpectTrue(
        firstConnected && secondConnected, "both shared-send-budget clients connected");
    if (!firstConnected || !secondConnected || !observer->WaitForOpenedCount(2, WaitLimit))
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }

    const std::shared_ptr<ServerCore::Session::Session> firstSession = observer->SessionAt(0);
    const std::shared_ptr<ServerCore::Session::Session> secondSession = observer->SessionAt(1);
    ServerCoreTest::ExpectTrue(firstSession != nullptr && secondSession != nullptr,
        "the Host exposed both sessions to its lifecycle observer");
    if (firstSession == nullptr || secondSession == nullptr)
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }

    auto receiveGate = std::make_shared<BlockingSessionReceiveGate>();
    const ScopedBeforeSessionReceiveGate receiveGateScope(receiveGate);
    const std::vector<std::byte> receiveTrigger{ std::byte{ 0 } };
    const bool triggerSent = firstClient.SendAll(receiveTrigger);
    const bool workerBlocked = triggerSent && receiveGate->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(
        workerBlocked, "the single Host I/O worker is blocked before send completions run");
    if (!workerBlocked)
    {
        receiveGateScope.Release();
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }

    ServerCore::Protocol::JsonValue::Object fields;
    fields.emplace("payload", ServerCore::Protocol::JsonValue(std::string(PayloadBytes, 'x')));
    const ServerCore::Protocol::JsonValue body(std::move(fields));
    const auto prepared = ServerCore::Protocol::PrepareMessage({ "runtime.send-budget", &body, nullptr, nullptr });
    ServerCoreTest::ExpectTrue(prepared.IsOk(), "a reusable message is prepared before competing for the shared budget");
    if (!prepared.IsOk())
    {
        receiveGateScope.Release();
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }
    const ServerCore::Core::Status firstSend = firstSession->Send("runtime.send-budget", body);
    const ServerCore::Core::Status secondSend = secondSession->SendPrepared(prepared.Value());
    const ServerCore::Core::Status sameSessionOverflow = firstSession->SendPrepared(prepared.Value());
    ServerCoreTest::ExpectTrue(firstSend.IsOk(),
        "the first session reserves more than half of the configured Host send budget");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::WouldBlock),
        static_cast<int>(secondSend.Code()),
        "prepared sends compete with ordinary sends for the same Host send budget");
    ServerCoreTest::ExpectTrue(sameSessionOverflow.Code() == ServerCore::Core::ErrorCode::WouldBlock,
        "prepared sends cannot bypass the per-session queue limit either");
    ServerCoreTest::ExpectEqual(prepared.Value().Size() + ServerCore::Protocol::HeaderSize,
        firstSession->QueuedSendBytes(), "queue bytes include one complete framed message, not rejected prepared sends");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, secondSession->QueuedSendBytes(),
        "a rejected prepared frame reserves no bytes on another session");
    const auto metrics = CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(metrics.IsOk(), "shared prepared-send metrics are observable with completions blocked");
    if (metrics.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::uint64_t{ 1 }, metrics.Value().queuedSendFrameCount,
            "WouldBlock does not increment the accepted send-frame count");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, metrics.Value().errorCount,
            "expected prepared-send backpressure is not an operational error");
    }

    receiveGateScope.Release();
    firstSession->Disconnect(
        ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed));
    secondSession->Disconnect(
        ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed));
    firstClient.Close();
    secondClient.Close();
    host.Stop();
}

void ServerHostPreparedSendsPreserveFramingLimitsAndMetrics()
{
    using ServerCore::Core::ErrorCode;
    namespace Protocol = ServerCore::Protocol;
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "Winsock is ready for the prepared-send integration test");
    if (!winsock.IsReady()) return;
    Protocol::JsonValue body(Protocol::JsonValue::Object{
        { "payload", Protocol::JsonValue(std::string(512, 'x') + "한글\"\\\n") } });
    const Protocol::JsonValue sequence(std::uint64_t{ 18446744073709551615ULL });
    auto prepared = Protocol::PrepareMessage({ "runtime.prepared", &body, &sequence, nullptr });
    ServerCoreTest::ExpectTrue(prepared.IsOk(), "the prepared TCP envelope is valid");
    if (!prepared.IsOk()) return;
    const std::vector<std::byte> expected(prepared.Value().Bytes().begin(), prepared.Value().Bytes().end());
    const std::size_t frameBytes = expected.size() + Protocol::HeaderSize;

    auto observer = std::make_shared<RetainingSessionObserver>();
    ServerCore::Runtime::ServerHost host;
    ServerCore::Runtime::ServerHostOptions options;
    options.port = static_cast<std::uint16_t>(PortBase + 39);
    options.maxConcurrentSessions = 1;
    options.ioWorkerThreadCount = 1;
    options.maxBodySize = static_cast<std::uint32_t>(prepared.Value().Size());
    const auto configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "the host accepts an exact prepared-body frame limit");
    if (!configured.IsOk()) return;
    host.SetSessionObserver(observer);
    const auto started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "the prepared-send host starts");
    if (!started.IsOk()) return;
    TestClient client;
    const bool connected = client.Connect(options.port);
    ServerCoreTest::ExpectTrue(connected, "the prepared-send client connects");
    const bool opened = connected && observer->WaitForOpenedCount(1, WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the host exposes the prepared-send session");
    if (!opened) { client.Close(); host.Stop(); return; }
    const auto session = observer->SessionAt(0);
    auto receiveGate = std::make_shared<BlockingSessionReceiveGate>();
    const ScopedBeforeSessionReceiveGate receiveGateScope(receiveGate);
    const std::vector<std::byte> trigger{ std::byte{ 0 } };
    const bool workerBlocked = client.SendAll(trigger) && receiveGate->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(workerBlocked, "I/O completions are blocked while queue ownership is inspected");
    if (!workerBlocked) { receiveGateScope.Release(); client.Close(); host.Stop(); return; }

    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, session->QueuedSendBytes(), "a new session has no queued outbound bytes");
    const auto first = session->SendPrepared(prepared.Value());
    auto owner = std::move(prepared.Value());
    body = Protocol::JsonValue(nullptr);
    const auto second = session->SendPrepared(owner);
    ServerCoreTest::ExpectTrue(first.IsOk() && second.IsOk(), "the same prepared envelope is reusable at the exact body limit");
    const auto empty = session->SendPrepared(prepared.Value());
    ServerCoreTest::ExpectTrue(!empty.IsOk(), "a moved-from prepared message must not emit an empty TCP frame");
    Protocol::JsonValue largerBody(Protocol::JsonValue::Object{
        { "payload", Protocol::JsonValue(std::string(513, 'x') + "한글\"\\\n") } });
    const auto larger = Protocol::PrepareMessage({ "runtime.prepared", &largerBody, &sequence, nullptr });
    ServerCoreTest::ExpectTrue(larger.IsOk(), "a message may be prepared independently of a host's frame limit");
    if (larger.IsOk())
    {
        ServerCoreTest::ExpectEqual(expected.size() + 1, larger.Value().Size(), "the oversized fixture crosses the frame body limit by exactly one byte");
        ServerCoreTest::ExpectTrue(session->SendPrepared(larger.Value()).Code() == ErrorCode::TooLarge,
            "prepared sends enforce the same per-host body limit as ordinary sends");
    }
    ServerCoreTest::ExpectEqual(frameBytes * 2, session->QueuedSendBytes(),
        "the public queue metric counts accepted headers and bodies but no rejected frames");
    const auto queued = CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(queued.IsOk(), "prepared-send metrics are captured on JobRunner");
    if (queued.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::uint64_t{ 2 }, queued.Value().queuedSendFrameCount,
            "each accepted prepared frame increments the counter exactly once");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 2 }, queued.Value().errorCount,
            "empty and oversized prepared sends each record one validation error");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, queued.Value().receivedFrameCount,
            "outbound prepared frames are not counted as received input");
        ServerCoreTest::ExpectTrue(queued.Value().sessionSendQueues.size() == 1 &&
            queued.Value().sessionSendQueues.front().queuedBytes == session->QueuedSendBytes(),
            "the session queue observation agrees with the host snapshot while completions are blocked");
    }
    receiveGateScope.Release();
    if (first.IsOk() && second.IsOk())
    {
        for (int index = 0; index < 2; ++index)
        {
            std::vector<std::byte> received;
            const bool complete = client.ReceiveFrame(received);
            ServerCoreTest::ExpectTrue(complete && received == expected,
                "real TCP preserves complete prepared-envelope bytes after the source is changed and preparation is moved");
            if (!complete) break;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + WaitLimit;
    while (session->QueuedSendBytes() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, session->QueuedSendBytes(), "completed prepared writes release all queued-byte reservations");
    session->Disconnect(ServerCore::Core::Status::FailWithoutMessage(ErrorCode::Closed));
    ServerCoreTest::ExpectTrue(observer->WaitForClosed(WaitLimit), "the retained prepared-send session closes");
    ServerCoreTest::ExpectTrue(session->SendPrepared(owner).Code() == ErrorCode::Closed,
        "prepared sends on a retained closed session are rejected");
    const auto closed = CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(closed.IsOk(), "metrics remain available after the prepared-send session closes");
    if (closed.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::uint64_t{ 2 }, closed.Value().queuedSendFrameCount,
            "a Closed prepared send cannot count as an accepted frame");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 2 }, closed.Value().errorCount,
            "normal closure and a Closed prepared send do not duplicate error counts");
    }
    client.Close();
    host.Stop();
}

void ServerHostEnforcesConcurrentSessionLimit()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the concurrent-session-limit test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 17);
    auto observer = std::make_shared<RecordingSessionObserver>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    options.maxConcurrentSessions = 1;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(),
        configured.IsOk()
            ? "ServerHost accepted a one-session limit"
            : ("ServerHost accepted a one-session limit, message: " + configured.Message()));
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(),
        started.IsOk()
            ? "ServerHost started with a one-session limit"
            : ("ServerHost started with a one-session limit, message: " + started.Message()));
    if (!started.IsOk())
    {
        return;
    }

    TestClient firstClient;
    const bool firstConnected = firstClient.Connect(port);
    ServerCoreTest::ExpectTrue(firstConnected, "the first limited-session client connected");
    if (!firstConnected)
    {
        host.Stop();
        return;
    }

    const bool firstOpened = observer->WaitForOpenedCount(1, WaitLimit);
    ServerCoreTest::ExpectTrue(firstOpened, "the first limited-session client opened a session");
    if (!firstOpened)
    {
        firstClient.Close();
        host.Stop();
        return;
    }
    const ServerCore::Session::SessionId firstId = observer->OpenedId();

    TestClient rejectedClient;
    const bool rejectedConnected = rejectedClient.Connect(port);
    ServerCoreTest::ExpectTrue(
        rejectedConnected, "the excess client completed the TCP handshake before Host rejection");
    if (!rejectedConnected)
    {
        firstClient.Close();
        host.Stop();
        return;
    }

    const bool rejectedPeerClosed = rejectedClient.WaitForPeerClose();
    ServerCoreTest::ExpectTrue(
        rejectedPeerClosed, "the Host closes an excess connection before opening a session");
    rejectedClient.Close();
    if (!rejectedPeerClosed)
    {
        firstClient.Close();
        host.Stop();
        return;
    }

    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->OpenedCount(),
        "the excess connection did not trigger a second session-open observer event");
    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> whileLimited =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        whileLimited.IsOk(), "metrics captured the admitted session while the limit was full");
    if (!whileLimited.IsOk())
    {
        firstClient.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1),
        whileLimited.Value().activeSessionCount,
        "only the admitted client occupies the concurrent-session limit");

    firstClient.Close();
    const bool firstClosed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(firstClosed, "closing the admitted client finalized its session");
    if (!firstClosed)
    {
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->ClosedCount(),
        "closing the admitted client emitted one session-close event");
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(firstId),
        static_cast<std::uint64_t>(observer->ClosedId()),
        "the admitted session closed before its capacity was reclaimed");

    // OnSessionClosed() 직전에 수락 슬롯을 되돌리며 별도 통지 계수가 callback 수명을 붙든다.
    // JobRunner에 넣은 이 스냅숏은 FinalizeOnRunner() 뒤에서 실행되므로, 다음 연결 전에 슬롯
    // 회수와 통지 완료가 모두 끝났음을 보장한다.
    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterFirstClose =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        afterFirstClose.IsOk(), "metrics waited for the closed session finalization to finish");
    if (!afterFirstClose.IsOk())
    {
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0),
        afterFirstClose.Value().activeSessionCount,
        "the closed session no longer occupies the concurrent-session limit");

    TestClient replacementClient;
    const bool replacementConnected = replacementClient.Connect(port);
    ServerCoreTest::ExpectTrue(
        replacementConnected, "a replacement client connected after slot recovery");
    if (!replacementConnected)
    {
        host.Stop();
        return;
    }

    const bool replacementOpened = observer->WaitForOpenedCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(
        replacementOpened, "the replacement client opened a session after slot recovery");
    if (!replacementOpened)
    {
        replacementClient.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(2), observer->OpenedCount(),
        "the replacement client is the second admitted session");
    ServerCoreTest::ExpectTrue(observer->OpenedId() != firstId,
        "the replacement client received a newly issued session id");

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterRecovery =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        afterRecovery.IsOk(), "metrics captured the replacement session after slot recovery");
    if (afterRecovery.IsOk())
    {
        ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1),
            afterRecovery.Value().activeSessionCount,
            "the replacement client occupies the recovered concurrent-session slot");
    }

    replacementClient.Close();
    host.Stop();
}

void ServerHostConfiguresFromConfig()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the Config-host test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 30);
    const ServerCore::Core::Result<ServerCore::Core::Config> config =
        LoadConfigSnapshotForHost("servercore.host.port = " + std::to_string(port) +
                                  "\n"
                                  "servercore.host.listen-address = 127.0.0.1\n"
                                  "servercore.host.io-worker-thread-count = 2\n"
                                  "servercore.host.parse-worker-thread-count = 1\n"
                                  "servercore.host.accept-backlog = 8\n"
                                  "servercore.host.idle-session-timeout-ms = 500\n"
                                  "servercore.host.graceful-close-timeout-ms = 250\n"
                                  "servercore.host.max-body-size = 128\n"
                                  "servercore.host.max-concurrent-sessions = 2\n"
                                  "servercore.host.max-total-send-queue-capacity-bytes = 2097152\n"
                                  "servercore.host.max-pending-receive-bytes = 512\n"
                                  "servercore.host.max-total-pending-receive-bytes = 1024\n"
                                  "servercore.host.max-pending-parse-bytes = 256\n"
                                  "servercore.host.max-total-pending-parse-bytes = 512\n"
                                  "servercore.host.max-pending-parse-tasks = 2\n"
                                  "servercore.host.max-total-pending-parse-tasks = 4\n"
                                  "summit.nickname-policy = local-only\n");
    ServerCoreTest::ExpectTrue(config.IsOk(), "the ServerHost Config fixture loads");
    if (!config.IsOk())
    {
        return;
    }

    ServerCore::Runtime::ServerHost host;
    const ServerCore::Core::Status configured = host.Configure(config.Value());
    ServerCoreTest::ExpectTrue(configured.IsOk(),
        configured.IsOk()
            ? "ServerHost accepts all of its Config keys"
            : ("ServerHost accepts all of its Config keys, message: " + configured.Message()));
    if (!configured.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(),
        started.IsOk()
            ? "ServerHost starts from its Config snapshot"
            : ("ServerHost starts from its Config snapshot, message: " + started.Message()));
    if (!started.IsOk())
    {
        return;
    }

    ServerCoreTest::ExpectEqual(port, host.Port(), "ServerHost listens on the Config port");
    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> metrics =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(metrics.IsOk(), "metrics can inspect a Config-configured Host");
    if (metrics.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::uint32_t{ 2 },
            metrics.Value().configuredIoWorkerThreadCount,
            "metrics reports the Config I/O worker count");
        ServerCoreTest::ExpectEqual(std::uint32_t{ 1 },
            metrics.Value().configuredParseWorkerThreadCount,
            "metrics reports the Config parse worker count");
    }

    host.Stop();
    ServerCoreTest::ExpectEqual(
        std::uint16_t{ 0 }, host.Port(), "stopping a Config-configured Host clears its port");
}

void ServerHostRejectsInvalidConfig()
{
    const auto expectConfigurationFailure = [](const std::string_view text,
                                                const ServerCore::Core::ErrorCode expected,
                                                const std::string_view what)
    {
        const ServerCore::Core::Result<ServerCore::Core::Config> config =
            LoadConfigSnapshotForHost(text);
        ServerCoreTest::ExpectTrue(config.IsOk(), "an invalid Host Config fixture still parses");
        if (!config.IsOk())
        {
            return;
        }

        ServerCore::Runtime::ServerHost host;
        const ServerCore::Core::Status configured = host.Configure(config.Value());
        ServerCoreTest::ExpectEqual(
            static_cast<int>(expected), static_cast<int>(configured.Code()), what);
    };
    const std::string validHostPort =
        "servercore.host.port = " + std::to_string(static_cast<std::uint16_t>(PortBase + 29)) +
        "\n";
    const auto expectOptionValidationFailure =
        [&expectConfigurationFailure, &validHostPort](
            const std::string_view setting, const std::string_view what)
    {
        std::string text(validHostPort);
        text.append(setting);
        expectConfigurationFailure(text, ServerCore::Core::ErrorCode::InvalidArgument, what);
    };
    const auto expectParseOptionValidationFailure =
        [&expectConfigurationFailure, &validHostPort](
            const std::string_view setting, const std::string_view what)
    {
        std::string text(validHostPort);
        text.append("servercore.host.parse-worker-thread-count = 1\n");
        text.append(setting);
        expectConfigurationFailure(text, ServerCore::Core::ErrorCode::InvalidArgument, what);
    };

    expectConfigurationFailure("# no ServerHost port\n", ServerCore::Core::ErrorCode::NotFound,
        "a Config without the required Host port reports NotFound");
    expectConfigurationFailure("servercore.host.port = not-a-number\n",
        ServerCore::Core::ErrorCode::InvalidFormat,
        "a non-integer Host port keeps Config's InvalidFormat result");
    expectConfigurationFailure("servercore.host.port = 2147483648\n",
        ServerCore::Core::ErrorCode::InvalidFormat,
        "a Host port outside Config's int grammar keeps InvalidFormat");
    expectConfigurationFailure("servercore.host.port = -1\n",
        ServerCore::Core::ErrorCode::InvalidArgument,
        "a negative Host port is not narrowed to an unsigned port");
    expectConfigurationFailure("servercore.host.port = 65536\n",
        ServerCore::Core::ErrorCode::InvalidArgument,
        "a Host port above uint16_t range is rejected");
    expectConfigurationFailure("servercore.host.port = 0\n",
        ServerCore::Core::ErrorCode::InvalidArgument,
        "a zero Host port still uses ServerHostOptions validation");
    expectConfigurationFailure(validHostPort + "servercore.host.io-worker-thread-count = 1x\n",
        ServerCore::Core::ErrorCode::InvalidFormat,
        "a malformed optional Host integer is not silently replaced by its default");
    expectConfigurationFailure(validHostPort + "servercore.host.max-body-size = -1\n",
        ServerCore::Core::ErrorCode::InvalidArgument,
        "a negative unsigned Host limit is rejected before narrowing");
    expectOptionValidationFailure("servercore.host.io-worker-thread-count = 0\n",
        "an optional I/O worker count uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.parse-worker-thread-count = -1\n",
        "an optional parse worker count uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.accept-backlog = 0\n",
        "an optional accept backlog uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.idle-session-timeout-ms = -1\n",
        "an optional idle timeout uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.graceful-close-timeout-ms = -1\n",
        "a negative graceful close timeout uses ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.graceful-close-timeout-ms = 0\n",
        "the graceful close deadline cannot be disabled through Config");
    expectConfigurationFailure(validHostPort + "servercore.host.graceful-close-timeout-ms = bad\n",
        ServerCore::Core::ErrorCode::InvalidFormat,
        "a malformed graceful close timeout is not silently replaced by the default");
    expectOptionValidationFailure("servercore.host.max-body-size = 0\n",
        "an optional body limit uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.max-concurrent-sessions = 0\n",
        "an optional concurrent-session limit uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.max-total-send-queue-capacity-bytes = 0\n",
        "an optional aggregate send queue capacity uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.max-pending-receive-bytes = 0\n",
        "an optional receive-byte limit uses existing ServerHostOptions validation");
    expectOptionValidationFailure("servercore.host.max-total-pending-receive-bytes = 0\n",
        "an optional total receive-byte limit uses existing ServerHostOptions validation");
    expectParseOptionValidationFailure("servercore.host.max-pending-parse-bytes = 0\n",
        "an optional parse-byte limit is applied when parse workers are enabled");
    expectParseOptionValidationFailure("servercore.host.max-total-pending-parse-bytes = 0\n",
        "an optional total parse-byte limit is applied when parse workers are enabled");
    expectParseOptionValidationFailure("servercore.host.max-body-size = 64\n"
                                       "servercore.host.max-pending-parse-bytes = 128\n"
                                       "servercore.host.max-total-pending-parse-bytes = 64\n",
        "a total parse-byte limit below the session limit uses ServerHostOptions validation");
    expectParseOptionValidationFailure("servercore.host.max-pending-parse-tasks = 0\n",
        "an optional parse-task limit is applied when parse workers are enabled");
    expectParseOptionValidationFailure("servercore.host.max-total-pending-parse-tasks = 0\n",
        "an optional total parse-task limit is applied when parse workers are enabled");
    expectConfigurationFailure(
        validHostPort + "servercore.host.io-worker-thread-count = " +
            std::to_string(ServerCore::Runtime::MaximumServerHostWorkerThreadCount + 1) + "\n",
        ServerCore::Core::ErrorCode::TooLarge,
        "a Config I/O worker count cannot exceed the Host safety limit");
    expectConfigurationFailure(
        validHostPort + "servercore.host.parse-worker-thread-count = " +
            std::to_string(ServerCore::Runtime::MaximumServerHostWorkerThreadCount + 1) + "\n",
        ServerCore::Core::ErrorCode::TooLarge,
        "a Config parse worker count cannot exceed the Host safety limit");
    const ServerCore::Core::Result<ServerCore::Core::Config> sharedSendBudgetConfig =
        LoadConfigSnapshotForHost(
            validHostPort + "servercore.host.max-concurrent-sessions = 300\n");
    ServerCoreTest::ExpectTrue(
        sharedSendBudgetConfig.IsOk(), "the legacy session-count Config fixture loads");
    if (sharedSendBudgetConfig.IsOk())
    {
        ServerCore::Runtime::ServerHost sharedSendBudgetHost;
        const ServerCore::Core::Status configured =
            sharedSendBudgetHost.Configure(sharedSendBudgetConfig.Value());
        ServerCoreTest::ExpectTrue(
            configured.IsOk(), "a Config without the new send-budget key preserves 300 sessions");
    }

    const ServerCore::Core::Result<ServerCore::Core::Config> maximumSessionsConfig =
        LoadConfigSnapshotForHost(validHostPort +
                                  "servercore.host.max-concurrent-sessions = 65536\n"
                                  "servercore.host.max-body-size = 8192\n");
    ServerCoreTest::ExpectTrue(
        maximumSessionsConfig.IsOk(), "the 65,536-session Config fixture loads");
    if (maximumSessionsConfig.IsOk())
    {
        ServerCore::Runtime::ServerHost maximumSessionsHost;
        const ServerCore::Core::Status configured =
            maximumSessionsHost.Configure(maximumSessionsConfig.Value());
        ServerCoreTest::ExpectTrue(
            configured.IsOk(), "Config accepts 65,536 sessions with 8 KiB bodies");
    }
    expectConfigurationFailure(validHostPort +
            "servercore.host.max-concurrent-sessions = 65537\n"
            "servercore.host.max-body-size = 8192\n",
        ServerCore::Core::ErrorCode::TooLarge,
        "Config preserves the concurrent-session safety boundary above 65,536");

    const ServerCore::Core::Result<ServerCore::Core::Config> parseDisabledConfig =
        LoadConfigSnapshotForHost(validHostPort +
                                  "servercore.host.parse-worker-thread-count = 0\n"
                                  "servercore.host.max-pending-parse-bytes = 0\n"
                                  "servercore.host.max-total-pending-parse-bytes = 0\n"
                                  "servercore.host.max-pending-parse-tasks = 0\n"
                                  "servercore.host.max-total-pending-parse-tasks = 0\n");
    ServerCoreTest::ExpectTrue(
        parseDisabledConfig.IsOk(), "the synchronous-parse Config fixture loads");
    if (parseDisabledConfig.IsOk())
    {
        ServerCore::Runtime::ServerHost parseDisabledHost;
        const ServerCore::Core::Status configured =
            parseDisabledHost.Configure(parseDisabledConfig.Value());
        ServerCoreTest::ExpectTrue(
            configured.IsOk(), "a Config preserves the disabled-parse option validation boundary");
    }

    const ServerCore::Core::Result<ServerCore::Core::Config> invalidAddressConfig =
        LoadConfigSnapshotForHost(
            validHostPort + "servercore.host.listen-address = not-an-ipv4-address\n");
    ServerCoreTest::ExpectTrue(
        invalidAddressConfig.IsOk(), "the invalid-address Config fixture loads");
    if (invalidAddressConfig.IsOk())
    {
        ServerCore::Runtime::ServerHost invalidAddressHost;
        const ServerCore::Core::Status configured =
            invalidAddressHost.Configure(invalidAddressConfig.Value());
        ServerCoreTest::ExpectTrue(
            configured.IsOk(), "Config mapping leaves IPv4 validation at ServerHost Start()");
        if (configured.IsOk())
        {
            const ServerCore::Core::Status started = invalidAddressHost.Start();
            ServerCoreTest::ExpectEqual(
                static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
                static_cast<int>(started.Code()),
                "an invalid Config listen address fails when ServerHost starts");
        }
        invalidAddressHost.Stop();
    }

    const std::uint16_t retainedPort = static_cast<std::uint16_t>(PortBase + 31);
    const ServerCore::Core::Result<ServerCore::Core::Config> validConfig =
        LoadConfigSnapshotForHost("servercore.host.port = " + std::to_string(retainedPort) + "\n");
    const ServerCore::Core::Result<ServerCore::Core::Config> rejectedConfig =
        LoadConfigSnapshotForHost(
            "servercore.host.port = " + std::to_string(static_cast<std::uint16_t>(PortBase + 32)) +
            "\n"
            "servercore.host.io-worker-thread-count = invalid\n");
    ServerCoreTest::ExpectTrue(validConfig.IsOk(), "the retained-setting Config fixture loads");
    ServerCoreTest::ExpectTrue(rejectedConfig.IsOk(), "the rejected-setting Config fixture loads");
    if (validConfig.IsOk() && rejectedConfig.IsOk())
    {
        ServerCore::Runtime::ServerHost host;
        const ServerCore::Core::Status initiallyConfigured = host.Configure(validConfig.Value());
        ServerCoreTest::ExpectTrue(
            initiallyConfigured.IsOk(), "a valid Config can establish a Host configuration");
        if (initiallyConfigured.IsOk())
        {
            const ServerCore::Core::Status rejected = host.Configure(rejectedConfig.Value());
            ServerCoreTest::ExpectEqual(
                static_cast<int>(ServerCore::Core::ErrorCode::InvalidFormat),
                static_cast<int>(rejected.Code()),
                "a rejected Config does not replace the current Host configuration");

            const ServerCore::Core::Status started = host.Start();
            ServerCoreTest::ExpectTrue(started.IsOk(),
                "the valid Host configuration remains startable after a rejected Config");
            if (started.IsOk())
            {
                ServerCoreTest::ExpectEqual(retainedPort, host.Port(),
                    "a rejected Config leaves the previous Host port in place");
            }
        }
        host.Stop();
    }
}

void ServerHostBindsRegistryBeforePrepostedWork()
{
    ServerCore::Runtime::ServerHost host;
    ServerCore::Runtime::ServerHostOptions options;
    options.port = static_cast<std::uint16_t>(PortBase + 9);

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted options before preposted work");
    if (!configured.IsOk())
    {
        return;
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool ran = false;
    std::size_t observedSessionCount = 1;
    const ServerCore::Core::Status posted = host.GetJobRunner().Post(
        [&]()
        {
            const std::size_t count = host.GetSessions().Count();
            {
                const std::lock_guard<std::mutex> guard(mutex);
                observedSessionCount = count;
                ran = true;
            }
            changed.notify_all();
        });
    ServerCoreTest::ExpectTrue(posted.IsOk(), "the Host work lease accepts a job before Start()");
    if (!posted.IsOk())
    {
        host.Stop();
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started with preposted work");
    if (!started.IsOk())
    {
        host.Stop();
        return;
    }

    {
        std::unique_lock<std::mutex> guard(mutex);
        const bool completed = changed.wait_for(guard, WaitLimit, [&]() { return ran; });
        ServerCoreTest::ExpectTrue(
            completed, "preposted work ran after the SessionRegistry binding completed");
    }
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), observedSessionCount,
        "preposted work used the bound registry");

    host.Stop();
}

void ServerHostSendsFinalFrameBeforeDisconnect()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the final-response test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 14);
    auto observer = std::make_shared<RecordingSessionObserver>();
    auto handler = std::make_shared<FinalResponseHandler>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "ServerHost accepted final-response options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register(
        "runtime.final",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, message); },
        1024);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the final-response handler registered");
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started for the final-response test");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the final-response client connected");
    if (!connected)
    {
        host.Stop();
        return;
    }

    const bool opened = observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the final-response session opened");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }

    const std::vector<std::byte> request =
        MakeFrame(R"({"type":"runtime.final","body":{"value":"goodbye"},"seq":"final-token"})");
    const bool sent = client.SendAll(request);
    ServerCoreTest::ExpectTrue(sent, "the final-response client sent its request");
    if (!sent)
    {
        client.Close();
        host.Stop();
        return;
    }

    const bool handled = handler->WaitForCall(WaitLimit);
    ServerCoreTest::ExpectTrue(handled, "the final-response handler ran");
    if (!handled)
    {
        client.Close();
        host.Stop();
        return;
    }

    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Ok),
        static_cast<int>(handler->FinalCode()), "SendAndDisconnect() accepted the final envelope");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(handler->AfterFinalCode()), "Send() after the final envelope is rejected");

    std::vector<std::byte> finalBody;
    const bool received = client.ReceiveFrame(finalBody);
    ServerCoreTest::ExpectTrue(received, "the client received the complete final frame");
    if (!received)
    {
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Protocol::Message> parsed =
        ServerCore::Protocol::ParseMessage(finalBody);
    ServerCoreTest::ExpectTrue(
        parsed.IsOk(), "the complete final frame is a valid message envelope");
    if (!parsed.IsOk())
    {
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Protocol::Message& response = parsed.Value();
    const ServerCore::Protocol::JsonValue* const sequence = response.Sequence();
    const std::string* const sequenceText = sequence == nullptr ? nullptr : sequence->TryString();
    const ServerCore::Protocol::JsonValue* const error = response.Error();
    const ServerCore::Protocol::JsonValue* const errorCode =
        error == nullptr ? nullptr : error->Find("code");
    const ServerCore::Protocol::JsonValue* const errorMessage =
        error == nullptr ? nullptr : error->Find("message");
    const std::string* const errorCodeText =
        errorCode == nullptr ? nullptr : errorCode->TryString();
    const std::string* const errorMessageText =
        errorMessage == nullptr ? nullptr : errorMessage->TryString();

    ServerCoreTest::ExpectEqual(std::string("runtime.final.reply"), std::string(response.Type()),
        "the final frame has the registered reply type");
    ServerCoreTest::ExpectTrue(
        response.Body() == nullptr, "the final error envelope intentionally has no body");
    ServerCoreTest::ExpectTrue(sequenceText != nullptr && *sequenceText == "final-token",
        "the final error envelope preserves the request sequence");
    ServerCoreTest::ExpectTrue(
        error != nullptr && error->IsObject(), "the final error envelope contains an error object");
    ServerCoreTest::ExpectTrue(errorCodeText != nullptr && *errorCodeText == "Rejected",
        "the final error envelope contains its error code");
    ServerCoreTest::ExpectTrue(
        errorMessageText != nullptr && *errorMessageText == "the test rejected the request",
        "the final error envelope contains its error message");

    const bool peerClosed = client.WaitForPeerClose();
    ServerCoreTest::ExpectTrue(
        peerClosed, "the final frame is followed directly by the graceful peer close");
    if (!peerClosed)
    {
        client.Close();
        host.Stop();
        return;
    }

    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(
        closed, "the final-response session closed after the final frame drained");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->OpenedCount(),
        "the final-response test opened one session");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->ClosedCount(),
        "the final-response test closed that session exactly once");
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(observer->OpenedId()),
        static_cast<std::uint64_t>(observer->ClosedId()),
        "the final-response observer closed the session it opened");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(observer->CloseCode()),
        "the final-response observer keeps the caller's close reason");

    client.Close();
    host.Stop();
}

void ServerHostDisconnectsIdleSessions()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the idle-session test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 15);
    auto observer = std::make_shared<RecordingSessionObserver>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    options.idleSessionTimeout = std::chrono::milliseconds(600);

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "ServerHost accepted idle-session options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started for the idle-session test");
    if (!started.IsOk())
    {
        host.Stop();
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the idle-session client connected");
    if (!connected)
    {
        host.Stop();
        return;
    }

    const bool opened = observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the idle-session observer saw the opened session");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }

    // 완결 메시지가 아니어도 TCP 바이트가 실제로 도착하면 idle 기준이 새로 시작해야 한다. 이
    // 한 바이트는 FrameReader의 머리를 완성하지 않으므로 게임 handler를 실행하지 않는다.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const std::vector<std::byte> partialHeader{ std::byte{ 0 } };
    const bool sent = client.SendAll(partialHeader);
    ServerCoreTest::ExpectTrue(sent, "the idle-session client sent a partial frame as activity");
    if (!sent)
    {
        client.Close();
        host.Stop();
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), observer->ClosedCount(),
        "a nonempty raw receive postpones the idle close past the original deadline");

    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(
        closed, "the Host closed the session after its renewed idle deadline");
    if (!closed)
    {
        client.Close();
        host.Stop();
        return;
    }

    const bool peerClosed = client.WaitForPeerClose();
    ServerCoreTest::ExpectTrue(
        peerClosed, "the idle-session client observed the server-side close");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->OpenedCount(),
        "the idle-session test opened one session");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->ClosedCount(),
        "the idle-session test closed the session exactly once");
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(observer->OpenedId()),
        static_cast<std::uint64_t>(observer->ClosedId()),
        "the idle-session observer closed the session it opened");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Timeout),
        static_cast<int>(observer->CloseCode()), "the idle-session observer receives Timeout");

    client.Close();
    host.Stop();
}

/// <summary>읽지 않는 상대의 종료 기한이 수신 활동과 무관하게 슬롯과 송신 예산을 회수한다.</summary>
void ServerHostBoundsGracefulCloseWithoutIdleTimeout()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() for graceful timeout succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 35);
    auto observer = std::make_shared<RetainingSessionObserver>();
    ServerCore::Runtime::ServerHost host;
    // 실제 Config 경로로 짧은 기한을 넣는다. 기본 5초가 남아 있거나 idle=0이 타이머를 끄면
    // 아래 2초 관찰 안에 닫힐 수 없으므로 설정 전달과 독립 실행을 함께 검증한다.
    const auto config = LoadConfigSnapshotForHost("servercore.host.port = " + std::to_string(port) +
        "\nservercore.host.listen-address = 127.0.0.1\n"
        "servercore.host.idle-session-timeout-ms = 0\n"
        "servercore.host.graceful-close-timeout-ms = 300\n"
        "servercore.host.max-concurrent-sessions = 1\n"
        "servercore.host.max-total-send-queue-capacity-bytes = 1048576\n");
    ServerCoreTest::ExpectTrue(config.IsOk(), "the graceful timeout Config fixture loads");
    if (!config.IsOk())
    {
        return;
    }
    const auto configured = host.Configure(config.Value());
    ServerCoreTest::ExpectTrue(configured.IsOk(), "graceful timeout is independent from idle timeout");
    if (!configured.IsOk())
    {
        return;
    }
    host.SetSessionObserver(observer);
    const auto started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "the graceful timeout Host starts");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port, 1024);
    const bool opened = connected && observer->WaitForOpenedCount(1, WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "a small-window client opens its session");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }
    const auto session = observer->SessionAt(0);
    const ServerCore::Protocol::JsonValue body(ServerCore::Protocol::JsonValue::Object{
        { "payload", ServerCore::Protocol::JsonValue(std::string(60000, 'x')) } });

    // OS 송신 버퍼가 처음 몇 프레임을 받아들인 뒤에도 반복해서 채운다. 상대는 단 한 바이트도
    // 읽지 않으므로 이후에는 TCP window와 실제 WSASend가 막힌다. 큐 잔량을 확인한 뒤에만
    // 종료 기한을 검증해, 이미 전송이 끝난 연결이 빨리 닫힌 것을 성공으로 오해하지 않는다.
    bool backpressureObserved = false;
    bool unexpectedSendFailure = false;
    for (int round = 0; round < 40 && !unexpectedSendFailure; ++round)
    {
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto sent = session->Send("runtime.graceful.payload", body);
            if (!sent.IsOk())
            {
                backpressureObserved = sent.Code() == ServerCore::Core::ErrorCode::WouldBlock;
                unexpectedSendFailure = !backpressureObserved;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto stalled = CaptureMetrics(host);
    const bool hasStalledQueue = stalled.IsOk() && stalled.Value().activeSessionCount == 1 &&
                                stalled.Value().sessionSendQueues[0].queuedBytes > 900000;
    ServerCoreTest::ExpectTrue(backpressureObserved && !unexpectedSendFailure && hasStalledQueue,
        "the non-reading peer retains a blocked send queue before graceful close");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Session::SessionState::Connected),
        static_cast<int>(session->State()),
        "the graceful deadline starts at Closing rather than at connection creation");
    if (!hasStalledQueue || unexpectedSendFailure)
    {
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Protocol::JsonValue finalBody(ServerCore::Protocol::JsonValue::Object{});
    const auto finalSend = session->SendAndDisconnect(
        ServerCore::Protocol::MessageFields{ "runtime.graceful.final", &finalBody },
        ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed));
    ServerCoreTest::ExpectTrue(finalSend.IsOk(), "the final frame fits behind the blocked payload");
    if (!finalSend.IsOk())
    {
        client.Close();
        host.Stop();
        return;
    }

    // Closing의 수신 callback은 이 바이트를 해석하지 않지만 마지막 수신 시각은 갱신한다.
    // 계속 보내도 절대 종료 기한은 늘지 않아야 한다.
    const std::vector<std::byte> inboundActivity{ std::byte{ 0 } };
    std::size_t acceptedActivity = 0;
    bool closed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!closed && std::chrono::steady_clock::now() < deadline)
    {
        if (client.SendAll(inboundActivity))
        {
            ++acceptedActivity;
        }
        closed = observer->WaitForClosed(std::chrono::milliseconds(20));
    }
    ServerCoreTest::ExpectTrue(acceptedActivity >= 2,
        "the peer continues sending activity while its graceful drain is blocked");
    ServerCoreTest::ExpectTrue(closed,
        "graceful close expires with idle disabled even while inbound activity continues");
    const auto afterClose = CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(afterClose.IsOk() && afterClose.Value().activeSessionCount == 0,
        "expired graceful close releases the only active session slot");
    if (!closed)
    {
        client.Close();
        host.Stop();
        return;
    }

    TestClient replacement;
    const bool replaced = replacement.Connect(port) && observer->WaitForOpenedCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(replaced, "a replacement client uses the reclaimed session slot");
    if (replaced)
    {
        const auto replacementSend = observer->SessionAt(1)->Send("runtime.graceful.recovered", body);
        ServerCoreTest::ExpectTrue(replacementSend.IsOk(),
            "a replacement session can reserve the shared send budget reclaimed from the drain");
    }
    client.Close();
    replacement.Close();
    host.Stop();
}

/// <summary>부팅 실패 중 소유자 파기가 다른 Stop의 실행 문맥 검사와 직렬화되는지 본다.</summary>
void ServerHostFailedStartSerializesOwnerResetWithStop()
{
    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 36);
    ServerCore::Net::Acceptor occupiedPort;
    const auto listening = occupiedPort.Listen("127.0.0.1", port, 8);
    ServerCoreTest::ExpectTrue(listening.IsOk(), "a listener reserves the startup-failure test port");
    if (!listening.IsOk())
    {
        return;
    }

    ServerCore::Runtime::ServerHost host;
    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.listenAddress = "127.0.0.1";
    options.parseWorkerThreadCount = 1;
    const auto configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "the startup-failure Host has valid options");
    if (!configured.IsOk())
    {
        return;
    }

    auto gate = std::make_shared<FailedStartOwnerGate>();
    ServerCore::Runtime::TestAccess::InstallFailedStartOwnerGate(gate);
    ServerCore::Core::Status startResult = ServerCore::Core::Status::Ok();
    std::thread starter([&host, &startResult] { startResult = host.Start(); });
    const bool resetEntered = gate->WaitForReset(WaitLimit);
    ServerCoreTest::ExpectTrue(resetEntered,
        "bind failure reaches cleanup after I/O and parse workers have been created");
    if (!resetEntered)
    {
        gate->Release();
        starter.join();
        ServerCore::Runtime::TestAccess::ClearFailedStartOwnerGate(gate);
        host.Stop();
        return;
    }

    std::thread stopper(
        [&host, gate]
        {
            gate->MarkStopCallerStarted();
            host.Stop();
        });
    ServerCoreTest::ExpectTrue(gate->WaitForStopCaller(WaitLimit),
        "a concurrent Stop caller begins while failed-start owner reset is held");
    const bool racedOwnerRead = gate->WaitForStopOwnerRead(std::chrono::milliseconds(100));
    ServerCoreTest::ExpectTrue(!racedOwnerRead,
        "Stop cannot inspect owner pointers while failed Start holds their destruction boundary");

    gate->Release();
    starter.join();
    stopper.join();
    ServerCoreTest::ExpectTrue(gate->WaitForStopOwnerRead(WaitLimit),
        "Stop inspects the completed cleanup only after the owner reset boundary releases");
    ServerCore::Runtime::TestAccess::ClearFailedStartOwnerGate(gate);
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::PlatformError),
        static_cast<int>(startResult.Code()), "the occupied port preserves the original Start failure");
    ServerCoreTest::ExpectTrue(!host.IsRunning(), "failed Start and concurrent Stop leave the Host stopped");
    ServerCoreTest::ExpectEqual(std::uint16_t{ 0 }, host.Port(), "failed startup exposes no port");
    occupiedPort.Stop();
    host.Stop();
}

void ServerHostConcurrentStopsWaitForBlockedHandler()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the concurrent-Stop test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 16);
    auto observer = std::make_shared<RecordingSessionObserver>();
    auto handler = std::make_shared<BlockingReceiveHandler>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 2;
    options.parseWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    options.idleSessionTimeout = std::chrono::seconds(5);

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "ServerHost accepted concurrent-Stop options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register("runtime.stop.block",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, *message.Body()); });
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the concurrent-Stop handler registered");
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started for the concurrent-Stop test");
    if (!started.IsOk())
    {
        host.Stop();
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the concurrent-Stop client connected");
    if (!connected)
    {
        host.Stop();
        return;
    }

    const bool opened = observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the concurrent-Stop observer saw the opened session");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }

    const std::vector<std::byte> frame = MakeFrame("{\"type\":\"runtime.stop.block\",\"body\":{}}");
    const bool sent = client.SendAll(frame);
    ServerCoreTest::ExpectTrue(sent, "the concurrent-Stop client sent a blocking frame");
    if (!sent)
    {
        client.Close();
        host.Stop();
        return;
    }

    const bool handlerEntered = handler->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(
        handlerEntered, "the concurrent-Stop handler is holding the JobRunner");
    if (!handlerEntered)
    {
        handler->Release();
        client.Close();
        host.Stop();
        return;
    }

    std::mutex stopMutex;
    std::condition_variable stopChanged;
    bool secondStopCallerStarted = false;
    std::size_t returnedStopCount = 0;
    const auto stopAndRecord = [&host, &stopMutex, &stopChanged, &returnedStopCount]()
    {
        host.Stop();
        {
            const std::lock_guard<std::mutex> guard(stopMutex);
            ++returnedStopCount;
        }
        stopChanged.notify_all();
    };

    std::thread firstStop(stopAndRecord);
    const auto stopDeadline = std::chrono::steady_clock::now() + WaitLimit;
    while (host.IsRunning() && std::chrono::steady_clock::now() < stopDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool firstStopBegan = !host.IsRunning();
    ServerCoreTest::ExpectTrue(firstStopBegan, "the first Stop() began before the second caller");
    if (!firstStopBegan)
    {
        handler->Release();
        firstStop.join();
        client.Close();
        return;
    }

    std::thread secondStop(
        [&stopAndRecord, &stopMutex, &stopChanged, &secondStopCallerStarted]()
        {
            {
                const std::lock_guard<std::mutex> guard(stopMutex);
                secondStopCallerStarted = true;
            }
            stopChanged.notify_all();
            stopAndRecord();
        });

    {
        std::unique_lock<std::mutex> guard(stopMutex);
        const bool secondCallerStarted = stopChanged.wait_for(
            guard, WaitLimit, [&secondStopCallerStarted]() { return secondStopCallerStarted; });
        ServerCoreTest::ExpectTrue(
            secondCallerStarted, "the second concurrent Stop() caller thread began");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        const std::lock_guard<std::mutex> guard(stopMutex);
        ServerCoreTest::ExpectEqual(std::size_t{ 0 }, returnedStopCount,
            "concurrent Stop() callers wait while the JobRunner handler is blocked");
    }
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(0), observer->ClosedCount(),
        "Stop() does not close the live session before the blocked handler releases");

    handler->Release();
    {
        std::unique_lock<std::mutex> guard(stopMutex);
        const bool bothReturned = stopChanged.wait_for(
            guard, WaitLimit, [&returnedStopCount]() { return returnedStopCount == 2; });
        ServerCoreTest::ExpectTrue(bothReturned,
            "both concurrent Stop() callers return after the blocked handler releases");
    }
    firstStop.join();
    secondStop.join();

    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(closed, "concurrent Stop() closes the live session");
    ServerCoreTest::ExpectTrue(!host.IsRunning(), "concurrent Stop() calls leave the Host stopped");
    ServerCoreTest::ExpectEqual(
        static_cast<std::uint16_t>(0), host.Port(), "concurrent Stop() clears the listening port");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(1), observer->ClosedCount(),
        "concurrent Stop() closes the live session exactly once");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(observer->CloseCode()), "concurrent Stop() keeps the normal close reason");

    client.Close();
    host.Stop();
}

void ServerHostReportsMetrics()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(winsock.IsReady(), "WSAStartup() for the metrics test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 12);
    auto observer = std::make_shared<RecordingSessionObserver>();
    auto handler = std::make_shared<MetricsResponseHandler>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 2;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "ServerHost accepted metrics-test options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register(
        "runtime.metrics",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, *message.Body()); },
        1024);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the metrics response handler registered");
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started for metrics");
    if (!started.IsOk())
    {
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> wrongThread =
        host.SnapshotMetrics();
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::InvalidArgument),
        static_cast<int>(wrongThread.GetStatus().Code()),
        "metrics snapshot rejects a non-JobRunner caller");

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the metrics client connected");
    if (!connected)
    {
        host.Stop();
        return;
    }

    const bool opened = observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the metrics session opened");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> initial =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        initial.IsOk(), "the initial metrics snapshot was captured on JobRunner");
    if (!initial.IsOk())
    {
        client.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(std::uint32_t{ 2 }, initial.Value().configuredIoWorkerThreadCount,
        "metrics reports the configured I/O worker count");
    ServerCoreTest::ExpectEqual(std::uint32_t{ 0 },
        initial.Value().configuredParseWorkerThreadCount,
        "metrics reports synchronous parsing when no parse worker is configured");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, initial.Value().activeSessionCount,
        "metrics reports one registered active session");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, initial.Value().sessionSendQueues.size(),
        "metrics emits one send queue entry for the active session");
    if (!initial.Value().sessionSendQueues.empty())
    {
        ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(observer->OpenedId()),
            static_cast<std::uint64_t>(initial.Value().sessionSendQueues.front().id),
            "metrics send queue belongs to the opened session");
        ServerCoreTest::ExpectEqual(std::size_t{ 0 },
            initial.Value().sessionSendQueues.front().queuedBytes,
            "a session with no response has no queued send bytes");
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, initial.Value().pendingReceiveBytes,
        "metrics starts with no pending receive bytes");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, initial.Value().pendingParseBytes,
        "metrics starts with no pending parse bytes in synchronous mode");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, initial.Value().pendingParseTaskCount,
        "metrics starts with no pending parse tasks in synchronous mode");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, initial.Value().receivedFrameCount,
        "metrics starts with no received frames");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, initial.Value().queuedSendFrameCount,
        "metrics starts with no queued send frames");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, initial.Value().errorCount,
        "metrics starts with no operational errors");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, initial.Value().skippedPeriodCount,
        "metrics starts with no skipped periods");

    const std::vector<std::byte> valid = MakeFrame("{\"type\":\"runtime.metrics\",\"body\":{}}");
    const bool validSent = client.SendAll(valid);
    ServerCoreTest::ExpectTrue(validSent, "the metrics client sent a valid frame");
    if (!validSent)
    {
        client.Close();
        host.Stop();
        return;
    }

    const bool handled = handler->WaitForCall(WaitLimit);
    ServerCoreTest::ExpectTrue(handled, "the metrics handler sent one response");
    if (!handled)
    {
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterValid =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        afterValid.IsOk(), "the post-response metrics snapshot was captured");
    if (!afterValid.IsOk())
    {
        client.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(std::uint64_t{ 1 }, afterValid.Value().receivedFrameCount,
        "a completed valid frame increments the received counter once");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 1 }, afterValid.Value().queuedSendFrameCount,
        "a response accepted by Connection::Send increments the queued-send counter once");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, afterValid.Value().errorCount,
        "a valid request-response exchange records no error");

    const std::vector<std::byte> malformed = MakeFrame("{");
    const bool malformedSent = client.SendAll(malformed);
    ServerCoreTest::ExpectTrue(
        malformedSent, "the metrics client sent a complete malformed JSON frame");
    if (!malformedSent)
    {
        client.Close();
        host.Stop();
        return;
    }

    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(closed, "malformed JSON closed the metrics session");
    if (!closed)
    {
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterMalformed =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        afterMalformed.IsOk(), "the post-error metrics snapshot was captured");
    if (afterMalformed.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::uint64_t{ 2 }, afterMalformed.Value().receivedFrameCount,
            "a complete malformed JSON frame is still a received frame");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 1 }, afterMalformed.Value().errorCount,
            "the malformed frame records one operational error");
        ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterMalformed.Value().activeSessionCount,
            "the finalized malformed session is absent from active metrics");
        ServerCoreTest::ExpectTrue(afterMalformed.Value().sessionSendQueues.empty(),
            "the finalized malformed session has no send queue entry");
    }

    client.Close();
    host.Stop();
}

void ServerHostParseWorkersPreserveSessionHandlerOrder()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the parse worker test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    constexpr std::size_t FrameCount = 8;
    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 13);
    auto observer = std::make_shared<RecordingSessionObserver>();
    ServerCore::Runtime::ServerHost host;
    auto handler = std::make_shared<BlockingOrderedParseHandler>(host);

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 2;
    options.parseWorkerThreadCount = 2;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "ServerHost accepted parse-worker options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register(
        "runtime.parse.order",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, message); },
        1024);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the ordered parse handler registered");
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started parse workers");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the parse worker client connected");
    if (!connected)
    {
        host.Stop();
        return;
    }

    const bool opened = observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the parse worker session opened");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }

    std::vector<std::byte> frames;
    for (std::size_t index = 0; index < FrameCount; ++index)
    {
        const std::string message =
            std::string("{\"type\":\"runtime.parse.order\",\"body\":{},\"seq\":\"") +
            std::to_string(index) + "\"}";
        const std::vector<std::byte> frame = MakeFrame(message);
        frames.insert(frames.end(), frame.begin(), frame.end());
    }

    const bool sent = client.SendAll(frames);
    ServerCoreTest::ExpectTrue(sent, "the parse worker client sent ordered frames");
    if (!sent)
    {
        handler->Release();
        client.Close();
        host.Stop();
        return;
    }

    const bool snapshotCaptured = handler->WaitUntilFirstSnapshot(WaitLimit);
    ServerCoreTest::ExpectTrue(snapshotCaptured,
        "the first parse worker result reached a JobRunner handler with a metrics snapshot");
    if (!snapshotCaptured)
    {
        handler->Release();
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> duringFirstHandler =
        handler->FirstSnapshot();
    ServerCoreTest::ExpectTrue(
        duringFirstHandler.IsOk(), "the handler read parse metrics from the JobRunner context");
    if (!duringFirstHandler.IsOk())
    {
        handler->Release();
        client.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(std::uint32_t{ 2 },
        duringFirstHandler.Value().configuredParseWorkerThreadCount,
        "metrics reports the configured parse worker count");
    ServerCoreTest::ExpectTrue(duringFirstHandler.Value().pendingParseTaskCount >= 1,
        "the first handler observes its still-reserved parse task");
    ServerCoreTest::ExpectTrue(duringFirstHandler.Value().pendingParseBytes >= 1,
        "the first handler observes its still-reserved parse bytes");
    ServerCoreTest::ExpectTrue(duringFirstHandler.Value().pendingParseTaskCount <=
                                   static_cast<std::size_t>(options.maxPendingParseTasks),
        "the observed parse task count stays within the configured session bound");
    ServerCoreTest::ExpectTrue(duringFirstHandler.Value().pendingParseBytes <=
                                   static_cast<std::size_t>(options.maxPendingParseBytes),
        "the observed parse bytes stay within the configured session bound");

    handler->Release();
    const bool handled = handler->WaitForCallCount(FrameCount, WaitLimit);
    ServerCoreTest::ExpectTrue(handled, "all ordered frames reached the parse worker handler");
    if (!handled)
    {
        client.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(
        FrameCount, handler->CallCount(), "the parse worker handler ran once for every frame");
    ServerCoreTest::ExpectTrue(handler->HasExpectedOrder(FrameCount),
        "one session preserves handler order after parse worker handoff");

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterDrain =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        afterDrain.IsOk(), "parse metrics were captured after the ordered drain");
    if (afterDrain.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::uint32_t{ 2 },
            afterDrain.Value().configuredParseWorkerThreadCount,
            "drained metrics retains the configured parse worker count");
        ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(FrameCount),
            afterDrain.Value().receivedFrameCount,
            "each ordered parse frame increments the received count once");
        ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterDrain.Value().pendingParseTaskCount,
            "all parse task reservations are released after the handler drain");
        ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterDrain.Value().pendingParseBytes,
            "all parse byte reservations are released after the handler drain");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 0 }, afterDrain.Value().errorCount,
            "the ordered parse worker path records no errors");
    }

    client.Close();
    host.Stop();
}

void ServerHostFallbackFinalizerReleasesQueuedParseWork()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the parse-finalizer fallback test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    constexpr std::size_t FrameCount = 3;
    const std::string message = "{\"type\":\"runtime.parse.fallback\",\"body\":{}}";
    const std::uint32_t bodySize = static_cast<std::uint32_t>(message.size());
    const std::vector<std::byte> frame = MakeFrame(message);
    std::vector<std::byte> frames;
    for (std::size_t index = 0; index < FrameCount; ++index)
    {
        frames.insert(frames.end(), frame.begin(), frame.end());
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 34);
    ServerCore::Runtime::ServerHost host;
    auto observer = std::make_shared<RetainingSessionObserver>();
    auto parseGate = std::make_shared<BlockingParseWorkerGate>();
    const ScopedBeforeParseGate parseGateScope(parseGate);

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.parseWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    options.maxBodySize = bodySize;
    options.maxPendingParseBytes = bodySize * static_cast<std::uint32_t>(FrameCount);
    options.maxTotalPendingParseBytes = options.maxPendingParseBytes;
    options.maxPendingParseTasks = static_cast<std::uint32_t>(FrameCount);
    options.maxTotalPendingParseTasks = options.maxPendingParseTasks;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted parse-finalizer fallback options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started for parse-finalizer fallback");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    const bool opened = connected && observer->WaitForOpenedCount(1, WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the parse-finalizer fallback session opened");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }

    const std::shared_ptr<ServerCore::Session::Session> retainedSession = observer->SessionAt(0);
    const bool sent = client.SendAll(frames);
    const bool workerBlocked = sent && parseGate->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(
        workerBlocked, "one parse task is active while two completed frames remain unscheduled");
    if (!workerBlocked)
    {
        parseGateScope.Release();
        client.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> beforeClose =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(beforeClose.IsOk(),
        "metrics captured all parse reservations before the fallback finalizer");
    if (beforeClose.IsOk())
    {
        ServerCoreTest::ExpectEqual(FrameCount, beforeClose.Value().pendingParseTaskCount,
            "one active and two queued parse tasks hold reservations before close");
        ServerCoreTest::ExpectEqual(bodySize * FrameCount, beforeClose.Value().pendingParseBytes,
            "all completed bodies hold parse bytes before close");
    }

    {
        const ScopedFinalizePostFailure failFinalizerPost;
        client.Close();
        const bool closed = observer->WaitForClosed(WaitLimit);
        ServerCoreTest::ExpectTrue(
            closed, "the forced finalizer fallback still emits a close notification");
        if (!closed)
        {
            parseGateScope.Release();
            host.Stop();
            return;
        }
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterFallback =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        afterFallback.IsOk(), "metrics captured parse accounting after fallback finalization");
    if (afterFallback.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterFallback.Value().activeSessionCount,
            "fallback finalization removes the closed retained session from the Registry");
        ServerCoreTest::ExpectEqual(std::size_t{ 1 }, afterFallback.Value().pendingParseTaskCount,
            "fallback finalization immediately releases both unscheduled parse tasks");
        ServerCoreTest::ExpectEqual(static_cast<std::size_t>(bodySize),
            afterFallback.Value().pendingParseBytes,
            "only the active worker body remains reserved after fallback finalization");
    }
    ServerCoreTest::ExpectTrue(
        retainedSession != nullptr &&
            retainedSession->State() == ServerCore::Session::SessionState::Closed,
        "game code still owns the closed Session while its queued parse budget is reclaimed");

    parseGateScope.Release();
    bool parseBudgetReleased = false;
    const auto deadline = std::chrono::steady_clock::now() + WaitLimit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> snapshot =
            CaptureMetrics(host);
        if (snapshot.IsOk() && snapshot.Value().pendingParseTaskCount == 0 &&
            snapshot.Value().pendingParseBytes == 0)
        {
            parseBudgetReleased = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ServerCoreTest::ExpectTrue(parseBudgetReleased,
        "the active worker releases its final reservation while the closed Session remains owned");

    host.Stop();
}

void ServerHostFallbackFinalizerAllowsReentrantDisconnect()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the reentrant finalizer test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 35);
    ServerCore::Runtime::ServerHost host;
    auto observer = std::make_shared<ReentrantCloseObserver>();
    auto connectionStartGate = std::make_shared<BlockingConnectionStartGate>();
    const ScopedBeforeConnectionStartGate gateScope(connectionStartGate);

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted reentrant finalizer options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(
        started.IsOk(), "ServerHost started for the reentrant finalizer test");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    const bool beforeConnectionStart =
        connected && connectionStartGate->WaitUntilEntered(WaitLimit);
    const bool opened = beforeConnectionStart && observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(
        opened, "the session opened while its Connection still had no pending receive operation");
    if (!opened)
    {
        gateScope.Release();
        client.Close();
        host.Stop();
        return;
    }

    const std::shared_ptr<ServerCore::Session::Session> session = observer->Session();
    ServerCoreTest::ExpectTrue(
        session != nullptr, "the reentrant finalizer observer retained its Session");
    if (session == nullptr)
    {
        gateScope.Release();
        client.Close();
        host.Stop();
        return;
    }

    {
        const ScopedFinalizePostFailure failFinalizerPost;
        session->Disconnect(
            ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed));
    }

    ServerCoreTest::ExpectTrue(observer->WaitForClosed(WaitLimit),
        "the synchronous fallback finalizer emitted its close notification");
    ServerCoreTest::ExpectTrue(observer->ReenteredBeforeCloseReturned(),
        "OnSessionClosed reentered Disconnect after the outer outbound lock was released");

    gateScope.Release();
    client.Close();
    host.Stop();
}

void ServerHostFallbackCloseNotificationCanStopHost()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the fallback close-stop test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 36);
    ServerCore::Runtime::ServerHost host;
    auto connectionStartGate = std::make_shared<BlockingConnectionStartGate>();
    const ScopedBeforeConnectionStartGate gateScope(connectionStartGate);
    auto observer = std::make_shared<StopFromCloseObserver>(host, connectionStartGate);

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted fallback close-stop options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(
        started.IsOk(), "ServerHost started for the fallback close-stop test");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    const bool beforeConnectionStart =
        connected && connectionStartGate->WaitUntilEntered(WaitLimit);
    const std::shared_ptr<ServerCore::Session::Session> session =
        beforeConnectionStart ? observer->WaitForSession(WaitLimit) : nullptr;
    ServerCoreTest::ExpectTrue(session != nullptr,
        "the close-stop session opened before its Connection started receive I/O");
    if (session == nullptr)
    {
        gateScope.Release();
        client.Close();
        host.Stop();
        return;
    }

    {
        const ScopedFinalizePostFailure failFinalizerPost;
        session->Disconnect(
            ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed));
    }

    ServerCoreTest::ExpectTrue(observer->StopReturned(),
        "fallback OnSessionClosed returned from ServerHost::Stop without waiting for itself");
    ServerCoreTest::ExpectTrue(
        !host.IsRunning(), "fallback OnSessionClosed completed the Host shutdown");

    client.Close();
    host.Stop();
}

void ServerHostFallbackCloseNotificationStopsStartingHost()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the Starting close-stop test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 38);
    ServerCore::Runtime::ServerHost host;
    auto connectionStartGate = std::make_shared<BlockingConnectionStartGate>();
    const ScopedBeforeConnectionStartGate connectionStartGateScope(connectionStartGate);
    auto hostRunningGate = std::make_shared<BlockingHostRunningGate>();
    const ScopedBeforeHostRunningGate hostRunningGateScope(hostRunningGate);
    auto observer = std::make_shared<StopFromCloseObserver>(host, connectionStartGate);

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted Starting close-stop options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);

    ServerCore::Core::Status startResult = ServerCore::Core::Status::Ok();
    std::mutex startResultMutex;
    bool startReturned = false;
    std::thread startThread(
        [&host, &startResult, &startResultMutex, &startReturned]()
        {
            ServerCore::Core::Status result = host.Start();
            {
                const std::lock_guard<std::mutex> guard(startResultMutex);
                startResult = std::move(result);
                startReturned = true;
            }
        });

    const bool beforeRunning = hostRunningGate->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(beforeRunning,
        "ServerHost paused after Acceptor start while its Lifecycle was still Starting");

    TestClient client;
    const bool connected = beforeRunning && client.Connect(port);
    const bool beforeConnectionStart =
        connected && connectionStartGate->WaitUntilEntered(WaitLimit);
    const std::shared_ptr<ServerCore::Session::Session> session =
        beforeConnectionStart ? observer->WaitForSession(WaitLimit) : nullptr;
    ServerCoreTest::ExpectTrue(session != nullptr,
        "the Starting Host opened a session before its Connection started receive I/O");

    bool stopReturnedBeforeRunning = false;
    if (session != nullptr)
    {
        const ScopedFinalizePostFailure failFinalizerPost;
        std::thread disconnectThread(
            [session]()
            {
                session->Disconnect(ServerCore::Core::Status::FailWithoutMessage(
                    ServerCore::Core::ErrorCode::Closed));
            });

        stopReturnedBeforeRunning = observer->WaitForStopReturned(std::chrono::seconds(2));
        ServerCoreTest::ExpectTrue(stopReturnedBeforeRunning,
            "fallback OnSessionClosed returned from Stop while Start remained at Starting");

        {
            const std::lock_guard<std::mutex> guard(startResultMutex);
            ServerCoreTest::ExpectTrue(!startReturned,
                "Start did not leave the deterministic Starting gate before it was released");
        }
        ServerCoreTest::ExpectTrue(!host.IsRunning(),
            "the gated ServerHost did not publish Running after the Stop request");
        ServerCoreTest::ExpectEqual(std::uint16_t{ 0 }, host.Port(),
            "the gated ServerHost did not publish its port after the Stop request");

        // 먼저 수락 handoff를 끝내야 Start가 수행할 Acceptor::Stop()이 안전하게 drain된다.
        connectionStartGateScope.Release();
        hostRunningGateScope.Release();
        disconnectThread.join();
    }
    else
    {
        connectionStartGateScope.Release();
        hostRunningGateScope.Release();
    }

    startThread.join();
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(startResult.Code()),
        "Start reports Closed after a close callback requested Stop during Starting");
    ServerCoreTest::ExpectTrue(
        !host.IsRunning(), "the deferred Starting Stop leaves ServerHost stopped");
    ServerCoreTest::ExpectEqual(
        std::uint16_t{ 0 }, host.Port(), "the deferred Starting Stop clears the published port");

    client.Close();
    host.Stop();
}

void ServerHostStopWaitsForCloseNotification()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the close-notification drain test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 37);
    ServerCore::Runtime::ServerHost host;
    auto observer = std::make_shared<BlockingCloseObserver>();

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted close-notification drain options");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(
        started.IsOk(), "ServerHost started for the close-notification drain test");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    const bool opened = connected && observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(opened, "the close-notification drain session opened");
    if (!opened)
    {
        observer->Release();
        client.Close();
        host.Stop();
        return;
    }

    client.Close();
    const bool closeEntered = observer->WaitForCloseEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(closeEntered, "OnSessionClosed entered its blocking observer");
    if (!closeEntered)
    {
        observer->Release();
        host.Stop();
        return;
    }

    std::atomic<bool> stopReturned = false;
    std::thread stopThread(
        [&]()
        {
            host.Stop();
            stopReturned.store(true, std::memory_order_release);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ServerCoreTest::ExpectTrue(!stopReturned.load(std::memory_order_acquire),
        "an external Stop waits while OnSessionClosed is still running");

    observer->Release();
    stopThread.join();

    ServerCoreTest::ExpectTrue(stopReturned.load(std::memory_order_acquire),
        "the external Stop returns after OnSessionClosed finishes");
    ServerCoreTest::ExpectTrue(
        observer->CloseReturned(), "the close observer returned before Host shutdown completed");
    host.Stop();
}

/// <summary>두 세션이 Host 전체 parse reservation을 동시에 요구하는 경로를 검사한다.</summary>
/// <remarks>
/// 첫 worker는 ParseReservation을 가진 채 gate에서 멈추지만 JobRunner는 비어 있다. 따라서 두 번째
/// 세션의 admission은 실제 전역 byte/task 계수와 경쟁한다. handler를 멈추는 방식이면 JobRunner도
/// 같이 멈춰 이 경쟁 자체를 만들 수 없다.
/// </remarks>
void VerifyGlobalParseBudget(const std::uint16_t port, const std::uint32_t totalByteFrameCount,
    const std::uint32_t totalTaskLimit)
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the aggregate parse budget test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::string message = "{\"type\":\"runtime.parse.budget\",\"body\":{}}";
    const std::uint32_t bodySize = static_cast<std::uint32_t>(message.size());
    const std::vector<std::byte> frame = MakeFrame(message);
    ServerCore::Runtime::ServerHost host;
    auto observer = std::make_shared<RecordingSessionObserver>();
    auto handler = std::make_shared<CountingParseHandler>();
    auto gate = std::make_shared<BlockingParseWorkerGate>();
    // 이 scope는 host보다 나중에 만들어져 모든 조기 반환에서도 Stop()보다 먼저 worker를 깨운다.
    const ScopedBeforeParseGate gateScope(gate);

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.parseWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    options.maxBodySize = bodySize;
    options.maxPendingParseBytes = bodySize;
    options.maxTotalPendingParseBytes = bodySize * totalByteFrameCount;
    options.maxPendingParseTasks = 1;
    options.maxTotalPendingParseTasks = totalTaskLimit;

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(configured.IsOk(), "ServerHost accepted an aggregate parse budget");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register(
        "runtime.parse.budget",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, message); },
        1024);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the aggregate parse budget handler registered");
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(
        started.IsOk(), "ServerHost started for the aggregate parse budget test");
    if (!started.IsOk())
    {
        return;
    }

    TestClient firstClient;
    const bool firstConnected = firstClient.Connect(port);
    ServerCoreTest::ExpectTrue(firstConnected, "the first aggregate parse budget client connected");
    if (!firstConnected)
    {
        return;
    }

    const bool firstOpened = observer->WaitForOpenedCount(1, WaitLimit);
    ServerCoreTest::ExpectTrue(firstOpened, "the first aggregate parse budget session opened");
    if (!firstOpened)
    {
        return;
    }
    const ServerCore::Session::SessionId firstId = observer->OpenedId();

    TestClient secondClient;
    const bool secondConnected = secondClient.Connect(port);
    ServerCoreTest::ExpectTrue(
        secondConnected, "the second aggregate parse budget client connected");
    if (!secondConnected)
    {
        return;
    }

    const bool bothOpened = observer->WaitForOpenedCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(
        bothOpened, "both aggregate parse budget sessions opened before their frames were sent");
    if (!bothOpened)
    {
        return;
    }
    const ServerCore::Session::SessionId secondId = observer->OpenedId();
    ServerCoreTest::ExpectTrue(
        firstId != secondId, "the aggregate parse budget clients received distinct session ids");

    const bool firstSent = firstClient.SendAll(frame);
    ServerCoreTest::ExpectTrue(firstSent, "the first aggregate parse budget frame was sent");
    if (!firstSent)
    {
        return;
    }

    const bool entered = gate->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(
        entered, "the first aggregate parse budget worker holds its reservation before parsing");
    if (!entered)
    {
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> firstReserved =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(firstReserved.IsOk(),
        "metrics captured the first aggregate parse reservation while the worker was gated");
    if (!firstReserved.IsOk())
    {
        return;
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 2 }, firstReserved.Value().activeSessionCount,
        "both sessions stay active while the first parse worker holds its reservation");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(bodySize),
        firstReserved.Value().pendingParseBytes,
        "the first worker holds exactly one aggregate parse body reservation");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, firstReserved.Value().pendingParseTaskCount,
        "the first worker holds exactly one aggregate parse task reservation");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 1 }, firstReserved.Value().receivedFrameCount,
        "the first aggregate parse frame is counted before worker parsing begins");

    const bool secondSent = secondClient.SendAll(frame);
    ServerCoreTest::ExpectTrue(secondSent,
        "the second aggregate parse budget frame reached the Host while the first was gated");
    if (!secondSent)
    {
        return;
    }

    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(
        closed, "the Host closes the second session when the aggregate parse budget rejects it");
    if (!closed)
    {
        return;
    }
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(secondId),
        static_cast<std::uint64_t>(observer->ClosedId()),
        "only the second session is closed by the aggregate parse budget");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(observer->CloseCode()),
        "aggregate parse budget overflow keeps a TooLarge close reason");
    if (observer->ClosedId() != secondId)
    {
        return;
    }

    const bool peerClosed = secondClient.WaitForPeerClose();
    ServerCoreTest::ExpectTrue(
        peerClosed, "the rejected aggregate parse budget client observes its close");
    if (!peerClosed)
    {
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterRejected =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(afterRejected.IsOk(),
        "metrics captured the remaining aggregate parse reservation after rejection");
    if (!afterRejected.IsOk())
    {
        return;
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, afterRejected.Value().activeSessionCount,
        "the rejected aggregate parse session no longer counts as active");
    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(bodySize),
        afterRejected.Value().pendingParseBytes,
        "rejection leaves only the first aggregate parse byte reservation");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, afterRejected.Value().pendingParseTaskCount,
        "rejection leaves only the first aggregate parse task reservation");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 1 }, afterRejected.Value().receivedFrameCount,
        "the rejected frame never becomes a received parse frame");

    gateScope.Release();
    const bool firstHandled = handler->WaitForCallCount(1, WaitLimit);
    ServerCoreTest::ExpectTrue(firstHandled,
        "the first aggregate parse frame reached its handler after the gate was released");
    if (!firstHandled)
    {
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterDrain =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(afterDrain.IsOk(),
        "metrics captured the drained aggregate parse reservation after handler completion");
    if (!afterDrain.IsOk())
    {
        return;
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterDrain.Value().pendingParseBytes,
        "the first aggregate parse byte reservation is released after handler completion");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterDrain.Value().pendingParseTaskCount,
        "the first aggregate parse task reservation is released after handler completion");

    const bool recoverySent = firstClient.SendAll(frame);
    ServerCoreTest::ExpectTrue(recoverySent,
        "the admitted session can send another frame after aggregate budget recovery");
    if (!recoverySent)
    {
        return;
    }
    const bool recoveryHandled = handler->WaitForCallCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(recoveryHandled,
        "the admitted session dispatches again after aggregate parse budget recovery");
    ServerCoreTest::ExpectTrue(handler->BodiesWereExpected(),
        "the aggregate parse budget handler received only the expected envelopes");
}

void ServerHostBoundsTotalPendingParseBytes()
{
    // 첫 reservation 하나가 전체 byte 예산을 모두 채우고, task 예산은 둘째 frame을 막지 않는다.
    VerifyGlobalParseBudget(static_cast<std::uint16_t>(PortBase + 18), 1, 2);
}

void ServerHostRollsBackParseBytesWhenTotalTaskLimitRejects()
{
    // 둘째 frame은 byte를 먼저 잡을 수 있지만 task reservation에서 거절된다. 그 byte가 즉시
    // 반납되지 않으면 VerifyGlobalParseBudget의 afterRejected 지표가 두 frame 크기로 남는다.
    VerifyGlobalParseBudget(static_cast<std::uint16_t>(PortBase + 19), 2, 1);
}

void ServerHostBoundsPendingReceiveBytes()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the receive budget test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 7);
    const std::vector<std::byte> frame = MakeFrame("{\"type\":\"runtime.block\",\"body\":{}}");
    auto observer = std::make_shared<RecordingSessionObserver>();
    auto handler = std::make_shared<BlockingReceiveHandler>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    // 첫 frame 하나는 수신 분할과 무관하게 정확히 이 상한까지 보관할 수 있다. 처리 중인 첫
    // batch에는 적어도 한 바이트가 남으므로, 같은 frame 하나를 더 보내면 그 조각들의 합은
    // 반드시 이 상한을 넘는다.
    options.maxPendingReceiveBytes = static_cast<std::uint32_t>(frame.size());

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted a finite pending receive budget");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register(
        "runtime.block",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, *message.Body()); },
        1024);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the blocking receive handler registered");
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "ServerHost started for the receive budget test");
    if (!started.IsOk())
    {
        return;
    }

    TestClient client;
    const bool connected = client.Connect(port);
    ServerCoreTest::ExpectTrue(connected, "the receive budget client connected");
    if (!connected)
    {
        host.Stop();
        return;
    }

    const bool opened = observer->WaitForOpened(WaitLimit);
    ServerCoreTest::ExpectTrue(
        opened, "the receive budget session opened before its frames were sent");
    if (!opened)
    {
        client.Close();
        host.Stop();
        return;
    }
    const ServerCore::Session::SessionId firstId = observer->OpenedId();

    ServerCoreTest::ExpectEqual(static_cast<std::size_t>(options.maxPendingReceiveBytes),
        frame.size(), "one valid frame exactly fits the pending receive budget");
    const bool firstSent = client.SendAll(frame);
    ServerCoreTest::ExpectTrue(firstSent, "the first frame was sent to the blocking handler");
    if (!firstSent)
    {
        handler->Release();
        host.Stop();
        return;
    }

    const bool entered = handler->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(entered, "the first handler is intentionally holding the JobRunner");
    if (!entered)
    {
        handler->Release();
        host.Stop();
        return;
    }

    const bool secondSent = client.SendAll(frame);
    ServerCoreTest::ExpectTrue(
        secondSent, "the second frame reached the Host while the first was blocked");
    if (!secondSent)
    {
        handler->Release();
        host.Stop();
        return;
    }

    // 수신 상한 거절은 I/O callback에서 Connection을 즉시 닫지만, observer 정리는 JobRunner에
    // 기다린다. 따라서 먼저 peer close를 보면 첫 처리기를 너무 일찍 풀어 예산을 되돌리는
    // 시간 경쟁 없이 실제 거절을 확인할 수 있다.
    const bool peerClosed = client.WaitForPeerClose();
    ServerCoreTest::ExpectTrue(peerClosed,
        "the receive-budget client observed its close before the first handler was released");
    handler->Release();
    if (!peerClosed)
    {
        host.Stop();
        return;
    }

    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(
        closed, "the Host closes only the session that exceeds its receive budget");
    if (!closed)
    {
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(firstId),
        static_cast<std::uint64_t>(observer->ClosedId()),
        "the receive budget closes the session that sent the overflowing frame");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(observer->CloseCode()),
        "receive budget overflow keeps a TooLarge close reason");

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterClose =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(afterClose.IsOk(),
        "metrics captured the receive budget after the overflowing session closed");
    if (!afterClose.IsOk())
    {
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterClose.Value().activeSessionCount,
        "the overflowing receive-budget session no longer counts as active");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterClose.Value().pendingReceiveBytes,
        "closing the overflowing session releases every pending receive byte");

    client.Close();

    TestClient recoveryClient;
    const bool recoveryConnected = recoveryClient.Connect(port);
    ServerCoreTest::ExpectTrue(
        recoveryConnected, "a new client connected after the receive budget was reclaimed");
    if (!recoveryConnected)
    {
        host.Stop();
        return;
    }
    const bool recoveryOpened = observer->WaitForOpenedCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(
        recoveryOpened, "the recovered receive-budget client opened a new session");
    if (!recoveryOpened)
    {
        recoveryClient.Close();
        host.Stop();
        return;
    }
    const bool recoverySent = recoveryClient.SendAll(frame);
    ServerCoreTest::ExpectTrue(recoverySent,
        "the recovered receive-budget client sent a frame after the budget was reclaimed");
    if (!recoverySent)
    {
        recoveryClient.Close();
        host.Stop();
        return;
    }
    const bool recoveryHandled = handler->WaitForCallCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(recoveryHandled,
        "the recovered receive-budget client reached the handler after the budget was reclaimed");
    if (!recoveryHandled)
    {
        recoveryClient.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterRecovery =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(
        afterRecovery.IsOk(), "metrics captured the recovered receive-budget session");
    if (afterRecovery.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::size_t{ 1 }, afterRecovery.Value().activeSessionCount,
            "the recovered receive-budget client is the only active session");
        ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterRecovery.Value().pendingReceiveBytes,
            "the recovered receive-budget frame releases its receive bytes after dispatch");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 2 }, afterRecovery.Value().receivedFrameCount,
            "only the first and recovered receive-budget frames reach frame dispatch");
    }

    recoveryClient.Close();
    host.Stop();
}

void ServerHostBoundsTotalPendingReceiveBytes()
{
    const WinsockGuard winsock;
    ServerCoreTest::ExpectTrue(
        winsock.IsReady(), "WSAStartup() for the aggregate receive budget test succeeded");
    if (!winsock.IsReady())
    {
        return;
    }

    const std::vector<std::byte> frame = MakeFrame("{\"type\":\"runtime.block\",\"body\":{}}");
    const std::uint16_t port = static_cast<std::uint16_t>(PortBase + 8);
    auto observer = std::make_shared<RecordingSessionObserver>();
    auto handler = std::make_shared<BlockingReceiveHandler>();
    ServerCore::Runtime::ServerHost host;

    ServerCore::Runtime::ServerHostOptions options;
    options.port = port;
    options.ioWorkerThreadCount = 1;
    options.acceptBacklog = 8;
    options.maxPendingReceiveBytes = 64;
    // 첫 번째 프레임이 처리기 안에 머무는 동안 이 전체 예산을 모두 점유한다. 두 번째 연결의
    // 첫 수신 바이트는 세션별 여유가 있어도 Host 전체 예산에서 거절되어야 한다.
    options.maxTotalPendingReceiveBytes = static_cast<std::uint32_t>(frame.size());

    ServerCoreTest::ExpectTrue(
        frame.size() < static_cast<std::size_t>(options.maxPendingReceiveBytes),
        "one valid frame stays below the per-session receive budget");

    const ServerCore::Core::Status configured = host.Configure(options);
    ServerCoreTest::ExpectTrue(
        configured.IsOk(), "ServerHost accepted an aggregate receive budget");
    if (!configured.IsOk())
    {
        return;
    }

    host.SetSessionObserver(observer);
    const ServerCore::Core::Status registered = host.GetDispatcher().Register(
        "runtime.block",
        [handler](const std::shared_ptr<ServerCore::Session::Session>& session,
            const ServerCore::Protocol::Message& message)
        { return handler->Handle(session, *message.Body()); },
        1024);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the aggregate-budget handler registered");
    if (!registered.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status started = host.Start();
    ServerCoreTest::ExpectTrue(
        started.IsOk(), "ServerHost started for the aggregate receive budget test");
    if (!started.IsOk())
    {
        return;
    }

    TestClient firstClient;
    const bool firstConnected = firstClient.Connect(port);
    ServerCoreTest::ExpectTrue(firstConnected, "the first aggregate-budget client connected");
    if (!firstConnected)
    {
        host.Stop();
        return;
    }

    const bool firstOpened = observer->WaitForOpenedCount(1, WaitLimit);
    ServerCoreTest::ExpectTrue(firstOpened,
        "the first aggregate receive-budget session opened before the second connected");
    if (!firstOpened)
    {
        firstClient.Close();
        host.Stop();
        return;
    }
    const ServerCore::Session::SessionId firstId = observer->OpenedId();

    TestClient secondClient;
    const bool secondConnected = secondClient.Connect(port);
    ServerCoreTest::ExpectTrue(secondConnected, "the second aggregate-budget client connected");
    if (!secondConnected)
    {
        firstClient.Close();
        host.Stop();
        return;
    }

    const bool bothOpened = observer->WaitForOpenedCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(bothOpened,
        "both aggregate-budget sessions opened before their receive budget is exercised");
    if (!bothOpened)
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }
    const ServerCore::Session::SessionId secondId = observer->OpenedId();
    ServerCoreTest::ExpectTrue(
        firstId != secondId, "the aggregate receive-budget clients received distinct session ids");

    const bool firstSent = firstClient.SendAll(frame);
    ServerCoreTest::ExpectTrue(firstSent, "the first aggregate-budget frame was sent");
    if (!firstSent)
    {
        handler->Release();
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }

    const bool entered = handler->WaitUntilEntered(WaitLimit);
    ServerCoreTest::ExpectTrue(entered, "the first aggregate-budget handler holds the JobRunner");
    if (!entered)
    {
        handler->Release();
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }

    const bool secondSent = secondClient.SendAll(frame);
    ServerCoreTest::ExpectTrue(
        secondSent, "the second client sent data while the total budget was full");
    if (!secondSent)
    {
        handler->Release();
        firstClient.Close();
        host.Stop();
        return;
    }

    // aggregate receive reservation은 I/O callback에서 잡힌다. JobRunner의 첫 handler를 먼저
    // 풀면 그 reservation도 풀려 둘째 callback이 정상 수락될 수 있으므로, peer close를 먼저
    // 확인해 두 세션이 실제로 전역 상한에서 경쟁했음을 고정한다.
    const bool peerClosed = secondClient.WaitForPeerClose();
    ServerCoreTest::ExpectTrue(peerClosed, "the rejected aggregate receive-budget client observed "
                                           "its close before the first handler was released");
    handler->Release();
    if (!peerClosed)
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }

    const bool closed = observer->WaitForClosed(WaitLimit);
    ServerCoreTest::ExpectTrue(
        closed, "the Host closes the connection that exceeds the aggregate budget");
    if (!closed)
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(static_cast<std::uint64_t>(secondId),
        static_cast<std::uint64_t>(observer->ClosedId()),
        "the aggregate receive budget closes only the second session");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(observer->CloseCode()),
        "aggregate receive budget overflow keeps a TooLarge close reason");

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterRejected =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(afterRejected.IsOk(),
        "metrics captured the admitted session after aggregate receive-budget rejection");
    if (!afterRejected.IsOk())
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, afterRejected.Value().activeSessionCount,
        "the first aggregate receive-budget session remains active after the second is rejected");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterRejected.Value().pendingReceiveBytes,
        "releasing the first handler drains the aggregate receive budget to zero");
    ServerCoreTest::ExpectEqual(std::uint64_t{ 1 }, afterRejected.Value().receivedFrameCount,
        "the rejected aggregate receive-budget frame never reaches frame dispatch");

    const bool recoverySent = firstClient.SendAll(frame);
    ServerCoreTest::ExpectTrue(recoverySent,
        "the admitted client sent another frame after the aggregate budget was reclaimed");
    if (!recoverySent)
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }
    const bool recoveryHandled = handler->WaitForCallCount(2, WaitLimit);
    ServerCoreTest::ExpectTrue(recoveryHandled,
        "the admitted client dispatches again after aggregate receive-budget recovery");
    if (!recoveryHandled)
    {
        firstClient.Close();
        secondClient.Close();
        host.Stop();
        return;
    }

    const ServerCore::Core::Result<ServerCore::Runtime::ServerMetricsSnapshot> afterRecovery =
        CaptureMetrics(host);
    ServerCoreTest::ExpectTrue(afterRecovery.IsOk(),
        "metrics captured aggregate receive-budget recovery after the second dispatch");
    if (afterRecovery.IsOk())
    {
        ServerCoreTest::ExpectEqual(std::size_t{ 1 }, afterRecovery.Value().activeSessionCount,
            "the admitted aggregate receive-budget session stays active after recovery");
        ServerCoreTest::ExpectEqual(std::size_t{ 0 }, afterRecovery.Value().pendingReceiveBytes,
            "the recovered aggregate receive-budget frame releases its receive bytes after "
            "dispatch");
        ServerCoreTest::ExpectEqual(std::uint64_t{ 2 }, afterRecovery.Value().receivedFrameCount,
            "only admitted aggregate receive-budget frames reach frame dispatch");
    }

    firstClient.Close();
    secondClient.Close();
    host.Stop();
}

const ServerCoreTest::CheckRegistration gServerHostRoutesSplitFramesAndStops{
    "Runtime.ServerHostRoutesSplitFramesAndStops", &ServerHostRoutesSplitFramesAndStops
};
const ServerCoreTest::CheckRegistration gServerHostRejectsRestartAfterStop{
    "Runtime.ServerHostRejectsRestartAfterStop", &ServerHostRejectsRestartAfterStop
};
const ServerCoreTest::CheckRegistration gServerHostRejectsInvalidOptions{
    "Runtime.ServerHostRejectsInvalidOptions", &ServerHostRejectsInvalidOptions
};
const ServerCoreTest::CheckRegistration gServerHostSharesConfiguredSendBudgetAcrossSessions{
    "Runtime.ServerHostSharesConfiguredSendBudgetAcrossSessions",
    &ServerHostSharesConfiguredSendBudgetAcrossSessions
};
const ServerCoreTest::CheckRegistration gServerHostEnforcesConcurrentSessionLimit{
    "Runtime.ServerHostEnforcesConcurrentSessionLimit", &ServerHostEnforcesConcurrentSessionLimit
};
const ServerCoreTest::CheckRegistration gServerHostConfiguresFromConfig{
    "Runtime.ServerHostConfiguresFromConfig", &ServerHostConfiguresFromConfig
};
const ServerCoreTest::CheckRegistration gServerHostRejectsInvalidConfig{
    "Runtime.ServerHostRejectsInvalidConfig", &ServerHostRejectsInvalidConfig
};
const ServerCoreTest::CheckRegistration gServerHostBindsRegistryBeforePrepostedWork{
    "Runtime.ServerHostBindsRegistryBeforePrepostedWork",
    &ServerHostBindsRegistryBeforePrepostedWork
};
const ServerCoreTest::CheckRegistration gServerHostSendsFinalFrameBeforeDisconnect{
    "Runtime.ServerHostSendsFinalFrameBeforeDisconnect", &ServerHostSendsFinalFrameBeforeDisconnect
};
const ServerCoreTest::CheckRegistration gServerHostDisconnectsIdleSessions{
    "Runtime.ServerHostDisconnectsIdleSessions", &ServerHostDisconnectsIdleSessions
};
const ServerCoreTest::CheckRegistration gServerHostPreparedSendsPreserveFramingLimitsAndMetrics{
    "Runtime.ServerHostPreparedSendsPreserveFramingLimitsAndMetrics",
    &ServerHostPreparedSendsPreserveFramingLimitsAndMetrics
};
const ServerCoreTest::CheckRegistration gServerHostBoundsGracefulCloseWithoutIdleTimeout{
    "Runtime.ServerHostBoundsGracefulCloseWithoutIdleTimeout", &ServerHostBoundsGracefulCloseWithoutIdleTimeout
};
const ServerCoreTest::CheckRegistration gServerHostFailedStartSerializesOwnerResetWithStop{
    "Runtime.ServerHostFailedStartSerializesOwnerResetWithStop", &ServerHostFailedStartSerializesOwnerResetWithStop
};
const ServerCoreTest::CheckRegistration gServerHostConcurrentStopsWaitForBlockedHandler{
    "Runtime.ServerHostConcurrentStopsWaitForBlockedHandler",
    &ServerHostConcurrentStopsWaitForBlockedHandler
};
const ServerCoreTest::CheckRegistration gServerHostReportsMetrics{
    "Runtime.ServerHostReportsMetrics", &ServerHostReportsMetrics
};
const ServerCoreTest::CheckRegistration gServerHostParseWorkersPreserveSessionHandlerOrder{
    "Runtime.ServerHostParseWorkersPreserveSessionHandlerOrder",
    &ServerHostParseWorkersPreserveSessionHandlerOrder
};
const ServerCoreTest::CheckRegistration gServerHostFallbackFinalizerReleasesQueuedParseWork{
    "Runtime.ServerHostFallbackFinalizerReleasesQueuedParseWork",
    &ServerHostFallbackFinalizerReleasesQueuedParseWork
};
const ServerCoreTest::CheckRegistration gServerHostFallbackFinalizerAllowsReentrantDisconnect{
    "Runtime.ServerHostFallbackFinalizerAllowsReentrantDisconnect",
    &ServerHostFallbackFinalizerAllowsReentrantDisconnect
};
const ServerCoreTest::CheckRegistration gServerHostFallbackCloseNotificationCanStopHost{
    "Runtime.ServerHostFallbackCloseNotificationCanStopHost",
    &ServerHostFallbackCloseNotificationCanStopHost
};
const ServerCoreTest::CheckRegistration gServerHostFallbackCloseNotificationStopsStartingHost{
    "Runtime.ServerHostFallbackCloseNotificationStopsStartingHost",
    &ServerHostFallbackCloseNotificationStopsStartingHost
};
const ServerCoreTest::CheckRegistration gServerHostStopWaitsForCloseNotification{
    "Runtime.ServerHostStopWaitsForCloseNotification", &ServerHostStopWaitsForCloseNotification
};
const ServerCoreTest::CheckRegistration gServerHostBoundsTotalPendingParseBytes{
    "Runtime.ServerHostBoundsTotalPendingParseBytes", &ServerHostBoundsTotalPendingParseBytes
};
const ServerCoreTest::CheckRegistration gServerHostRollsBackParseBytesWhenTotalTaskLimitRejects{
    "Runtime.ServerHostRollsBackParseBytesWhenTotalTaskLimitRejects",
    &ServerHostRollsBackParseBytesWhenTotalTaskLimitRejects
};
const ServerCoreTest::CheckRegistration gServerHostBoundsPendingReceiveBytes{
    "Runtime.ServerHostBoundsPendingReceiveBytes", &ServerHostBoundsPendingReceiveBytes
};
const ServerCoreTest::CheckRegistration gServerHostBoundsTotalPendingReceiveBytes{
    "Runtime.ServerHostBoundsTotalPendingReceiveBytes", &ServerHostBoundsTotalPendingReceiveBytes
};
}
