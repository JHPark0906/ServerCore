#include "ServerCore/Observability/AsyncLogger.h"
#include "Observability/MetricsInternal.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace ServerCore::Observability
{
namespace
{
using Core::ErrorCode;
using Core::LogLevel;
using Core::Status;
bool ValidLevel(LogLevel level) noexcept { return level >= LogLevel::Trace && level <= LogLevel::Error; }
Status Fail(ErrorCode code) noexcept { return Status::FailWithoutMessage(code); }
std::mutex consoleMutex;
}
class AsyncLogger::State
{
public:
    struct Record { LogLevel level; std::string message; std::int64_t timestamp; };
    Status Start(const LoggerOptions& value)
    {
        if (!ValidLevel(value.minimumLevel) || (!value.console && value.file.empty()) ||
            !value.maxQueuedMessages || !value.maxRetainedBytes || !value.maxMessageBytes ||
            value.maxMessageBytes > value.maxRetainedBytes || value.retainedFiles > 100 ||
            (!value.file.empty() && (value.maxFileBytes < 128 || value.retainedFiles == 0 ||
                value.maxMessageBytes > (value.maxFileBytes - 80) / 4)) ||
            value.file.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
            return Fail(ErrorCode::InvalidArgument);
        const std::lock_guard join(joinMutex);
        const std::lock_guard guard(mutex);
        if (used || stopping) return Fail(ErrorCode::Closed);
        used = true;
        try
        {
            options = value;
            if (!options.file.empty())
            {
                std::error_code error;
                options.file = std::filesystem::absolute(options.file, error);
                if (error) return Fail(ErrorCode::PlatformError);
                file.open(options.file, std::ios::binary | std::ios::app);
                if (!file) return Fail(ErrorCode::PlatformError);
                const auto size = std::filesystem::file_size(options.file, error);
                if (error) return Fail(ErrorCode::PlatformError);
                fileBytes = size;
            }
            worker = std::thread([this] { Run(); });
            ready = true;
            return Status::Ok();
        }
        catch (...) { return Status::AllocationFailure(); }
    }
    Status Write(LogLevel level, std::string_view message) noexcept
    {
        try
        {
            const std::lock_guard guard(mutex);
            const auto reject = [&](ErrorCode code) { Detail::Add(metrics.droppedMessages); return Fail(code); };
            if (!ValidLevel(level)) return reject(ErrorCode::InvalidArgument);
            if (!ready || stopping) return reject(ErrorCode::Closed);
            if (level < options.minimumLevel) { Detail::Add(metrics.filteredMessages); return Status::Ok(); }
            if (message.size() > options.maxMessageBytes) return reject(ErrorCode::TooLarge);
            if (queue.size() >= options.maxQueuedMessages || message.size() > options.maxRetainedBytes - metrics.retainedBytes)
                return reject(ErrorCode::WouldBlock);
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            queue.push_back({level, std::string(message), now});
            metrics.retainedBytes += message.size();
            Detail::Add(metrics.acceptedMessages);
            wake.notify_one();
            return Status::Ok();
        }
        catch (...)
        {
            const std::lock_guard guard(mutex);
            Detail::Add(metrics.droppedMessages);
            return Status::AllocationFailure();
        }
    }
    Status SetLevel(LogLevel level) noexcept
    {
        if (!ValidLevel(level)) return Fail(ErrorCode::InvalidArgument);
        const std::lock_guard guard(mutex);
        if (!ready || stopping) return Fail(ErrorCode::Closed);
        options.minimumLevel = level;
        return Status::Ok();
    }
    void RequestStop() noexcept { const std::lock_guard guard(mutex); stopping = true; wake.notify_all(); }
    Status Stop()
    {
        const std::lock_guard join(joinMutex);
        RequestStop();
        if (worker.joinable()) worker.join();
        if (file.is_open()) file.close();
        const std::lock_guard guard(mutex);
        return metrics.outputErrors ? Fail(ErrorCode::PlatformError) : Status::Ok();
    }
    LoggerMetricsSnapshot Snapshot() const noexcept
    {
        const std::lock_guard guard(mutex);
        auto result = metrics;
        result.pendingMessages = queue.size();
        return result;
    }
private:
    static std::string Format(const Record& record)
    {
        static constexpr const char* names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
        std::string line = std::to_string(record.timestamp) + " " + names[static_cast<unsigned>(record.level)] + " ";
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char value : record.message)
        {
            if (value < 32 || value == 127)
            {
                line += "\\x";
                line += hex[value >> 4];
                line += hex[value & 15];
            }
            else line += static_cast<char>(value);
        }
        line += '\n';
        return line;
    }
    std::filesystem::path Rotated(unsigned index) const
    {
        auto path = options.file;
        path += "." + std::to_string(index);
        return path;
    }
    bool Rotate()
    {
        file.close();
        std::error_code error;
        std::filesystem::remove(Rotated(options.retainedFiles), error);
        if (error) return false;
        for (unsigned index = options.retainedFiles; index > 1; --index)
        {
            const auto previous = Rotated(index - 1);
            if (!std::filesystem::exists(previous, error)) { if (error) return false; continue; }
            std::filesystem::rename(previous, Rotated(index), error);
            if (error) return false;
        }
        std::filesystem::rename(options.file, Rotated(1), error);
        if (error) return false;
        file.clear();
        file.open(options.file, std::ios::binary | std::ios::trunc);
        fileBytes = 0;
        return static_cast<bool>(file);
    }
    bool Output(const std::string& line)
    {
        bool success = true;
        if (options.console)
        {
            const std::lock_guard guard(consoleMutex);
            success = std::fwrite(line.data(), 1, line.size(), stderr) == line.size() && std::fflush(stderr) == 0;
        }
        if (!options.file.empty())
        {
            if (fileBytes > options.maxFileBytes || line.size() > options.maxFileBytes - fileBytes)
                if (!Rotate()) return false;
            file.write(line.data(), static_cast<std::streamsize>(line.size()));
            file.flush();
            fileBytes += line.size();
            success = static_cast<bool>(file) && success;
        }
        return success;
    }
    void Run() noexcept
    {
        for (;;)
        {
            Record record;
            {
                std::unique_lock guard(mutex);
                wake.wait(guard, [this] { return stopping || !queue.empty(); });
                if (queue.empty() && stopping) break;
                record = std::move(queue.front());
                queue.pop_front();
            }
            bool success = false;
            try { success = Output(Format(record)); } catch (...) {}
            const auto bytes = record.message.size();
            // Admission may reuse these bytes as soon as the counter falls.
            // Release the actual allocation before making its charge available.
            std::string{}.swap(record.message);
            {
                const std::lock_guard guard(mutex);
                metrics.retainedBytes -= bytes;
                if (success) Detail::Add(metrics.writtenMessages);
                else Detail::Add(metrics.outputErrors);
            }
        }
        if (file.is_open()) file.close();
    }
    mutable std::mutex mutex;
    std::mutex joinMutex;
    std::condition_variable wake;
    LoggerOptions options;
    LoggerMetricsSnapshot metrics;
    std::deque<Record> queue;
    std::thread worker;
    std::ofstream file;
    std::uint64_t fileBytes = 0;
    bool used = false, ready = false, stopping = false;
};
AsyncLogger::AsyncLogger() : mState(std::make_unique<State>()) {}
AsyncLogger::~AsyncLogger() { (void)mState->Stop(); }
Core::Status AsyncLogger::Start(const LoggerOptions& options) { return mState->Start(options); }
void AsyncLogger::Write(Core::LogLevel level, std::string_view message) noexcept { (void)mState->Write(level, message); }
Core::Status AsyncLogger::TryWrite(Core::LogLevel level, std::string_view message) noexcept { return mState->Write(level, message); }
Core::Status AsyncLogger::SetMinimumLevel(Core::LogLevel level) noexcept { return mState->SetLevel(level); }
void AsyncLogger::RequestStop() noexcept { mState->RequestStop(); }
Core::Status AsyncLogger::Stop() { return mState->Stop(); }
LoggerMetricsSnapshot AsyncLogger::GetMetrics() const noexcept { return mState->Snapshot(); }
}
