#include "ServerCore/Protocol/Json.h"

#include "Core/Utf8Internal.h"
#include "Protocol/JsonInternal.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ServerCore::Protocol
{
namespace
{
class JsonParseFailure final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct TopLevelMembers
{
    bool hasBody = false;
    std::size_t rawBodySize = 0;
};

/// <summary>입력 파싱과 직접 만든 값의 직렬화에 공통으로 적용하는 최대 중첩 깊이다.</summary>
/// <remarks>
/// JsonValue는 호출자가 재귀적으로 직접 만들 수도 있으므로, 파서에만 상한이 있으면 송신 경로가
/// 무제한 재귀를 타게 된다. 같은 값을 두 방향에 적용해 스택 사용량의 계약도 맞춘다.
/// </remarks>
constexpr std::size_t MaximumJsonNestingDepth = 256;

class Parser final
{
public:
    explicit Parser(const std::string_view text)
        : mText(text)
    {
    }

    [[nodiscard]] JsonValue ParseDocument()
    {
        SkipByteOrderMark();
        SkipWhitespace();
        JsonValue result = ParseValue(0);
        SkipWhitespace();
        if (mPosition != mText.size())
        {
            Fail("unexpected trailing data");
        }
        return result;
    }

    [[nodiscard]] Detail::ParsedEnvelopeDocument ParseEnvelopeDocument()
    {
        SkipByteOrderMark();
        SkipWhitespace();

        TopLevelMembers members;
        JsonValue result;
        if (mPosition < mText.size() && mText[mPosition] == '{')
        {
            result = ParseObject(1, &members);
        }
        else
        {
            result = ParseValue(0);
        }

        SkipWhitespace();
        if (mPosition != mText.size())
        {
            Fail("unexpected trailing data");
        }

        Detail::ParsedEnvelopeDocument document;
        document.value = std::move(result);
        document.hasBody = members.hasBody;
        document.rawBodySize = members.rawBodySize;
        return document;
    }

private:
    [[noreturn]] void Fail(const char* message) const
    {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t index = 0; index < mPosition && index < mText.size(); ++index)
        {
            if (mText[index] == '\n')
            {
                ++line;
                column = 1;
            }
            else
            {
                ++column;
            }
        }
        throw JsonParseFailure(std::string(message) + " at line " + std::to_string(line) +
                               ", column " + std::to_string(column));
    }

    void SkipByteOrderMark()
    {
        constexpr unsigned char First = 0xEF;
        constexpr unsigned char Second = 0xBB;
        constexpr unsigned char Third = 0xBF;
        constexpr std::size_t Length = 3;
        if (mPosition == 0 && mText.size() >= Length &&
            static_cast<unsigned char>(mText[0]) == First &&
            static_cast<unsigned char>(mText[1]) == Second &&
            static_cast<unsigned char>(mText[2]) == Third)
        {
            mPosition += Length;
        }
    }

    void SkipWhitespace()
    {
        while (
            mPosition < mText.size() && (mText[mPosition] == ' ' || mText[mPosition] == '\t' ||
                                            mText[mPosition] == '\r' || mText[mPosition] == '\n'))
        {
            ++mPosition;
        }
    }

    [[nodiscard]] bool Consume(const char value)
    {
        if (mPosition < mText.size() && mText[mPosition] == value)
        {
            ++mPosition;
            return true;
        }
        return false;
    }

    [[nodiscard]] JsonValue ParseValue(const std::size_t depth)
    {
        if (depth > MaximumJsonNestingDepth)
        {
            Fail("maximum nesting depth exceeded");
        }
        if (mPosition >= mText.size())
        {
            Fail("expected a value");
        }

        switch (mText[mPosition])
        {
        case 'n':
            ParseLiteral("null");
            return JsonValue(nullptr);
        case 't':
            ParseLiteral("true");
            return JsonValue(true);
        case 'f':
            ParseLiteral("false");
            return JsonValue(false);
        case '"':
            return JsonValue(ParseString());
        case '[':
            return ParseArray(depth + 1);
        case '{':
            return ParseObject(depth + 1, nullptr);
        default:
            if (mText[mPosition] == '-' || (mText[mPosition] >= '0' && mText[mPosition] <= '9'))
            {
                return ParseNumber();
            }
            Fail("invalid value");
        }
    }

    void ParseLiteral(const std::string_view literal)
    {
        if (mText.substr(mPosition, literal.size()) != literal)
        {
            Fail("invalid literal");
        }
        mPosition += literal.size();
    }

    static void AppendUtf8(std::string& output, const std::uint32_t codePoint)
    {
        if (codePoint <= 0x7FU)
        {
            output.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FFU)
        {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else if (codePoint <= 0xFFFFU)
        {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    [[nodiscard]] std::uint32_t ParseHex4()
    {
        if (mPosition + 4 > mText.size())
        {
            Fail("incomplete unicode escape");
        }

        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
        {
            const char character = mText[mPosition++];
            value <<= 4U;
            if (character >= '0' && character <= '9')
            {
                value |= static_cast<std::uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            }
            else if (character >= 'A' && character <= 'F')
            {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            }
            else
            {
                Fail("invalid unicode escape");
            }
        }
        return value;
    }

    [[nodiscard]] std::string ParseString()
    {
        if (!Consume('"'))
        {
            Fail("expected a string");
        }

        std::string result;
        while (mPosition < mText.size())
        {
            const unsigned char character = static_cast<unsigned char>(mText[mPosition++]);
            if (character == '"')
            {
                return result;
            }
            if (character < 0x20U)
            {
                Fail("unescaped control character");
            }
            if (character != '\\')
            {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (mPosition >= mText.size())
            {
                Fail("incomplete escape sequence");
            }

            switch (mText[mPosition++])
            {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u':
            {
                std::uint32_t codePoint = ParseHex4();
                if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
                {
                    if (mPosition + 2 > mText.size() || mText[mPosition] != '\\' ||
                        mText[mPosition + 1] != 'u')
                    {
                        Fail("missing low surrogate");
                    }
                    mPosition += 2;
                    const std::uint32_t low = ParseHex4();
                    if (low < 0xDC00U || low > 0xDFFFU)
                    {
                        Fail("invalid low surrogate");
                    }
                    codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
                }
                else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU)
                {
                    Fail("unexpected low surrogate");
                }
                AppendUtf8(result, codePoint);
                break;
            }
            default:
                Fail("invalid escape sequence");
            }
        }
        Fail("unterminated string");
    }

    [[nodiscard]] JsonValue ParseNumber()
    {
        const std::size_t start = mPosition;
        (void)Consume('-');
        if (Consume('0'))
        {
            if (mPosition < mText.size() &&
                std::isdigit(static_cast<unsigned char>(mText[mPosition])) != 0)
            {
                Fail("leading zero in number");
            }
        }
        else
        {
            if (mPosition >= mText.size() || mText[mPosition] < '1' || mText[mPosition] > '9')
            {
                Fail("invalid number");
            }
            while (mPosition < mText.size() &&
                   std::isdigit(static_cast<unsigned char>(mText[mPosition])) != 0)
            {
                ++mPosition;
            }
        }
        if (Consume('.'))
        {
            if (mPosition >= mText.size() ||
                std::isdigit(static_cast<unsigned char>(mText[mPosition])) == 0)
            {
                Fail("invalid fraction");
            }
            while (mPosition < mText.size() &&
                   std::isdigit(static_cast<unsigned char>(mText[mPosition])) != 0)
            {
                ++mPosition;
            }
        }
        if (mPosition < mText.size() && (mText[mPosition] == 'e' || mText[mPosition] == 'E'))
        {
            ++mPosition;
            if (mPosition < mText.size() && (mText[mPosition] == '+' || mText[mPosition] == '-'))
            {
                ++mPosition;
            }
            if (mPosition >= mText.size() ||
                std::isdigit(static_cast<unsigned char>(mText[mPosition])) == 0)
            {
                Fail("invalid exponent");
            }
            while (mPosition < mText.size() &&
                   std::isdigit(static_cast<unsigned char>(mText[mPosition])) != 0)
            {
                ++mPosition;
            }
        }

        const std::string_view text = mText.substr(start, mPosition - start);
        const char* const first = text.data();
        const char* const last = first + text.size();
        const bool hasFractionOrExponent = text.find_first_of(".eE") != std::string_view::npos;
        if (!hasFractionOrExponent)
        {
            // JSON 정수는 double로 돌아가면 2^53 이후부터 조용히 값이 바뀐다. wire에서 받은
            // 정수는 signed/unsigned 64-bit 범위까지만 값 그대로 보관하고, 그 밖은 반올림하지
            // 않고 형식 오류로 돌린다. -0은 부호를 보존하기 위해 부동소수 경로에 남긴다.
            if (text.front() == '-')
            {
                if (text != "-0")
                {
                    std::int64_t integer = 0;
                    const auto conversion = std::from_chars(first, last, integer);
                    if (conversion.ec == std::errc{} && conversion.ptr == last)
                    {
                        return JsonValue(integer);
                    }
                    Fail("integer is out of range");
                }
            }
            else
            {
                std::uint64_t integer = 0;
                const auto conversion = std::from_chars(first, last, integer);
                if (conversion.ec == std::errc{} && conversion.ptr == last)
                {
                    return JsonValue(integer);
                }
                Fail("integer is out of range");
            }
        }

        double result = 0.0;
        const auto conversion = std::from_chars(first, last, result, std::chars_format::general);
        if (conversion.ec != std::errc{} || conversion.ptr != last || !std::isfinite(result))
        {
            Fail("number is out of range");
        }
        return JsonValue(result);
    }

    [[nodiscard]] JsonValue ParseArray(const std::size_t depth)
    {
        (void)Consume('[');
        SkipWhitespace();
        JsonValue::Array result;
        if (Consume(']'))
        {
            return JsonValue(std::move(result));
        }
        while (true)
        {
            result.push_back(ParseValue(depth));
            SkipWhitespace();
            if (Consume(']'))
            {
                return JsonValue(std::move(result));
            }
            if (!Consume(','))
            {
                Fail("expected ',' or ']'");
            }
            SkipWhitespace();
        }
    }

    [[nodiscard]] JsonValue ParseObject(const std::size_t depth, TopLevelMembers* topLevelMembers)
    {
        (void)Consume('{');
        SkipWhitespace();
        JsonValue::Object result;
        if (Consume('}'))
        {
            return JsonValue(std::move(result));
        }
        while (true)
        {
            if (mPosition >= mText.size() || mText[mPosition] != '"')
            {
                Fail("expected an object key");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':'))
            {
                Fail("expected ':'");
            }
            SkipWhitespace();

            // body 상한은 다시 직렬화한 크기가 아니라 원래 wire 토큰의 범위로 검사한다.
            // 공백과 escape 표기를 바꿔 같은 JSON 값을 보내도 실제 입력 바이트 수가 남는다.
            const std::size_t valueStart = mPosition;
            JsonValue value = ParseValue(depth);
            const std::size_t valueEnd = mPosition;
            if (topLevelMembers != nullptr && key == "body")
            {
                topLevelMembers->hasBody = true;
                topLevelMembers->rawBodySize = valueEnd - valueStart;
            }
            // 중복 키를 처음/마지막 값으로 해석하는 소비자 차이가 없도록 모든 깊이에서 거절한다.
            if (!result.emplace(std::move(key), std::move(value)).second)
            {
                Fail("duplicate object key");
            }

            SkipWhitespace();
            if (Consume('}'))
            {
                return JsonValue(std::move(result));
            }
            if (!Consume(','))
            {
                Fail("expected ',' or '}'");
            }
            SkipWhitespace();
        }
    }

    std::string_view mText;
    std::size_t mPosition = 0;
};

void DumpEscapedString(const std::string& value, std::string& output)
{
    constexpr char HexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (const char character : value)
    {
        const auto raw = static_cast<unsigned char>(character);
        switch (raw)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (raw < 0x20U)
            {
                output += "\\u00";
                output.push_back(HexDigits[raw >> 4U]);
                output.push_back(HexDigits[raw & 0x0FU]);
            }
            else
            {
                output.push_back(character);
            }
            break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] Core::Status DumpValue(
    const JsonValue& value, std::string& output, const std::size_t depth)
{
    if (depth > MaximumJsonNestingDepth)
    {
        return Core::Status::Fail(
            Core::ErrorCode::InvalidArgument, "maximum JSON nesting depth exceeded");
    }
    if (value.IsNull())
    {
        output += "null";
        return Core::Status::Ok();
    }
    if (const bool* boolean = value.TryBoolean())
    {
        output += *boolean ? "true" : "false";
        return Core::Status::Ok();
    }
    if (const std::int64_t* integer = value.TryInt64())
    {
        char buffer[32] = {};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *integer);
        if (result.ec != std::errc{})
        {
            return Core::Status::Fail(
                Core::ErrorCode::PlatformError, "a signed JSON integer could not be formatted");
        }
        output.append(buffer, result.ptr);
        return Core::Status::Ok();
    }
    if (const std::uint64_t* integer = value.TryUInt64())
    {
        char buffer[32] = {};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *integer);
        if (result.ec != std::errc{})
        {
            return Core::Status::Fail(
                Core::ErrorCode::PlatformError, "an unsigned JSON integer could not be formatted");
        }
        output.append(buffer, result.ptr);
        return Core::Status::Ok();
    }
    if (const double* number = value.TryNumber())
    {
        if (!std::isfinite(*number))
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "a JSON number must be finite");
        }

        char buffer[32] = {};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *number);
        if (result.ec == std::errc{})
        {
            output.append(buffer, result.ptr);
            // 최단 표기가 정수 토큰이면 64-bit 정수 범위를 넘는 유한 double도 생긴다.
            // 파서가 그 토큰을 범위 밖 정수로 거절하지 않도록 double 표기를 보존한다.
            const std::string_view formatted(buffer,
                static_cast<std::size_t>(result.ptr - buffer));
            if (formatted.find_first_of(".eE") == std::string_view::npos)
            {
                output += ".0";
            }
        }
        else
        {
            return Core::Status::Fail(
                Core::ErrorCode::PlatformError, "a finite JSON number could not be formatted");
        }
        return Core::Status::Ok();
    }
    if (const std::string* string = value.TryString())
    {
        if (!Core::Detail::IsValidUtf8(*string))
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "a JSON string must be valid UTF-8");
        }
        DumpEscapedString(*string, output);
        return Core::Status::Ok();
    }
    if (const JsonValue::Array* array = value.TryArray())
    {
        output.push_back('[');
        bool first = true;
        for (const JsonValue& element : *array)
        {
            if (!first)
            {
                output += ", ";
            }
            first = false;
            Core::Status elementStatus = DumpValue(element, output, depth + 1);
            if (!elementStatus.IsOk())
            {
                return std::move(elementStatus);
            }
        }
        output.push_back(']');
        return Core::Status::Ok();
    }

    const JsonValue::Object* object = value.TryObject();
    if (object == nullptr)
    {
        output += "null";
        return Core::Status::Ok();
    }

    std::vector<const std::pair<const std::string, JsonValue>*> members;
    members.reserve(object->size());
    for (const auto& member : *object)
    {
        if (!Core::Detail::IsValidUtf8(member.first))
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "a JSON object key must be valid UTF-8");
        }
        members.push_back(&member);
    }
    std::sort(members.begin(), members.end(),
        [](const auto* left, const auto* right) { return left->first < right->first; });

    output.push_back('{');
    bool first = true;
    for (const auto* member : members)
    {
        if (!first)
        {
            output += ", ";
        }
        first = false;
        DumpEscapedString(member->first, output);
        output += ": ";
        Core::Status memberStatus = DumpValue(member->second, output, depth + 1);
        if (!memberStatus.IsOk())
        {
            return std::move(memberStatus);
        }
    }
    output.push_back('}');
    return Core::Status::Ok();
}

