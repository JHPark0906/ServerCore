#pragma once

#include "ServerCore/Core/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ServerCore::Protocol
{
/// <summary>
/// ServerCore의 오류 계약을 따르는 JSON 값 타입이다.
/// </summary>
/// <remarks>
/// 파싱 실패는 예외가 아니라 Core::Result의 InvalidFormat으로 나온다. 외부 호출자가 잘못된
/// 타입을 조회해도 예외가 나오지 않도록 Try 계열 접근자만 공개한다.
///
/// 값은 값 의미론을 갖는다. 객체의 키 순서는 보관할 때 정해지지 않지만 Dump는 키를 정렬하여
/// 같은 값이면 같은 UTF-8 텍스트를 만든다.
/// 숫자 토큰 중 정수는 signed/unsigned 64-bit로 정확히 보관한다. 그 범위를 벗어난 정수는
/// double로 반올림하지 않고 InvalidFormat이다. 소수점이나 지수 표기가 있는 유한 수는
/// double로 보관하므로, 정확한 정수 값이 필요하면 정수 표기를 써야 한다.
/// </remarks>
class JsonValue
{
private:
    // TryNumber()용 double 근사값과 정확한 정수를 함께 둔다. 정수 정밀도가 필요한 호출자는
    // TryInt64()/TryUInt64()를 쓴다.
    struct Number
    {
        explicit Number(double value) noexcept;
        explicit Number(std::int64_t value) noexcept;
        explicit Number(std::uint64_t value) noexcept;

        std::variant<std::int64_t, std::uint64_t, double> value;
        double asDouble = 0.0;
    };

public:
    using Array = std::vector<JsonValue>;
    using Object = std::unordered_map<std::string, JsonValue>;

    JsonValue() = default;
    explicit JsonValue(std::nullptr_t);
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    template <typename Integer>
        requires(std::is_integral_v<std::remove_cv_t<Integer>> &&
            !std::is_same_v<std::remove_cv_t<Integer>, bool> &&
            sizeof(std::remove_cv_t<Integer>) <= sizeof(std::uint64_t))
    explicit JsonValue(Integer value)
    {
        if constexpr (std::is_signed_v<std::remove_cv_t<Integer>>)
        {
            mValue = Number(static_cast<std::int64_t>(value));
        }
        else
        {
            mValue = Number(static_cast<std::uint64_t>(value));
        }
    }
    explicit JsonValue(std::string value);
    explicit JsonValue(Array value);
    explicit JsonValue(Object value);

    [[nodiscard]] bool IsNull() const noexcept;
    [[nodiscard]] bool IsBoolean() const noexcept;
    [[nodiscard]] bool IsNumber() const noexcept;
    [[nodiscard]] bool IsString() const noexcept;
    [[nodiscard]] bool IsArray() const noexcept;
    [[nodiscard]] bool IsObject() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;

    /// <summary>객체 키를 찾는다. 객체가 아니거나 키가 없으면 널이다.</summary>
    [[nodiscard]] const JsonValue* Find(std::string_view key) const noexcept;

    /// <summary>배열 원소를 찾는다. 배열이 아니거나 범위를 벗어나면 널이다.</summary>
    [[nodiscard]] const JsonValue* At(std::size_t index) const noexcept;

    [[nodiscard]] const Array* TryArray() const noexcept;
    [[nodiscard]] const Object* TryObject() const noexcept;
    [[nodiscard]] const std::string* TryString() const noexcept;
    [[nodiscard]] const bool* TryBoolean() const noexcept;
    /// <summary>
    /// 숫자의 double 근사값을 준다. 정수가 정확히 필요하면 TryInt64()/TryUInt64()를 쓴다.
    /// </summary>
    [[nodiscard]] const double* TryNumber() const noexcept;
    [[nodiscard]] const std::int64_t* TryInt64() const noexcept;
    [[nodiscard]] const std::uint64_t* TryUInt64() const noexcept;

    /// <summary>
    /// 유효한 UTF-8 JSON 텍스트를 파싱한다. 범위를 벗어난 정수와 유한 double로 표현할 수 없는
    /// 소수/지수 수는 InvalidFormat이다.
    /// </summary>
    [[nodiscard]] static Core::Result<JsonValue> Parse(std::string_view text);

    /// <summary>바이트로 받은 UTF-8 JSON을 파싱한다.</summary>
    [[nodiscard]] static Core::Result<JsonValue> ParseBytes(std::span<const std::byte> bytes);

    /// <summary>이 값을 결정적인 UTF-8 JSON 텍스트로 쓴다.</summary>
    /// <remarks>
    /// JsonValue는 일반 C++ 값으로도 직접 만들 수 있으므로, 문자열에 잘못된 UTF-8이 있거나
    /// 숫자가 NaN/Infinity이면 JSON으로 쓸 수 없다. 그런 값은 InvalidArgument로 거절하며,
    /// 성공 결과만 유효한 UTF-8 JSON이다.
    /// </remarks>
    [[nodiscard]] Core::Result<std::string> Dump() const;

private:
    using Storage = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    Storage mValue = nullptr;
};
}
