#include "ServerCore/Runtime/ServerHost.h"

#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/Clock.h"
#include "ServerCore/Core/JobQueue.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/IoContext.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/PeriodicRunner.h"

#include "Net/AcceptorInternal.h"
#include "Net/ConnectionInternal.h"
#include "Runtime/ParseWorkerPoolInternal.h"
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
#include "Runtime/ServerHostTestAccess.h"
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ServerCore::Runtime
{
namespace
{
// FrameReader는 첫 수신 때 header + maximumBodySize 저장소를 만들므로, 설정 하나로 연결마다
// 수 GiB를 잡지 못하게 Host 경계에서 제한한다. 이 값은 wire 규격 상한이 아니라 서버 보호 상한이다.
constexpr std::uint32_t MaximumHostBodySize = 16u * 1024u * 1024u;
constexpr std::uint32_t MaximumHostPendingReceiveBytes = 16u * 1024u * 1024u;
constexpr std::uint32_t MaximumHostConcurrentSessions = 65536;
constexpr std::uint32_t MaximumHostTotalPendingReceiveBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t MaximumHostPendingParseBytes = 16u * 1024u * 1024u;
constexpr std::uint32_t MaximumHostTotalPendingParseBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t MaximumHostPendingParseTasks = 4096;
constexpr std::uint32_t MaximumHostTotalPendingParseTasks = 65536;
// 65,536개 세션의 8 KiB 본문과 각 4-byte 머리는 512.25 MiB다. 연결 상한을 늘려도
// 큰 본문 설정까지 무제한 허용하지 않도록 FrameReader 저장소 합계는 별도로 제한한다.
constexpr std::size_t MaximumHostFrameReaderStorageBytes = 1024u * 1024u * 1024u;
constexpr std::uint32_t MaximumHostTotalSendQueueCapacityBytes = 512u * 1024u * 1024u;
constexpr std::chrono::milliseconds MinimumSessionTimeoutScanPeriod{ 10 };
constexpr std::chrono::milliseconds MaximumSessionTimeoutScanPeriod{ 1000 };

struct CloseNotificationThreadContext
{
    const void* host = nullptr;
    const CloseNotificationThreadContext* previous = nullptr;
};

thread_local const CloseNotificationThreadContext* gCloseNotificationContext = nullptr;

/// <summary>현재 스레드가 이 Host의 OnSessionClosed 안에 중첩된 깊이다.</summary>
[[nodiscard]] std::size_t CurrentCloseNotificationDepth(const void* const host) noexcept
{
    std::size_t depth = 0;
    for (const CloseNotificationThreadContext* context = gCloseNotificationContext;
        context != nullptr; context = context->previous)
    {
        if (context->host == host)
        {
            ++depth;
        }
    }
    return depth;
}

/// <summary>다른 Host까지 중첩될 수 있는 종료 통지의 스레드 문맥을 되돌린다.</summary>
class ScopedCloseNotificationContext final
{
public:
    explicit ScopedCloseNotificationContext(const void* const host) noexcept
        : mContext{ host, gCloseNotificationContext }
    {
        gCloseNotificationContext = &mContext;
    }

    ~ScopedCloseNotificationContext() { gCloseNotificationContext = mContext.previous; }

    ScopedCloseNotificationContext(const ScopedCloseNotificationContext&) = delete;
    ScopedCloseNotificationContext& operator=(const ScopedCloseNotificationContext&) = delete;

private:
    CloseNotificationThreadContext mContext;
};

constexpr std::string_view HostConfigPortKey = "servercore.host.port";
constexpr std::string_view HostConfigListenAddressKey = "servercore.host.listen-address";
constexpr std::string_view HostConfigIoWorkerThreadCountKey =
    "servercore.host.io-worker-thread-count";
constexpr std::string_view HostConfigParseWorkerThreadCountKey =
    "servercore.host.parse-worker-thread-count";
constexpr std::string_view HostConfigAcceptBacklogKey = "servercore.host.accept-backlog";
constexpr std::string_view HostConfigIdleSessionTimeoutKey =
    "servercore.host.idle-session-timeout-ms";
constexpr std::string_view HostConfigGracefulCloseTimeoutKey =
    "servercore.host.graceful-close-timeout-ms";
constexpr std::string_view HostConfigMaxBodySizeKey = "servercore.host.max-body-size";
constexpr std::string_view HostConfigMaxConcurrentSessionsKey =
    "servercore.host.max-concurrent-sessions";
constexpr std::string_view HostConfigMaxTotalSendQueueCapacityBytesKey =
    "servercore.host.max-total-send-queue-capacity-bytes";
constexpr std::string_view HostConfigMaxPendingReceiveBytesKey =
    "servercore.host.max-pending-receive-bytes";
constexpr std::string_view HostConfigMaxTotalPendingReceiveBytesKey =
    "servercore.host.max-total-pending-receive-bytes";
constexpr std::string_view HostConfigMaxPendingParseBytesKey =
    "servercore.host.max-pending-parse-bytes";
constexpr std::string_view HostConfigMaxTotalPendingParseBytesKey =
    "servercore.host.max-total-pending-parse-bytes";
constexpr std::string_view HostConfigMaxPendingParseTasksKey =
    "servercore.host.max-pending-parse-tasks";
constexpr std::string_view HostConfigMaxTotalPendingParseTasksKey =
    "servercore.host.max-total-pending-parse-tasks";

[[nodiscard]] std::chrono::milliseconds SessionTimeoutScanPeriod(
    const ServerHostOptions& options) noexcept
{
    const std::chrono::milliseconds timeout = options.idleSessionTimeout.count() == 0
                                                 ? options.gracefulCloseTimeout
                                                 : std::min(options.idleSessionTimeout,
                                                       options.gracefulCloseTimeout);
    // 유휴 검사가 꺼져 있어도 graceful 종료 기한은 검사한다. 짧은 제한이 긴 제한의 검사 주기를
    // 기다리지 않게 하고, 아주 작은 값에서도 busy loop를 만들지는 않는다.
    return std::clamp(timeout / 4, MinimumSessionTimeoutScanPeriod, MaximumSessionTimeoutScanPeriod);
}

[[nodiscard]] Core::Status ValidateOptions(const ServerHostOptions& options)
{
    if (options.port == 0)
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "ServerHost port must not be zero");
    }
    if (options.ioWorkerThreadCount <= 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost I/O worker thread count must be positive");
    }
    if (options.ioWorkerThreadCount > MaximumServerHostWorkerThreadCount)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost I/O worker thread count exceeds the host safety limit");
    }
    if (options.parseWorkerThreadCount < 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost parse worker thread count cannot be negative");
    }
    if (options.parseWorkerThreadCount > MaximumServerHostWorkerThreadCount)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost parse worker thread count exceeds the host safety limit");
    }
    if (options.acceptBacklog <= 0)
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "ServerHost accept backlog must be positive");
    }
    if (options.idleSessionTimeout < std::chrono::milliseconds::zero())
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "ServerHost idle session timeout cannot be negative");
    }
    if (options.gracefulCloseTimeout <= std::chrono::milliseconds::zero())
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost graceful close timeout must be positive");
    }
    if (options.maxBodySize == 0)
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "ServerHost frame body limit must be positive");
    }
    if (options.maxBodySize > MaximumHostBodySize)
    {
        return Core::Status::Fail(
            Core::ErrorCode::TooLarge, "ServerHost frame body limit exceeds the host safety limit");
    }
    if (static_cast<std::size_t>(options.maxBodySize) >
        Net::SendQueueLimitBytes - Protocol::HeaderSize)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost frame body limit cannot fit in one Connection send queue entry");
    }
    if (options.maxConcurrentSessions == 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost concurrent session limit must be positive");
    }
    if (options.maxConcurrentSessions > MaximumHostConcurrentSessions)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost concurrent session limit exceeds the host safety limit");
    }
    if (options.maxTotalSendQueueCapacityBytes == 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost total send queue capacity must be positive");
    }
    if (options.maxTotalSendQueueCapacityBytes < Net::SendQueueLimitBytes)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost total send queue capacity must fit one Connection send queue");
    }
    if (options.maxTotalSendQueueCapacityBytes > MaximumHostTotalSendQueueCapacityBytes)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost total send queue capacity exceeds the host safety limit");
    }
    const std::size_t frameReaderStoragePerSession =
        Protocol::HeaderSize + static_cast<std::size_t>(options.maxBodySize);
    if (static_cast<std::size_t>(options.maxConcurrentSessions) >
        MaximumHostFrameReaderStorageBytes / frameReaderStoragePerSession)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost FrameReader storage exceeds the aggregate host safety limit");
    }
    if (options.maxPendingReceiveBytes == 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost pending receive byte limit must be positive");
    }
    if (options.maxPendingReceiveBytes > MaximumHostPendingReceiveBytes)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost pending receive byte limit exceeds the host safety limit");
    }
    if (options.maxTotalPendingReceiveBytes == 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost total pending receive byte limit must be positive");
    }
    if (options.maxTotalPendingReceiveBytes > MaximumHostTotalPendingReceiveBytes)
    {
        return Core::Status::Fail(Core::ErrorCode::TooLarge,
            "ServerHost total pending receive byte limit exceeds the host safety limit");
    }

    if (options.parseWorkerThreadCount != 0)
    {
        if (options.maxPendingParseBytes == 0 || options.maxTotalPendingParseBytes == 0)
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "ServerHost parse byte limits must be positive");
        }
        if (options.maxPendingParseBytes > MaximumHostPendingParseBytes ||
            options.maxTotalPendingParseBytes > MaximumHostTotalPendingParseBytes)
        {
            return Core::Status::Fail(Core::ErrorCode::TooLarge,
                "ServerHost parse byte limit exceeds the host safety limit");
        }
        if (options.maxPendingParseBytes < options.maxBodySize ||
            options.maxTotalPendingParseBytes < options.maxBodySize)
        {
            return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost parse byte limits must fit one maximum-size frame body");
        }
        if (options.maxTotalPendingParseBytes < options.maxPendingParseBytes)
        {
            return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost total parse byte limit cannot be smaller than the per-session limit");
        }
        if (options.maxPendingParseTasks == 0 || options.maxTotalPendingParseTasks == 0)
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "ServerHost parse task limits must be positive");
        }
        if (options.maxPendingParseTasks > MaximumHostPendingParseTasks ||
            options.maxTotalPendingParseTasks > MaximumHostTotalPendingParseTasks)
        {
            return Core::Status::Fail(Core::ErrorCode::TooLarge,
                "ServerHost parse task limit exceeds the host safety limit");
        }
        if (options.maxTotalPendingParseTasks < options.maxPendingParseTasks)
        {
            return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost total parse task limit cannot be smaller than the per-session limit");
        }
    }

    return Core::Status::Ok();
}

[[nodiscard]] Core::Status WithConfigKeyContext(
    const std::string_view key, const Core::Status& status)
{
    SERVERCORE_ASSERT(!status.IsOk(), "a Config lookup failure must not contain an Ok status");

    std::string message("ServerHost config '");
    message.append(key);
    message.append("': ");
    message.append(status.Message());
    return Core::Status::Fail(status.Code(), std::move(message));
}

[[nodiscard]] Core::Status ConfigMappingResourceFailure()
{
    return Core::Status::AllocationFailure();
}

[[nodiscard]] Core::Status PlatformFailureFrom(const std::exception& failure) noexcept
{
    try
    {
        return Core::Status::Fail(Core::ErrorCode::PlatformError, failure.what());
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}

[[nodiscard]] Core::Status ApplyOptionalInt(
    const Core::Config& config, const std::string_view key, int& destination)
{
    const Core::Result<int> value = config.GetInt(key);
    if (!value.IsOk())
    {
        if (value.GetStatus().Code() == Core::ErrorCode::NotFound)
        {
            return Core::Status::Ok();
        }
        return WithConfigKeyContext(key, value.GetStatus());
    }
    destination = value.Value();
    return Core::Status::Ok();
}

[[nodiscard]] Core::Status ApplyOptionalUint32(
    const Core::Config& config, const std::string_view key, std::uint32_t& destination)
{
    const Core::Result<int> value = config.GetInt(key);
    if (!value.IsOk())
    {
        if (value.GetStatus().Code() == Core::ErrorCode::NotFound)
        {
            return Core::Status::Ok();
        }
        return WithConfigKeyContext(key, value.GetStatus());
    }
    if (value.Value() < 0)
    {
        std::string message("ServerHost config '");
        message.append(key);
        message.append("' cannot be negative");
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument, std::move(message));
    }

    destination = static_cast<std::uint32_t>(value.Value());
    return Core::Status::Ok();
}

[[nodiscard]] Core::Status ApplyOptionalString(
    const Core::Config& config, const std::string_view key, std::string& destination)
{
    const Core::Result<std::string> value = config.GetString(key);
    if (!value.IsOk())
    {
        if (value.GetStatus().Code() == Core::ErrorCode::NotFound)
        {
            return Core::Status::Ok();
        }
        return WithConfigKeyContext(key, value.GetStatus());
    }

    destination = value.Value();
    return Core::Status::Ok();
}

void LogStatus(const Core::LogLevel level, const Core::Status& status) noexcept
{
    Core::GetGlobalLogger().Write(level, status.Message());
}

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
struct BeforeParseGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeParseGate> gate;
};

struct BeforeSessionReceiveGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeSessionReceiveGate> gate;
};

struct BeforeConnectionStartGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeConnectionStartGate> gate;
};

struct BeforeHostRunningGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeHostRunningGate> gate;
};

struct FailedStartOwnerGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IFailedStartOwnerGate> gate;
};

[[nodiscard]] BeforeParseGateSlot& GetBeforeParseGateSlot()
{
    static BeforeParseGateSlot slot;
    return slot;
}

[[nodiscard]] BeforeSessionReceiveGateSlot& GetBeforeSessionReceiveGateSlot()
{
    static BeforeSessionReceiveGateSlot slot;
    return slot;
}

[[nodiscard]] BeforeConnectionStartGateSlot& GetBeforeConnectionStartGateSlot()
{
    static BeforeConnectionStartGateSlot slot;
    return slot;
}

[[nodiscard]] BeforeHostRunningGateSlot& GetBeforeHostRunningGateSlot()
{
    static BeforeHostRunningGateSlot slot;
    return slot;
}

[[nodiscard]] FailedStartOwnerGateSlot& GetFailedStartOwnerGateSlot()
{
    static FailedStartOwnerGateSlot slot;
    return slot;
}

[[nodiscard]] std::shared_ptr<TestAccess::IFailedStartOwnerGate> GetFailedStartOwnerGateForTest()
{
    const std::lock_guard<std::mutex> guard(GetFailedStartOwnerGateSlot().mutex);
    return GetFailedStartOwnerGateSlot().gate.lock();
}

[[nodiscard]] std::atomic<bool>& GetFinalizePostFailureFlag()
{
    static std::atomic<bool> failNext{ false };
    return failNext;
}

void WaitBeforeParseForTest()
{
    std::shared_ptr<TestAccess::IBeforeParseGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeParseGateSlot().mutex);
        gate = GetBeforeParseGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeParse();
    }
}

