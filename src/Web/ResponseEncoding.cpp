#include "Web/ResponseEncoding.h"
#include "Web/WebProtocol.h"

#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <utility>

namespace ServerCore::Web::Detail
{
namespace
{
using Core::ErrorCode;
using Core::Status;
bool Append(std::string& output, std::string_view value, std::size_t limit)
{
    if (output.size() > limit || value.size() > limit - output.size()) return false;
    output.append(value);
    return true;
}
}

Core::Result<PreparedResponseHead> PrepareResponseHead(const HttpResponseHead& response,
    bool head, std::size_t maxHeaders)
{
    return PrepareResponseHead(response.status, response.headers, response.contentLength,
        head, response.close, maxHeaders);
}

Core::Result<PreparedResponseHead> PrepareResponseHead(unsigned int status,
    const HttpHeaders& headers, std::optional<std::uint64_t> length,
    bool head, bool close, std::size_t maxHeaders)
{
    using Result = Core::Result<PreparedResponseHead>;
    if (status < 200 || status > 599)
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    try
    {
        PreparedResponseHead prepared;
        bool haveDate = false;
        bool haveUpgrade = false;
        if (!Append(prepared.wire, "HTTP/1.1 ", maxHeaders) ||
            !Append(prepared.wire, std::to_string(status), maxHeaders) ||
            !Append(prepared.wire, " ", maxHeaders) ||
            !Append(prepared.wire, ReasonPhrase(status), maxHeaders) ||
            !Append(prepared.wire, "\r\n", maxHeaders))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        for (const auto& [name, value] : headers)
        {
            if (!IsToken(name) || !ValidHeaderValue(value) || EqualInsensitive(name, "content-length") ||
                EqualInsensitive(name, "transfer-encoding") || EqualInsensitive(name, "connection") ||
                EqualInsensitive(name, "trailer"))
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
            if (EqualInsensitive(name, "upgrade"))
            {
                if (!ValidUpgradeProtocols(value))
                    return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
                haveUpgrade = true;
            }
            if (EqualInsensitive(name, "date"))
            {
                if (haveDate) return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
                haveDate = true;
            }
            if (!Append(prepared.wire, name, maxHeaders) || !Append(prepared.wire, ": ", maxHeaders) ||
                !Append(prepared.wire, value, maxHeaders) || !Append(prepared.wire, "\r\n", maxHeaders))
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        }
        if (status == 426 && !haveUpgrade)
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        if (!haveDate && (!Append(prepared.wire, "Date: ", maxHeaders) ||
            !Append(prepared.wire, HttpDate(std::chrono::system_clock::now()), maxHeaders) ||
            !Append(prepared.wire, "\r\n", maxHeaders)))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        const bool noContent = status == 204 || status == 205 || status == 304;
        prepared.mode = head || noContent ? ResponseBodyMode::None :
            length ? ResponseBodyMode::FixedLength : ResponseBodyMode::Chunked;
        prepared.length = status == 204 || status == 205 || status == 304 ? 0 : length.value_or(0);
        // No payload or final chunk is emitted for HEAD/204/205/304. HEAD can
        // advertise a known GET representation length without producing its data.
        if (status == 205 || (status != 204 && status != 304 && length))
        {
            if (!Append(prepared.wire, "Content-Length: ", maxHeaders) ||
                !Append(prepared.wire, std::to_string(prepared.length), maxHeaders) ||
                !Append(prepared.wire, "\r\n", maxHeaders))
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        }
        else if (prepared.mode == ResponseBodyMode::Chunked &&
            !Append(prepared.wire, "Transfer-Encoding: chunked\r\n", maxHeaders))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        if (!Append(prepared.wire, close ? "Connection: close" : "Connection: keep-alive", maxHeaders) ||
            (haveUpgrade && !Append(prepared.wire, ", Upgrade", maxHeaders)) ||
            !Append(prepared.wire, "\r\n\r\n", maxHeaders))
            return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
        return Result::FromValue(std::move(prepared));
    }
    catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
}

Core::Result<std::string> EncodeChunk(std::span<const std::byte> bytes, std::size_t maxChunkBytes)
{
    using Result = Core::Result<std::string>;
    if (bytes.empty()) return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    if (bytes.size() > maxChunkBytes)
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
    std::array<char, sizeof(std::size_t) * 2> digits{};
    const auto encoded = std::to_chars(digits.data(), digits.data() + digits.size(), bytes.size(), 16);
    const auto digitCount = static_cast<std::size_t>(encoded.ptr - digits.data());
    if (encoded.ec != std::errc{} || bytes.size() > (std::numeric_limits<std::size_t>::max)() - digitCount - 4)
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
    try
    {
        std::string wire;
        wire.reserve(bytes.size() + digitCount + 4);
        wire.append(digits.data(), digitCount);
        wire += "\r\n";
        wire.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        wire += "\r\n";
        return Result::FromValue(std::move(wire));
    }
    catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
}
}