[[nodiscard]] Core::Status StatusWithMessageNoThrow(
    const Core::ErrorCode code, const std::string_view message) noexcept
{
    try
    {
        return Core::Status::Fail(code, std::string(message));
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}

[[nodiscard]] Core::Status InvalidFormatFrom(const JsonParseFailure& failure) noexcept
{
    return StatusWithMessageNoThrow(Core::ErrorCode::InvalidFormat, failure.what());
}

[[nodiscard]] Core::Result<JsonValue> ParseText(const std::string_view text)
{
    if (!Core::Detail::IsValidUtf8(text))
    {
        return Core::Result<JsonValue>::FromStatus(StatusWithMessageNoThrow(
            Core::ErrorCode::InvalidFormat, "JSON text is not valid UTF-8"));
    }

    try
    {
        return Core::Result<JsonValue>::FromValue(Parser(text).ParseDocument());
    }
    catch (const JsonParseFailure& failure)
    {
        return Core::Result<JsonValue>::FromStatus(InvalidFormatFrom(failure));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<JsonValue>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& failure)
    {
        return Core::Result<JsonValue>::FromStatus(
            StatusWithMessageNoThrow(Core::ErrorCode::PlatformError, failure.what()));
    }
}

[[nodiscard]] std::string_view AsText(const std::span<const std::byte> bytes) noexcept
{
    const char* const data = bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data());
    return std::string_view(data, bytes.size());
}
}

