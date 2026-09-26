// ServerHostOptions 검증과 Config 키 매핑이다.
#include "Runtime/ServerHostState.h"

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

/// <summary>Host 내부 작업이 JobRunner control lane에 동시에 붙들 수 있는 작업 수의 상한이다.</summary>
/// <remarks>
/// 세션마다 열기·수신 batch·종료 정리 작업이 각각 많아야 하나씩 대기한다(수신은 mReceiveJobQueued,
/// 종료는 연결의 단일 끊김 통지가 막는다). parse 완료 작업은 저마다 Host parse task 예산 한 건을 쥐므로
/// 그 상한을 넘지 않는다. 여기에 실행 중인 수신 작업이 다음 batch를 예약하는 순간의 한 건과 세션 만료
/// 주기 callback 한 건을 더한다. Configure는 control lane을 이 값 아래로 두지 않으므로 Host 작업이
/// 용량 부족(WouldBlock)으로 세션을 끊거나 종료 통지를 runner 밖으로 밀어내지 않는다. 이 성질은
/// Runtime.HostControlLaneFitsSessionWork가 고정한다.
/// </remarks>
[[nodiscard]] std::size_t RequiredHostControlJobs(const ServerHostOptions& options) noexcept
{
    const std::size_t parseCompletions =
        options.parseWorkerThreadCount == 0 ? 0 : options.maxTotalPendingParseTasks;
    return 3 * static_cast<std::size_t>(options.maxConcurrentSessions) + parseCompletions + 2;
}

[[nodiscard]] Core::Status ValidateOptions(const ServerHostOptions& options)
{
    if (options.maxPendingReceiveChunks == 0 || options.maxPendingReceiveChunks > 65536)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    if (options.frameCompletionTimeout.count() < 0 || options.authenticationTimeout.count() < 0 ||
        (options.payloadMode != Protocol::PayloadMode::Json &&
            options.payloadMode != Protocol::PayloadMode::Binary) ||
        (options.payloadMode == Protocol::PayloadMode::Binary &&
            options.parseWorkerThreadCount != 0) ||
        (options.payloadMode == Protocol::PayloadMode::Binary &&
            options.maxBodySize < Protocol::BinaryMessageHeaderSize))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
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
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "ServerHost graceful close timeout must be positive");
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
    if (options.maxPendingReceiveBytes < Net::MaximumReceiveChunkBytes ||
        options.maxTotalPendingReceiveBytes < Net::MaximumReceiveChunkBytes)
    {
        // 수신 callback 한 번이 이만큼까지 넘기므로, 더 작은 예산은 대기 중인 바이트가 없어도 정상
        // 수신 하나를 TooLarge로 끊을 수 있다.
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost pending receive byte limits must fit one transport receive");
    }
    if (options.maxInputBytesPerSecond != 0 &&
        options.maxInputBytesPerSecond < options.maxBodySize + Protocol::HeaderSize)
    {
        // 한 창의 바이트 상한이 최대 frame 하나보다 작으면 그 frame은 보통 속도로 보낼 수 없다.
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "ServerHost input byte rate must fit one maximum-size frame");
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
}

[[nodiscard]] std::chrono::milliseconds SessionTimeoutScanPeriod(
    const ServerHostOptions& options) noexcept
{
    std::chrono::milliseconds timeout =
        options.idleSessionTimeout.count() == 0
            ? options.gracefulCloseTimeout
            : std::min(options.idleSessionTimeout, options.gracefulCloseTimeout);
    for (const auto extra : { options.frameCompletionTimeout, options.authenticationTimeout })
        if (extra.count() != 0)
            timeout = std::min(timeout, extra);
    // 유휴 검사가 꺼져 있어도 graceful 종료 기한은 검사한다. 짧은 제한이 긴 제한의 검사 주기를
    // 기다리지 않게 하고, 아주 작은 값에서도 busy loop를 만들지는 않는다.
    return std::clamp(
        timeout / 4, MinimumSessionTimeoutScanPeriod, MaximumSessionTimeoutScanPeriod);
}

