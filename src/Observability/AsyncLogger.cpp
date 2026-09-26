#include "ServerCore/Observability/AsyncLogger.h"
#include "Observability/MetricsInternal.h"
#include <algorithm>
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
bool ValidLevel(LogLevel level) noexcept
{
    return level >= LogLevel::Trace && level <= LogLevel::Error;
}
Status Fail(ErrorCode code) noexcept
{
    return Status::FailWithoutMessage(code);
}
std::mutex consoleMutex;
}
class AsyncLogger::State
{
public:
    struct Record
    {
        LogLevel level;
        std::string message;
        std::int64_t timestamp;
        bool structured = false;
    };
    Status Start(const LoggerOptions& value)
    {
        if (!ValidLevel(value.minimumLevel) || (!value.console && value.file.empty()) ||
            !value.maxQueuedMessages || !value.maxRetainedBytes || !value.maxMessageBytes ||
            value.maxMessageBytes > value.maxRetainedBytes || value.retainedFiles > 100 ||
            (!value.file.empty() && (value.maxFileBytes < 128 || value.retainedFiles == 0 ||
                                        value.maxMessageBytes > (value.maxFileBytes - 80) / 4)) ||
            value.file.native().find(std::filesystem::path::value_type{}) !=
                std::filesystem::path::string_type::npos)
            return Fail(ErrorCode::InvalidArgument);
        const std::lock_guard join(joinMutex);
        const std::lock_guard guard(mutex);
        if (used || stopping)
            return Fail(ErrorCode::Closed);
        used = true;
        // 시작을 마치기 전에 실패하면 아무것도 시작하지 않은 상태로 되돌려, 원인을 고친 뒤 같은
        // 객체로 다시 시작할 수 있게 한다. Observability.LoggerStartCanRetryAfterFailure가 고정한다.
        const auto abandon = [this](Status status)
        {
            if (file.is_open())
                file.close();
            file.clear();
            fileBytes = 0;
            used = false;
            return status;
        };
        try
        {
            options = value;
            if (!options.file.empty())
            {
                std::error_code error;
                options.file = std::filesystem::absolute(options.file, error);
                if (error)
                    return abandon(Fail(ErrorCode::PlatformError));
                file.open(options.file, std::ios::binary | std::ios::app);
                if (!file)
                    return abandon(Fail(ErrorCode::PlatformError));
                const auto size = std::filesystem::file_size(options.file, error);
                if (error)
                    return abandon(Fail(ErrorCode::PlatformError));
                fileBytes = size;
            }
            worker = std::thread([this] { Run(); });
            ready = true;
            return Status::Ok();
        }
        catch (...)
        {
            return abandon(Status::AllocationFailure());
        }
    }
    Status Write(LogLevel level, std::string_view message,
        const Core::LogRecord* structured = nullptr) noexcept
    {
        try
        {
            const std::lock_guard guard(mutex);
            const auto reject = [&](ErrorCode code)
            {
                Detail::Add(metrics.droppedMessages);
                return Fail(code);
            };
            if (!ValidLevel(level))
                return reject(ErrorCode::InvalidArgument);
            if (!ready || stopping)
                return reject(ErrorCode::Closed);
            if (level < options.minimumLevel)
            {
                Detail::Add(metrics.filteredMessages);
                return Status::Ok();
            }
            std::string encoded;
            if (structured)
            {
                auto formatted = Core::FormatLogRecord(
                    *structured, (std::min)(options.maxMessageBytes, Core::MaxStructuredLogBytes));
                if (!formatted.IsOk())
                    return reject(formatted.GetStatus().Code());
                encoded = std::move(formatted).Value();
                message = encoded;
            }
            if (message.size() > options.maxMessageBytes)
                return reject(ErrorCode::TooLarge);
            if (queue.size() >= options.maxQueuedMessages ||
                message.size() > options.maxRetainedBytes - metrics.retainedBytes)
                return reject(ErrorCode::WouldBlock);
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            const auto retainedSize = message.size();
            // Copy the exact view: retaining the formatter's spare capacity
            // would let a small charged record retain a much larger allocation.
            queue.push_back({ level, std::string(message), now, structured != nullptr });
            metrics.retainedBytes += retainedSize;
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
        if (!ValidLevel(level))
            return Fail(ErrorCode::InvalidArgument);
        const std::lock_guard guard(mutex);
        if (!ready || stopping)
            return Fail(ErrorCode::Closed);
        options.minimumLevel = level;
        return Status::Ok();
    }
    void RequestStop() noexcept
    {
        const std::lock_guard guard(mutex);
        stopping = true;
        wake.notify_all();
    }
    Status Stop()
    {
        const std::lock_guard join(joinMutex);
        RequestStop();
        if (worker.joinable())
            worker.join();
        if (file.is_open())
            file.close();
        const std::lock_guard guard(mutex);
        stopped = true;
        return metrics.outputErrors ? Fail(ErrorCode::PlatformError) : Status::Ok();
    }
    LoggerMetricsSnapshot Snapshot() const noexcept
    {
        const std::lock_guard guard(mutex);
        auto result = metrics;
        result.pendingMessages = queue.size();
        result.outstandingMessages = queue.size() + (writing ? 1u : 0u);
        result.lifecycle = stopped    ? Lifecycle::Stopped
                           : stopping ? Lifecycle::Draining
                           : ready    ? Lifecycle::Running
                                      : Lifecycle::Created;
        return result;
    }

private:
    static std::string Format(const Record& record)
    {
        if (record.structured)
            return "{\"timestamp_ms\":" + std::to_string(record.timestamp) + "," +
                   record.message.substr(1) + "\n";
        static constexpr const char* names[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR" };
        std::string line = std::to_string(record.timestamp) + " " +
                           names[static_cast<unsigned>(record.level)] + " ";
        constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char value : record.message)
        {
            if (value < 32 || value == 127)
            {
                line += "\\x";
                line += hex[value >> 4];
                line += hex[value & 15];
            }
            else
                line += static_cast<char>(value);
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
    /// <summary>회전 중인 활성 파일 조각이 file.1로 옮겨지기 전까지 머무는 자리다.</summary>
    std::filesystem::path Pending() const
    {
        auto path = options.file;
        path += ".rotating";
        return path;
    }
    /// <summary>Pending()의 조각을 file.1로 보관한다. 실패하면 조각은 Pending()에 남는다.</summary>
    /// <remarks>
    /// 가장 앞의 빈 번호까지만 밀므로, 앞선 시도가 중간에 멈춘 뒤 다시 불려도 보관 파일을 더 지우지
    /// 않는다. 모든 번호가 차 있을 때만 가장 오래된 file.N을 지운다.
    /// </remarks>
    bool ArchivePending()
    {
        std::error_code error;
        unsigned free = 1;
        while (free <= options.retainedFiles && std::filesystem::exists(Rotated(free), error))
            ++free;
        if (error)
            return false;
        if (free > options.retainedFiles)
        {
            free = options.retainedFiles;
            std::filesystem::remove(Rotated(free), error);
            if (error)
                return false;
        }
        for (unsigned index = free; index > 1; --index)
        {
            std::filesystem::rename(Rotated(index - 1), Rotated(index), error);
            if (error)
                return false;
        }
        std::filesystem::rename(Pending(), Rotated(1), error);
        return !error;
    }
    /// <summary>활성 경로를 다시 열고 fileBytes를 그 파일의 실제 크기로 맞춘다.</summary>
    void Reopen(const std::ios::openmode mode)
    {
        file.clear();
        file.open(options.file, std::ios::binary | mode);
        std::error_code error;
        const auto size = std::filesystem::file_size(options.file, error);
        if (!error)
            fileBytes = size;
    }
    /// <summary>활성 파일을 보관하고 새 활성 파일을 연다. 모든 단계가 성공했을 때만 true다.</summary>
    /// <remarks>
    /// 활성 파일을 Pending()으로 먼저 옮기고, 그것이 성공한 뒤에만 보관 파일을 지우거나 민다.
    /// 외부 도구가 활성 파일을 잡고 있거나 지워서 옮기기가 실패하면 아무것도 지우지 않고 활성
    /// 경로를 이어쓰기로 다시 연다. 다음 기록이 회전을 다시 시도한다.
    /// Observability.LoggerRotationFailureKeepsArchives가 이 순서를 고정한다.
    /// </remarks>
    bool Rotate()
    {
        file.close();
        std::error_code error;
        const bool pendingExists = std::filesystem::exists(Pending(), error);
        bool moved = false;
        if (!error && (!pendingExists || ArchivePending()))
        {
            std::filesystem::rename(options.file, Pending(), error);
            moved = !error;
        }
        if (!moved)
        {
            Reopen(std::ios::app);
            return false;
        }
        const bool archived = ArchivePending();
        Reopen(std::ios::trunc);
        return archived && static_cast<bool>(file);
    }
    bool Output(const std::string& line)
    {
        bool success = true;
        if (options.console)
        {
            const std::lock_guard guard(consoleMutex);
            success = std::fwrite(line.data(), 1, line.size(), stderr) == line.size() &&
                      std::fflush(stderr) == 0;
        }
        if (!options.file.empty())
        {
            const auto fits = [&]
            {
                return fileBytes <= options.maxFileBytes &&
                       line.size() <= options.maxFileBytes - fileBytes;
            };
            bool rotated = true;
            if (!fits())
            {
                // 회전이 실패해도 활성 파일은 한도를 넘겨 자라지 않는다. 자리가 없으면 이 기록을
                // 출력 오류로 버리고, 다음 기록이 회전을 다시 시도한다.
                rotated = Rotate();
                if (!fits())
                    return false;
            }
            file.write(line.data(), static_cast<std::streamsize>(line.size()));
            file.flush();
            fileBytes += line.size();
            success = rotated && static_cast<bool>(file) && success;
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
                if (queue.empty() && stopping)
                    break;
                record = std::move(queue.front());
                queue.pop_front();
                writing = true;
            }
            bool success = false;
            try
            {
                success = Output(Format(record));
            }
            catch (...)
            {
            }
            const auto bytes = record.message.size();
            // Admission may reuse these bytes as soon as the counter falls.
            // Release the actual allocation before making its charge available.
            std::string{}.swap(record.message);
            {
                const std::lock_guard guard(mutex);
                metrics.retainedBytes -= bytes;
                writing = false;
                if (success)
                    Detail::Add(metrics.writtenMessages);
                else
                    Detail::Add(metrics.outputErrors);
            }
        }
        if (file.is_open())
            file.close();
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
    bool used = false, ready = false, stopping = false, stopped = false, writing = false;
};
AsyncLogger::AsyncLogger()
    : mState(std::make_unique<State>())
{
}
AsyncLogger::~AsyncLogger()
{
    (void)mState->Stop();
}
Core::Status AsyncLogger::Start(const LoggerOptions& options)
{
    return mState->Start(options);
}
void AsyncLogger::Write(Core::LogLevel level, std::string_view message) noexcept
{
    (void)mState->Write(level, message);
}
Core::Status AsyncLogger::TryWrite(Core::LogLevel level, std::string_view message) noexcept
{
    return mState->Write(level, message);
}
Core::Status AsyncLogger::TryWriteRecord(const Core::LogRecord& record) noexcept
{
    return mState->Write(record.level, record.message, &record);
}
Core::Status AsyncLogger::SetMinimumLevel(Core::LogLevel level) noexcept
{
    return mState->SetLevel(level);
}
void AsyncLogger::RequestStop() noexcept
{
    mState->RequestStop();
}
Core::Status AsyncLogger::Stop()
{
    return mState->Stop();
}
LoggerMetricsSnapshot AsyncLogger::GetMetrics() const noexcept
{
    return mState->Snapshot();
}
}