void WaitBeforeSessionReceiveForTest()
{
    std::shared_ptr<TestAccess::IBeforeSessionReceiveGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeSessionReceiveGateSlot().mutex);
        gate = GetBeforeSessionReceiveGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeSessionReceive();
    }
}

void WaitBeforeConnectionStartForTest()
{
    std::shared_ptr<TestAccess::IBeforeConnectionStartGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeConnectionStartGateSlot().mutex);
        gate = GetBeforeConnectionStartGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeConnectionStart();
    }
}

void WaitBeforeHostRunningForTest()
{
    std::shared_ptr<TestAccess::IBeforeHostRunningGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeHostRunningGateSlot().mutex);
        gate = GetBeforeHostRunningGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeHostRunning();
    }
}

[[nodiscard]] bool ConsumeFinalizePostFailureForTest() noexcept
{
    return GetFinalizePostFailureFlag().exchange(false, std::memory_order_acq_rel);
}
#endif
}

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
void TestAccess::InstallBeforeParseGate(std::shared_ptr<IBeforeParseGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeParseGateSlot().mutex);
    GetBeforeParseGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeParseGate(const std::shared_ptr<IBeforeParseGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeParseGateSlot().mutex);
    if (GetBeforeParseGateSlot().gate.lock() == expected)
    {
        GetBeforeParseGateSlot().gate.reset();
    }
}

void TestAccess::InstallBeforeSessionReceiveGate(std::shared_ptr<IBeforeSessionReceiveGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeSessionReceiveGateSlot().mutex);
    GetBeforeSessionReceiveGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeSessionReceiveGate(
    const std::shared_ptr<IBeforeSessionReceiveGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeSessionReceiveGateSlot().mutex);
    if (GetBeforeSessionReceiveGateSlot().gate.lock() == expected)
    {
        GetBeforeSessionReceiveGateSlot().gate.reset();
    }
}

void TestAccess::InstallBeforeConnectionStartGate(std::shared_ptr<IBeforeConnectionStartGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeConnectionStartGateSlot().mutex);
    GetBeforeConnectionStartGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeConnectionStartGate(
    const std::shared_ptr<IBeforeConnectionStartGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeConnectionStartGateSlot().mutex);
    if (GetBeforeConnectionStartGateSlot().gate.lock() == expected)
    {
        GetBeforeConnectionStartGateSlot().gate.reset();
    }
}

void TestAccess::InstallBeforeHostRunningGate(std::shared_ptr<IBeforeHostRunningGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeHostRunningGateSlot().mutex);
    GetBeforeHostRunningGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeHostRunningGate(const std::shared_ptr<IBeforeHostRunningGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeHostRunningGateSlot().mutex);
    if (GetBeforeHostRunningGateSlot().gate.lock() == expected)
    {
        GetBeforeHostRunningGateSlot().gate.reset();
    }
}

void TestAccess::FailNextFinalizePost() noexcept
{
    GetFinalizePostFailureFlag().store(true, std::memory_order_release);
}

void TestAccess::InstallFailedStartOwnerGate(std::shared_ptr<IFailedStartOwnerGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetFailedStartOwnerGateSlot().mutex);
    GetFailedStartOwnerGateSlot().gate = std::move(gate);
}

void TestAccess::ClearFailedStartOwnerGate(const std::shared_ptr<IFailedStartOwnerGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetFailedStartOwnerGateSlot().mutex);
    if (GetFailedStartOwnerGateSlot().gate.lock() == expected)
    {
        GetFailedStartOwnerGateSlot().gate.reset();
    }
}

void TestAccess::ClearFinalizePostFailure() noexcept
{
    GetFinalizePostFailureFlag().store(false, std::memory_order_release);
}
#endif

class ServerHost::State : public std::enable_shared_from_this<State>
{
public:
    class NetworkSession;

    State() = default;

    ~State() = default;

    Core::Status Configure(const Core::Config& config);
    Core::Status Configure(const ServerHostOptions& options);
    void SetLogger(std::shared_ptr<Core::ILogger> logger);
    Dispatch::Dispatcher& GetDispatcher() noexcept;
    const Session::SessionRegistry& GetSessions() const noexcept;
    [[nodiscard]] JobRunner::Lease GetJobRunner() const noexcept;
    void SetSessionObserver(std::weak_ptr<Session::ISessionObserver> observer);
    Core::Status Start();
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
    [[nodiscard]] bool BeginNetworkSession();

    mutable std::mutex mLifecycleMutex;
    std::condition_variable mLifecycleChanged;
    Lifecycle mLifecycle = Lifecycle::Ready;
    bool mConfigured = false;
    bool mStopRequestedDuringStart = false;
    std::size_t mOutstandingSessions = 0;
    std::size_t mInFlightCloseNotifications = 0;
    std::vector<std::shared_ptr<NetworkSession>> mSessionSlots;

    ServerHostOptions mOptions;
    std::shared_ptr<Core::ILogger> mLogger;
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
    std::atomic<bool> mRunning{ false };
    std::atomic<std::uint16_t> mPort{ 0 };
    std::atomic<std::size_t> mPendingReceiveBytes{ 0 };
    std::atomic<std::size_t> mPendingParseBytes{ 0 };
    std::atomic<std::size_t> mPendingParseTasks{ 0 };
    std::atomic<std::uint64_t> mReceivedFrameCount{ 0 };
    std::atomic<std::uint64_t> mQueuedSendFrameCount{ 0 };
    std::atomic<std::uint64_t> mErrorCount{ 0 };
};

