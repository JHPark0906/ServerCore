#include "ServerCore/Web/RequestData.h"
#include "Core/Utf8Internal.h"
#include "Web/WebProtocol.h"
#include <algorithm>

namespace ServerCore::Web
{
namespace
{
using Core::ErrorCode;
template <class T> Core::Result<T> Fail(ErrorCode code)
{
    return Core::Result<T>::FromStatus(Core::Status::FailWithoutMessage(code));
}
bool Valid(const FieldLimits& limits) noexcept
{
    return limits.maxBytes && limits.maxFields && limits.maxNameBytes && limits.maxValueBytes;
}
bool Text(std::string_view value) noexcept
{
    return !value.contains('\0') && Core::Detail::IsValidUtf8(value);
}
int Hex(char value) noexcept
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}
std::string_view Trim(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}
bool CookieValue(std::string_view value) noexcept
{
    for (const unsigned char ch : value)
        if (!(ch == 0x21 || (ch >= 0x23 && ch <= 0x2B) || (ch >= 0x2D && ch <= 0x3A) ||
                (ch >= 0x3C && ch <= 0x5B) || (ch >= 0x5D && ch <= 0x7E)))
            return false;
    return true;
}
Core::Result<FormFields> ParseFields(std::string_view input, bool form, const FieldLimits& limits)
{
    if (!Valid(limits))
        return Fail<FormFields>(ErrorCode::InvalidArgument);
    if (input.size() > limits.maxBytes)
        return Fail<FormFields>(ErrorCode::TooLarge);
    FormFields result;
    while (!input.empty())
    {
        const auto next = input.find('&');
        const auto field = input.substr(0, next);
        input = next == input.npos ? std::string_view{} : input.substr(next + 1);
        if (field.empty())
            continue;
        if (result.size() == limits.maxFields)
            return Fail<FormFields>(ErrorCode::TooLarge);
        const auto equal = field.find('=');
        auto name = PercentDecodeComponent(field.substr(0, equal), form, limits.maxNameBytes);
        if (!name.IsOk())
            return Core::Result<FormFields>::FromStatus(std::move(name).TakeStatus());
        auto value = PercentDecodeComponent(
            equal == field.npos ? std::string_view{} : field.substr(equal + 1), form,
            limits.maxValueBytes);
        if (!value.IsOk())
            return Core::Result<FormFields>::FromStatus(std::move(value).TakeStatus());
        result.emplace_back(std::move(name.Value()), std::move(value.Value()));
    }
    return Core::Result<FormFields>::FromValue(std::move(result));
}
bool Domain(std::string_view value) noexcept
{
    if (value.empty())
        return true;
    if (value.front() == '.')
        value.remove_prefix(1);
    if (value.empty() || value.size() > 253)
        return false;
    while (!value.empty())
    {
        const auto dot = value.find('.');
        const auto label = value.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            return false;
        for (const unsigned char ch : label)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-'))
                return false;
        if (dot == value.npos)
            return true;
        value.remove_prefix(dot + 1);
        if (value.empty())
            return false;
    }
    return true;
}
}
Core::Result<std::string> PercentDecodeComponent(
    std::string_view input, bool plusAsSpace, std::size_t maxBytes)
{
    if (!maxBytes)
        return Fail<std::string>(ErrorCode::InvalidArgument);
    try
    {
        std::string result;
        result.reserve((std::min)(input.size(), maxBytes));
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            unsigned char ch = static_cast<unsigned char>(input[i]);
            if (ch == '%')
            {
                if (input.size() - i < 3 || Hex(input[i + 1]) < 0 || Hex(input[i + 2]) < 0)
                    return Fail<std::string>(ErrorCode::InvalidFormat);
                ch = static_cast<unsigned char>(Hex(input[i + 1]) * 16 + Hex(input[i + 2]));
                i += 2;
            }
            else if (ch == '+' && plusAsSpace)
                ch = ' ';
            if (result.size() == maxBytes)
                return Fail<std::string>(ErrorCode::TooLarge);
            result.push_back(static_cast<char>(ch));
        }
        if (!Text(result))
            return Fail<std::string>(ErrorCode::InvalidFormat);
        return Core::Result<std::string>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<std::string> PercentEncodeComponent(
    std::string_view input, bool formMode, std::size_t maxBytes)
{
    if (!maxBytes || !Text(input))
        return Fail<std::string>(ErrorCode::InvalidArgument);
    try
    {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string result;
        for (const unsigned char ch : input)
        {
            const bool literal = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                 (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' ||
                                 (formMode ? ch == '*' : ch == '~');
            const std::size_t count = literal || (formMode && ch == ' ') ? 1 : 3;
            if (count > maxBytes - result.size())
                return Fail<std::string>(ErrorCode::TooLarge);
            if (literal)
                result.push_back(static_cast<char>(ch));
            else if (formMode && ch == ' ')
                result.push_back('+');
            else
            {
                result.push_back('%');
                result.push_back(hex[ch >> 4]);
                result.push_back(hex[ch & 15]);
            }
        }
        return Core::Result<std::string>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<FormFields> ParseQueryParameters(std::string_view target, const FieldLimits& limits)
{
    if (target.find('#') != target.npos)
        return Fail<FormFields>(ErrorCode::InvalidFormat);
    const auto question = target.find('?');
    try
    {
        return ParseFields(
            question == target.npos ? std::string_view{} : target.substr(question + 1), false,
            limits);
    }
    catch (...)
    {
        return Core::Result<FormFields>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<FormFields> ParseFormUrlEncoded(std::string_view body, const FieldLimits& limits)
{
    try
    {
        return ParseFields(body, true, limits);
    }
    catch (...)
    {
        return Core::Result<FormFields>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<FormFields> ParseCookieHeader(std::string_view input, const FieldLimits& limits)
{
    if (!Valid(limits))
        return Fail<FormFields>(ErrorCode::InvalidArgument);
    if (input.size() > limits.maxBytes)
        return Fail<FormFields>(ErrorCode::TooLarge);
    try
    {
        FormFields result;
        while (!input.empty())
        {
            const auto next = input.find(';');
            const auto field = Trim(input.substr(0, next));
            input = next == input.npos ? std::string_view{} : input.substr(next + 1);
            if (field.empty())
                continue;
            const auto equal = field.find('=');
            if (equal == field.npos)
                return Fail<FormFields>(ErrorCode::InvalidFormat);
            const auto name = field.substr(0, equal);
            auto value = field.substr(equal + 1);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            if (!Detail::IsToken(name) || !CookieValue(value))
                return Fail<FormFields>(ErrorCode::InvalidFormat);
            if (result.size() == limits.maxFields || name.size() > limits.maxNameBytes ||
                value.size() > limits.maxValueBytes)
                return Fail<FormFields>(ErrorCode::TooLarge);
            result.emplace_back(name, value);
        }
        return Core::Result<FormFields>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Core::Result<FormFields>::FromStatus(Core::Status::AllocationFailure());
    }
}
Core::Result<std::string> SerializeSetCookie(
    std::string_view name, std::string_view value, const CookieOptions& options)
{
    if (!options.maxBytes || !Detail::IsToken(name) || !CookieValue(value) ||
        !Domain(options.domain) || (!options.path.empty() && options.path.front() != '/') ||
        (options.sameSite != CookieSameSite::Unspecified &&
            options.sameSite != CookieSameSite::Lax && options.sameSite != CookieSameSite::Strict &&
            options.sameSite != CookieSameSite::None) ||
        (options.sameSite == CookieSameSite::None && !options.secure) ||
        (name.starts_with("__Secure-") && !options.secure) ||
        (name.starts_with("__Host-") &&
            (!options.secure || !options.domain.empty() || options.path != "/")))
        return Fail<std::string>(ErrorCode::InvalidArgument);
    for (const unsigned char ch : options.path)
        if (ch < 0x20 || ch > 0x7E || ch == ';')
            return Fail<std::string>(ErrorCode::InvalidArgument);
    try
    {
        std::string result;
        const auto append = [&](std::string_view text)
        {
            if (text.size() > options.maxBytes - result.size())
                return false;
            result.append(text);
            return true;
        };
        if (!append(name) || !append("=") || !append(value))
            return Fail<std::string>(ErrorCode::TooLarge);
        if (!options.domain.empty() && (!append("; Domain=") || !append(options.domain)))
            return Fail<std::string>(ErrorCode::TooLarge);
        if (!options.path.empty() && (!append("; Path=") || !append(options.path)))
            return Fail<std::string>(ErrorCode::TooLarge);
        if (options.maxAgeSeconds &&
            (!append("; Max-Age=") ||
                !append(std::to_string((std::max)(std::int64_t{ 0 }, *options.maxAgeSeconds)))))
            return Fail<std::string>(ErrorCode::TooLarge);
        if (options.secure && !append("; Secure"))
            return Fail<std::string>(ErrorCode::TooLarge);
        if (options.httpOnly && !append("; HttpOnly"))
            return Fail<std::string>(ErrorCode::TooLarge);
        const auto sameSite = options.sameSite == CookieSameSite::Lax      ? "; SameSite=Lax"
                              : options.sameSite == CookieSameSite::Strict ? "; SameSite=Strict"
                              : options.sameSite == CookieSameSite::None   ? "; SameSite=None"
                                                                           : "";
        if (!append(sameSite))
            return Fail<std::string>(ErrorCode::TooLarge);
        return Core::Result<std::string>::FromValue(std::move(result));
    }
    catch (...)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
}
}
