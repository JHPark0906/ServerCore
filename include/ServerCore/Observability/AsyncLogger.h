#pragma once
#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
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
    bool console = true; // stderr
    std::filesystem::path file{}; // Empty disables file output; parent must exist.
    std::size_t maxQueuedMessages = 1024;
    std::size_t maxRetainedBytes = 1024 * 1024; // Queued + currently writing text.
    std::size_t maxMessageBytes = 4096;
    std::uint64_t maxFileBytes = 10 * 1024 * 1024;
    unsigned retainedFiles = 3; // file.1 ... file.N, plus the active file.
};
struct LoggerMetricsSnapshot
{
    std::size_t pendingMessages = 0, retainedBytes = 0;
    std::uint64_t acceptedMessages = 0, writtenMessages = 0, filteredMessages = 0;
    std::uint64_t droppedMessages = 0, outputErrors = 0;
};
// Install with a server's SetLogger. Start once on a control thread. Write
// only copies/enqueues bounded text; file/console I/O runs on a private worker.
// Overload drops the new message; TryWrite exposes WouldBlock/TooLarge/Closed.
// CR/LF, NUL and other control bytes are escaped into one physical log line.
// Stop drains accepted messages and joins, but cannot bound a stalled filesystem
// or console. Stop reports PlatformError if any output failed. No fsync guarantee.
class AsyncLogger final : public Core::ILogger
{
public:
    AsyncLogger();
    ~AsyncLogger() override;
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    Core::Status Start(const LoggerOptions& options = {});
    void Write(Core::LogLevel level, std::string_view message) noexcept override;
    Core::Status TryWrite(Core::LogLevel level, std::string_view message) noexcept;
    Core::Status SetMinimumLevel(Core::LogLevel level) noexcept;
    void RequestStop() noexcept;
    Core::Status Stop();
    [[nodiscard]] LoggerMetricsSnapshot GetMetrics() const noexcept;
private:
    class State;
    std::unique_ptr<State> mState;
};
}