class ServerHost::State::NetworkSession final : public ServerCore::Session::Session,
                                                public Net::IConnectionObserver,
                                                public std::enable_shared_from_this<NetworkSession>
{
private:
    /// <summary>세션별 parse reservation과 Host 전체 reservation을 함께 반납하는 공유 장부다.</summary>
    /// <remarks>
    /// worker task는 NetworkSession을 살려 두지 않는다. 이 장부만 shared_ptr로 들고 완료 뒤에
    /// 반납하므로, 원격 종료 뒤 늦은 parse 완료가 닫힌 세션이나 Registry를 만질 수 없다.
    /// </remarks>
    class ParseAccounting
    {
    public:
        ParseAccounting(std::weak_ptr<ServerHost::State> host, const std::size_t byteLimit,
            const std::size_t taskLimit) noexcept
            : mHost(std::move(host))
            , mByteLimit(byteLimit)
            , mTaskLimit(taskLimit)
        {
        }

        ~ParseAccounting()
        {
            const std::size_t bytes = mPendingBytes.exchange(0, std::memory_order_acq_rel);
            const std::size_t tasks = mPendingTasks.exchange(0, std::memory_order_acq_rel);
            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                // ParseWorkerPool은 Host 소멸 전에 join되므로, 이 최후 정리는 비정상 JobRunner
                // 실패 경로의 잔여분뿐이다.
                host->ReleasePendingParseWork(bytes, tasks);
            }
        }

        [[nodiscard]] bool TryReserve(
            const std::size_t byteCount, const std::size_t taskCount) noexcept
        {
            if (!TryReserveLocal(mPendingBytes, mByteLimit, byteCount))
            {
                return false;
            }
            if (!TryReserveLocal(mPendingTasks, mTaskLimit, taskCount))
            {
                ReleaseLocal(mPendingBytes, byteCount);
                return false;
            }

            const std::shared_ptr<ServerHost::State> host = mHost.lock();
            if (host != nullptr && host->TryReservePendingParseWork(byteCount, taskCount))
            {
                return true;
            }

            ReleaseLocal(mPendingBytes, byteCount);
            ReleaseLocal(mPendingTasks, taskCount);
            return false;
        }

        void Release(const std::size_t byteCount, const std::size_t taskCount) noexcept
        {
            ReleaseLocal(mPendingBytes, byteCount);
            ReleaseLocal(mPendingTasks, taskCount);
            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->ReleasePendingParseWork(byteCount, taskCount);
            }
        }

    private:
        [[nodiscard]] static bool TryReserveLocal(std::atomic<std::size_t>& pending,
            const std::size_t limit, const std::size_t amount) noexcept
        {
            if (amount == 0)
            {
                return true;
            }

            std::size_t current = pending.load(std::memory_order_acquire);
            for (;;)
            {
                if (current > limit || amount > limit - current)
                {
                    return false;
                }
                if (pending.compare_exchange_weak(current, current + amount,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return true;
                }
            }
        }

        static void ReleaseLocal(
            std::atomic<std::size_t>& pending, const std::size_t amount) noexcept
        {
            if (amount == 0)
            {
                return;
            }

            std::size_t current = pending.load(std::memory_order_acquire);
            for (;;)
            {
                SERVERCORE_ASSERT(
                    current >= amount, "released parse work exceeded the NetworkSession budget");
                if (pending.compare_exchange_weak(current, current - amount,
                        std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    return;
                }
            }
        }

        std::weak_ptr<ServerHost::State> mHost;
        std::size_t mByteLimit;
        std::size_t mTaskLimit;
        std::atomic<std::size_t> mPendingBytes{ 0 };
        std::atomic<std::size_t> mPendingTasks{ 0 };
    };

    /// <summary>FrameReader가 본문 vector를 복사하기 전에 parse reservation을 확보한다.</summary>
    /// <remarks>
    /// 한 Receive batch 안의 여러 완결 frame을 Append 뒤에 한꺼번에 예약하면, 작은 frame이
    /// 많은 경우 실제 deque 할당이 task 상한을 순간적으로 넘을 수 있다. 이 객체는 callback
    /// 안에서 프레임 하나씩 먼저 예약하고, 복사에 실패한 마지막 frame만 Reconcile에서 되돌린다.
    /// </remarks>
    class ParseAdmission
    {
    public:
        explicit ParseAdmission(ParseAccounting& accounting) noexcept
            : mAccounting(accounting)
        {
        }

        ~ParseAdmission()
        {
            // FrameReader::Append()가 Status 설명 문자열을 만들다가 OOM을 던지는 식으로
            // ReleaseNotStored까지 돌아오지 못해도, callback 안에서 먼저 잡은 reservation은
            // 이 범위가 끝날 때 반드시 반납한다.
            if (!mSettled)
            {
                mAccounting.Release(mReservedBytes, mReservedTasks);
            }
        }

        [[nodiscard]] static bool Admit(void* const context, const std::size_t byteCount) noexcept
        {
            ParseAdmission* const admission = static_cast<ParseAdmission*>(context);
            if (admission == nullptr || !admission->mAccounting.TryReserve(byteCount, 1))
            {
                return false;
            }

            // ParseAccounting의 설정 상한이 size_t보다 훨씬 작으므로 이 합계는 overflow하지 않는다.
            admission->mReservedBytes += byteCount;
            ++admission->mReservedTasks;
            return true;
        }

        void ReleaseNotStored(const std::size_t storedBytes, const std::size_t storedTasks) noexcept
        {
            SERVERCORE_ASSERT(mReservedBytes >= storedBytes,
                "FrameReader stored more parse bytes than it admitted");
            SERVERCORE_ASSERT(mReservedTasks >= storedTasks,
                "FrameReader stored more parse tasks than it admitted");
            mAccounting.Release(mReservedBytes - storedBytes, mReservedTasks - storedTasks);
            mSettled = true;
        }

    private:
        ParseAccounting& mAccounting;
        std::size_t mReservedBytes = 0;
        std::size_t mReservedTasks = 0;
        bool mSettled = false;
    };

    /// <summary>예약한 frame 하나와 그 parse 결과를 worker와 JobRunner 사이에서 함께 붙든다.</summary>
    /// <remarks>
    /// raw body와 parsed Message를 예산 장부와 별도 lambda capture로 두면 capture 멤버의 소멸
    /// 순서는 C++가 정하지 않는다. 그러면 실행되지 않고 버린 작업에서 예산이 실제 payload보다
    /// 먼저 풀릴 수 있다. 이 객체의 소멸자는 payload를 명시적으로 먼저 놓고 마지막에 예산을
    /// 반납해 어느 큐에서 파기되더라도 같은 순서를 지킨다.
    /// </remarks>
    class ParseWorkItem
    {
    public:
        ParseWorkItem(
            std::shared_ptr<ParseAccounting> accounting, std::vector<std::byte> body) noexcept
            : mAccounting(std::move(accounting))
            , mByteCount(body.size())
            , mBody(std::move(body))
        {
        }

        ~ParseWorkItem()
        {
            mMessage.reset();
            std::vector<std::byte>().swap(mBody);
            if (mAccounting != nullptr)
            {
                mAccounting->Release(mByteCount, 1);
            }
        }

        ParseWorkItem(const ParseWorkItem&) = delete;
        ParseWorkItem& operator=(const ParseWorkItem&) = delete;

        void Parse()
        {
            SERVERCORE_ASSERT(!mMessage.has_value(), "a parse work item was parsed more than once");
            mMessage.emplace(Protocol::ParseMessage(mBody));
            // parsed Message가 필요한 값을 모두 소유한 뒤에는 wire body를 즉시 놓는다. 이 객체가
            // completion queue에 머무는 동안 예산은 계속 유지된다.
            std::vector<std::byte>().swap(mBody);
        }

        [[nodiscard]] Core::Result<Protocol::Message>& Message()
        {
            SERVERCORE_ASSERT(mMessage.has_value(), "a parse work item has no completed result");
            return *mMessage;
        }

    private:
        std::shared_ptr<ParseAccounting> mAccounting;
        std::size_t mByteCount;
        std::vector<std::byte> mBody;
        std::optional<Core::Result<Protocol::Message>> mMessage;
    };

    /// <summary>동기 Connection 호출이 돌아올 때까지 fallback OnSessionClosed를 미룬다.</summary>
    // Send/Disconnect의 지역 잠금보다 먼저 생성해야 한다. 역순 소멸로 outbound 잠금을 놓은
    // 뒤에만 종료 관찰자를 부를 수 있어, 관찰자가 다시 Send/Disconnect해도 교착하지 않는다.
    class FinalizationDeferral
    {
    public:
        explicit FinalizationDeferral(NetworkSession& session) noexcept
            : mSession(session)
            , mActive(session.BeginFinalizationDeferral())
        {
        }

        ~FinalizationDeferral()
        {
            if (mActive)
            {
                mSession.CompleteLifecycleNotification();
            }
        }

        FinalizationDeferral(const FinalizationDeferral&) = delete;
        FinalizationDeferral& operator=(const FinalizationDeferral&) = delete;

    private:
        NetworkSession& mSession;
        bool mActive;
    };

public:
    NetworkSession(std::weak_ptr<ServerHost::State> host, const ServerCore::Session::SessionId id,
        std::shared_ptr<Net::Connection> connection, const std::uint32_t maxBodySize,
        const std::uint32_t maxPendingReceiveBytes, const std::uint32_t maxPendingParseBytes,
        const std::uint32_t maxPendingParseTasks)
        : mHost(std::move(host))
        , mId(id)
        , mConnection(std::move(connection))
        , mLastReceivedMilliseconds(Core::MillisecondsSinceProcessStart())
        , mFrameReader(maxBodySize)
        , mMaxBodySize(maxBodySize)
        , mMaxPendingReceiveBytes(maxPendingReceiveBytes)
    {
        SERVERCORE_ASSERT(mConnection != nullptr, "NetworkSession was given a null connection");
        if (maxPendingParseTasks != 0)
        {
            mParseAccounting = std::make_shared<ParseAccounting>(mHost,
                static_cast<std::size_t>(maxPendingParseBytes),
                static_cast<std::size_t>(maxPendingParseTasks));
        }
    }

    [[nodiscard]] ServerCore::Session::SessionId Id() const noexcept override { return mId; }

    [[nodiscard]] ServerCore::Session::SessionState State() const noexcept override
    {
        return mSessionState.load(std::memory_order_acquire);
    }

    Core::Status MarkAuthenticated() override
    {
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr)
        {
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        }
        if (!host->mJobRunner.IsCurrentThread())
        {
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        }

        const std::shared_ptr<NetworkSession> self = shared_from_this();
        {
            const std::lock_guard<std::mutex> guard(mFinalizationMutex);
            if (mFinalized.load(std::memory_order_relaxed))
            {
                return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
            }

            ServerCore::Session::SessionState current =
                mSessionState.load(std::memory_order_acquire);
            for (;;)
            {
                if (current == ServerCore::Session::SessionState::Authenticated)
                {
                    return Core::Status::FailWithoutMessage(Core::ErrorCode::AlreadyExists);
                }
                if (current == ServerCore::Session::SessionState::Closing ||
                    current == ServerCore::Session::SessionState::Closed)
                {
                    return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
                }

                if (mSessionState.compare_exchange_weak(current,
                        ServerCore::Session::SessionState::Authenticated, std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    ++mLifecycleNotificationDepth;
                    break;
                }
            }
        }

        // OnSessionOpened 관찰자 안에서 인증이 중첩될 수 있고, 이 callback 안에서도 즉시
        // Disconnect할 수 있다. notification depth가 마지막 callback 반환까지 종료를 미룬다.
        host->NotifyAuthenticated(self);
        CompleteLifecycleNotification();
        return Core::Status::Ok();
    }

    Core::Status Send(const Protocol::MessageFields& fields) override
    {
        FinalizationDeferral finalization(*this);
        const std::lock_guard<std::mutex> guard(mOutboundMutex);
        if (!CanSendLocked())
        {
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }

        try
        {
            Core::Result<std::vector<std::byte>> framed = PrepareOutboundFrame(fields);
            if (!framed.IsOk())
            {
                return RecordAndReturn(std::move(framed).TakeStatus());
            }

            Net::ConnectionSendOutcome sendOutcome = mConnection->SendWithOutcome(framed.Value());
            Core::Status queued = std::move(sendOutcome.status);
            if (!queued.IsOk())
            {
                // 즉시 소켓 실패는 Connection이 같은 사유로 OnDisconnected를 보낸다. 그 경로가
                // error counter를 한 번 올리게 하고, 연결이 열린 채인 큐 할당 실패만 여기서 센다.
                if (sendOutcome.connectionClosedByFailure)
                {
                    return queued;
                }
                return RecordAndReturn(std::move(queued));
            }

            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->RecordQueuedSendFrame();
            }
            return queued;
        }
        catch (const std::bad_alloc&)
        {
            return RecordAndReturn(Core::Status::AllocationFailure());
        }
        catch (const std::system_error& error)
        {
            return RecordAndReturn(PlatformFailureFrom(error));
        }
    }

    Core::Status SendPrepared(const Protocol::PreparedMessage& prepared) override
    {
        FinalizationDeferral finalization(*this);
        const std::lock_guard<std::mutex> guard(mOutboundMutex);
        if (!CanSendLocked())
        {
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }

        // 준비 객체를 이동한 뒤의 빈 저장소는 정상 봉투가 아니다. 프레이머는 범용이라
        // 0길이 body도 인코딩하므로 메시지 계약을 이 경계에서 지킨다.
        if (prepared.Size() == 0)
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));

        try
        {
            Core::Result<std::vector<std::byte>> framed = Protocol::EncodeFrame(prepared.Bytes(), mMaxBodySize);
            if (!framed.IsOk())
            {
                return RecordAndReturn(std::move(framed).TakeStatus());
            }

            Net::ConnectionSendOutcome sendOutcome = mConnection->SendWithOutcome(framed.Value());
            Core::Status queued = std::move(sendOutcome.status);
            if (!queued.IsOk())
            {
                // 즉시 소켓 실패는 Connection이 같은 사유로 OnDisconnected를 보낸다. 그 경로가
                // error counter를 한 번 올리게 하고, 연결이 열린 채인 큐 할당 실패만 여기서 센다.
                if (sendOutcome.connectionClosedByFailure)
                {
                    return queued;
                }
                return RecordAndReturn(std::move(queued));
            }

            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->RecordQueuedSendFrame();
            }
            return queued;
        }
        catch (const std::bad_alloc&)
        {
            return RecordAndReturn(Core::Status::AllocationFailure());
        }
        catch (const std::system_error& error)
        {
            return RecordAndReturn(PlatformFailureFrom(error));
        }
    }

    Core::Status SendAndDisconnect(
        const Protocol::MessageFields& fields, Core::Status reason) override
    {
        FinalizationDeferral finalization(*this);
        const std::lock_guard<std::mutex> guard(mOutboundMutex);
        if (!BeginGracefulCloseLocked(std::move(reason)))
        {
            return RecordAndReturn(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }

        try
        {
            Core::Result<std::vector<std::byte>> framed = PrepareOutboundFrame(fields);
            if (!framed.IsOk())
            {
                mConnection->Close();
                return RecordAndReturn(std::move(framed).TakeStatus());
            }

            Net::ConnectionSendOutcome sendOutcome = mConnection->SendWithOutcome(framed.Value());
            Core::Status queued = std::move(sendOutcome.status);
            if (!queued.IsOk())
            {
                mConnection->Close();
                if (sendOutcome.connectionClosedByFailure)
                {
                    return queued;
                }
                return RecordAndReturn(std::move(queued));
            }

            if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
            {
                host->RecordQueuedSendFrame();
            }
            mConnection->CloseAfterSend();
            return queued;
        }
        catch (const std::bad_alloc&)
        {
            mConnection->Close();
            return RecordAndReturn(Core::Status::AllocationFailure());
        }
        catch (const std::system_error& error)
        {
            mConnection->Close();
            return RecordAndReturn(PlatformFailureFrom(error));
        }
    }

    void Disconnect(Core::Status reason) override
    {
        FinalizationDeferral finalization(*this);
        const std::lock_guard<std::mutex> guard(mOutboundMutex);
        DisconnectLocked(std::move(reason));
    }

    void OnBytesReceived(std::span<const std::byte> bytes) override
    {
        if (bytes.empty())
        {
            return;
        }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        WaitBeforeSessionReceiveForTest();
#endif

        {
            // 유휴 검사와 이 갱신은 같은 잠금을 쓴다. 검사 쪽이 오래된 시각을 읽은 뒤
            // SendAndDisconnect()의 drain을 강제로 취소하는 일을 막기 위한 수명 경계다.
            const std::lock_guard<std::mutex> guard(mIdleMutex);
            mLastReceivedMilliseconds = Core::MillisecondsSinceProcessStart();
        }

        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
        {
            return;
        }

        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr)
        {
            mConnection->Close();
            return;
        }
        if (!host->IsAccepting())
        {
            return;
        }

        bool queueReceiveJob = false;
        bool reservedHostReceiveBytes = false;
        Core::Status rejected = Core::Status::Ok();
        try
        {
            {
                const std::lock_guard<std::mutex> guard(mReceiveMutex);
                const std::size_t limit = static_cast<std::size_t>(mMaxPendingReceiveBytes);
                if (bytes.size() > limit || mBufferedReceiveBytes > limit - bytes.size())
                {
                    rejected = Core::Status::Fail(Core::ErrorCode::TooLarge,
                        "the session exceeded its pending receive byte limit");
                }
                else if (!host->TryReservePendingReceiveBytes(bytes.size()))
                {
                    rejected = Core::Status::Fail(Core::ErrorCode::TooLarge,
                        "the ServerHost exceeded its total pending receive byte limit");
                }
                else
                {
                    reservedHostReceiveBytes = true;
                    mPendingReceiveBytes.insert(
                        mPendingReceiveBytes.end(), bytes.begin(), bytes.end());
                    mBufferedReceiveBytes += bytes.size();
                    if (!mReceiveJobQueued)
                    {
                        mReceiveJobQueued = true;
                        queueReceiveJob = true;
                    }
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            if (reservedHostReceiveBytes)
            {
                host->ReleasePendingReceiveBytes(bytes.size());
            }
            rejected = Core::Status::AllocationFailure();
        }
        catch (...)
        {
            if (reservedHostReceiveBytes)
            {
                host->ReleasePendingReceiveBytes(bytes.size());
            }
            rejected = Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }

        if (!rejected.IsOk())
        {
            FailAndDisconnect(std::move(rejected));
            return;
        }
        if (!queueReceiveJob)
        {
            return;
        }

        Core::Status posted = Core::Status::Ok();
        try
        {
            const std::shared_ptr<NetworkSession> self = shared_from_this();
            posted = host->mJobRunner.Post([self]() { self->ConsumePendingBytesOnRunner(); });
        }
        catch (const std::bad_alloc&)
        {
            posted = Core::Status::AllocationFailure();
        }
        catch (...)
        {
            posted = Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }
        if (!posted.IsOk())
        {
            FailAndDisconnect(std::move(posted));
        }
    }

    void OnDisconnected(Core::Status reason) noexcept override
    {
        if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
        {
            host->RecordError(reason);
        }
        try
        {
            const std::lock_guard<std::mutex> guard(mCloseReasonMutex);
            RememberCloseReasonLocked(std::move(reason));
            // 사유와 Closed 공개를 같은 임계구역에서 정한다. 로컬 종료가 Closing을 먼저 차지한
            // 뒤 transport callback이 그 사유보다 앞서 들어가는 경쟁을 막는다.
            mSessionState.store(
                ServerCore::Session::SessionState::Closed, std::memory_order_release);
        }
        catch (...)
        {
            // mutex 구현 실패 같은 최후 경계에서도 끊김 정리 자체는 반드시 계속한다.
            mSessionState.store(
                ServerCore::Session::SessionState::Closed, std::memory_order_release);
        }
        DiscardQueuedReceiveBytes();

        if (mFinalized.load(std::memory_order_acquire))
        {
            return;
        }

        try
        {
            const std::shared_ptr<ServerHost::State> host = mHost.lock();
            if (host == nullptr)
            {
                return;
            }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
            if (ConsumeFinalizePostFailureForTest())
            {
                FinalizeAfterRunnerFailure();
                return;
            }
#endif

            const std::shared_ptr<NetworkSession> self = shared_from_this();
            const Core::Status posted =
                host->mJobRunner.Post([self]() { self->FinalizeOnRunner(); });
            if (!posted.IsOk())
            {
                // 정상 Host 종료 중에는 JobRunner가 이 작업을 drain하므로 여기에 오지 않는다.
                // 외부 실행자 실패/OOM처럼 더는 queue에 넣을 수 없는 경우에도 Stop이 세션 수에서
                // 영구 대기하지 않도록 최소 종료 정리를 보장한다.
                FinalizeAfterRunnerFailure();
            }
        }
        catch (...)
        {
            FinalizeAfterRunnerFailure();
        }
    }

    void OpenOnRunner()
    {
        try
        {
            const std::shared_ptr<ServerHost::State> host = mHost.lock();
            if (host == nullptr)
            {
                return;
            }

            SERVERCORE_ASSERT(host->mJobRunner.IsCurrentThread(),
                "NetworkSession::OpenOnRunner() must run in the ServerHost JobRunner context");

            const std::shared_ptr<NetworkSession> self = shared_from_this();
            Core::Status registered = Core::Status::Ok();
            bool finalizeClosedSession = false;
            bool rejectClosingSession = false;
            {
                // Register와 open 통지 시작을 하나의 전이로 만든다. I/O 스레드의 최후 정리가
                // 이 사이에 들어오면 등록된 닫힌 세션이 Registry에 남거나 OnClosed가 OnOpened보다
                // 먼저 나갈 수 있다.
                const std::lock_guard<std::mutex> guard(mFinalizationMutex);
                if (mFinalized.load(std::memory_order_relaxed))
                {
                    return;
                }

                const ServerCore::Session::SessionState state =
                    mSessionState.load(std::memory_order_acquire);
                if (state == ServerCore::Session::SessionState::Closed)
                {
                    finalizeClosedSession = true;
                }
                else if (!host->IsAccepting() ||
                         state == ServerCore::Session::SessionState::Closing)
                {
                    rejectClosingSession = true;
                }
                else
                {
                    registered = host->mRegistry.Register(self);
                    if (registered.IsOk())
                    {
                        mRegistered.store(true, std::memory_order_release);
                        mOpened.store(true, std::memory_order_release);
                        ++mLifecycleNotificationDepth;
                    }
                }
            }

            if (finalizeClosedSession)
            {
                FinalizeOnRunner();
                return;
            }
            if (rejectClosingSession)
            {
                Disconnect(Core::Status::Fail(
                    Core::ErrorCode::Closed, "the ServerHost stopped accepting this session"));
                return;
            }
            if (!registered.IsOk())
            {
                FailAndDisconnect(std::move(registered));
                return;
            }

            // 관찰자는 이 callback 안에서 곧바로 Disconnect할 수 있다. callback 동안에는
            // finalization 잠금을 잡지 않고, 실행자 Post까지 실패한 종료만 아래로 미뤄
            // OnOpened 뒤에 Registry 제거와 OnClosed가 이어지게 한다.
            host->NotifyOpened(self);

            CompleteLifecycleNotification();
        }
        catch (const std::bad_alloc&)
        {
            FailAndDisconnect(Core::Status::AllocationFailure());
        }
    }

    void AbortBeforeOpened(Core::Status reason) noexcept
    {
        RecordFailure(reason);
        try
        {
            RememberCloseReason(std::move(reason));
        }
        catch (...)
        {
            // 사유 보관 실패는 수락 수명 정리를 막지 않는다.
        }

        mSessionState.store(ServerCore::Session::SessionState::Closed, std::memory_order_release);
        DiscardQueuedReceiveBytes();

        // Open 작업을 queue에 넣지 못한 경로도 I/O thread의 finalizer fallback과 같은 claim을
        // 쓴다. 둘이 겹쳐도 예약·Registry·Host slot은 정확히 한 번만 정리된다.
        FinalizeAfterRunnerFailure();

        try
        {
            mConnection->Close();
        }
        catch (...)
        {
            Core::ReportAssertFailure("Connection::Close() did not throw", __FILE__, __LINE__,
                "a Connection threw while aborting a session before it opened");
        }
    }

private:
    void ConsumePendingBytesOnRunner();
    void CompleteReceiveBatchOnRunner(std::size_t batchSize);
    void DiscardQueuedReceiveBytes();
    void DiscardUnregisteredId() noexcept;
    void FinalizeAfterRunnerFailure() noexcept;

public:
    [[nodiscard]] std::size_t QueuedSendBytes() const noexcept override
    {
        return mConnection->QueuedSendBytes();
    }

    /// <summary>graceful 절대 기한이나 열린 세션의 유휴 제한을 넘겼으면 즉시 닫는다.</summary>
    /// <remarks>
    /// Closing은 수신 시각 대신 Closing 시작 시각을 본다. 따라서 유휴 검사가 정상 drain을
    /// 앞질러 취소하거나, 종료 중 들어온 바이트가 drain 기한을 늘릴 수 없다. 잠금 순서는
    /// idle 뒤 outbound이며 수신 경로는 두 잠금을 함께 잡지 않으므로 역순 교착이 없다.
    /// </remarks>
    [[nodiscard]] bool DisconnectIfExpired(const std::uint64_t nowMilliseconds,
        const std::uint64_t idleTimeoutMilliseconds, const std::uint64_t gracefulTimeoutMilliseconds)
    {
        FinalizationDeferral finalization(*this);
        const std::lock_guard<std::mutex> idleGuard(mIdleMutex);
        const std::lock_guard<std::mutex> outboundGuard(mOutboundMutex);
        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (state == ServerCore::Session::SessionState::Closed)
        {
            return false;
        }
        if (state == ServerCore::Session::SessionState::Closing)
        {
            if (!mGracefulCloseStartedMilliseconds.has_value() ||
                *mGracefulCloseStartedMilliseconds > nowMilliseconds ||
                nowMilliseconds - *mGracefulCloseStartedMilliseconds < gracefulTimeoutMilliseconds)
            {
                return false;
            }
            // 첫 종료 사유는 유지한다. 남은 WSASend 버퍼는 취소 완료 뒤 회수되며, 그 뒤에만
            // OnDisconnected가 세션 슬롯을 반환한다. 새 할당도 필요하지 않다.
            mConnection->Close();
            return true;
        }
        if (idleTimeoutMilliseconds == 0 || mLastReceivedMilliseconds > nowMilliseconds ||
            nowMilliseconds - mLastReceivedMilliseconds < idleTimeoutMilliseconds)
        {
            return false;
        }

        DisconnectLocked(
            Core::Status::Fail(Core::ErrorCode::Timeout, "the session exceeded its idle timeout"));
        return true;
    }

private:
    void CompleteLifecycleNotification() noexcept
    {
        bool finalizeAfterNotification = false;
        {
            const std::lock_guard<std::mutex> guard(mFinalizationMutex);
            SERVERCORE_ASSERT(mLifecycleNotificationDepth != 0,
                "a NetworkSession lifecycle notification completed without beginning");
            --mLifecycleNotificationDepth;
            if (mLifecycleNotificationDepth == 0)
            {
                finalizeAfterNotification = std::exchange(mFinalizeAfterNotification, false);
            }
        }

        if (finalizeAfterNotification)
        {
            FinalizeAfterRunnerFailure();
        }
    }

    void RecordFailure(const Core::Status& status) const noexcept
    {
        if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
        {
            host->RecordError(status);
        }
    }

    [[nodiscard]] Core::Status RecordAndReturn(Core::Status status) const
    {
        RecordFailure(status);
        return status;
    }

    [[nodiscard]] bool CanSendLocked() const noexcept
    {
        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        return state != ServerCore::Session::SessionState::Closing &&
               state != ServerCore::Session::SessionState::Closed;
    }

    [[nodiscard]] bool BeginGracefulCloseLocked(Core::Status reason)
    {
        // local 종료의 선형화 지점은 상태 전이와 사유 설치를 함께 보호하는 이 잠금 안이다.
        // mOutboundMutex 다음에 이 잠금을 얻으며, 역순으로 두 잠금을 잡는 경로는 없다.
        const std::lock_guard<std::mutex> reasonGuard(mCloseReasonMutex);
        ServerCore::Session::SessionState current = mSessionState.load(std::memory_order_acquire);
        for (;;)
        {
            if (current == ServerCore::Session::SessionState::Closing ||
                current == ServerCore::Session::SessionState::Closed)
            {
                return false;
            }

            if (mSessionState.compare_exchange_weak(current,
                    ServerCore::Session::SessionState::Closing, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                RememberCloseReasonLocked(std::move(reason));
                mGracefulCloseStartedMilliseconds = Core::MillisecondsSinceProcessStart();
                return true;
            }
        }
    }

    void DisconnectLocked(Core::Status reason)
    {
        std::unique_lock<std::mutex> reasonGuard(mCloseReasonMutex);
        ServerCore::Session::SessionState current = mSessionState.load(std::memory_order_acquire);
        for (;;)
        {
            if (current == ServerCore::Session::SessionState::Closed)
            {
                return;
            }
            if (current == ServerCore::Session::SessionState::Closing)
            {
                // SendAndDisconnect()가 시작한 drain도 Host 종료 같은 명시적 즉시 종료는 취소할 수
                // 있어야 한다. 첫 종료 사유는 이미 보관되어 있으므로 여기서 덮어쓰지 않는다.
                reasonGuard.unlock();
                mConnection->Close();
                return;
            }

            if (mSessionState.compare_exchange_weak(current,
                    ServerCore::Session::SessionState::Closing, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                RememberCloseReasonLocked(std::move(reason));
                // Close()는 진행 중인 I/O가 없으면 OnDisconnected를 동기 호출할 수 있다. 그
                // callback이 같은 사유 잠금을 얻도록 transport 경계 전에 놓는다.
                reasonGuard.unlock();
                mConnection->Close();
                return;
            }
        }
    }

    [[nodiscard]] Core::Result<std::vector<std::byte>> PrepareOutboundFrame(
        const Protocol::MessageFields& fields) const
    {
        Core::Result<std::vector<std::byte>> serialized = Protocol::SerializeMessage(fields);
        if (!serialized.IsOk())
        {
            return Core::Result<std::vector<std::byte>>::FromStatus(
                std::move(serialized).TakeStatus());
        }

        return Protocol::EncodeFrame(serialized.Value(), mMaxBodySize);
    }

    void FailAndDisconnect(Core::Status failure)
    {
        RecordFailure(failure);
        Disconnect(std::move(failure));
    }

    void ConsumeBytesOnRunner(const std::vector<std::byte>& bytes)
    {
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr || !host->mJobRunner.IsCurrentThread())
        {
            return;
        }

        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
        {
            return;
        }

        if (mParseAccounting == nullptr)
        {
            ConsumeSynchronouslyOnRunner(host, bytes);
            return;
        }

        ConsumeWithParseWorkersOnRunner(bytes);
    }

    void ConsumeSynchronouslyOnRunner(
        const std::shared_ptr<ServerHost::State>& host, const std::vector<std::byte>& bytes)
    {
        Core::Status appended = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            const ServerCore::Session::SessionState state =
                mSessionState.load(std::memory_order_acquire);
            if (state == ServerCore::Session::SessionState::Closing ||
                state == ServerCore::Session::SessionState::Closed ||
                mFinalized.load(std::memory_order_acquire))
            {
                return;
            }
            appended = mFrameReader.Append(bytes);
        }
        if (!appended.IsOk())
        {
            FailAndDisconnect(std::move(appended));
            return;
        }

        for (;;)
        {
            Core::Result<std::vector<std::byte>> frame = [&]()
            {
                const std::lock_guard<std::mutex> guard(mParseStateMutex);
                return mFrameReader.TakeNextFrame();
            }();
            if (!frame.IsOk())
            {
                if (frame.GetStatus().Code() == Core::ErrorCode::WouldBlock)
                {
                    return;
                }

                FailAndDisconnect(std::move(frame).TakeStatus());
                return;
            }

            host->RecordReceivedFrame();

            Core::Result<Protocol::Message> message = Protocol::ParseMessage(frame.Value());
            if (!message.IsOk())
            {
                FailAndDisconnect(std::move(message).TakeStatus());
                return;
            }

            if (!BeginMessageDispatch())
            {
                return;
            }

            Core::Status dispatched = Core::Status::Ok();
            try
            {
                dispatched = host->mDispatcher.Dispatch(shared_from_this(), message.Value());
            }
            catch (...)
            {
                CompleteLifecycleNotification();
                throw;
            }
            CompleteLifecycleNotification();
            if (!dispatched.IsOk())
            {
                host->RecordError(dispatched);
                // L4는 handler 거부, type별 body 상한, body 없는 error 봉투를 연결 종료로
                // 번역하지 않는다. UnknownType의 Disconnect 정책도 Dispatcher가 이미 처리한다.
                LogStatus(Core::LogLevel::Warn, dispatched);
            }

            const ServerCore::Session::SessionState afterDispatch =
                mSessionState.load(std::memory_order_acquire);
            if (afterDispatch == ServerCore::Session::SessionState::Closing ||
                afterDispatch == ServerCore::Session::SessionState::Closed)
            {
                return;
            }
        }
    }

    void ConsumeWithParseWorkersOnRunner(const std::vector<std::byte>& bytes)
    {
        SERVERCORE_ASSERT(mParseAccounting != nullptr,
            "parse worker consumption requires a NetworkSession parse accounting ledger");
        Core::Status appended = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            const ServerCore::Session::SessionState state =
                mSessionState.load(std::memory_order_acquire);
            if (state == ServerCore::Session::SessionState::Closing ||
                state == ServerCore::Session::SessionState::Closed ||
                mFinalized.load(std::memory_order_acquire))
            {
                return;
            }

            const std::size_t completedBytesBefore = mFrameReader.CompletedBodyBytes();
            const std::size_t completedFramesBefore = mFrameReader.CompletedFrameCount();
            ParseAdmission admission(*mParseAccounting);
            appended = mFrameReader.Append(bytes, &admission, &ParseAdmission::Admit);

            const std::size_t completedBytesAfter = mFrameReader.CompletedBodyBytes();
            const std::size_t completedFramesAfter = mFrameReader.CompletedFrameCount();
            SERVERCORE_ASSERT(completedBytesAfter >= completedBytesBefore,
                "FrameReader completed bytes decreased while appending");
            SERVERCORE_ASSERT(completedFramesAfter >= completedFramesBefore,
                "FrameReader completed frame count decreased while appending");
            const std::size_t newCompletedBytes = completedBytesAfter - completedBytesBefore;
            const std::size_t newCompletedFrames = completedFramesAfter - completedFramesBefore;
            admission.ReleaseNotStored(newCompletedBytes, newCompletedFrames);
            mUnscheduledParseBytes += newCompletedBytes;
            mUnscheduledParseTasks += newCompletedFrames;

            if (!appended.IsOk())
            {
                // admission 거부 또는 FrameReader 자체 실패 전에 같은 batch의 앞 frame이 완료될
                // 수 있다. fallback finalizer와 같은 잠금 아래 모두 비워 reservation을 한 번만
                // 반납한다.
                DiscardQueuedParseFramesLocked();
            }
        }

        if (!appended.IsOk())
        {
            FailAndDisconnect(std::move(appended));
            return;
        }

        StartNextParseOnRunner();
    }

    // 서로 다른 세션의 JSON만 병렬화한다. 한 세션은 현재 결과의 JobRunner 처리까지 끝낸 뒤
    // 다음 본문을 넘겨, Join 뒤 Profile/Chat이 worker 완료 순서 때문에 앞서 실행되지 않게 한다.
    void StartNextParseOnRunner()
    {
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr || !host->mJobRunner.IsCurrentThread() || !host->IsAccepting() ||
            mParseAccounting == nullptr)
        {
            return;
        }

        std::shared_ptr<ParseWorkItem> work;
        std::vector<std::byte> body;
        Core::Status preparation = Core::Status::Ok();
        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            const ServerCore::Session::SessionState state =
                mSessionState.load(std::memory_order_acquire);
            if (state == ServerCore::Session::SessionState::Closing ||
                state == ServerCore::Session::SessionState::Closed ||
                mFinalized.load(std::memory_order_acquire) || mParseInFlight)
            {
                return;
            }

            Core::Result<std::vector<std::byte>> nextBody = mFrameReader.TakeNextFrame();
            if (!nextBody.IsOk())
            {
                if (nextBody.GetStatus().Code() == Core::ErrorCode::WouldBlock)
                {
                    return;
                }
                preparation = std::move(nextBody).TakeStatus();
            }
            else
            {
                body = std::move(nextBody.Value());
                SERVERCORE_ASSERT(mUnscheduledParseTasks != 0,
                    "a parse frame was removed without a pending task reservation");
                SERVERCORE_ASSERT(mUnscheduledParseBytes >= body.size(),
                    "a parse frame was removed without enough pending byte reservation");
                --mUnscheduledParseTasks;
                mUnscheduledParseBytes -= body.size();

                try
                {
                    work = std::make_shared<ParseWorkItem>(mParseAccounting, std::move(body));
                    mParseInFlight = true;
                }
                catch (const std::bad_alloc&)
                {
                    // body는 completed queue에서 이미 빠졌으므로 이 reservation은 여기서 직접
                    // 반납한다. 실제 payload를 먼저 놓아 aggregate 상한에 빈틈을 만들지 않는다.
                    const std::size_t bodyBytes = body.size();
                    std::vector<std::byte>().swap(body);
                    mParseAccounting->Release(bodyBytes, 1);
                    preparation = Core::Status::AllocationFailure();
                }
            }
        }

        if (!preparation.IsOk())
        {
            FailAndDisconnect(std::move(preparation));
            return;
        }

        host->RecordReceivedFrame();

        if (host->mParsePool == nullptr)
        {
            {
                const std::lock_guard<std::mutex> guard(mParseStateMutex);
                mParseInFlight = false;
            }
            FailAndDisconnect(Core::Status::Fail(
                Core::ErrorCode::Closed, "the ServerHost parse worker pool is not available"));
            return;
        }

        const std::weak_ptr<NetworkSession> self = weak_from_this();
        const JobRunner::Lease runner = host->mJobRunner.AcquireLease();
        Core::Status posted = Core::Status::Ok();
        try
        {
            posted = host->mParsePool->Post(
                [self, runner, work]() mutable
                {
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
                    WaitBeforeParseForTest();
#endif
                    work->Parse();
                    const auto failCompletionPost = [&self]() noexcept
                    {
                        try
                        {
                            if (const std::shared_ptr<NetworkSession> session = self.lock())
                            {
                                // 이 실패 경로에서는 추가 할당도 실패할 수 있다. 빈 설명을 써서
                                // Status 자체가 두 번째 OOM 원인이 되지 않게 한다.
                                session->FailAndDisconnect(Core::Status::AllocationFailure());
                            }
                        }
                        catch (...)
                        {
                            // worker 밖으로 예외를 새면 ParseWorkerPool이 프로세스를 중단한다. 연결
                            // Close의 최선 노력까지 실패한 경우에는 reservation 소멸만 보장한다.
                        }
                    };

                    try
                    {
                        Core::Status completed = runner.Post(
                            [self, work]() mutable
                            {
                                if (const std::shared_ptr<NetworkSession> session = self.lock())
                                {
                                    session->CompleteParsedMessageOnRunner(std::move(work));
                                }
                            });
                        if (!completed.IsOk())
                        {
                            // JobRunner가 살아 있는 동안의 allocation 실패는 이 세션이 다음 frame을
                            // 영구히 기다리게 해서는 안 된다. worker는 Dispatcher/Registry를 만지지
                            // 않고 thread-safe한 Disconnect 경계만 통과시킨다. 정상 Stop의 Closed도
                            // 같은 경로를 타지만 CloseAllSessions가 뒤이어 정리하므로 무해하다.
                            if (const std::shared_ptr<NetworkSession> session = self.lock())
                            {
                                session->FailAndDisconnect(std::move(completed));
                            }
                        }
                    }
                    catch (...)
                    {
                        // lambda를 std::function으로 바꾸는 과정도 JobRunner::Post() 호출 전에
                        // 할당 실패할 수 있다. 그 경우에도 연결을 닫아 reservation을 끝낸다.
                        failCompletionPost();
                    }
                });
        }
        catch (const std::bad_alloc&)
        {
            posted = Core::Status::AllocationFailure();
        }
        catch (...)
        {
            posted = Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }

        if (!posted.IsOk())
        {
            {
                const std::lock_guard<std::mutex> guard(mParseStateMutex);
                mParseInFlight = false;
            }
            FailAndDisconnect(std::move(posted));
        }
    }

    [[nodiscard]] bool BeginMessageDispatch() noexcept
    {
        const std::lock_guard<std::mutex> guard(mFinalizationMutex);
        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (mFinalized.load(std::memory_order_relaxed) ||
            state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed)
        {
            return false;
        }

        // off-runner finalizer fallback은 이 깊이가 0이 될 때까지 OnSessionClosed를 미룬다.
        // 그러므로 닫힘 통지가 이미 시작된 뒤 새 handler가 호출되거나, 실행 중인 handler보다
        // 닫힘 통지가 앞서는 순서가 생기지 않는다.
        ++mLifecycleNotificationDepth;
        return true;
    }

    [[nodiscard]] bool BeginFinalizationDeferral() noexcept
    {
        const std::lock_guard<std::mutex> guard(mFinalizationMutex);
        if (mFinalized.load(std::memory_order_relaxed))
        {
            return false;
        }

        ++mLifecycleNotificationDepth;
        return true;
    }

    void CompleteParsedMessageOnRunner(std::shared_ptr<ParseWorkItem> work)
    {
        SERVERCORE_ASSERT(work != nullptr, "a parse completion arrived without owned work");

        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr || !host->mJobRunner.IsCurrentThread())
        {
            return;
        }

        {
            const std::lock_guard<std::mutex> guard(mParseStateMutex);
            SERVERCORE_ASSERT(mParseInFlight,
                "a parse completion arrived without an active NetworkSession parse task");
            mParseInFlight = false;
        }

        const ServerCore::Session::SessionState state =
            mSessionState.load(std::memory_order_acquire);
        if (!host->IsAccepting() || state == ServerCore::Session::SessionState::Closing ||
            state == ServerCore::Session::SessionState::Closed ||
            mFinalized.load(std::memory_order_acquire))
        {
            return;
        }

        Core::Result<Protocol::Message>& message = work->Message();
        if (!message.IsOk())
        {
            FailAndDisconnect(std::move(message).TakeStatus());
            return;
        }

        if (!BeginMessageDispatch())
        {
            return;
        }

        Core::Status dispatched = Core::Status::Ok();
        try
        {
            dispatched = host->mDispatcher.Dispatch(shared_from_this(), message.Value());
        }
        catch (...)
        {
            CompleteLifecycleNotification();
            throw;
        }
        CompleteLifecycleNotification();
        if (!dispatched.IsOk())
        {
            host->RecordError(dispatched);
            LogStatus(Core::LogLevel::Warn, dispatched);
        }

        const ServerCore::Session::SessionState afterDispatch =
            mSessionState.load(std::memory_order_acquire);
        if (afterDispatch != ServerCore::Session::SessionState::Closing &&
            afterDispatch != ServerCore::Session::SessionState::Closed)
        {
            StartNextParseOnRunner();
        }
    }

    void DiscardQueuedParseFrames() noexcept
    {
        const std::lock_guard<std::mutex> guard(mParseStateMutex);
        DiscardQueuedParseFramesLocked();
    }

    void DiscardQueuedParseFramesLocked() noexcept
    {
        // 동기 parse도 handler가 현재 frame에서 Disconnect하면 같은 Append에서 완성된 뒤 frame을
        // 남길 수 있다. 외부가 닫힌 Session을 계속 소유해도 completed/current payload뿐 아니라
        // maxBodySize 크기의 codec 저장소까지 즉시 놓는다.
        mFrameReader.ReleaseStorage();
        if (mParseAccounting != nullptr)
        {
            mParseAccounting->Release(mUnscheduledParseBytes, mUnscheduledParseTasks);
        }
        mUnscheduledParseBytes = 0;
        mUnscheduledParseTasks = 0;
    }

    void FinalizeOnRunner()
    {
        const std::shared_ptr<ServerHost::State> host = mHost.lock();
        if (host == nullptr)
        {
            return;
        }

        SERVERCORE_ASSERT(host->mJobRunner.IsCurrentThread(),
            "NetworkSession::FinalizeOnRunner() must run in the ServerHost JobRunner context");

        bool registered = false;
        bool opened = false;
        {
            const std::lock_guard<std::mutex> guard(mFinalizationMutex);
            if (mFinalized.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }

            registered = mRegistered.exchange(false, std::memory_order_acq_rel);
            opened = mOpened.exchange(false, std::memory_order_acq_rel);
        }

        // 아직 worker에 넘기지 않은 완료 frame은 이 시점부터 처리하지 않는다. 실행 중인 한 건은
        // ParseReservation이 worker/완료 lambda 수명 끝까지 따로 붙들므로 여기서 건드리지 않는다.
        DiscardQueuedParseFrames();

        if (registered)
        {
            const Core::Status unregistered = host->mRegistry.Unregister(mId);
            if (!unregistered.IsOk() && unregistered.Code() != Core::ErrorCode::NotFound)
            {
                LogStatus(Core::LogLevel::Warn, unregistered);
            }
        }
        else
        {
            // OpenOnRunner 전에 끊겼거나 Register 자체가 실패한 경우에도 IssueId 예약은 남기지
            // 않는다. 이 호출은 I/O 완료와 경합해도 SessionRegistry의 예약 잠금이 한 번만
            // 소비하게 한다.
            DiscardUnregisteredId();
        }

        // slot을 통지 전에 비워야 외부 fallback 통지 안의 Stop()이 자기 세션을 기다리지 않는다.
        // 대신 별도의 통지 계수를 같은 잠금에서 먼저 올려, 다른 Stop/Run 호출은 callback 반환까지
        // 계속 기다리게 한다. slot이 마지막 소유자여도 아래 통지 동안 this가 살아 있어야 한다.
        const std::shared_ptr<NetworkSession> keepAlive = weak_from_this().lock();
        SERVERCORE_ASSERT(
            keepAlive != nullptr, "a finalizing NetworkSession lost all shared ownership");
        host->FinishNetworkSession(this, opened);

        if (opened)
        {
            host->NotifyClosed(mId, TakeCloseReason());
        }
    }

    void RememberCloseReason(Core::Status reason)
    {
        const std::lock_guard<std::mutex> guard(mCloseReasonMutex);
        RememberCloseReasonLocked(std::move(reason));
    }

    void RememberCloseReasonLocked(Core::Status reason)
    {
        if (mHasCloseReason)
        {
            return;
        }

        mCloseReason = std::move(reason);
        mHasCloseReason = true;
    }

    [[nodiscard]] Core::Status TakeCloseReason()
    {
        const std::lock_guard<std::mutex> guard(mCloseReasonMutex);
        if (mHasCloseReason)
        {
            mHasCloseReason = false;
            return std::move(mCloseReason);
        }

        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    }

    std::weak_ptr<ServerHost::State> mHost;
    ServerCore::Session::SessionId mId;
    std::shared_ptr<Net::Connection> mConnection;
    std::uint64_t mLastReceivedMilliseconds = 0;
    // FrameReader는 원래 JobRunner 단일 스레드 전용이다. finalizer Post 실패만 I/O 스레드에서
    // completed payload를 직접 버려야 하므로, 그 fallback과 모든 parser 상태 변경을 직렬화한다.
    std::mutex mParseStateMutex;
    Protocol::FrameReader mFrameReader;
    std::uint32_t mMaxBodySize;
    std::uint32_t mMaxPendingReceiveBytes;
    std::shared_ptr<ParseAccounting> mParseAccounting;
    std::size_t mUnscheduledParseBytes = 0;
    std::size_t mUnscheduledParseTasks = 0;
    bool mParseInFlight = false;
    std::mutex mIdleMutex;
    std::mutex mReceiveMutex;
    std::vector<std::byte> mPendingReceiveBytes;
    // mPendingReceiveBytes에서 꺼내 처리 중인 batch도 포함한다. vector를 swap했다는 이유로
    // 수신 예산을 먼저 돌려주면 처리 중인 payload와 새 수신이 상한 밖에서 공존할 수 있다.
    std::size_t mBufferedReceiveBytes = 0;
    bool mReceiveJobQueued = false;
    std::mutex mOutboundMutex;
    // mOutboundMutex가 보호하며, 시계가 0인 첫 밀리초에도 유효한 시작 시각이므로 optional이다.
    std::optional<std::uint64_t> mGracefulCloseStartedMilliseconds;
    std::atomic<ServerCore::Session::SessionState> mSessionState{
        ServerCore::Session::SessionState::Connected
    };
    std::mutex mFinalizationMutex;
    std::atomic<bool> mRegistered{ false };
    std::atomic<bool> mOpened{ false };
    std::atomic<bool> mFinalized{ false };
    std::size_t mLifecycleNotificationDepth = 0;
    bool mFinalizeAfterNotification = false;
    std::mutex mCloseReasonMutex;
    Core::Status mCloseReason = Core::Status::Ok();
    bool mHasCloseReason = false;
};