JsonValue::Number::Number(const double number) noexcept
    : value(number)
    , asDouble(number)
{
}

JsonValue::Number::Number(const std::int64_t integer) noexcept
    : value(integer)
    , asDouble(static_cast<double>(integer))
{
}

JsonValue::Number::Number(const std::uint64_t integer) noexcept
    : value(integer)
    , asDouble(static_cast<double>(integer))
{
}

JsonValue::JsonValue(std::nullptr_t)
    : mValue(nullptr)
{
}

JsonValue::JsonValue(const bool value)
    : mValue(value)
{
}

JsonValue::JsonValue(const double value)
    : mValue(Number(value))
{
}

JsonValue::JsonValue(std::string value)
    : mValue(std::move(value))
{
}

JsonValue::JsonValue(Array value)
    : mValue(std::move(value))
{
}

JsonValue::JsonValue(Object value)
    : mValue(std::move(value))
{
}

bool JsonValue::IsNull() const noexcept
{
    return std::holds_alternative<std::nullptr_t>(mValue);
}

bool JsonValue::IsBoolean() const noexcept
{
    return std::holds_alternative<bool>(mValue);
}

bool JsonValue::IsNumber() const noexcept
{
    return std::holds_alternative<Number>(mValue);
}

bool JsonValue::IsString() const noexcept
{
    return std::holds_alternative<std::string>(mValue);
}

