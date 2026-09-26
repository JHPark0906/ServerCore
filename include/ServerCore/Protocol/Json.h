#pragma once

#include "ServerCore/Export.h"

#include "ServerCore/Core/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace ServerCore::Protocol
{
/// <summary>JSON 입력 하나를 파싱할 때 만들 수 있는 값의 상한이다.</summary>
struct JsonParseLimits
{
    /// <summary>만들 수 있는 값(DOM 노드) 수의 상한이다. 기본은 제한 없음이다.</summary>
    /// <remarks>
    /// 배열·객체·문자열·숫자·true·false·null을 모두 값 하나로 센다. 객체 멤버의 값은 세고 키는 세지
    /// 않는다. 넘으면 그 값을 만들기 전에 TooLarge로 끝낸다(Protocol.JsonParseLimitsValueCount).
    /// 입력 바이트 수로 막는 상한과 달리 이 값은 DOM 메모리를 막는다. [0,0,…]은 두 바이트마다 값
    /// 하나를 만들어 입력보다 수십 배 큰 DOM이 될 수 있기 때문이다.
    /// </remarks>
    std::size_t maxValues = (std::numeric_limits<std::size_t>::max)();
};

/// <summary>
/// ServerCore의 오류 계약을 따르는 JSON 값 타입이다.
/// </summary>
/// <remarks>
/// 파싱 실패는 예외가 아니라 Core::Result의 InvalidFormat으로 나온다. 외부 호출자가 잘못된
/// 타입을 조회해도 예외가 나오지 않도록 Try 계열 접근자만 공개한다.
///
/// 값은 값 의미론을 갖는다. 객체는 키를 바이트 순서로 정렬해 보관하고 Dump도 그 순서로 써서
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
        SERVERCORE_API explicit Number(double value) noexcept;
        SERVERCORE_API explicit Number(std::int64_t value) noexcept;
        SERVERCORE_API explicit Number(std::uint64_t value) noexcept;

        std::variant<std::int64_t, std::uint64_t, double> value;
        double asDouble = 0.0;
    };

    // signed char·unsigned char는 std::int8_t·std::uint8_t로 쓰이므로 정수로 남긴다.
    template <typename Type>
    static constexpr bool IsCharacter = std::is_same_v<std::remove_cv_t<Type>, char> ||
                                        std::is_same_v<std::remove_cv_t<Type>, wchar_t> ||
                                        std::is_same_v<std::remove_cv_t<Type>, char8_t> ||
                                        std::is_same_v<std::remove_cv_t<Type>, char16_t> ||
                                        std::is_same_v<std::remove_cv_t<Type>, char32_t>;