void ServerHost::State::NetworkSession::ConsumePendingBytesOnRunner()
{
    const std::shared_ptr<ServerHost::State> host = mHost.lock();
    if (host == nullptr || !host->mJobRunner.IsCurrentThread())
    {
        return;
    }

    std::vector<std::byte> batch;
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        batch.swap(mPendingReceiveBytes);
    }

    if (batch.empty())
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        mReceiveJobQueued = false;
        return;
    }

    const ServerCore::Session::SessionState state = mSessionState.load(std::memory_order_acquire);
    if (!host->IsAccepting() || state == ServerCore::Session::SessionState::Closing ||
        state == ServerCore::Session::SessionState::Closed)
    {
        const std::size_t batchSize = batch.size();
        std::vector<std::byte>().swap(batch);
        CompleteReceiveBatchOnRunner(batchSize);
        DiscardQueuedReceiveBytes();
        return;
    }

    const std::size_t batchSize = batch.size();
    ConsumeBytesOnRunner(batch);
    // 다른 I/O thread가 반환된 Host 예산으로 새 수신 vector를 만들기 전에, 이 batch의 실제
    // payload 저장소부터 해제한다.
    std::vector<std::byte>().swap(batch);
    CompleteReceiveBatchOnRunner(batchSize);

    bool queueContinuation = false;
    bool discardQueuedBytes = false;
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        const ServerCore::Session::SessionState afterConsume =
            mSessionState.load(std::memory_order_acquire);
        if (!host->IsAccepting() || afterConsume == ServerCore::Session::SessionState::Closing ||
            afterConsume == ServerCore::Session::SessionState::Closed)
        {
            discardQueuedBytes = true;
        }
        else if (!mPendingReceiveBytes.empty())
        {
            // 한 배치만 처리하고 줄 끝으로 다시 예약한다. 바쁜 한 연결이 JobRunner를 계속
            // 점유하지 않으면서도, 세션마다 대기 작업은 항상 하나뿐이다.
            queueContinuation = true;
        }
        else
        {
            mReceiveJobQueued = false;
        }
    }

    if (discardQueuedBytes)
    {
        DiscardQueuedReceiveBytes();
        return;
    }

    if (queueContinuation)
    {
        Core::Status posted = Core::Status::Ok();
        try
        {
            const std::shared_ptr<NetworkSession> self = shared_from_this();
            posted = host->mJobRunner.Post([self]() { self->ConsumePendingBytesOnRunner(); });
        }
        catch (const std::bad_alloc&)
        {
            posted = Core::Status::AllocationFailure();
        }
        catch (...)
        {
            posted = Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }
        if (!posted.IsOk())
        {
            FailAndDisconnect(std::move(posted));
        }
    }
}

