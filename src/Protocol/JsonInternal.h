#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/Json.h"

#include <cstddef>
#include <span>

namespace ServerCore::Protocol::Detail
{
/// <summary>봉투의 최상위 body가 wire에서 차지한 범위를 함께 보관한 파싱 결과다.</summary>
struct ParsedEnvelopeDocument
{
    JsonValue value;
    bool hasBody = false;
    std::size_t rawBodySize = 0;
};

[[nodiscard]] Core::Result<ParsedEnvelopeDocument> ParseEnvelopeDocument(
    std::span<const std::byte> bytes);

/// <summary>준비된 JSON 조각이 들어갈 바깥 컨테이너 깊이까지 포함해 검증한다.</summary>
[[nodiscard]] Core::Result<std::string> DumpJsonAtDepth(const JsonValue& value, std::size_t depth);
}
