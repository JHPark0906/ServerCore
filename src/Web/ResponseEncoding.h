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
enum class ResponseBodyMode { None, FixedLength, Chunked };

struct PreparedResponseHead
{
    std::string wire;
    ResponseBodyMode mode = ResponseBodyMode::None;
    std::uint64_t length = 0;
};

SERVERCORE_TEST_API Core::Result<PreparedResponseHead> PrepareResponseHead(const HttpResponseHead& response,
    bool head, std::size_t maxHeaders);
// Buffered responses share the same validation without copying their header list.
SERVERCORE_TEST_API Core::Result<PreparedResponseHead> PrepareResponseHead(unsigned int status,
    const HttpHeaders& headers, std::optional<std::uint64_t> length,
    bool head, bool close, std::size_t maxHeaders);

// Empty writes are handled by the writer; only Finish emits the final zero chunk.
SERVERCORE_TEST_API Core::Result<std::string> EncodeChunk(std::span<const std::byte> bytes, std::size_t maxChunkBytes);
inline constexpr std::string_view FinalChunk = "0\r\n\r\n";
}