bool JsonValue::IsArray() const noexcept
{
    return std::holds_alternative<Array>(mValue);
}

bool JsonValue::IsObject() const noexcept
{
    return std::holds_alternative<Object>(mValue);
}

std::size_t JsonValue::Size() const noexcept
{
    if (const Array* array = TryArray())
    {
        return array->size();
    }
    if (const Object* object = TryObject())
    {
        return object->size();
    }
    return 0;
}

const JsonValue* JsonValue::Find(const std::string_view key) const noexcept
{
    const Object* const object = TryObject();
    if (object == nullptr)
    {
        return nullptr;
    }
    for (const auto& member : *object)
    {
        if (member.first == key)
        {
            return &member.second;
        }
    }
    return nullptr;
}

const JsonValue* JsonValue::At(const std::size_t index) const noexcept
{
    const Array* const array = TryArray();
    if (array == nullptr || index >= array->size())
    {
        return nullptr;
    }
    return &(*array)[index];
}

const JsonValue::Array* JsonValue::TryArray() const noexcept
{
    return std::get_if<Array>(&mValue);
}

const JsonValue::Object* JsonValue::TryObject() const noexcept
{
    return std::get_if<Object>(&mValue);
}

const std::string* JsonValue::TryString() const noexcept
{
    return std::get_if<std::string>(&mValue);
}

