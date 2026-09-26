#pragma once
#include "ServerCore/Export.h"

#include "ServerCore/Web/HttpServer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ServerCore::Web::Detail
{
/// <summary>응답 본문의 끝을 무엇으로 알리는가.</summary>
/// <remarks>CloseDelimited는 청크를 쓸 수 없는 HTTP/1.0 요청에만 쓰며, 본문 끝이 곧 연결 종료다.</remarks>
enum class ResponseBodyMode
{
    None,
    FixedLength,
    Chunked,
    CloseDelimited
};

struct PreparedResponseHead
{
    std::string wire;
    ResponseBodyMode mode = ResponseBodyMode::None;
    std::uint64_t length = 0;
};

/// <param name="chunkedAllowed">
/// 거짓이면 길이 모르는 본문을 청크 대신 CloseDelimited로 두고 Connection: close를 붙인다.
/// HTTP/1.0 요청에는 Transfer-Encoding을 보내지 않는다(RFC 9112 §6.1).
/// </param>
SERVERCORE_TEST_API Core::Result<PreparedResponseHead> PrepareResponseHead(
    const HttpResponseHead& response, bool head, std::size_t maxHeaders,
    bool chunkedAllowed = true);
// Buffered responses share the same validation without copying their header list.
SERVERCORE_TEST_API Core::Result<PreparedResponseHead> PrepareResponseHead(unsigned int status,
    const HttpHeaders& headers, std::optional<std::uint64_t> length, bool head, bool close,
    std::size_t maxHeaders, bool chunkedAllowed = true);

// Empty writes are handled by the writer; only Finish emits the final zero chunk.
SERVERCORE_TEST_API Core::Result<std::string> EncodeChunk(
    std::span<const std::byte> bytes, std::size_t maxChunkBytes);
inline constexpr std::string_view FinalChunk = "0\r\n\r\n";
}
