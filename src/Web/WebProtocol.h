#pragma once
#include "ServerCore/Export.h"

#include "ServerCore/Web/HttpServer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ServerCore::Web::Detail
{
[[nodiscard]] SERVERCORE_TEST_API bool EqualInsensitive(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] SERVERCORE_TEST_API bool IsToken(std::string_view value) noexcept;
[[nodiscard]] SERVERCORE_TEST_API bool HasToken(const HttpRequest& request, std::string_view header, std::string_view token);
[[nodiscard]] SERVERCORE_TEST_API bool ValidHeaderValue(std::string_view value) noexcept;
[[nodiscard]] SERVERCORE_TEST_API bool ValidUpgradeProtocols(std::string_view value) noexcept;

enum class HttpParseKind { NeedMore, Continue, Complete, Error, Headers, Body };
struct HttpParseResult
{
    HttpParseKind kind = HttpParseKind::NeedMore;
    unsigned int status = 400;
};

// Consumes processed bytes from input and retains only the decoded request.
class HttpParser
{
public:
    SERVERCORE_TEST_API HttpParser(std::size_t maxHeaders, std::size_t maxBody, bool separateHeaders = false);
    SERVERCORE_TEST_API HttpParseResult Parse(std::string& input, HttpRequest& output);
    SERVERCORE_TEST_API bool ConfigureBody(bool streaming, std::size_t maxBody, std::size_t maxChunk) noexcept;
    bool TakeContinue() noexcept { const auto value = mContinue; mContinue = false; return value; }
    [[nodiscard]] SERVERCORE_TEST_API bool Started() const noexcept;
    // Preserved on failure so generated HEAD errors also omit their content.
    [[nodiscard]] SERVERCORE_TEST_API bool IsHeadRequest() const noexcept;
private:
    enum class Stage { Headers, FixedBody, ChunkSize, ChunkData, ChunkEnd, Trailers, Failed };
    SERVERCORE_TEST_API HttpParseResult ParseHeaders(std::string_view block);
    SERVERCORE_TEST_API HttpParseResult Fail(unsigned int status);
    SERVERCORE_TEST_API HttpParseResult Complete(HttpRequest& output);
    Stage mStage = Stage::Headers;
    std::size_t mMaxHeaders;
    std::size_t mMaxBody;
    std::size_t mRemaining = 0;
    std::size_t mChunkOverhead = 0;
    std::size_t mTrailerBytes = 0;
    std::size_t mHeaderSearch = 0;
    bool mContinue = false;
    bool mHeadRequest = false;
    bool mSeparateHeaders = false, mStreaming = false;
    std::size_t mBodyBytes = 0, mMaxChunk = 0;
    HttpRequest mRequest;
};

// Returns false for an invalid response or response framing fields supplied by
// the application. Framing is exclusively owned by the server.
SERVERCORE_TEST_API bool SerializeResponse(const HttpResponse& response, bool head, bool close,
    std::size_t maxHeaders, std::size_t maxBody, std::string& output);
[[nodiscard]] SERVERCORE_TEST_API std::string_view ReasonPhrase(unsigned int status) noexcept;
[[nodiscard]] SERVERCORE_TEST_API std::string HttpDate(std::chrono::system_clock::time_point time);
[[nodiscard]] SERVERCORE_TEST_API bool WebSocketAccept(std::string_view key, std::string& accept);
[[nodiscard]] SERVERCORE_TEST_API bool ValidCloseCode(std::uint16_t code) noexcept;
SERVERCORE_TEST_API bool SelectWebSocketSubprotocol(const HttpRequest& request,
    const std::vector<std::string>& supported, std::string& selected);
// Retains at most one incomplete UTF-8 code point, reusing Core validation.
struct Utf8FragmentState
{
    char pending[4]{};
    std::size_t size = 0;
    SERVERCORE_TEST_API bool Append(std::span<const std::byte> bytes, bool final) noexcept;
};

enum class FrameParseKind { NeedMore, Complete, Error };
struct WebSocketFrame
{
    bool final = true;
    std::uint8_t opcode = 0;
    std::vector<std::byte> payload;
};
struct FrameParseResult
{
    FrameParseKind kind = FrameParseKind::NeedMore;
    std::size_t consumed = 0;
    std::uint16_t closeCode = 1002;
};
SERVERCORE_TEST_API FrameParseResult ParseClientFrame(std::span<const std::byte> input,
    std::size_t maxPayload, WebSocketFrame& frame);
[[nodiscard]] SERVERCORE_TEST_API std::vector<std::byte> EncodeServerFrame(
    std::uint8_t opcode, std::span<const std::byte> payload, bool final = true);
}