void ServerHost::State::NetworkSession::CompleteReceiveBatchOnRunner(const std::size_t batchSize)
{
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        SERVERCORE_ASSERT(mBufferedReceiveBytes >= batchSize,
            "completed receive bytes exceeded the NetworkSession budget");
        mBufferedReceiveBytes -= batchSize;
    }

    if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
    {
        host->ReleasePendingReceiveBytes(batchSize);
    }
}

void ServerHost::State::NetworkSession::DiscardQueuedReceiveBytes()
{
    std::size_t queuedBytes = 0;
    {
        const std::lock_guard<std::mutex> guard(mReceiveMutex);
        queuedBytes = mPendingReceiveBytes.size();
        SERVERCORE_ASSERT(mBufferedReceiveBytes >= queuedBytes,
            "discarded receive bytes exceeded the NetworkSession budget");
        mBufferedReceiveBytes -= queuedBytes;
        std::vector<std::byte>().swap(mPendingReceiveBytes);
        mReceiveJobQueued = false;
    }

    if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
    {
        host->ReleasePendingReceiveBytes(queuedBytes);
    }
}

void ServerHost::State::NetworkSession::DiscardUnregisteredId() noexcept
{
    if (mRegistered.load(std::memory_order_acquire))
    {
        return;
    }

    if (const std::shared_ptr<ServerHost::State> host = mHost.lock())
    {
        try
        {
            // 이미 Register()가 예약을 소비했거나 다른 종료 경로가 먼저 버렸다면
            // InvalidArgument가 정상이다. 이 함수의 목적은 예약 메모리를 남기지 않는 것이며
            // 번호 자체는 어떤 경우에도 재사용되지 않는다.
            (void)host->mRegistry.DiscardIssuedId(mId);
        }
        catch (...)
        {
            // I/O 종료 경로에서 예외를 내보내면 worker를 잃는다. 예약 정리 실패는 다음 Host
            // 수명까지 남을 수 있지만, 종료와 outstanding 계수 정리는 계속해야 한다.
        }
    }
}

