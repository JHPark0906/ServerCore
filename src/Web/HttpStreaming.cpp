#include "ServerCore/Web/HttpStreaming.h"
#include "Core/Utf8Internal.h"
#include "Web/FileResponseInternal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ServerCore::Web
{
namespace
{
using Core::ErrorCode;
using Core::Status;

template<class Integer>
std::string DecimalCount(Integer value)
{
    // libc++ file_clock uses a 128-bit count. Preserve every timestamp bit,
    // including negative minima, without relying on to_string overloads.
    std::array<char, 64> storage{};
    auto cursor = storage.end();
    const bool negative = value < 0;
    do
    {
        const auto digit = value % 10;
        *--cursor = static_cast<char>('0' + static_cast<int>(digit < 0 ? -digit : digit));
        value /= 10;
    } while (value != 0);
    if (negative) *--cursor = '-';
    return {cursor, storage.end()};
}

struct FileTaskLifetime
{
    explicit FileTaskLifetime(std::shared_ptr<const HttpRequestContext> value) : context(std::move(value)) {}
    ~FileTaskLifetime()
    {
        // The executor can discard a queued cancelled job without invoking it.
        // Admission failures must still leave the caller's response untouched.
        if (admitted.load(std::memory_order_acquire) && !completed.load(std::memory_order_acquire))
            context->response->Abort();
    }
    std::shared_ptr<const HttpRequestContext> context;
    std::atomic<bool> admitted{false};
    std::atomic<bool> completed{false};
};

bool Append(std::string& output, std::string_view value, std::size_t limit)
{
    if (output.size() > limit || value.size() > limit - output.size()) return false;
    output.append(value);
    return true;
}

Status WaitForCapacity(HttpResponseWriter& writer, std::size_t bytes, std::stop_token cancellation)
{
    std::mutex mutex;
    std::condition_variable_any wake;
    bool ready = false;
    Status result = Status::Ok();
    auto registered = writer.WaitForWriteCapacity(bytes, [&](Status status) {
        {
            const std::lock_guard guard(mutex);
            result = std::move(status);
            ready = true;
        }
        wake.notify_all();
    });
    if (!registered.IsOk()) return std::move(registered).TakeStatus();
    auto subscription = std::move(registered.Value());
    {
        std::unique_lock guard(mutex);
        (void)wake.wait(guard, cancellation, [&ready] { return ready; });
    }
    // Reset joins any in-flight callback before its stack references disappear.
    // It must run without the callback's mutex held.
    subscription.Reset();
    if (cancellation.stop_requested()) return Status::FailWithoutMessage(ErrorCode::Cancelled);
    return result;
}

template <class Operation>
Status RetryWrite(HttpResponseWriter& writer, std::size_t bytes,
    std::stop_token cancellation, Operation operation)
{
    for (;;)
    {
        if (cancellation.stop_requested()) return Status::FailWithoutMessage(ErrorCode::Cancelled);
        auto result = operation();
        if (result.Code() != ErrorCode::WouldBlock) return result;
        auto available = WaitForCapacity(writer, bytes, cancellation);
        if (!available.IsOk()) return available;
    }
}

Status FileError(HttpResponseWriter& writer, ErrorCode error, unsigned int httpStatus)
{
    HttpResponse response;
    response.status = httpStatus;
    response.close = true;
    if (!writer.Complete(response).IsOk()) (void)writer.Abort();
    return Status::FailWithoutMessage(error);
}

Status TransferFile(const std::shared_ptr<const HttpRequestContext>& context,
    const std::filesystem::path& path, HttpResponseHead head, std::size_t chunkBytes,
    std::stop_token cancellation)
{
    auto& writer = *context->response;
    try
    {
        if (cancellation.stop_requested())
        {
            (void)writer.Abort();
            return Status::FailWithoutMessage(ErrorCode::Cancelled);
        }
        std::error_code error;
        const auto attributes = std::filesystem::status(path, error);
        if (error)
            return FileError(writer, error == std::errc::no_such_file_or_directory ?
                ErrorCode::NotFound : ErrorCode::PlatformError,
                error == std::errc::no_such_file_or_directory ? 404u : 500u);
        if (!std::filesystem::is_regular_file(attributes))
            return FileError(writer, ErrorCode::NotFound, 404);
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return FileError(writer, ErrorCode::PlatformError, 500);
        const auto end = input.tellg();
        if (end == std::ifstream::pos_type(-1)) return FileError(writer, ErrorCode::PlatformError, 500);
        const auto length = static_cast<std::streamoff>(end);
        if (length < 0) return FileError(writer, ErrorCode::PlatformError, 500);
        head.contentLength = static_cast<std::uint64_t>(length);
        std::uint64_t offset = 0;
        if (head.status == 200)
        {
            const auto modified = std::filesystem::last_write_time(path, error);
            if (error) return FileError(writer, ErrorCode::PlatformError, 500);
#if defined(_MSC_VER)
            const auto convertedModified = std::chrono::clock_cast<std::chrono::system_clock>(modified);
#else
            const auto convertedModified = std::chrono::file_clock::to_sys(modified);
#endif
            const auto now = std::chrono::system_clock::now();
            const auto systemModified = convertedModified > now ? now :
                (convertedModified < std::chrono::system_clock::time_point::min() ? std::chrono::system_clock::time_point::min() :
                    std::chrono::time_point_cast<std::chrono::system_clock::duration>(convertedModified));
            std::string entityTag;
            for (const auto& [name, value] : head.headers)
                if (Detail::EqualInsensitive(name, "etag"))
                {
                    if (!entityTag.empty() || !Detail::ValidEntityTag(value))
                        return FileError(writer, ErrorCode::InvalidArgument, 500);
                    entityTag = value;
                }
            if (entityTag.empty())
            {
                entityTag = "W/\"" + std::to_string(length) + "-" + DecimalCount(modified.time_since_epoch().count()) + "\"";
                head.headers.emplace_back("ETag", entityTag);
            }
            std::erase_if(head.headers, [](const auto& field) {
                return Detail::EqualInsensitive(field.first, "last-modified") || Detail::EqualInsensitive(field.first, "accept-ranges") ||
                    Detail::EqualInsensitive(field.first, "content-range");
            });
            head.headers.emplace_back("Last-Modified", Detail::HttpDate(systemModified));
            head.headers.emplace_back("Accept-Ranges", "bytes");
            const auto plan = Detail::PlanFileResponse(context->request, static_cast<std::uint64_t>(length), entityTag, systemModified);
            head.status = plan.status; head.contentLength = plan.length; offset = plan.offset;
            if (!plan.contentRange.empty()) head.headers.emplace_back("Content-Range", plan.contentRange);
        }
        auto started = RetryWrite(writer, 0, cancellation, [&] { return writer.Start(head); });
        if (!started.IsOk()) { (void)writer.Abort(); return started; }
        const bool noContent = writer.IsHeadRequest() || head.status == 204 || head.status == 205 || head.status == 304;
        if (!noContent)
        {
            input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            if (!input) { (void)writer.Abort(); return Status::FailWithoutMessage(ErrorCode::PlatformError); }
            std::vector<std::byte> buffer(chunkBytes);
            std::uint64_t remaining = *head.contentLength;
            while (remaining != 0)
            {
                if (cancellation.stop_requested())
                { (void)writer.Abort(); return Status::FailWithoutMessage(ErrorCode::Cancelled); }
                const auto count = static_cast<std::size_t>((std::min)(remaining, static_cast<std::uint64_t>(chunkBytes)));
                input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
                if (input.gcount() != static_cast<std::streamsize>(count) || input.bad())
                { (void)writer.Abort(); return Status::FailWithoutMessage(ErrorCode::PlatformError); }
                auto sent = RetryWrite(writer, count, cancellation,
                    [&] { return writer.Write(std::span<const std::byte>(buffer.data(), count)); });
                if (!sent.IsOk()) { (void)writer.Abort(); return sent; }
                remaining -= count;
            }
        }
        auto finished = RetryWrite(writer, 0, cancellation, [&] { return writer.Finish(); });
        if (!finished.IsOk()) (void)writer.Abort();
        return finished;
    }
    catch (...)
    {
        (void)writer.Abort();
        return Status::AllocationFailure();
    }
}
}

Core::Result<std::string> EncodeSseEvent(const SseEvent& event, std::size_t maxBytes)
{
    using Result = Core::Result<std::string>;
    if (!Core::Detail::IsValidUtf8(event.data) || !Core::Detail::IsValidUtf8(event.event) ||
        event.event.find_first_of("\r\n") != std::string::npos ||
        (event.id && (!Core::Detail::IsValidUtf8(*event.id) ||
            event.id->find_first_of("\r\n") != std::string::npos || event.id->find('\0') != std::string::npos)))
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    try
    {
        std::string wire;
        const auto field = [&](std::string_view name, std::string_view value) {
            return Append(wire, name, maxBytes) && Append(wire, ": ", maxBytes) &&
                Append(wire, value, maxBytes) && Append(wire, "\n", maxBytes);
        };
        if (!event.event.empty() && !field("event", event.event))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        if (event.id && !field("id", *event.id))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        if (event.retryMilliseconds && !field("retry", std::to_string(*event.retryMilliseconds)))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        std::string_view remaining(event.data);
        for (;;)
        {
            const auto end = remaining.find_first_of("\r\n");
            if (!field("data", remaining.substr(0, end)))
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
            if (end == std::string_view::npos) break;
            std::size_t consumed = end + 1;
            if (remaining[end] == '\r' && consumed < remaining.size() && remaining[consumed] == '\n') ++consumed;
            remaining.remove_prefix(consumed);
        }
        if (!Append(wire, "\n", maxBytes))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        return Result::FromValue(std::move(wire));
    }
    catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
}

Core::Status WriteSseEvent(HttpResponseWriter& writer, const SseEvent& event)
{
    auto encoded = EncodeSseEvent(event, writer.MaxWriteBytes());
    if (!encoded.IsOk()) return std::move(encoded).TakeStatus();
    const auto& bytes = encoded.Value();
    return writer.Write(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

Core::Result<Runtime::TaskHandle> SendFile(Runtime::TaskExecutor& executor,
    std::shared_ptr<const HttpRequestContext> context, std::filesystem::path path,
    HttpResponseHead head)
{
    using Result = Core::Result<Runtime::TaskHandle>;
    if (!context || !context->response || path.empty() ||
        path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    const auto chunkBytes = (std::min)(context->response->MaxWriteBytes(), std::size_t{64 * 1024});
    if (chunkBytes == 0)
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
    try
    {
        // Do not move spare caller capacities into the queued closure. Charge
        // the bounded read buffer and every owned metadata allocation instead.
        const auto& native = path.native();
        std::filesystem::path compactPath(std::filesystem::path::string_type(native.data(), native.size()));
        HttpResponseHead compactHead;
        compactHead.status = head.status;
        compactHead.contentLength = head.contentLength;
        compactHead.close = head.close;
        if (head.headers.size() > compactHead.headers.max_size() - 4)
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        // Reserve generated ETag/Last-Modified/Accept-Ranges/Content-Range slots
        // before admission so growth cannot escape the declared byte charge.
        compactHead.headers.reserve(head.headers.size() + 4);
        for (const auto& [name, value] : head.headers)
            compactHead.headers.emplace_back(std::string(name.data(), name.size()), std::string(value.data(), value.size()));
        Runtime::TaskOptions options;
        // Bounded generated validators/range headers are also owned while the
        // task runs, in addition to caller metadata copied below.
        options.retainedBytes = chunkBytes + 1024;
        options.parentToken = context->response->GetCancellationToken();
        const auto charge = [&](std::size_t count, std::size_t width = 1) {
            const auto available = (std::numeric_limits<std::size_t>::max)() - options.retainedBytes;
            if (count > available / width) return false;
            options.retainedBytes += count * width;
            return true;
        };
        if (!charge(sizeof(std::filesystem::path) + sizeof(HttpResponseHead)) ||
            !charge(compactPath.native().capacity(), sizeof(std::filesystem::path::value_type)) ||
            !charge(compactHead.headers.capacity(), sizeof(HttpHeaders::value_type)))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        for (const auto& [name, value] : compactHead.headers)
            if (!charge(name.capacity()) || !charge(value.capacity()))
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        const auto lifetime = std::make_shared<FileTaskLifetime>(std::move(context));
        auto submitted = executor.Submit([lifetime, path = std::move(compactPath), head = std::move(compactHead), chunkBytes]
            (std::stop_token cancellation) mutable {
            auto result = TransferFile(lifetime->context, path, std::move(head), chunkBytes, cancellation);
            lifetime->completed.store(true, std::memory_order_release);
            return result;
        }, options);
        if (submitted.IsOk()) lifetime->admitted.store(true, std::memory_order_release);
        return submitted;
    }
    catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
}
}
