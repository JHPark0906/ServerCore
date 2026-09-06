#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/Json.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ServerCore::Protocol
{
/// <summary>
/// 프레임 본문을 해석한 결과다. 봉투를 뜯은 상태라고 보면 된다.
/// </summary>
/// <remarks>
/// 와이어 규격 - 봉투의 모양:
/// { "type": "&lt;문자열&gt;", "body": { ... }, "seq": &lt;임의 JSON 값&gt; }
/// type은 필수다. body는 기본적으로 필수지만 error가 있으면 생략할 수 있다. seq는 있으면
/// 응답으로 같은 JSON 값이 돌아가도록 보존하며, 코어는 그 타입을 해석하지 않는다. 특히
/// signed/unsigned 64-bit 정수는 double 반올림 없이 보존한다.
///
/// 왜 타입이 머리가 아니라 본문에 있는가:
/// 타입을 숫자로 머리에 넣으면 장르가 다른 두 게임이 번호 공간을 나눠 써야 하고, 한쪽이 번호를
/// 옮기면 다른 쪽이 조용히 깨진다. 문자열이면 각 게임이 자기 이름만 등록하면 되고 충돌 관리가
/// 필요 없다. 대가는 라우팅 전에 JSON을 파싱해야 한다는 것인데, 텍스트 형식을 고른 시점에
/// 이미 지불된 비용이다.
///
/// 왜 body를 한 겹 감쌌는가:
/// 봉투 필드와 게임 필드가 같은 평면에 있으면, 나중에 봉투에 필드를 더할 때 게임이 이미 쓰던
/// 이름과 부딪칠 수 있다. 한 겹 감싸면 그 일이 구조적으로 없다.
///
/// 소유권과 수명:
/// Type()과 Body()가 돌려주는 것은 이 객체가 살아 있는 동안만 유효한 참조다.
///
/// 약속하지 않는 것:
/// - body 안을 검증하지 않는다. 봉투가 형식에 맞고 type이 문자열이면 통과다.
///   내용의 옳고 그름은 게임 백엔드의 몫이다.
/// - 메시지의 의미를 모른다. 라우팅도 하지 않는다.
/// </remarks>
class Message
{
public:
    /// <summary>봉투의 type 값이다. 디스패치의 열쇠다.</summary>
    [[nodiscard]] std::string_view Type() const;

    /// <summary>봉투의 body 값이다. 없으면 널이다.</summary>
    [[nodiscard]] const JsonValue* Body() const noexcept;

    /// <summary>봉투의 seq 값이다. 없으면 널이다.</summary>
    [[nodiscard]] const JsonValue* Sequence() const noexcept;

    /// <summary>봉투의 error 값이다. 없으면 널이다.</summary>
    [[nodiscard]] const JsonValue* Error() const noexcept;

    /// <summary>원래 wire에서 body 값만 차지한 UTF-8 바이트 수다.</summary>
    [[nodiscard]] std::size_t RawBodySize() const noexcept;

private:
    friend Core::Result<Message> ParseMessage(std::span<const std::byte> jsonBody);

    std::string mType;
    std::optional<JsonValue> mBody;
    std::optional<JsonValue> mSequence;
    std::optional<JsonValue> mError;
    std::size_t mRawBodySize = 0;
};

/// <summary>직렬화할 봉투 필드다.</summary>
// 이 구조체는 문자열과 JSON 값을 빌려 쓴다. SerializeMessage 또는 Session::Send 호출이
// 끝날 때까지만 원본을 유지하면 되며, 비동기 송신 큐는 직렬화한 바이트 사본을 소유한다.
struct MessageFields
{
    std::string_view type;
    const JsonValue* body = nullptr;
    const JsonValue* sequence = nullptr;
    const JsonValue* error = nullptr;
};

class PreparedMessage;

/// <summary>검증·직렬화를 마친 불변 JSON 값이다. 여러 수신자의 봉투에 재사용한다.</summary>
/// <remarks>원본 JsonValue를 빌리지 않는다. 이동한 뒤 비어 있는 값은 조립에 사용할 수 없다.</remarks>
class PreparedJsonValue
{
public:
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return mBytes; }
    [[nodiscard]] std::size_t Size() const noexcept { return mBytes.size(); }
private:
    PreparedJsonValue() = default;
    friend class Core::Result<PreparedJsonValue>;
    friend Core::Result<PreparedJsonValue> PrepareJsonValue(const JsonValue& value);
    std::vector<std::byte> mBytes;
};