void ServerHost::State::NetworkSession::FinalizeAfterRunnerFailure() noexcept
{
    const std::shared_ptr<ServerHost::State> host = mHost.lock();
    if (host == nullptr)
    {
        return;
    }

    bool registered = false;
    bool opened = false;
    {
        const std::lock_guard<std::mutex> guard(mFinalizationMutex);

        // OnSessionOpened 안에서 동기 Disconnect가 일어나고 finalizer Post까지 실패할 수 있다.
        // callback의 순서를 뒤집거나 같은 mutex에 재진입하지 않고, OpenOnRunner가 callback을
        // 반환한 직후 이 정리를 다시 수행하게 한다.
        if (mLifecycleNotificationDepth != 0)
        {
            mFinalizeAfterNotification = true;
            return;
        }
        if (mFinalized.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        registered = mRegistered.exchange(false, std::memory_order_acq_rel);
        opened = mOpened.exchange(false, std::memory_order_acq_rel);
    }

    // 외부 게임 코드가 닫힌 Session shared_ptr를 계속 들고 있어도 completed payload와 그
    // Host aggregate reservation은 finalization 시점에 끝나야 한다. runner와 경합할 수 있으므로
    // FrameReader를 포함한 parser 상태는 전용 잠금 아래 비운다. worker가 가진 한 건은 별도의
    // ParseReservation이 끝날 때 반납된다.
    DiscardQueuedParseFrames();

    // 이 경로는 JobRunner 자체가 더는 정리 작업을 받지 못하는 비정상 경계다. Unregister의
    // thread-safe 정리 경계로 닫힌 세션을 목록에서 빼고, 등록 전이었다면 발급 예약만 버린다.
    // 어느 쪽이든 Host의 preallocated slot과 outstanding 계수까지 정확히 한 번 정리한다.
    if (registered)
    {
        try
        {
            const Core::Status unregistered = host->mRegistry.Unregister(mId);
            if (!unregistered.IsOk() && unregistered.Code() != Core::ErrorCode::NotFound)
            {
                LogStatus(Core::LogLevel::Warn, unregistered);
            }
        }
        catch (...)
        {
            Core::ReportAssertFailure("SessionRegistry::Unregister() did not throw", __FILE__,
                __LINE__, "fallback session finalization could not update the session registry");
        }
    }
    else
    {
        DiscardUnregisteredId();
    }

    const std::shared_ptr<NetworkSession> keepAlive = weak_from_this().lock();
    SERVERCORE_ASSERT(
        keepAlive != nullptr, "a finalizing NetworkSession lost all shared ownership");
    host->FinishNetworkSession(this, opened);

    if (opened)
    {
        try
        {
            host->NotifyClosed(mId, TakeCloseReason());
        }
        catch (...)
        {
            Core::ReportAssertFailure("fallback close notification did not throw", __FILE__,
                __LINE__, "fallback session finalization could not notify a session close");
        }
    }
}

Core::Status ServerHost::State::Configure(const Core::Config& config)
{
    try
    {
        ServerHostOptions options;

        const Core::Result<int> port = config.GetInt(HostConfigPortKey);
        if (!port.IsOk())
        {
            return WithConfigKeyContext(HostConfigPortKey, port.GetStatus());
        }
        if (port.Value() < 0 ||
            port.Value() > static_cast<int>((std::numeric_limits<std::uint16_t>::max)()))
        {
            return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost config 'servercore.host.port' must fit a TCP port number");
        }
        options.port = static_cast<std::uint16_t>(port.Value());

        Core::Status listenAddress =
            ApplyOptionalString(config, HostConfigListenAddressKey, options.listenAddress);
        if (!listenAddress.IsOk())
        {
            return std::move(listenAddress);
        }
        Core::Status ioWorkers =
            ApplyOptionalInt(config, HostConfigIoWorkerThreadCountKey, options.ioWorkerThreadCount);
        if (!ioWorkers.IsOk())
        {
            return std::move(ioWorkers);
        }
        Core::Status parseWorkers = ApplyOptionalInt(
            config, HostConfigParseWorkerThreadCountKey, options.parseWorkerThreadCount);
        if (!parseWorkers.IsOk())
        {
            return std::move(parseWorkers);
        }
        Core::Status acceptBacklog =
            ApplyOptionalInt(config, HostConfigAcceptBacklogKey, options.acceptBacklog);
        if (!acceptBacklog.IsOk())
        {
            return std::move(acceptBacklog);
        }

        const Core::Result<int> idleSessionTimeout = config.GetInt(HostConfigIdleSessionTimeoutKey);
        if (idleSessionTimeout.IsOk())
        {
            options.idleSessionTimeout = std::chrono::milliseconds(idleSessionTimeout.Value());
        }
        else if (idleSessionTimeout.GetStatus().Code() != Core::ErrorCode::NotFound)
        {
            return WithConfigKeyContext(
                HostConfigIdleSessionTimeoutKey, idleSessionTimeout.GetStatus());
        }

        const Core::Result<int> gracefulCloseTimeout = config.GetInt(HostConfigGracefulCloseTimeoutKey);
        if (gracefulCloseTimeout.IsOk())
        {
            options.gracefulCloseTimeout = std::chrono::milliseconds(gracefulCloseTimeout.Value());
        }
        else if (gracefulCloseTimeout.GetStatus().Code() != Core::ErrorCode::NotFound)
        {
            return WithConfigKeyContext(
                HostConfigGracefulCloseTimeoutKey, gracefulCloseTimeout.GetStatus());
        }

        Core::Status maxBodySize =
            ApplyOptionalUint32(config, HostConfigMaxBodySizeKey, options.maxBodySize);
        if (!maxBodySize.IsOk())
        {
            return std::move(maxBodySize);
        }
        Core::Status maxConcurrentSessions = ApplyOptionalUint32(
            config, HostConfigMaxConcurrentSessionsKey, options.maxConcurrentSessions);
        if (!maxConcurrentSessions.IsOk())
        {
            return std::move(maxConcurrentSessions);
        }
        Core::Status maxTotalSendQueueCapacityBytes = ApplyOptionalUint32(config,
            HostConfigMaxTotalSendQueueCapacityBytesKey, options.maxTotalSendQueueCapacityBytes);
        if (!maxTotalSendQueueCapacityBytes.IsOk())
        {
            return std::move(maxTotalSendQueueCapacityBytes);
        }
        Core::Status maxPendingReceiveBytes = ApplyOptionalUint32(
            config, HostConfigMaxPendingReceiveBytesKey, options.maxPendingReceiveBytes);
        if (!maxPendingReceiveBytes.IsOk())
        {
            return std::move(maxPendingReceiveBytes);
        }
        Core::Status maxTotalPendingReceiveBytes = ApplyOptionalUint32(
            config, HostConfigMaxTotalPendingReceiveBytesKey, options.maxTotalPendingReceiveBytes);
        if (!maxTotalPendingReceiveBytes.IsOk())
        {
            return std::move(maxTotalPendingReceiveBytes);
        }
        Core::Status maxPendingParseBytes = ApplyOptionalUint32(
            config, HostConfigMaxPendingParseBytesKey, options.maxPendingParseBytes);
        if (!maxPendingParseBytes.IsOk())
        {
            return std::move(maxPendingParseBytes);
        }
        Core::Status maxTotalPendingParseBytes = ApplyOptionalUint32(
            config, HostConfigMaxTotalPendingParseBytesKey, options.maxTotalPendingParseBytes);
        if (!maxTotalPendingParseBytes.IsOk())
        {
            return std::move(maxTotalPendingParseBytes);
        }
        Core::Status maxPendingParseTasks = ApplyOptionalUint32(
            config, HostConfigMaxPendingParseTasksKey, options.maxPendingParseTasks);
        if (!maxPendingParseTasks.IsOk())
        {
            return std::move(maxPendingParseTasks);
        }
        Core::Status maxTotalPendingParseTasks = ApplyOptionalUint32(
            config, HostConfigMaxTotalPendingParseTasksKey, options.maxTotalPendingParseTasks);
        if (!maxTotalPendingParseTasks.IsOk())
        {
            return std::move(maxTotalPendingParseTasks);
        }

        return Configure(options);
    }
    catch (const std::bad_alloc&)
    {
        return ConfigMappingResourceFailure();
    }
    catch (const std::exception&)
    {
        return ConfigMappingResourceFailure();
    }
    catch (...)
    {
        return ConfigMappingResourceFailure();
    }
}

Core::Status ServerHost::State::Configure(const ServerHostOptions& options)
{
    Core::Status valid = ValidateOptions(options);
    if (!valid.IsOk())
    {
        return std::move(valid);
    }

    std::optional<ServerHostOptions> copiedOptions;
    try
    {
        copiedOptions.emplace(options);
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::exception& error)
    {
        return PlatformFailureFrom(error);
    }

    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    if (mLifecycle != Lifecycle::Ready)
    {
        return Core::Status::Fail(
            Core::ErrorCode::Closed, "ServerHost cannot be configured after startup has begun");
    }

    mOptions = std::move(*copiedOptions);
    mConfigured = true;
    return Core::Status::Ok();
}

void ServerHost::State::SetLogger(std::shared_ptr<Core::ILogger> logger)
{
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    SERVERCORE_ASSERT(
        mLifecycle == Lifecycle::Ready, "ServerHost::SetLogger() must be called before Start()");
    mLogger = std::move(logger);
}

Dispatch::Dispatcher& ServerHost::State::GetDispatcher() noexcept
{
    return mDispatcher;
}

const Session::SessionRegistry& ServerHost::State::GetSessions() const noexcept
{
    return mRegistry;
}

JobRunner::Lease ServerHost::State::GetJobRunner() const noexcept
{
    return mJobRunner.AcquireLease();
}

void ServerHost::State::SetSessionObserver(std::weak_ptr<Session::ISessionObserver> observer)
{
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    SERVERCORE_ASSERT(mLifecycle == Lifecycle::Ready,
        "ServerHost::SetSessionObserver() must be called before Start()");
    mSessionObserver = std::move(observer);
}

Core::Status ServerHost::State::StartJobRunner()
{
    try
    {
        const std::shared_ptr<State> self = shared_from_this();
        std::shared_ptr<std::promise<Core::Status>> bound =
            std::make_shared<std::promise<Core::Status>>();
        std::future<Core::Status> result = bound->get_future();
        mJobThread = std::thread(
            [self, bound]()
            {
                // JobRunner가 대기열을 보기 전에 Registry를 이 실행 스레드에 묶는다. Start 전에
                // 받아 둔 Lease 작업도 여기보다 먼저 실행될 수 없으므로, 첫 작업부터 세션 표를
                // 안전하게 쓸 수 있다.
                const auto publishFailure = [&bound](Core::Status failure) noexcept
                {
                    try
                    {
                        bound->set_value(std::move(failure));
                    }
                    catch (...)
                    {
                        // 이미 성공 결과를 넘긴 뒤 RunUntilStopped가 실패한 경우와, promise 자체의
                        // 예외 모두 아래 Stop으로 수렴한다. 시작 호출자는 parent 참조를 놓았으므로
                        // 아직 결과가 없었다면 broken_promise로라도 반드시 깨어난다.
                    }
                };

                try
                {
                    Core::Status boundStatus = self->mRegistry.BindToCurrentThread();
                    const bool boundSuccessfully = boundStatus.IsOk();
                    bound->set_value(std::move(boundStatus));
                    if (!boundSuccessfully)
                    {
                        self->mJobRunner.Stop();
                        return;
                    }

                    self->mJobRunner.RunUntilStopped();
                }
                catch (const std::bad_alloc&)
                {
                    publishFailure(Core::Status::AllocationFailure());
                    self->mJobRunner.Stop();
                }
                catch (...)
                {
                    publishFailure(
                        Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError));
                    self->mJobRunner.Stop();
                }
            });

        // worker만 promise를 소유하게 한다. set_value 자체가 예외를 내더라도 worker 종료 시
        // future가 broken_promise로 준비되어 Start()가 영구히 기다리지 않는다.
        bound.reset();
        return result.get();
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::system_error& error)
    {
        return PlatformFailureFrom(error);
    }
    catch (const std::future_error& error)
    {
        return PlatformFailureFrom(error);
    }
}

Core::Status ServerHost::State::Start()
{
    std::optional<ServerHostOptions> optionsSnapshot;
    std::shared_ptr<Core::ILogger> logger;
    {
        const std::lock_guard<std::mutex> guard(mLifecycleMutex);
        if (mLifecycle == Lifecycle::Starting || mLifecycle == Lifecycle::Running)
        {
            return Core::Status::Fail(
                Core::ErrorCode::AlreadyExists, "ServerHost is already started");
        }
        if (mLifecycle == Lifecycle::Stopping || mLifecycle == Lifecycle::Stopped)
        {
            return Core::Status::Fail(
                Core::ErrorCode::Closed, "ServerHost cannot be started again");
        }
        if (!mConfigured)
        {
            return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost requires ServerHostOptions before Start()");
        }

        try
        {
            optionsSnapshot.emplace(mOptions);
        }
        catch (const std::bad_alloc&)
        {
            return Core::Status::AllocationFailure();
        }
        catch (const std::exception& error)
        {
            return PlatformFailureFrom(error);
        }

        logger = mLogger;
        mLifecycle = Lifecycle::Starting;
    }

    const ServerHostOptions& options = *optionsSnapshot;

    Core::SetGlobalLogger(std::move(logger));

    try
    {
        try
        {
            // 종료 자체가 새 메모리를 요구하지 않도록, 수락할 수 있는 세션의 소유 슬롯을 포트를
            // 열기 전에 모두 확보한다. Stop은 이 배열을 직접 훑어 Connection을 닫는다. Registry에
            // 들어가기 전 끊긴 세션도 OnDisconnected까지 살아 있어야 하므로 슬롯이 강하게 소유한다.
            const std::lock_guard<std::mutex> lifecycleGuard(mLifecycleMutex);
            mSessionSlots.resize(options.maxConcurrentSessions);
            mIo = std::make_unique<Net::IoContext>();
            mAcceptor = std::make_unique<Net::Acceptor>();
            Net::AcceptorAccess::SetSendBudget(*mAcceptor,
                std::make_shared<Net::SendBudget>(options.maxTotalSendQueueCapacityBytes));
            if (options.parseWorkerThreadCount != 0)
            {
                mParsePool = std::make_unique<ParseWorkerPool>();
            }
        }
        catch (const std::bad_alloc&)
        {
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }

        Core::Status ioStarted = Core::Status::Ok();
        try
        {
            ioStarted = mIo->Start(options.ioWorkerThreadCount);
        }
        catch (const std::bad_alloc&)
        {
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }
        catch (const std::exception& error)
        {
            CompleteFailedStart();
            return PlatformFailureFrom(error);
        }
        if (!ioStarted.IsOk())
        {
            CompleteFailedStart();
            return std::move(ioStarted);
        }

        Core::Status runnerStarted = StartJobRunner();
        if (!runnerStarted.IsOk())
        {
            CompleteFailedStart();
            return std::move(runnerStarted);
        }

        if (mParsePool != nullptr)
        {
            Core::Status parseStarted = mParsePool->Start(options.parseWorkerThreadCount);
            if (!parseStarted.IsOk())
            {
                CompleteFailedStart();
                return std::move(parseStarted);
            }
        }

        {
            try
            {
                const std::weak_ptr<State> self = weak_from_this();
                mSessionTimeoutRunner = std::make_unique<PeriodicRunner>(mJobRunner.AcquireLease(),
                    SessionTimeoutScanPeriod(options),
                    [self]()
                    {
                        if (const std::shared_ptr<State> host = self.lock())
                        {
                            host->CloseExpiredSessionsOnRunner();
                        }
                    });
            }
            catch (const std::bad_alloc&)
            {
                CompleteFailedStart();
                return Core::Status::AllocationFailure();
            }
            catch (const std::exception& error)
            {
                CompleteFailedStart();
                return PlatformFailureFrom(error);
            }

            Core::Status timeoutTimerStarted = mSessionTimeoutRunner->Start();
            if (!timeoutTimerStarted.IsOk())
            {
                CompleteFailedStart();
                return std::move(timeoutTimerStarted);
            }
        }

        try
        {
            const std::weak_ptr<State> self = weak_from_this();
            mAcceptor->SetConnectionHandler(
                [self](std::shared_ptr<Net::Connection> connection)
                {
                    if (const std::shared_ptr<State> host = self.lock())
                    {
                        host->OnConnectionAccepted(std::move(connection));
                        return;
                    }

                    connection->Close();
                });
        }
        catch (const std::bad_alloc&)
        {
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }

        try
        {
            Core::Status listened =
                mAcceptor->Listen(options.listenAddress, options.port, options.acceptBacklog);
            if (!listened.IsOk())
            {
                CompleteFailedStart();
                return std::move(listened);
            }

            // 이 시점부터 등록표는 읽기 전용이다. 수락 전이므로 첫 메시지가 등록 중인 표를 볼 수 없다.
            mDispatcher.Freeze();
            mAccepting.store(true, std::memory_order_release);

            Core::Status accepting = mAcceptor->Start(*mIo);
            if (!accepting.IsOk())
            {
                mAccepting.store(false, std::memory_order_release);
                CompleteFailedStart();
                return std::move(accepting);
            }
        }
        catch (const std::bad_alloc&)
        {
            mAccepting.store(false, std::memory_order_release);
            CompleteFailedStart();
            return Core::Status::AllocationFailure();
        }
        catch (const std::exception& error)
        {
            mAccepting.store(false, std::memory_order_release);
            CompleteFailedStart();
            return PlatformFailureFrom(error);
        }
        catch (...)
        {
            mAccepting.store(false, std::memory_order_release);
            CompleteFailedStart();
            return Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
        }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        // 수락기가 실제로 동작하지만 Lifecycle은 아직 Starting인 좁은 경계를 고정한다.
        WaitBeforeHostRunningForTest();
#endif

        mPort.store(options.port, std::memory_order_release);
        mRunning.store(true, std::memory_order_release);
        bool stopAfterStart = false;
        {
            const std::lock_guard<std::mutex> guard(mLifecycleMutex);
            mLifecycle = Lifecycle::Running;
            stopAfterStart = mStopRequestedDuringStart;
            mLifecycleChanged.notify_all();
        }

        // 수락기가 시작된 아주 짧은 Starting 구간에도 연결 종료 callback이 올 수 있다. 그
        // callback에서 Stop을 요청했다면 callback 자신을 기다리지 말고 여기서 부팅을 끝낸 뒤
        // 정상 종료 순서를 대신 수행한다.
        if (stopAfterStart)
        {
            Stop();
            return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
        }

        return Core::Status::Ok();
    }
    catch (const std::bad_alloc&)
    {
        mAccepting.store(false, std::memory_order_release);
        CompleteFailedStart();
        return Core::Status::AllocationFailure();
    }
    catch (...)
    {
        mAccepting.store(false, std::memory_order_release);
        CompleteFailedStart();
        return Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError);
    }
}

