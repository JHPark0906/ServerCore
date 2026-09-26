#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/Json.h"

#include <cstddef>
#include <span>

namespace ServerCore::Protocol::Detail
{
/// <summary>봉투의 최상위 body가 wire에서 차지한 범위를 함께 보관한 파싱 결과다.</summary>
/// <remarks>
/// 최상위 객체의 멤버를 JsonValue로 감싸지 않고 그대로 둔다. JsonValue 안의 객체는 const로만
/// 보이므로 감싸 두면 ParseMessage가 body를 깊이 복사하는 수밖에 없다.
/// Protocol.ParseMessageMovesTheParsedBody가 body가 복사되지 않고 옮겨지는 것을 고정한다.
/// </remarks>
struct ParsedEnvelopeDocument
{
    /// <summary>최상위가 객체였으면 그 멤버들이다. 아니면 비어 있다.</summary>
    JsonValue::Object members;
    bool isObject = false;
    bool hasBody = false;
    std::size_t rawBodySize = 0;
};

[[nodiscard]] Core::Result<ParsedEnvelopeDocument> ParseEnvelopeDocument(
    std::span<const std::byte> bytes, std::size_t maxValues);

/// <summary>준비된 JSON 조각이 들어갈 바깥 컨테이너 깊이까지 포함해 검증한다.</summary>
[[nodiscard]] Core::Result<std::string> DumpJsonAtDepth(const JsonValue& value, std::size_t depth);
}
