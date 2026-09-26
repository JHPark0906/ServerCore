#pragma once

// ServerHost::State의 정의다. ServerHost 구현 파일들만 include한다.
#include "Runtime/ServerHostInternal.h"

namespace ServerCore::Runtime
{
class ServerHost::State : public std::enable_shared_from_this<State>
{
public:
    class NetworkSession;

    State() = default;

    ~State() = default;

    Core::Status Configure(const Core::Config& config);
    Core::Status Configure(const ServerHostOptions& options);
    Core::Status SetLogger(std::shared_ptr<Core::ILogger> logger);
    Core::Status SetBinaryHandler(ServerHost::BinaryHandler handler);
    Core::Status AttachDatagramTransport(std::shared_ptr<DatagramTransport> transport);
    Core::Result<Protocol::DatagramCodec::Token> GetDatagramToken(Session::SessionId id) const;
    void LogStatus(Core::LogLevel level, const Core::Status& status,
        Session::SessionId session = Session::SessionId::Invalid) const noexcept
    {
        if (!mLogger)
            return;
        // 설명 없는 Status(FailWithoutMessage)도 원인을 알 수 있게 오류 코드 수치를 함께 남긴다.
        char code[12]{};
        const auto converted =
            std::to_chars(code, code + sizeof(code), static_cast<int>(status.Code()));
        const Core::LogField fields[]{ { "error_code",
            std::string_view(code, static_cast<std::size_t>(converted.ptr - code)) } };
        (void)Core::WriteLog(*mLogger,
            { level, status.Message(), fields, { 0, static_cast<std::uint64_t>(session), 0, 0 } });
    }
    /// <summary>원격 메시지 하나 때문에 생긴 실패를 Host의 속도 제한 아래 Warn으로 남긴다.</summary>
    void LogMessageFailure(
        const Core::Status& status, const Session::SessionId session) const noexcept
    {
        if (!mLogger || (mMessageLogLimiter && !mMessageLogLimiter->Admit(*mLogger)))
            return;
        LogStatus(Core::LogLevel::Warn, status, session);
    }
    Dispatch::Dispatcher& GetDispatcher() noexcept;
    const Session::SessionRegistry& GetSessions() const noexcept;
    [[nodiscard]] JobRunner::Lease GetJobRunner() const noexcept;
    Core::Status SetSessionObserver(std::weak_ptr<Session::ISessionObserver> observer);
    Core::Status Start();
    Core::Status BeginDrain();
    Core::Status DrainStatus() const;
    Core::Status StopGracefully(std::chrono::steady_clock::time_point deadline);
    void Stop();
    int Run();
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::uint16_t Port() const noexcept;
    [[nodiscard]] Core::Result<ServerMetricsSnapshot> SnapshotMetrics() const;

    void OnConnectionAccepted(std::shared_ptr<Net::Connection> connection) noexcept;
    [[nodiscard]] bool IsAccepting() const noexcept;
    void TrackNetworkSession(const std::shared_ptr<NetworkSession>& session) noexcept;
    void FinishNetworkSession(
        const NetworkSession* session, bool beginCloseNotification = false) noexcept;
    void FinishCloseNotification() noexcept;
    void NotifyOpened(const std::shared_ptr<Session::Session>& session);
    void NotifyAuthenticated(const std::shared_ptr<Session::Session>& session);
    void NotifyClosed(Session::SessionId id, Core::Status reason);
    [[nodiscard]] bool TryReservePendingReceiveBytes(std::size_t byteCount) noexcept;
    void ReleasePendingReceiveBytes(std::size_t byteCount) noexcept;
    [[nodiscard]] bool TryReservePendingParseWork(
        std::size_t byteCount, std::size_t taskCount) noexcept;
    void ReleasePendingParseWork(std::size_t byteCount, std::size_t taskCount) noexcept;
    void RecordReceivedFrame() noexcept;
    void RecordQueuedSendFrame() noexcept;
    void RecordError(const Core::Status& status) noexcept;

private:
    enum class Lifecycle
    {
        Ready,
        Starting,
        Running,
        Stopping,
        Stopped
    };

    Core::Status StartJobRunner();
    void CompleteFailedStart() noexcept;
    void CloseAllSessions() noexcept;
    void CloseExpiredSessionsOnRunner();
    void AdvanceDrain();
    [[nodiscard]] bool BeginNetworkSession();

    mutable std::mutex mLifecycleMutex;
    std::condition_variable mLifecycleChanged;
    Lifecycle mLifecycle = Lifecycle::Ready;
    bool mConfigured = false;
    // Start가 성공으로 돌아갔는지다. Run은 이것으로 이미 끝난 정상 종료와 시작 전·시작 실패를 가른다.
    bool mStartSucceeded = false;
    bool mStopRequestedDuringStart = false;
    bool mDrainStarting = false;
    bool mDrainSendsStarted = false;
    std::size_t mOutstandingSessions = 0;
    std::size_t mInFlightCloseNotifications = 0;
    std::vector<std::shared_ptr<NetworkSession>> mSessionSlots;
    // 비어 있는 mSessionSlots 번호의 스택이다. Start가 슬롯 수만큼 용량을 미리 잡으므로 수락과 종료는
    // 할당 없이 O(1)로 슬롯을 주고받는다. mSessionSlots와 함께 mLifecycleMutex가 보호한다.
    std::vector<std::size_t> mFreeSessionSlots;

    ServerHostOptions mOptions;
    std::shared_ptr<Core::ILogger> mLogger;
    // Start가 만들고 그 뒤로 바뀌지 않는다. Dispatcher에 준 기록기도 같은 제한을 쓴다.
    std::shared_ptr<MessageLogLimiter> mMessageLogLimiter;
    std::shared_ptr<DatagramTransport> mDatagrams;
    ServerHost::BinaryHandler mBinaryHandler;
    std::weak_ptr<Session::ISessionObserver> mSessionObserver;

    Dispatch::Dispatcher mDispatcher;
    Session::SessionRegistry mRegistry;
    JobRunner mJobRunner;
    std::thread mJobThread;
    std::unique_ptr<PeriodicRunner> mSessionTimeoutRunner;
    std::unique_ptr<ParseWorkerPool> mParsePool;
    std::unique_ptr<Net::IoContext> mIo;
    std::unique_ptr<Net::Acceptor> mAcceptor;

    std::atomic<bool> mAccepting{ false };
    std::atomic<bool> mDraining{ false };
    std::atomic<bool> mRunning{ false };
    std::atomic<std::uint16_t> mPort{ 0 };
    std::atomic<std::size_t> mPendingReceiveBytes{ 0 };
    std::atomic<std::size_t> mPendingParseBytes{ 0 };
    std::atomic<std::size_t> mPendingParseTasks{ 0 };
    std::atomic<std::uint64_t> mReceivedFrameCount{ 0 };
    std::atomic<std::uint64_t> mQueuedSendFrameCount{ 0 };
    std::atomic<std::uint64_t> mErrorCount{ 0 };
};
}