void ServerHost::State::CompleteFailedStart() noexcept
{
    mAccepting.store(false, std::memory_order_release);
    mRunning.store(false, std::memory_order_release);
    mPort.store(0, std::memory_order_release);

    // PeriodicRunner는 JobRunner에 일을 넣을 수 있으므로, 실행자를 멈추기 전에 timer thread를
    // 먼저 거둔다. 이미 큐에 든 콜백은 mAccepting=false를 보고 아무 세션도 건드리지 않는다.
    if (mSessionTimeoutRunner != nullptr)
    {
        mSessionTimeoutRunner->Stop();
        mSessionTimeoutRunner.reset();
    }

    if (mAcceptor != nullptr)
    {
        mAcceptor->Stop();
    }

    if (mParsePool != nullptr)
    {
        mParsePool->StopAndDiscard();
    }

    // Start()가 mAccepting=true를 공개한 뒤 수락기 준비에서 실패할 수도 있다. 그 짧은 사이에
    // 만들어진 세션도 정상 Stop과 같은 순서로 닫고, overlapped 완료가 모두 돌아와 slot을
    // 반납할 때까지 I/O를 먼저 내리지 않는다.
    CloseAllSessions();
    {
        std::unique_lock<std::mutex> guard(mLifecycleMutex);
        mLifecycleChanged.wait(guard,
            [this]() { return mOutstandingSessions == 0 && mInFlightCloseNotifications == 0; });
    }

    if (mIo != nullptr)
    {
        mIo->Stop();
    }

    mJobRunner.Stop();
    if (mJobThread.joinable())
    {
        mJobThread.join();
    }

    // Starting을 기다리기 전 Stop()도 I/O·parse 소유자를 읽어 자기 실행 문맥을 검사한다.
    // 정상 종료와 같은 잠금 안에서 파기해야 실패 정리와 그 읽기 사이의 경합이 없다.
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
    if (const auto gate = GetFailedStartOwnerGateForTest())
    {
        gate->WaitBeforeFailedStartOwnerReset();
    }
#endif
    mAcceptor.reset();
    mIo.reset();
    mParsePool.reset();
    mLifecycle = Lifecycle::Stopped;
    mLifecycleChanged.notify_all();
}

void ServerHost::State::Stop()
{
    Net::Acceptor* acceptor = nullptr;
    Net::IoContext* io = nullptr;
    const std::size_t callerCloseNotificationDepth = CurrentCloseNotificationDepth(this);
    {
        std::unique_lock<std::mutex> guard(mLifecycleMutex);

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        if (const auto gate = GetFailedStartOwnerGateForTest())
        {
            gate->OnStopOwnerRead();
        }
#endif
        // Starting을 기다리기 전에 현재 실행 문맥부터 거절한다. Start 전에 받아 둔 Lease 작업이나
        // 시작 도중 들어온 I/O callback이 여기서 기다리면 Start 실패 정리의 join과 서로 막힌다.
        SERVERCORE_ASSERT(!mJobRunner.IsCurrentThread(),
            "ServerHost::Stop() cannot run from its JobRunner thread");
        if (mIo != nullptr)
        {
            SERVERCORE_ASSERT(!mIo->IsCurrentThreadIoThread(),
                "ServerHost::Stop() cannot run from one of its I/O threads");
        }
        if (mParsePool != nullptr)
        {
            SERVERCORE_ASSERT(!mParsePool->IsCurrentThread(),
                "ServerHost::Stop() cannot run from one of its parse worker threads");
        }
        SERVERCORE_ASSERT(mInFlightCloseNotifications >= callerCloseNotificationDepth,
            "ServerHost close-notification thread depth exceeded its lifecycle count");

        while (mLifecycle == Lifecycle::Starting)
        {
            if (callerCloseNotificationDepth != 0)
            {
                // Start 실패 정리도 이 callback 계수가 0이 되기를 기다린다. callback이 여기서
                // Starting 종료를 기다리면 서로 막히므로 요청만 남기고 Start 호출자가 이어서
                // 정상 Stop을 수행하게 한다.
                mStopRequestedDuringStart = true;
                return;
            }
            mLifecycleChanged.wait(guard);
        }

        if (mLifecycle == Lifecycle::Ready)
        {
            return;
        }
        if (mLifecycle == Lifecycle::Stopped)
        {
            if (callerCloseNotificationDepth == 0)
            {
                mLifecycleChanged.wait(
                    guard, [this]() { return mInFlightCloseNotifications == 0; });
            }
            return;
        }
        if (mLifecycle == Lifecycle::Stopping)
        {
            // 다른 Stop은 이 callback의 반환을 기다린다. 여기서 그 Stop을 기다리면 순환하므로
            // callback 문맥은 이미 진행 중인 종료 요청에 합류한 것으로 보고 곧바로 돌아간다.
            if (callerCloseNotificationDepth != 0)
            {
                return;
            }
            mLifecycleChanged.wait(guard, [this]()
                { return mLifecycle == Lifecycle::Stopped && mInFlightCloseNotifications == 0; });
            return;
        }

        mLifecycle = Lifecycle::Stopping;
        mAccepting.store(false, std::memory_order_release);
        mRunning.store(false, std::memory_order_release);
        mPort.store(0, std::memory_order_release);
        acceptor = mAcceptor.get();
        io = mIo.get();
    }

    // Stop 뒤 새 timeout callback이 JobRunner에 들어가면 종료 작업과 순서가 섞인다. timer를 먼저
    // 멈추면 이미 들어간 callback도 mAccepting=false 경로로 no-op이 된다.
    if (mSessionTimeoutRunner != nullptr)
    {
        mSessionTimeoutRunner->Stop();
        mSessionTimeoutRunner.reset();
    }

    // Acceptor는 I/O가 살아 있을 때 먼저 drain해야 한다. 그 뒤부터는 OnConnectionAccepted가
    // 새 세션을 등록하지 못하고, 이미 만든 후보만 JobRunner에서 닫히기를 기다리면 된다.
    if (acceptor != nullptr)
    {
        acceptor->Stop();
    }

    // 파서 worker의 완료가 JobRunner 종료 뒤에 post되지 않게 먼저 입력을 닫고 worker를 거둔다.
    // Stop이 이미 mAccepting=false를 보였으므로 진행 중인 파싱 결과도 게임 handler로 가지 않는다.
    if (mParsePool != nullptr)
    {
        mParsePool->StopAndDiscard();
    }

    // 수락 handler가 모두 빠진 뒤이므로 슬롯에는 이 시점의 모든 NetworkSession이 들어 있다.
    // startup 때 미리 확보한 슬롯을 직접 훑어, 종료 작업 enqueue나 Registry snapshot 할당 없이
    // 스레드 안전한 Disconnect를 시작한다.
    CloseAllSessions();

    {
        std::unique_lock<std::mutex> guard(mLifecycleMutex);
        mLifecycleChanged.wait(guard,
            [this, callerCloseNotificationDepth]()
            {
                return mOutstandingSessions == 0 &&
                       mInFlightCloseNotifications <= callerCloseNotificationDepth;
            });
    }

    // NetworkSession은 OnDisconnected가 온 뒤에야 outstanding에서 빠진다. 즉 여기서는 각
    // Connection의 overlapped 작업도 끝났으므로 I/O를 멈춰도 완료 수명은 남지 않는다.
    if (io != nullptr)
    {
        io->Stop();
    }

    mJobRunner.Stop();
    if (mJobThread.joinable())
    {
        mJobThread.join();
    }

    // Lifecycle이 Stopping인 동안 다른 Stop() 호출자는 이 잠금에서 현재 I/O·parse worker 소유자를
    // 읽어 자기 스레드가 금지된 문맥인지 확인한다. 따라서 그 포인터들을 파기하고 Stopped를 알리는
    // 전이를 하나의 임계 구역으로 묶어, 두 번째 호출자가 이미 파기한 객체를 역참조하지 않게 한다.
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    mAcceptor.reset();
    mIo.reset();
    mParsePool.reset();
    mLifecycle = Lifecycle::Stopped;
    mLifecycleChanged.notify_all();
}

int ServerHost::State::Run()
{
    std::unique_lock<std::mutex> guard(mLifecycleMutex);
    if (mLifecycle != Lifecycle::Running && mLifecycle != Lifecycle::Stopping)
    {
        return 1;
    }

    mLifecycleChanged.wait(guard,
        [this]() { return mLifecycle == Lifecycle::Stopped && mInFlightCloseNotifications == 0; });
    return 0;
}

bool ServerHost::State::IsRunning() const noexcept
{
    return mRunning.load(std::memory_order_acquire);
}

std::uint16_t ServerHost::State::Port() const noexcept
{
    return mPort.load(std::memory_order_acquire);
}

Core::Result<ServerMetricsSnapshot> ServerHost::State::SnapshotMetrics() const
{
    if (!mJobRunner.IsCurrentThread())
    {
        return Core::Result<ServerMetricsSnapshot>::FromStatus(
            Core::Status::Fail(Core::ErrorCode::InvalidArgument,
                "ServerHost::SnapshotMetrics() must run in the ServerHost JobRunner context"));
    }

    try
    {
        ServerMetricsSnapshot snapshot;
        snapshot.configuredIoWorkerThreadCount =
            static_cast<std::uint32_t>(mOptions.ioWorkerThreadCount);
        snapshot.configuredParseWorkerThreadCount =
            static_cast<std::uint32_t>(mOptions.parseWorkerThreadCount);
        snapshot.pendingReceiveBytes = mPendingReceiveBytes.load(std::memory_order_acquire);
        snapshot.pendingJobCount = mJobRunner.PendingCount();
        snapshot.pendingParseBytes = mPendingParseBytes.load(std::memory_order_acquire);
        snapshot.pendingParseTaskCount = mPendingParseTasks.load(std::memory_order_acquire);
        snapshot.receivedFrameCount = mReceivedFrameCount.load(std::memory_order_relaxed);
        snapshot.queuedSendFrameCount = mQueuedSendFrameCount.load(std::memory_order_relaxed);
        snapshot.errorCount = mErrorCount.load(std::memory_order_relaxed);
        snapshot.skippedPeriodCount = mJobRunner.PeriodicSkippedCount();

        snapshot.sessionSendQueues.reserve(mRegistry.Count());
        mRegistry.ForEach(
            [&snapshot](const std::shared_ptr<Session::Session>& session)
            {
                // ServerHost가 Registry에 넣는 구현은 NetworkSession 하나뿐이다. Session API에 전송 계층
                // 지표를 새로 새지 않기 위해 이 L5 조립 지점에서만 구체 구현을 읽는다.
                const std::shared_ptr<NetworkSession> networkSession =
                    std::dynamic_pointer_cast<NetworkSession>(session);
                SERVERCORE_ASSERT(networkSession != nullptr,
                    "ServerHost registry contained a session it did not create");
                if (networkSession == nullptr)
                {
                    return;
                }
                snapshot.sessionSendQueues.push_back(
                    SessionSendQueueSnapshot{ session->Id(), networkSession->QueuedSendBytes() });
            });
        std::sort(snapshot.sessionSendQueues.begin(), snapshot.sessionSendQueues.end(),
            [](const SessionSendQueueSnapshot& left, const SessionSendQueueSnapshot& right)
            { return static_cast<std::uint64_t>(left.id) < static_cast<std::uint64_t>(right.id); });
        snapshot.activeSessionCount = snapshot.sessionSendQueues.size();
        return Core::Result<ServerMetricsSnapshot>::FromValue(std::move(snapshot));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<ServerMetricsSnapshot>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& error)
    {
        return Core::Result<ServerMetricsSnapshot>::FromStatus(PlatformFailureFrom(error));
    }
}

bool ServerHost::State::IsAccepting() const noexcept
{
    return mAccepting.load(std::memory_order_acquire);
}