/// <summary>ServerHost가 JSON 본문 하나의 파싱에 적용하는 값 수 상한이다.</summary>
/// <remarks>0이면 maxBodySize / 8과 1024 중 큰 값이다. 근거는 ServerHostOptions의 주석에 있다.</remarks>
[[nodiscard]] Protocol::JsonParseLimits HostJsonParseLimits(
    const ServerHostOptions& options) noexcept
{
    const std::size_t configured = options.maxJsonValuesPerMessage;
    return { configured != 0
                 ? configured
                 : (std::max)(std::size_t{ 1024 }, std::size_t{ options.maxBodySize } / 8) };
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
            return listenAddress;
        }
        Core::Status ioWorkers =
            ApplyOptionalInt(config, HostConfigIoWorkerThreadCountKey, options.ioWorkerThreadCount);
        if (!ioWorkers.IsOk())
        {
            return ioWorkers;
        }
        Core::Status parseWorkers = ApplyOptionalInt(
            config, HostConfigParseWorkerThreadCountKey, options.parseWorkerThreadCount);
        if (!parseWorkers.IsOk())
        {
            return parseWorkers;
        }
        Core::Status acceptBacklog =
            ApplyOptionalInt(config, HostConfigAcceptBacklogKey, options.acceptBacklog);
        if (!acceptBacklog.IsOk())
        {
            return acceptBacklog;
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

        const Core::Result<int> gracefulCloseTimeout =
            config.GetInt(HostConfigGracefulCloseTimeoutKey);
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
            return maxBodySize;
        }
        Core::Status maxConcurrentSessions = ApplyOptionalUint32(
            config, HostConfigMaxConcurrentSessionsKey, options.maxConcurrentSessions);
        if (!maxConcurrentSessions.IsOk())
        {
            return maxConcurrentSessions;
        }
        Core::Status maxTotalSendQueueCapacityBytes = ApplyOptionalUint32(config,
            HostConfigMaxTotalSendQueueCapacityBytesKey, options.maxTotalSendQueueCapacityBytes);
        if (!maxTotalSendQueueCapacityBytes.IsOk())
        {
            return maxTotalSendQueueCapacityBytes;
        }
        Core::Status maxPendingReceiveBytes = ApplyOptionalUint32(
            config, HostConfigMaxPendingReceiveBytesKey, options.maxPendingReceiveBytes);
        if (!maxPendingReceiveBytes.IsOk())
        {
            return maxPendingReceiveBytes;
        }
        Core::Status maxTotalPendingReceiveBytes = ApplyOptionalUint32(
            config, HostConfigMaxTotalPendingReceiveBytesKey, options.maxTotalPendingReceiveBytes);
        if (!maxTotalPendingReceiveBytes.IsOk())
        {
            return maxTotalPendingReceiveBytes;
        }
        Core::Status maxPendingParseBytes = ApplyOptionalUint32(
            config, HostConfigMaxPendingParseBytesKey, options.maxPendingParseBytes);
        if (!maxPendingParseBytes.IsOk())
        {
            return maxPendingParseBytes;
        }
        Core::Status maxTotalPendingParseBytes = ApplyOptionalUint32(
            config, HostConfigMaxTotalPendingParseBytesKey, options.maxTotalPendingParseBytes);
        if (!maxTotalPendingParseBytes.IsOk())
        {
            return maxTotalPendingParseBytes;
        }
        Core::Status maxPendingParseTasks = ApplyOptionalUint32(
            config, HostConfigMaxPendingParseTasksKey, options.maxPendingParseTasks);
        if (!maxPendingParseTasks.IsOk())
        {
            return maxPendingParseTasks;
        }
        Core::Status maxTotalPendingParseTasks = ApplyOptionalUint32(
            config, HostConfigMaxTotalPendingParseTasksKey, options.maxTotalPendingParseTasks);
        if (!maxTotalPendingParseTasks.IsOk())
        {
            return maxTotalPendingParseTasks;
        }

        for (const auto& field : { std::pair{ "servercore.host.frame-completion-timeout-ms",
                                       &options.frameCompletionTimeout },
                 std::pair{ "servercore.host.authentication-timeout-ms",
                     &options.authenticationTimeout } })
        {
            const auto value = config.GetInt(field.first);
            if (value.IsOk())
                *field.second = std::chrono::milliseconds(value.Value());
            else if (value.GetStatus().Code() != Core::ErrorCode::NotFound)
                return value.GetStatus();
        }
        for (const auto& field : { std::pair{ "servercore.host.max-input-bytes-per-second",
                                       &options.maxInputBytesPerSecond },
                 std::pair{ "servercore.host.max-input-frames-per-second",
                     &options.maxInputFramesPerSecond },
                 std::pair{ "servercore.host.max-pending-receive-chunks",
                     &options.maxPendingReceiveChunks },
                 std::pair{ "servercore.host.max-message-failure-logs-per-second",
                     &options.maxMessageFailureLogsPerSecond },
                 std::pair{ "servercore.host.max-json-values-per-message",
                     &options.maxJsonValuesPerMessage } })
        {
            auto value = ApplyOptionalUint32(config, field.first, *field.second);
            if (!value.IsOk())
                return value;
        }
        std::string mode = "json";
        auto modeStatus = ApplyOptionalString(config, "servercore.host.payload-mode", mode);
        if (!modeStatus.IsOk())
            return modeStatus;
        if (mode == "binary")
            options.payloadMode = Protocol::PayloadMode::Binary;
        else if (mode != "json")
            return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);

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
        return valid;
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

    if (options.jobRunner.maxControlJobs == 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    JobRunnerOptions runnerOptions = options.jobRunner;
    runnerOptions.maxControlJobs =
        (std::max)(runnerOptions.maxControlJobs, RequiredHostControlJobs(options));
    const auto runnerConfigured = mJobRunner.Configure(runnerOptions);
    if (!runnerConfigured.IsOk())
        return runnerConfigured;
    mOptions = std::move(*copiedOptions);
    mConfigured = true;
    return Core::Status::Ok();
}
}
