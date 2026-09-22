#pragma once

#include "ServerCore/Web/HttpServer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ServerCore::Web::Detail
{
[[nodiscard]] bool EqualInsensitive(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool IsToken(std::string_view value) noexcept;
[[nodiscard]] bool HasToken(const HttpRequest& request, std::string_view header, std::string_view token);
[[nodiscard]] bool ValidHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool ValidUpgradeProtocols(std::string_view value) noexcept;

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
    HttpParser(std::size_t maxHeaders, std::size_t maxBody, bool separateHeaders = false);
    HttpParseResult Parse(std::string& input, HttpRequest& output);
    bool ConfigureBody(bool streaming, std::size_t maxBody, std::size_t maxChunk) noexcept;
    bool TakeContinue() noexcept { const auto value = mContinue; mContinue = false; return value; }
    [[nodiscard]] bool Started() const noexcept;
    // Preserved on failure so generated HEAD errors also omit their content.
    [[nodiscard]] bool IsHeadRequest() const noexcept;
private:
    enum class Stage { Headers, FixedBody, ChunkSize, ChunkData, ChunkEnd, Trailers, Failed };
    HttpParseResult ParseHeaders(std::string_view block);
    HttpParseResult Fail(unsigned int status);
    HttpParseResult Complete(HttpRequest& output);
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
bool SerializeResponse(const HttpResponse& response, bool head, bool close,
    std::size_t maxHeaders, std::size_t maxBody, std::string& output);
[[nodiscard]] std::string_view ReasonPhrase(unsigned int status) noexcept;
[[nodiscard]] std::string HttpDate(std::chrono::system_clock::time_point time);
[[nodiscard]] bool WebSocketAccept(std::string_view key, std::string& accept);
[[nodiscard]] bool ValidCloseCode(std::uint16_t code) noexcept;

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
FrameParseResult ParseClientFrame(std::span<const std::byte> input,
    std::size_t maxPayload, WebSocketFrame& frame);
[[nodiscard]] std::vector<std::byte> EncodeServerFrame(
    std::uint8_t opcode, std::span<const std::byte> payload);
}
