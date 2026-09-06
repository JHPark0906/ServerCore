#include "ServerCore/Core/Config.h"

#include "Core/Utf8Internal.h"

#include <Windows.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ServerCore::Core
{
namespace
{
using Values = std::map<std::string, std::string, std::less<>>;

constexpr std::string_view Utf8ByteOrderMark = "\xEF\xBB\xBF";
constexpr std::size_t ConfigReadBufferBytes = 64 * 1024;

class FileHandle final
{
public:
    explicit FileHandle(const HANDLE handle) noexcept
        : mHandle(handle)
    {
    }

    ~FileHandle()
    {
        if (mHandle != INVALID_HANDLE_VALUE)
        {
            (void)::CloseHandle(mHandle);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) = delete;
    FileHandle& operator=(FileHandle&&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return mHandle; }

private:
    HANDLE mHandle = INVALID_HANDLE_VALUE;
};

[[nodiscard]] bool IsHorizontalWhitespace(const char character) noexcept
{
    return character == ' ' || character == '\t';
}

[[nodiscard]] std::string_view TrimHorizontalWhitespace(std::string_view text) noexcept
{
    while (!text.empty() && IsHorizontalWhitespace(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsHorizontalWhitespace(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool IsValidKey(const std::string_view key) noexcept
{
    if (key.empty())
    {
        return false;
    }

    for (const char character : key)
    {
        const bool isLetter =
            (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
        const bool isDigit = character >= '0' && character <= '9';
        if (!isLetter && !isDigit && character != '.' && character != '_' && character != '-')
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool HasUnsupportedLineEnding(const std::string_view text) noexcept
{
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == '\r' && (index + 1 == text.size() || text[index + 1] != '\n'))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Status InvalidLine(const std::size_t line, const std::string_view reason)
{
    std::string message("config line ");
    message.append(std::to_string(line));
    message.append(": ");
    message.append(reason);
    return Status::Fail(ErrorCode::InvalidFormat, std::move(message));
}

[[nodiscard]] Status Win32Failure(const std::string_view operation, const unsigned long error)
{
    std::string message(operation);
    message.append(" failed, GetLastError()=");
    message.append(std::to_string(error));
    return Status::Fail(ErrorCode::PlatformError, std::move(message));
}

[[nodiscard]] Result<std::wstring> ToWidePath(const std::string_view path)
{
    if (path.empty() || path.find('\0') != std::string_view::npos || !Detail::IsValidUtf8(path))
    {
        return Result<std::wstring>::FromStatus(
            Status::Fail(ErrorCode::InvalidArgument, "config path must be non-empty UTF-8 text"));
    }
    if (path.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return Result<std::wstring>::FromStatus(
            Status::Fail(ErrorCode::InvalidArgument, "config path is too long for Windows"));
    }

    const int wideLength = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
    if (wideLength == 0)
    {
        return Result<std::wstring>::FromStatus(
            Win32Failure("MultiByteToWideChar(config path)", ::GetLastError()));
    }

    std::wstring widePath(static_cast<std::size_t>(wideLength), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
            static_cast<int>(path.size()), widePath.data(), wideLength) == 0)
    {
        return Result<std::wstring>::FromStatus(
            Win32Failure("MultiByteToWideChar(config path)", ::GetLastError()));
    }
    return Result<std::wstring>::FromValue(std::move(widePath));
}

[[nodiscard]] Result<std::string> ReadConfigText(const std::string_view path)
{
    Result<std::wstring> widePath = ToWidePath(path);
    if (!widePath.IsOk())
    {
        return Result<std::string>::FromStatus(std::move(widePath).TakeStatus());
    }

    const HANDLE rawFile = ::CreateFileW(widePath.Value().c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rawFile == INVALID_HANDLE_VALUE)
    {
        const unsigned long error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        {
            return Result<std::string>::FromStatus(
                Status::Fail(ErrorCode::NotFound, "config file was not found"));
        }
        return Result<std::string>::FromStatus(Win32Failure("CreateFileW(config path)", error));
    }

    const FileHandle file(rawFile);
    std::array<char, ConfigReadBufferBytes> buffer{};
    std::string text;
    for (;;)
    {
        DWORD bytesRead = 0;
        if (::ReadFile(file.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead,
                nullptr) == FALSE)
        {
            return Result<std::string>::FromStatus(
                Win32Failure("ReadFile(config file)", ::GetLastError()));
        }
        if (bytesRead == 0)
        {
            break;
        }

        text.append(buffer.data(), static_cast<std::size_t>(bytesRead));
    }
    return Result<std::string>::FromValue(std::move(text));
}

[[nodiscard]] Result<Values> ParseConfigText(std::string_view text)
{
    if (text.find('\0') != std::string_view::npos)
    {
        return Result<Values>::FromStatus(
            Status::Fail(ErrorCode::InvalidFormat, "config text contains a NUL byte"));
    }
    if (!Detail::IsValidUtf8(text))
    {
        return Result<Values>::FromStatus(
            Status::Fail(ErrorCode::InvalidFormat, "config text is not valid UTF-8"));
    }
    if (HasUnsupportedLineEnding(text))
    {
        return Result<Values>::FromStatus(
            Status::Fail(ErrorCode::InvalidFormat, "config text uses an unsupported line ending"));
    }
    if (text.starts_with(Utf8ByteOrderMark))
    {
        text.remove_prefix(Utf8ByteOrderMark.size());
    }

    Values values;
    std::size_t lineStart = 0;
    std::size_t lineNumber = 1;
    while (lineStart < text.size())
    {
        const std::size_t newline = text.find('\n', lineStart);
        const std::size_t lineEnd = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(lineStart, lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        const std::string_view trimmedLine = TrimHorizontalWhitespace(line);
        if (!trimmedLine.empty() && trimmedLine.front() != '#')
        {
            const std::size_t separator = trimmedLine.find('=');
            if (separator == std::string_view::npos)
            {
                return Result<Values>::FromStatus(InvalidLine(lineNumber, "missing '=' separator"));
            }

            const std::string_view key = TrimHorizontalWhitespace(trimmedLine.substr(0, separator));
            if (!IsValidKey(key))
            {
                return Result<Values>::FromStatus(InvalidLine(lineNumber, "key is not valid"));
            }
            const std::string_view value =
                TrimHorizontalWhitespace(trimmedLine.substr(separator + 1));
            const auto insertion = values.emplace(std::string(key), std::string(value));
            if (!insertion.second)
            {
                return Result<Values>::FromStatus(
                    InvalidLine(lineNumber, "key is declared more than once"));
            }
        }

        if (newline == std::string_view::npos)
        {
            break;
        }
        lineStart = newline + 1;
        ++lineNumber;
    }
    return Result<Values>::FromValue(std::move(values));
}

[[nodiscard]] Status MissingKey()
{
    return Status::Fail(ErrorCode::NotFound, "config key was not found");
}
}

struct Config::Storage
{
    Values values;
};

Config::Config(std::shared_ptr<const Storage> storage)
    : mStorage(std::move(storage))
{
}

Config::~Config() = default;

Result<Config> Config::LoadFromFile(const std::string_view path)
{
    try
    {
        Result<std::string> text = ReadConfigText(path);
        if (!text.IsOk())
        {
            return Result<Config>::FromStatus(std::move(text).TakeStatus());
        }

        Result<Values> values = ParseConfigText(text.Value());
        if (!values.IsOk())
        {
            return Result<Config>::FromStatus(std::move(values).TakeStatus());
        }

        // 파일 전체의 검증이 끝난 값만 불변 저장소로 공개한다. 조회자는 파싱 중간 상태를
        // 볼 수 없고, Config 복사본끼리는 별도 잠금 없이 같은 완성된 설정을 읽는다.
        auto storage = std::make_shared<Storage>();
        storage->values = std::move(values.Value());
        return Result<Config>::FromValue(Config(std::move(storage)));
    }
    catch (const std::exception&)
    {
        return Result<Config>::FromStatus(Status::AllocationFailure());
    }
}

Result<int> Config::GetInt(const std::string_view key) const
{
    try
    {
        if (!IsValidKey(key))
        {
            return Result<int>::FromStatus(
                Status::Fail(ErrorCode::InvalidArgument, "config key is not valid"));
        }

        const std::string* const value = FindValue(key);
        if (value == nullptr)
        {
            return Result<int>::FromStatus(MissingKey());
        }

        int integer = 0;
        const auto conversion =
            std::from_chars(value->data(), value->data() + value->size(), integer, 10);
        if (conversion.ec != std::errc{} || conversion.ptr != value->data() + value->size())
        {
            return Result<int>::FromStatus(
                Status::Fail(ErrorCode::InvalidFormat, "config value is not a decimal int"));
        }
        return Result<int>::FromValue(integer);
    }
    catch (const std::exception&)
    {
        return Result<int>::FromStatus(Status::AllocationFailure());
    }
}

Result<std::string> Config::GetString(const std::string_view key) const
{
    try
    {
        if (!IsValidKey(key))
        {
            return Result<std::string>::FromStatus(
                Status::Fail(ErrorCode::InvalidArgument, "config key is not valid"));
        }

        const std::string* const value = FindValue(key);
        if (value == nullptr)
        {
            return Result<std::string>::FromStatus(MissingKey());
        }

        return Result<std::string>::FromValue(*value);
    }
    catch (const std::exception&)
    {
        return Result<std::string>::FromStatus(Status::AllocationFailure());
    }
}

const std::string* Config::FindValue(const std::string_view key) const noexcept
{
    if (mStorage == nullptr)
    {
        return nullptr;
    }

    const auto found = mStorage->values.find(key);
    return found == mStorage->values.end() ? nullptr : &found->second;
}
}