public:
    using Array = std::vector<JsonValue>;
    /// <remarks>
    /// 해시 표가 아니라 키 비교로 정렬한 트리다. seed 없는 표준 해시는 외부 입력이 한 버킷에 몰리는
    /// 키를 고를 수 있어, 객체 하나를 파싱하는 시간이 키 수의 제곱으로 늘어난다. 트리는 삽입 한 번의
    /// 비교 횟수가 키 수의 로그로 묶인다. Protocol.JsonObjectKeysResistHashFlooding이 이 선택을 고정한다.
    /// </remarks>
    using Object = std::map<std::string, JsonValue, std::less<>>;

    JsonValue() = default;
    SERVERCORE_API explicit JsonValue(std::nullptr_t);
    SERVERCORE_API explicit JsonValue(bool value);
    SERVERCORE_API explicit JsonValue(double value);
    template <typename Integer>
        requires(std::is_integral_v<std::remove_cv_t<Integer>> &&
                 !std::is_same_v<std::remove_cv_t<Integer>, bool> && !IsCharacter<Integer> &&
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
    /// <summary>문자 하나는 숫자로도 문자열로도 받지 않는다. 'A'가 65가 되지 않게 막는다.</summary>
    /// <remarks>Protocol.JsonTextConstructorsKeepTheirType의 static_assert가 이 막힘을 고정한다.</remarks>
    template <typename Character>
        requires IsCharacter<Character>
    explicit JsonValue(Character) = delete;
    /// <summary>NUL로 끝나는 C 문자열을 JSON 문자열로 보관한다.</summary>
    /// <remarks>
    /// 이 생성자가 없으면 문자열 리터럴이 포인터→bool 표준 변환을 타 true가 된다.
    /// 널 포인터는 계약 위반이며 SERVERCORE_ASSERT로 끊는다.
    /// </remarks>
    SERVERCORE_API explicit JsonValue(const char* value);
    /// <summary>문자열이 아닌 포인터는 bool로 바뀌지 않게 막는다.</summary>
    /// <remarks>Protocol.JsonTextConstructorsKeepTheirType의 static_assert가 이 막힘을 고정한다.</remarks>
    template <typename Pointee>
        requires(!std::is_same_v<Pointee, char> && !std::is_same_v<Pointee, const char>)
    explicit JsonValue(Pointee*) = delete;
    SERVERCORE_API explicit JsonValue(std::string value);
    SERVERCORE_API explicit JsonValue(Array value);
    SERVERCORE_API explicit JsonValue(Object value);

    [[nodiscard]] SERVERCORE_API bool IsNull() const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsBoolean() const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsNumber() const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsString() const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsArray() const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsObject() const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t Size() const noexcept;

    /// <summary>객체 키를 찾는다. 객체가 아니거나 키가 없으면 널이다.</summary>
    [[nodiscard]] SERVERCORE_API const JsonValue* Find(std::string_view key) const noexcept;

    /// <summary>배열 원소를 찾는다. 배열이 아니거나 범위를 벗어나면 널이다.</summary>
    [[nodiscard]] SERVERCORE_API const JsonValue* At(std::size_t index) const noexcept;

    [[nodiscard]] SERVERCORE_API const Array* TryArray() const noexcept;
    [[nodiscard]] SERVERCORE_API const Object* TryObject() const noexcept;
    [[nodiscard]] SERVERCORE_API const std::string* TryString() const noexcept;
    [[nodiscard]] SERVERCORE_API const bool* TryBoolean() const noexcept;
    /// <summary>
    /// 숫자의 double 근사값을 준다. 정수가 정확히 필요하면 TryInt64()/TryUInt64()를 쓴다.
    /// </summary>
    [[nodiscard]] SERVERCORE_API const double* TryNumber() const noexcept;
    [[nodiscard]] SERVERCORE_API const std::int64_t* TryInt64() const noexcept;
    [[nodiscard]] SERVERCORE_API const std::uint64_t* TryUInt64() const noexcept;

    /// <summary>
    /// 유효한 UTF-8 JSON 텍스트를 파싱한다. 범위를 벗어난 정수와, 절댓값이 너무 커서 유한 double로
    /// 표현할 수 없는 소수/지수 수는 InvalidFormat이다. 절댓값이 너무 작아 double로 표현할 수 없는
    /// 수는 부호를 지킨 0이 된다(Protocol.JsonNumbersUnderflowToSignedZero가 고정한다).
    /// </summary>
    [[nodiscard]] SERVERCORE_API static Core::Result<JsonValue> Parse(std::string_view text);

    /// <summary>limits 안에서 파싱한다. 값 수 상한을 넘으면 TooLarge다.</summary>
    [[nodiscard]] SERVERCORE_API static Core::Result<JsonValue> Parse(
        std::string_view text, const JsonParseLimits& limits);

    /// <summary>바이트로 받은 UTF-8 JSON을 파싱한다.</summary>
    [[nodiscard]] SERVERCORE_API static Core::Result<JsonValue> ParseBytes(
        std::span<const std::byte> bytes);

    /// <summary>limits 안에서 바이트로 받은 UTF-8 JSON을 파싱한다. 값 수 상한을 넘으면 TooLarge다.</summary>
    [[nodiscard]] SERVERCORE_API static Core::Result<JsonValue> ParseBytes(
        std::span<const std::byte> bytes, const JsonParseLimits& limits);

    /// <summary>이 값을 결정적인 UTF-8 JSON 텍스트로 쓴다.</summary>
    /// <remarks>
    /// JsonValue는 일반 C++ 값으로도 직접 만들 수 있으므로, 문자열에 잘못된 UTF-8이 있거나
    /// 숫자가 NaN/Infinity이면 JSON으로 쓸 수 없다. 그런 값은 InvalidArgument로 거절하며,
    /// 성공 결과만 유효한 UTF-8 JSON이다.
    /// </remarks>
    [[nodiscard]] SERVERCORE_API Core::Result<std::string> Dump() const;

private:
    using Storage = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    Storage mValue = nullptr;
};
}
