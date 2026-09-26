#pragma once
#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Export.h"
#include "ServerCore/Observability/Observation.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace ServerCore::Observability
{
struct LoggerOptions
{
    Core::LogLevel minimumLevel = Core::LogLevel::Info;
    bool console = true;          // stderr
    std::filesystem::path file{}; // Empty disables file output; parent must exist.
    std::size_t maxQueuedMessages = 1024;
    std::size_t maxRetainedBytes = 1024 * 1024; // Queued + currently writing text.
    std::size_t maxMessageBytes = 4096;
    std::uint64_t maxFileBytes = 10 * 1024 * 1024;
    /// <remarks>
    /// 회전은 활성 파일을 file.rotating으로 먼저 옮기고, 그것이 성공한 뒤에만 가장 오래된 보관
    /// 파일을 지운다. 다른 프로세스가 활성 파일을 잡고 있거나 지워서 옮기기가 실패하면 보관 파일은
    /// 그대로 두고, maxFileBytes 안에 들어가지 않는 기록은 출력 오류로 세고 버린다. 다음 기록이
    /// 회전을 다시 시도한다. Observability.LoggerRotationFailureKeepsArchives가 이 동작을 고정한다.
    /// </remarks>
    unsigned retainedFiles = 3; // file.1 ... file.N, plus the active file.
};
struct LoggerMetricsSnapshot
{
    std::size_t pendingMessages = 0, retainedBytes = 0;
    std::uint64_t acceptedMessages = 0, writtenMessages = 0, filteredMessages = 0;
    std::uint64_t droppedMessages = 0, outputErrors = 0;
    Lifecycle lifecycle = Lifecycle::Created;
    std::size_t outstandingMessages = 0; // Queued + currently writing, including empty text.
};
// Install with a server's SetLogger. Start once on a control thread. Write
// only copies/enqueues bounded text; file/console I/O runs on a private worker.
// Overload drops the new message; TryWrite exposes WouldBlock/TooLarge/Closed.
// CR/LF, NUL and other control bytes are escaped into one physical log line.
// Stop drains accepted messages and joins, but cannot bound a stalled filesystem
// or console. Stop reports PlatformError if any output failed. No fsync guarantee.
class AsyncLogger final : public Core::ILogger, public Core::IStructuredLogger
{
public:
    SERVERCORE_API AsyncLogger();
    SERVERCORE_API ~AsyncLogger() override;
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    /// <remarks>
    /// 성공한 뒤에는 다시 시작할 수 없다(Closed). 실패한 Start는 아무것도 시작하지 않은 상태로
    /// 되돌리므로, 원인을 고친 뒤 같은 객체로 다시 부를 수 있다.
    /// Observability.LoggerStartCanRetryAfterFailure가 이 동작을 고정한다.
    /// </remarks>
    SERVERCORE_API Core::Status Start(const LoggerOptions& options = {});
    SERVERCORE_API void Write(Core::LogLevel level, std::string_view message) noexcept override;
    SERVERCORE_API Core::Status TryWrite(Core::LogLevel level, std::string_view message) noexcept;
    // Encoded JSON bytes count against maxMessageBytes/maxRetainedBytes.
    SERVERCORE_API Core::Status TryWriteRecord(const Core::LogRecord& record) noexcept override;
    SERVERCORE_API Core::Status SetMinimumLevel(Core::LogLevel level) noexcept;
    SERVERCORE_API void RequestStop() noexcept;
    SERVERCORE_API Core::Status Stop();
    [[nodiscard]] SERVERCORE_API LoggerMetricsSnapshot GetMetrics() const noexcept;

private:
    class State;
    std::unique_ptr<State> mState;
};
}