/// <summary>검증된 UTF-8 봉투를 소유한다. 길이 접두사는 세션의 송신 경계에서 붙인다.</summary>
/// <remarks>원시 바이트로 임의 생성할 수 없다. 준비 함수만 봉투의 JSON 계약을 확정한다.</remarks>
class PreparedMessage
{
public:
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return mBytes; }
    [[nodiscard]] std::size_t Size() const noexcept { return mBytes.size(); }
private:
    PreparedMessage() = default;
    friend class Core::Result<PreparedMessage>;
    friend Core::Result<PreparedMessage> PrepareMessage(const MessageFields& fields);
    friend Core::Result<PreparedMessage> PrepareArrayMessage(std::string_view type,
        std::string_view arrayKey, std::span<const PreparedJsonValue* const> items);
    std::vector<std::byte> mBytes;
};

/// <summary>UTF-8·유한 숫자 검증을 거쳐 재사용할 JSON 값 하나를 만든다.</summary>
/// <remarks>조립할 봉투/body/배열의 깊이 3도 JSON 중첩 한도에 포함해 검증한다.</remarks>
Core::Result<PreparedJsonValue> PrepareJsonValue(const JsonValue& value);
/// <summary>일반 봉투를 한 번 직렬화한다. SendPrepared는 같은 봉투를 다시 직렬화하지 않는다.</summary>
Core::Result<PreparedMessage> PrepareMessage(const MessageFields& fields);
/// <summary>body의 한 배열에 준비된 바이트를 복사한다. JSON 값을 재파싱·재직렬화하지 않는다.</summary>
/// <remarks>type과 배열 키는 정상 JSON 문자열로 escape한다. null/이동 후 빈 항목은 거절한다.
/// Size()는 봉투 전체 크기이며 실제 TCP 프레임 예산에는 길이 접두사 4바이트도 포함한다.</remarks>
Core::Result<PreparedMessage> PrepareArrayMessage(std::string_view type,
    std::string_view arrayKey, std::span<const PreparedJsonValue* const> items);

/// <summary>
/// 프레임 본문(UTF-8 JSON)을 해석해 메시지로 만든다.
/// </summary>
/// <returns>
/// JSON이 깨졌거나 type이 없거나 문자열이 아니면 ErrorCode::InvalidFormat.
/// 이 실패는 예상되는 실패다. 와이어 건너편은 언제든 이런 것을 보낼 수 있다.
/// </returns>
/// <remarks>
/// 입력 span과 반환 Message 이외의 가변 전역 상태를 읽거나 쓰지 않는다. 따라서 서로 다른 본문에
/// 대한 호출은 동시에 해도 안전하다. 단, 반환 Message의 참조는 일반적인 객체 수명 규칙을 따른다.
/// ServerHost는 이 성질을 이용해 FrameReader 뒤의 JSON 해석만 별도 parse worker에서 병렬화한다.
/// </remarks>
Core::Result<Message> ParseMessage(std::span<const std::byte> jsonBody);

/// <summary>선택 필드까지 포함해 봉투 하나를 직렬화한다.</summary>
/// <remarks>
/// body가 없으면 error가 반드시 있어야 한다. error는 객체이고 그 안의 code는 문자열이어야
/// 한다. 코어는 code의 의미는 검증하지 않는다.
/// </remarks>
Core::Result<std::vector<std::byte>> SerializeMessage(const MessageFields& fields);

/// <summary>
/// 타입과 본문으로 봉투를 만들어 UTF-8 JSON 바이트로 만든다.
/// </summary>
/// <remarks>머리는 붙이지 않는다. 그것은 EncodeFrame의 일이다.</remarks>
Core::Result<std::vector<std::byte>> SerializeMessage(std::string_view type, const JsonValue& body);
}