const bool* JsonValue::TryBoolean() const noexcept
{
    return std::get_if<bool>(&mValue);
}

const double* JsonValue::TryNumber() const noexcept
{
    const Number* const number = std::get_if<Number>(&mValue);
    return number != nullptr ? &number->asDouble : nullptr;
}

const std::int64_t* JsonValue::TryInt64() const noexcept
{
    const Number* const number = std::get_if<Number>(&mValue);
    return number != nullptr ? std::get_if<std::int64_t>(&number->value) : nullptr;
}

const std::uint64_t* JsonValue::TryUInt64() const noexcept
{
    const Number* const number = std::get_if<Number>(&mValue);
    return number != nullptr ? std::get_if<std::uint64_t>(&number->value) : nullptr;
}

Core::Result<JsonValue> JsonValue::Parse(const std::string_view text)
{
    return ParseText(text);
}

Core::Result<JsonValue> JsonValue::ParseBytes(const std::span<const std::byte> bytes)
{
    return ParseText(AsText(bytes));
}

Core::Result<std::string> JsonValue::Dump() const
{
    return Detail::DumpJsonAtDepth(*this, 0);
}

Core::Result<std::string> Detail::DumpJsonAtDepth(const JsonValue& value, const std::size_t depth)
{
    try
    {
        std::string result;
        Core::Status dumpStatus = DumpValue(value, result, depth);
        if (!dumpStatus.IsOk())
        {
            return Core::Result<std::string>::FromStatus(std::move(dumpStatus));
        }
        return Core::Result<std::string>::FromValue(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<std::string>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& failure)
    {
        return Core::Result<std::string>::FromStatus(
            StatusWithMessageNoThrow(Core::ErrorCode::PlatformError, failure.what()));
    }
}

namespace Detail
{
Core::Result<ParsedEnvelopeDocument> ParseEnvelopeDocument(const std::span<const std::byte> bytes)
{
    const std::string_view text = AsText(bytes);
    if (!Core::Detail::IsValidUtf8(text))
    {
        return Core::Result<ParsedEnvelopeDocument>::FromStatus(StatusWithMessageNoThrow(
            Core::ErrorCode::InvalidFormat, "JSON text is not valid UTF-8"));
    }

    try
    {
        return Core::Result<ParsedEnvelopeDocument>::FromValue(
            Parser(text).ParseEnvelopeDocument());
    }
    catch (const JsonParseFailure& failure)
    {
        return Core::Result<ParsedEnvelopeDocument>::FromStatus(InvalidFormatFrom(failure));
    }
    catch (const std::bad_alloc&)
    {
        return Core::Result<ParsedEnvelopeDocument>::FromStatus(Core::Status::AllocationFailure());
    }
    catch (const std::exception& failure)
    {
        return Core::Result<ParsedEnvelopeDocument>::FromStatus(
            StatusWithMessageNoThrow(Core::ErrorCode::PlatformError, failure.what()));
    }
}
}
}