bool ServerHost::State::TryReservePendingReceiveBytes(const std::size_t byteCount) noexcept
{
    if (byteCount == 0)
    {
        return true;
    }

    const std::size_t limit = static_cast<std::size_t>(mOptions.maxTotalPendingReceiveBytes);
    std::size_t current = mPendingReceiveBytes.load(std::memory_order_acquire);
    for (;;)
    {
        if (current > limit || byteCount > limit - current)
        {
            return false;
        }
        if (mPendingReceiveBytes.compare_exchange_weak(
                current, current + byteCount, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true;
        }
    }
}

void ServerHost::State::ReleasePendingReceiveBytes(const std::size_t byteCount) noexcept
{
    if (byteCount == 0)
    {
        return;
    }

    std::size_t current = mPendingReceiveBytes.load(std::memory_order_acquire);
    for (;;)
    {
        SERVERCORE_ASSERT(current >= byteCount,
            "released receive bytes exceeded the ServerHost aggregate budget");
        if (mPendingReceiveBytes.compare_exchange_weak(
                current, current - byteCount, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return;
        }
    }
}

bool ServerHost::State::TryReservePendingParseWork(
    const std::size_t byteCount, const std::size_t taskCount) noexcept
{
    const auto reserve = [](std::atomic<std::size_t>& pending, const std::size_t limit,
                             const std::size_t amount) noexcept
    {
        if (amount == 0)
        {
            return true;
        }

        std::size_t current = pending.load(std::memory_order_acquire);
        for (;;)
        {
            if (current > limit || amount > limit - current)
            {
                return false;
            }
            if (pending.compare_exchange_weak(current, current + amount, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return true;
            }
        }
    };
    const auto release = [](std::atomic<std::size_t>& pending, const std::size_t amount) noexcept
    {
        if (amount == 0)
        {
            return;
        }

        std::size_t current = pending.load(std::memory_order_acquire);
        for (;;)
        {
            SERVERCORE_ASSERT(
                current >= amount, "released parse work exceeded the ServerHost aggregate budget");
            if (pending.compare_exchange_weak(current, current - amount, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return;
            }
        }
    };

    const std::size_t byteLimit = static_cast<std::size_t>(mOptions.maxTotalPendingParseBytes);
    const std::size_t taskLimit = static_cast<std::size_t>(mOptions.maxTotalPendingParseTasks);
    if (!reserve(mPendingParseBytes, byteLimit, byteCount))
    {
        return false;
    }
    if (reserve(mPendingParseTasks, taskLimit, taskCount))
    {
        return true;
    }

    release(mPendingParseBytes, byteCount);
    return false;
}

void ServerHost::State::ReleasePendingParseWork(
    const std::size_t byteCount, const std::size_t taskCount) noexcept
{
    const auto release = [](std::atomic<std::size_t>& pending, const std::size_t amount) noexcept
    {
        if (amount == 0)
        {
            return;
        }

        std::size_t current = pending.load(std::memory_order_acquire);
        for (;;)
        {
            SERVERCORE_ASSERT(
                current >= amount, "released parse work exceeded the ServerHost aggregate budget");
            if (pending.compare_exchange_weak(current, current - amount, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return;
            }
        }
    };

    release(mPendingParseBytes, byteCount);
    release(mPendingParseTasks, taskCount);
}

void ServerHost::State::RecordReceivedFrame() noexcept
{
    mReceivedFrameCount.fetch_add(1, std::memory_order_relaxed);
}

void ServerHost::State::RecordQueuedSendFrame() noexcept
{
    mQueuedSendFrameCount.fetch_add(1, std::memory_order_relaxed);
}

void ServerHost::State::RecordError(const Core::Status& status) noexcept
{
    if (status.IsOk() || status.Code() == Core::ErrorCode::Closed ||
        status.Code() == Core::ErrorCode::Timeout || status.Code() == Core::ErrorCode::WouldBlock)
    {
        return;
    }

    mErrorCount.fetch_add(1, std::memory_order_relaxed);
}

bool ServerHost::State::BeginNetworkSession()
{
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    if (!mAccepting.load(std::memory_order_acquire))
    {
        return false;
    }
    if (mOutstandingSessions >= static_cast<std::size_t>(mOptions.maxConcurrentSessions))
    {
        return false;
    }

    ++mOutstandingSessions;
    return true;
}

void ServerHost::State::TrackNetworkSession(const std::shared_ptr<NetworkSession>& session) noexcept
{
    SERVERCORE_ASSERT(session != nullptr, "ServerHost cannot track a null NetworkSession");

    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    SERVERCORE_ASSERT(mOutstandingSessions != 0,
        "ServerHost tracked a NetworkSession before reserving an outstanding slot");

    for (std::shared_ptr<NetworkSession>& slot : mSessionSlots)
    {
        if (slot == nullptr)
        {
            slot = session;
            return;
        }
    }

    SERVERCORE_ASSERT(false, "ServerHost exhausted its preallocated NetworkSession shutdown slots");
}

void ServerHost::State::FinishNetworkSession(
    const NetworkSession* const session, const bool beginCloseNotification) noexcept
{
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    SERVERCORE_ASSERT(
        mOutstandingSessions != 0, "ServerHost finalized a network session that it did not count");

    // slot/outstanding이 0으로 보이는 순간과 OnSessionClosed 시작 사이에 Stop이 빠져나가지 않게
    // 같은 잠금 안에서 통지 계수를 먼저 올린다. callback 안의 Stop만 TLS 깊이만큼 이 계수를
    // 제외할 수 있고, 다른 Stop과 Run은 통지가 실제로 끝날 때까지 기다린다.
    if (beginCloseNotification)
    {
        ++mInFlightCloseNotifications;
    }

    if (session != nullptr)
    {
        bool removed = false;
        for (std::shared_ptr<NetworkSession>& slot : mSessionSlots)
        {
            if (slot != nullptr && slot.get() == session)
            {
                slot.reset();
                removed = true;
                break;
            }
        }
        SERVERCORE_ASSERT(
            removed, "ServerHost finalized a NetworkSession absent from its shutdown slots");
    }

    --mOutstandingSessions;
    if (mOutstandingSessions == 0)
    {
        mLifecycleChanged.notify_all();
    }
}

void ServerHost::State::FinishCloseNotification() noexcept
{
    {
        const std::lock_guard<std::mutex> guard(mLifecycleMutex);
        SERVERCORE_ASSERT(mInFlightCloseNotifications != 0,
            "ServerHost completed a close notification that it did not count");
        --mInFlightCloseNotifications;
    }
    mLifecycleChanged.notify_all();
}

void ServerHost::State::CloseAllSessions() noexcept
{
    for (std::size_t index = 0; index < mSessionSlots.size(); ++index)
    {
        std::shared_ptr<NetworkSession> session;
        {
            const std::lock_guard<std::mutex> guard(mLifecycleMutex);
            session = mSessionSlots[index];
        }

        if (session == nullptr)
        {
            continue;
        }

        try
        {
            session->Disconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        }
        catch (...)
        {
            // 예외를 삼키고 기다리면 이 세션의 OnDisconnected가 오지 않아 Stop()이 영구히
            // 멈춘다. Disconnect의 공개 no-throw 계약 위반을 이 자리에서 명확히 드러낸다.
            Core::ReportAssertFailure("Session::Disconnect() did not throw", __FILE__, __LINE__,
                "a Session implementation threw while ServerHost was stopping");
        }
    }
}

void ServerHost::State::CloseExpiredSessionsOnRunner()
{
    SERVERCORE_ASSERT(mJobRunner.IsCurrentThread(),
        "ServerHost::CloseExpiredSessionsOnRunner() must run in the JobRunner context");

    if (!mAccepting.load(std::memory_order_acquire))
    {
        return;
    }

    const std::uint64_t nowMilliseconds = Core::MillisecondsSinceProcessStart();
    const std::uint64_t idleTimeoutMilliseconds =
        static_cast<std::uint64_t>(mOptions.idleSessionTimeout.count());
    const std::uint64_t gracefulTimeoutMilliseconds =
        static_cast<std::uint64_t>(mOptions.gracefulCloseTimeout.count());
    try
    {
        mRegistry.ForEach(
            [nowMilliseconds, idleTimeoutMilliseconds, gracefulTimeoutMilliseconds](
                const std::shared_ptr<Session::Session>& session)
            {
                // ServerHost의 Registry에는 NetworkSession만 들어간다. 이 조립 지점에서만 구체 구현의
                // 유휴 시각을 읽어 Session 공개 API에 Host 내부 상태가 새지 않게 한다.
                const std::shared_ptr<NetworkSession> networkSession =
                    std::dynamic_pointer_cast<NetworkSession>(session);
                SERVERCORE_ASSERT(networkSession != nullptr,
                    "ServerHost registry contained a session it did not create");
                if (networkSession == nullptr)
                {
                    return;
                }
                (void)networkSession->DisconnectIfExpired(
                    nowMilliseconds, idleTimeoutMilliseconds, gracefulTimeoutMilliseconds);
            });
    }
    catch (const std::bad_alloc&)
    {
        // 유휴 검사는 다음 주기에 다시 온다. 메모리가 부족한 순간의 snapshot을 건너뛰되,
        // PeriodicRunner 콜백 밖으로 예외를 보내 서버 전체를 끝내지는 않는다.
        RecordError(Core::Status::AllocationFailure());
    }
}

void ServerHost::State::NotifyOpened(const std::shared_ptr<Session::Session>& session)
{
    if (const std::shared_ptr<Session::ISessionObserver> observer = mSessionObserver.lock())
    {
        try
        {
            observer->OnSessionOpened(session);
        }
        catch (...)
        {
            Core::ReportAssertFailure("OnSessionOpened() did not throw", __FILE__, __LINE__,
                "a ServerHost session observer threw from OnSessionOpened()");
        }
    }
}

void ServerHost::State::NotifyAuthenticated(const std::shared_ptr<Session::Session>& session)
{
    if (const std::shared_ptr<Session::ISessionObserver> observer = mSessionObserver.lock())
    {
        try
        {
            observer->OnSessionAuthenticated(session);
        }
        catch (...)
        {
            Core::ReportAssertFailure("OnSessionAuthenticated() did not throw", __FILE__, __LINE__,
                "a ServerHost session observer threw from OnSessionAuthenticated()");
        }
    }
}

void ServerHost::State::NotifyClosed(const Session::SessionId id, Core::Status reason)
{
    {
        const ScopedCloseNotificationContext notificationContext(this);
        if (const std::shared_ptr<Session::ISessionObserver> observer = mSessionObserver.lock())
        {
            try
            {
                observer->OnSessionClosed(id, std::move(reason));
            }
            catch (...)
            {
                Core::ReportAssertFailure("OnSessionClosed() did not throw", __FILE__, __LINE__,
                    "a ServerHost session observer threw from OnSessionClosed()");
            }
        }
    }
    FinishCloseNotification();
}

void ServerHost::State::OnConnectionAccepted(std::shared_ptr<Net::Connection> connection) noexcept
{
    if (connection == nullptr)
    {
        return;
    }

    if (!IsAccepting())
    {
        connection->Close();
        return;
    }

    bool counted = false;
    bool issued = false;
    Session::SessionId issuedId = Session::SessionId::Invalid;
    std::shared_ptr<NetworkSession> session;
    try
    {
        // 상한이 찬 연결에는 번호를 발급하지 않는다. 그렇지 않으면 거절 연결이 IssueId 예약을
        // 계속 남겨 세션 상한과 무관하게 레지스트리 메모리를 키울 수 있다.
        if (!BeginNetworkSession())
        {
            connection->Close();
            return;
        }
        counted = true;

        Core::Result<Session::SessionId> id = mRegistry.IssueId();
        if (!id.IsOk())
        {
            RecordError(id.GetStatus());
            LogStatus(Core::LogLevel::Error, id.GetStatus());
            FinishNetworkSession(nullptr);
            counted = false;
            connection->Close();
            return;
        }
        issuedId = id.Value();
        issued = true;

        session = std::make_shared<NetworkSession>(weak_from_this(), issuedId, connection,
            mOptions.maxBodySize, mOptions.maxPendingReceiveBytes, mOptions.maxPendingParseBytes,
            mOptions.parseWorkerThreadCount == 0 ? 0 : mOptions.maxPendingParseTasks);
        TrackNetworkSession(session);
        connection->SetObserver(session);

        Core::Status posted = mJobRunner.Post([session]() { session->OpenOnRunner(); });
        if (!posted.IsOk())
        {
            session->AbortBeforeOpened(std::move(posted));
        }
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        else
        {
            // Acceptor는 이 callback이 돌아온 뒤 Connection::Start()를 부른다. pending I/O가 없는
            // Connection::Close()의 동기 종료 경계를 시험할 수 있도록 그 직전만 멈춘다.
            WaitBeforeConnectionStartForTest();
        }
#endif
    }
    catch (const std::bad_alloc&)
    {
        if (session != nullptr)
        {
            session->AbortBeforeOpened(Core::Status::AllocationFailure());
        }
        else if (counted)
        {
            RecordError(Core::Status::AllocationFailure());
            if (issued)
            {
                try
                {
                    (void)mRegistry.DiscardIssuedId(issuedId);
                }
                catch (...)
                {
                    // 지속 OOM에서도 I/O 완료 경계 밖으로 예외를 내보내지 않는다.
                }
            }
            FinishNetworkSession(nullptr);
            connection->Close();
        }
        else
        {
            connection->Close();
        }
    }
    catch (...)
    {
        if (session != nullptr)
        {
            session->AbortBeforeOpened(
                Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError));
        }
        else if (counted)
        {
            RecordError(Core::Status::FailWithoutMessage(Core::ErrorCode::PlatformError));
            if (issued)
            {
                try
                {
                    (void)mRegistry.DiscardIssuedId(issuedId);
                }
                catch (...)
                {
                    // 이 경로는 I/O 완료 처리다. 예약 정리 실패가 worker 밖으로 새면 안 된다.
                }
            }
            FinishNetworkSession(nullptr);
            connection->Close();
        }
        else
        {
            connection->Close();
        }
    }
}

ServerHost::ServerHost()
    : mState(std::make_shared<State>())
{
}

ServerHost::~ServerHost()
{
    Stop();
}

Core::Status ServerHost::Configure(const Core::Config& config)
{
    return mState->Configure(config);
}

Core::Status ServerHost::Configure(const ServerHostOptions& options)
{
    return mState->Configure(options);
}

void ServerHost::SetLogger(std::shared_ptr<Core::ILogger> logger)
{
    mState->SetLogger(std::move(logger));
}

Dispatch::Dispatcher& ServerHost::GetDispatcher() noexcept
{
    return mState->GetDispatcher();
}

const Session::SessionRegistry& ServerHost::GetSessions() const noexcept
{
    return mState->GetSessions();
}

JobRunner::Lease ServerHost::GetJobRunner() const noexcept
{
    return mState->GetJobRunner();
}

void ServerHost::SetSessionObserver(std::weak_ptr<Session::ISessionObserver> observer)
{
    mState->SetSessionObserver(std::move(observer));
}

Core::Status ServerHost::Start()
{
    return mState->Start();
}

void ServerHost::Stop()
{
    mState->Stop();
}

int ServerHost::Run()
{
    return mState->Run();
}

bool ServerHost::IsRunning() const noexcept
{
    return mState->IsRunning();
}

std::uint16_t ServerHost::Port() const noexcept
{
    return mState->Port();
}

Core::Result<ServerMetricsSnapshot> ServerHost::SnapshotMetrics() const
{
    return mState->SnapshotMetrics();
}
}
