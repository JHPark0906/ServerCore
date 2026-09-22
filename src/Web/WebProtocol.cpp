#include "Web/WebProtocol.h"
#include "Web/ResponseEncoding.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <limits>

namespace ServerCore::Web
{
std::string_view HttpRequest::Path() const noexcept
{
    return std::string_view(target).substr(0, target.find('?'));
}

std::string_view HttpRequest::Header(std::string_view name) const noexcept
{
    for (const auto& field : headers)
    {
        if (Detail::EqualInsensitive(field.first, name)) return field.second;
    }
    return {};
}

std::string_view HttpRequest::PathParameter(std::string_view name) const noexcept
{
    for (const auto& parameter : pathParameters)
        if (parameter.first == name) return parameter.second;
    return {};
}

namespace Detail
{
namespace
{
char Lower(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

std::string_view Trim(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

bool TokenList(std::string_view value) noexcept
{
    while (true)
    {
        const std::size_t comma = value.find(',');
        if (!IsToken(Trim(value.substr(0, comma)))) return false;
        if (comma == std::string_view::npos) return true;
        value.remove_prefix(comma + 1);
    }
}

bool UpgradeProtocols(std::string_view value) noexcept
{
    while (true)
    {
        const auto comma = value.find(',');
        const auto protocol = Trim(value.substr(0, comma));
        const auto slash = protocol.find('/');
        if (!IsToken(protocol.substr(0, slash)) ||
            (slash != std::string_view::npos && !IsToken(protocol.substr(slash + 1)))) return false;
        if (comma == std::string_view::npos) return true;
        value.remove_prefix(comma + 1);
    }
}

bool ParseHeader(std::string_view line, std::string& name, std::string& value)
{
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || !IsToken(line.substr(0, colon))) return false;
    const auto fieldValue = Trim(line.substr(colon + 1));
    if (!ValidHeaderValue(fieldValue)) return false;
    name.assign(line.substr(0, colon));
    std::transform(name.begin(), name.end(), name.begin(), Lower);
    value.assign(fieldValue);
    return true;
}

bool ValidChunkExtensions(std::string_view value) noexcept
{
    // RFC 9112 chunk-ext: *( BWS ";" BWS token [ BWS "=" BWS (token / quoted-string) ] ).
    while (true)
    {
        value = Trim(value);
        if (value.empty()) return true;
        if (value.front() != ';') return false;
        value = Trim(value.substr(1));
        std::size_t count = 0;
        while (count < value.size() && IsToken(value.substr(count, 1))) ++count;
        if (count == 0) return false;
        value = Trim(value.substr(count));
        if (value.empty() || value.front() == ';') continue;
        if (value.front() != '=') return false;
        value = Trim(value.substr(1));
        if (value.empty()) return false;
        if (value.front() == '"')
        {
            value.remove_prefix(1);
            bool closed = false;
            while (!value.empty())
            {
                const auto ch = static_cast<unsigned char>(value.front());
                value.remove_prefix(1);
                if (ch == '"') { closed = true; break; }
                if (ch == '\\')
                {
                    if (value.empty()) return false;
                    const auto escaped = static_cast<unsigned char>(value.front());
                    if ((escaped < 32 && escaped != 9) || escaped == 127) return false;
                    value.remove_prefix(1);
                }
                else if ((ch < 32 && ch != 9) || ch == 127) return false;
            }
            if (!closed) return false;
        }
        else
        {
            count = 0;
            while (count < value.size() && IsToken(value.substr(count, 1))) ++count;
            if (count == 0) return false;
            value.remove_prefix(count);
        }
    }
}

bool ForbiddenTrailer(std::string_view name) noexcept
{
    constexpr std::array names{ "content-length", "transfer-encoding", "host", "connection",
        "upgrade", "trailer", "te", "authorization", "proxy-authorization", "cookie",
        "content-type", "content-encoding", "expect" };
    for (const auto* field : names) if (name == field) return true;
    return name.starts_with("sec-websocket-");
}

constexpr std::string_view Base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(std::span<const std::byte> input)
{
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3)
    {
        const auto first = std::to_integer<unsigned int>(input[offset]);
        const auto second = offset + 1 < input.size() ? std::to_integer<unsigned int>(input[offset + 1]) : 0u;
        const auto third = offset + 2 < input.size() ? std::to_integer<unsigned int>(input[offset + 2]) : 0u;
        result.push_back(Base64Alphabet[first >> 2]);
        result.push_back(Base64Alphabet[((first & 3u) << 4) | (second >> 4)]);
        result.push_back(offset + 1 < input.size() ? Base64Alphabet[((second & 15u) << 2) | (third >> 6)] : '=');
        result.push_back(offset + 2 < input.size() ? Base64Alphabet[third & 63u] : '=');
    }
    return result;
}

// SHA-1 is required only for RFC 6455's public handshake, never for passwords or authentication.
std::array<std::byte, 20> Sha1(std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(((text.size() + 72) / 64) * 64);
    for (char ch : text) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    bytes.push_back(std::byte{0x80});
    while (bytes.size() % 64 != 56) bytes.push_back(std::byte{0});
    const std::uint64_t bits = static_cast<std::uint64_t>(text.size()) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::byte>((bits >> shift) & 255u));
    std::array<std::uint32_t, 5> hash{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64)
    {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0; index < 16; ++index)
        {
            for (std::size_t byte = 0; byte < 4; ++byte)
                words[index] = (words[index] << 8) | std::to_integer<std::uint32_t>(bytes[offset + index * 4 + byte]);
        }
        for (std::size_t index = 16; index < 80; ++index)
            words[index] = std::rotl(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
        auto [a, b, c, d, e] = hash;
        for (std::size_t index = 0; index < 80; ++index)
        {
            std::uint32_t function = 0;
            std::uint32_t constant = 0;
            if (index < 20) { function = (b & c) | (~b & d); constant = 0x5a827999u; }
            else if (index < 40) { function = b ^ c ^ d; constant = 0x6ed9eba1u; }
            else if (index < 60) { function = (b & c) | (b & d) | (c & d); constant = 0x8f1bbcdcu; }
            else { function = b ^ c ^ d; constant = 0xca62c1d6u; }
            const std::uint32_t next = std::rotl(a, 5) + function + e + constant + words[index];
            e = d; d = c; c = std::rotl(b, 30); b = a; a = next;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d; hash[4] += e;
    }
    std::array<std::byte, 20> result{};
    for (std::size_t index = 0; index < hash.size(); ++index)
        for (std::size_t byte = 0; byte < 4; ++byte)
            result[index * 4 + byte] = static_cast<std::byte>((hash[index] >> ((3 - byte) * 8)) & 255u);
    return result;
}
}

bool EqualInsensitive(std::string_view left, std::string_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (Lower(left[index]) != Lower(right[index])) return false;
    return true;
}

bool IsToken(std::string_view value) noexcept
{
    if (value.empty()) return false;
    for (const char ch : value)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) continue;
        if (std::string_view("!#$%&'*+-.^_`|~").find(ch) == std::string_view::npos) return false;
    }
    return true;
}

bool ValidHeaderValue(std::string_view value) noexcept
{
    for (char ch : value)
    {
        const auto byte = static_cast<unsigned char>(ch);
        if ((byte < 32 && byte != 9) || byte == 127) return false;
    }
    return true;
}

bool ValidUpgradeProtocols(std::string_view value) noexcept
{
    return UpgradeProtocols(value);
}

bool HasToken(const HttpRequest& request, std::string_view header, std::string_view token)
{
    for (const auto& [name, field] : request.headers)
    {
        if (!EqualInsensitive(name, header)) continue;
        std::string_view remaining(field);
        while (true)
        {
            const auto comma = remaining.find(',');
            if (EqualInsensitive(Trim(remaining.substr(0, comma)), token)) return true;
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
        }
    }
    return false;
}

HttpParser::HttpParser(std::size_t maxHeaders, std::size_t maxBody, bool separateHeaders)
    : mMaxHeaders(maxHeaders), mMaxBody(maxBody), mSeparateHeaders(separateHeaders) {}

bool HttpParser::ConfigureBody(bool streaming, std::size_t maxBody, std::size_t maxChunk) noexcept
{
    mStreaming = streaming;
    mMaxBody = maxBody;
    mMaxChunk = maxChunk;
    return (streaming || maxChunk != 0) && mRemaining <= maxBody;
}

bool HttpParser::Started() const noexcept { return mStage != Stage::Headers; }
bool HttpParser::IsHeadRequest() const noexcept { return mHeadRequest; }

HttpParseResult HttpParser::Fail(unsigned int status)
{
    mStage = Stage::Failed;
    return {HttpParseKind::Error, status};
}

HttpParseResult HttpParser::Complete(HttpRequest& output)
{
    output = std::move(mRequest);
    mRequest = {};
    mStage = Stage::Headers;
    mRemaining = mChunkOverhead = mTrailerBytes = mHeaderSearch = 0;
    mContinue = false;
    mHeadRequest = false;
    mBodyBytes = 0;
    return {HttpParseKind::Complete, 200};
}

HttpParseResult HttpParser::ParseHeaders(std::string_view block)
{
    const auto lineEnd = block.find("\r\n");
    if (lineEnd == std::string_view::npos) return Fail(400);
    const auto line = block.substr(0, lineEnd);
    const auto firstSpace = line.find(' ');
    const auto secondSpace = firstSpace == std::string_view::npos ? firstSpace : line.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos ||
        line.find(' ', secondSpace + 1) != std::string_view::npos || !IsToken(line.substr(0, firstSpace))) return Fail(400);
    if (line.substr(secondSpace + 1) != "HTTP/1.1") return Fail(505);
    const auto target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    if (target.empty() || (target.front() != '/' && target != "*")) return Fail(400);
    for (char ch : target)
        if (static_cast<unsigned char>(ch) < 33 || static_cast<unsigned char>(ch) >= 127 || ch == '#') return Fail(400);
    mRequest.method.assign(line.substr(0, firstSpace));
    mRequest.target.assign(target);
    block.remove_prefix(lineEnd + 2);
    bool haveHost = false;
    bool haveLength = false;
    bool haveTransfer = false;
    bool haveExpect = false;
    unsigned int transferFailure = 0;
    while (!block.empty())
    {
        const auto end = block.find("\r\n");
        std::string name;
        std::string value;
        if (!ParseHeader(block.substr(0, end), name, value) || mRequest.headers.size() >= 128) return Fail(400);
        if (name == "host")
        {
            if (haveHost || value.empty() || value.find_first_of(" \t,/@?#\\") != std::string::npos) return Fail(400);
            haveHost = true;
        }
        else if (name == "content-length")
        {
            if (haveLength || value.empty()) return Fail(400);
            for (char ch : value) if (ch < '0' || ch > '9') return Fail(400);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), mRemaining);
            if (parsed.ec != std::errc{} || mRemaining > mMaxBody) return Fail(413);
            haveLength = true;
        }
        else if (name == "transfer-encoding")
        {
            if (haveTransfer || !TokenList(value)) return Fail(400);
            haveTransfer = true;
            if (!EqualInsensitive(value, "chunked"))
            {
                std::string_view codings(value);
                std::size_t chunkedCount = 0;
                bool lastChunked = false;
                while (true)
                {
                    const auto comma = codings.find(',');
                    lastChunked = EqualInsensitive(Trim(codings.substr(0, comma)), "chunked");
                    if (lastChunked) ++chunkedCount;
                    if (comma == std::string_view::npos) break;
                    codings.remove_prefix(comma + 1);
                }
                // Request framing requires exactly one final chunked coding.
                // Unsupported codings before it are distinct from ambiguous framing.
                transferFailure = chunkedCount == 1 && lastChunked ? 501u : 400u;
            }
        }
        else if (name == "connection")
        {
            if (!TokenList(value)) return Fail(400);
        }
        else if (name == "upgrade")
        {
            if (!UpgradeProtocols(value)) return Fail(400);
        }
        else if (name == "expect")
        {
            if (haveExpect || !EqualInsensitive(value, "100-continue")) return Fail(417);
            haveExpect = true;
        }
        mRequest.headers.emplace_back(std::move(name), std::move(value));
        if (end == std::string_view::npos) break;
        block.remove_prefix(end + 2);
    }
    if (!haveHost || (haveLength && haveTransfer)) return Fail(400);
    if (transferFailure != 0) return Fail(transferFailure);
    mStage = haveTransfer ? Stage::ChunkSize : Stage::FixedBody;
    mContinue = haveExpect && (haveTransfer || mRemaining != 0);
    return {};
}

HttpParseResult HttpParser::Parse(std::string& input, HttpRequest& output)
{
    while (true)
    {
        if (mStage == Stage::Failed) return {HttpParseKind::Error, 400};
        if (mStage == Stage::Headers)
        {
            // The method remains recognizable even when an oversized header or
            // unsupported version prevents construction of a complete request.
            mHeadRequest = input.starts_with("HEAD ");
            const auto end = input.find("\r\n\r\n", mHeaderSearch);
            if (end == std::string::npos)
            {
                if (input.size() >= mMaxHeaders) return Fail(431);
                mHeaderSearch = input.size() > 3 ? input.size() - 3 : 0;
                return {};
            }
            if (end + 4 > mMaxHeaders) return Fail(431);
            const auto result = ParseHeaders(std::string_view(input).substr(0, end + 2));
            if (result.kind == HttpParseKind::Error) return result;
            input.erase(0, end + 4);
            if (mSeparateHeaders)
            { output = std::move(mRequest); mRequest = {}; return {HttpParseKind::Headers, 200}; }
        }
        if (mContinue)
        {
            mContinue = false;
            return {HttpParseKind::Continue, 100};
        }
        if (mStage == Stage::FixedBody)
        {
            const auto count = (std::min)({input.size(), mRemaining, mStreaming ? mMaxChunk : mRemaining});
            mRequest.body.append(input, 0, count);
            input.erase(0, count);
            mRemaining -= count;
            mBodyBytes += count;
            if (mStreaming && count != 0)
            { output.body = std::move(mRequest.body); mRequest.body.clear(); return {HttpParseKind::Body, 200}; }
            if (mRemaining == 0) return Complete(output);
            return {};
        }
        if (mStage == Stage::ChunkSize)
        {
            const auto end = input.find("\r\n");
            if (end == std::string::npos)
            {
                if (input.size() > mMaxHeaders - mChunkOverhead) return Fail(431);
                return {};
            }
            if (end + 2 > mMaxHeaders - mChunkOverhead) return Fail(431);
            const auto line = std::string_view(input).substr(0, end);
            const auto hexEnd = line.find_first_not_of("0123456789abcdefABCDEF");
            const auto digits = line.substr(0, hexEnd);
            if (digits.empty() || (hexEnd != std::string_view::npos && !ValidChunkExtensions(line.substr(hexEnd)))) return Fail(400);
            const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), mRemaining, 16);
            if (result.ec != std::errc{} || mRemaining > mMaxBody - mBodyBytes) return Fail(413);
            mChunkOverhead += end + 2;
            input.erase(0, end + 2);
            mStage = mRemaining == 0 ? Stage::Trailers : Stage::ChunkData;
        }
        if (mStage == Stage::ChunkData)
        {
            const auto count = (std::min)({input.size(), mRemaining, mStreaming ? mMaxChunk : mRemaining});
            mRequest.body.append(input, 0, count);
            input.erase(0, count);
            mRemaining -= count;
            mBodyBytes += count;
            if (mStreaming && count != 0)
            {
                if (mRemaining == 0) mStage = Stage::ChunkEnd;
                output.body = std::move(mRequest.body); mRequest.body.clear();
                return {HttpParseKind::Body, 200};
            }
            if (mRemaining != 0) return {};
            mStage = Stage::ChunkEnd;
        }
        if (mStage == Stage::ChunkEnd)
        {
            if (input.size() < 2) return {};
            if (!input.starts_with("\r\n")) return Fail(400);
            if (mChunkOverhead > mMaxHeaders - 2) return Fail(431);
            mChunkOverhead += 2;
            input.erase(0, 2);
            if (mStreaming) mChunkOverhead = 0;
            mStage = Stage::ChunkSize;
            continue;
        }
        if (mStage == Stage::Trailers)
        {
            const auto end = input.find("\r\n");
            if (end == std::string::npos)
            {
                if (input.size() > mMaxHeaders - mTrailerBytes) return Fail(431);
                return {};
            }
            if (end + 2 > mMaxHeaders - mTrailerBytes) return Fail(431);
            mTrailerBytes += end + 2;
            if (end == 0)
            {
                input.erase(0, 2);
                return Complete(output);
            }
            std::string name;
            std::string value;
            if (!ParseHeader(std::string_view(input).substr(0, end), name, value) || ForbiddenTrailer(name)) return Fail(400);
            // Trailers cannot change the routing, framing or application request headers.
            input.erase(0, end + 2);
        }
    }
}

std::string_view ReasonPhrase(unsigned int status) noexcept
{
    switch (status)
    {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 426: return "Upgrade Required";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Response";
    }
}

std::string HttpDate(std::chrono::system_clock::time_point time)
{
    using namespace std::chrono;
    const auto seconds = floor<std::chrono::seconds>(time);
    const auto dayPoint = floor<days>(seconds);
    const year_month_day date{dayPoint};
    const weekday weekDay{dayPoint};
    const hh_mm_ss timeOfDay{seconds - dayPoint};
    constexpr std::array daysOfWeek{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr std::array months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::array<char, 30> text{};
    const int written = std::snprintf(text.data(), text.size(), "%s, %02u %s %04d %02lld:%02lld:%02lld GMT",
        daysOfWeek[weekDay.c_encoding()], static_cast<unsigned int>(date.day()),
        months[static_cast<unsigned int>(date.month()) - 1], static_cast<int>(date.year()),
        static_cast<long long>(timeOfDay.hours().count()), static_cast<long long>(timeOfDay.minutes().count()),
        static_cast<long long>(timeOfDay.seconds().count()));
    return written == 29 ? std::string(text.data(), 29) : std::string{};
}

bool SerializeResponse(const HttpResponse& response, bool head, bool close,
    std::size_t maxHeaders, std::size_t maxBody, std::string& output)
{
    if (response.body.size() > maxBody) return false;
    auto prepared = PrepareResponseHead(response.status, response.headers,
        static_cast<std::uint64_t>(response.body.size()), head, close, maxHeaders);
    if (!prepared.IsOk()) return false;
    output = std::move(prepared.Value().wire);
    if (prepared.Value().mode != ResponseBodyMode::None) output += response.body;
    return true;
}

bool WebSocketAccept(std::string_view key, std::string& accept)
{
    if (key.size() != 24 || key.substr(22) != "==") return false;
    for (std::size_t index = 0; index < 22; ++index)
        if (Base64Alphabet.find(key[index]) == std::string_view::npos) return false;
    if ((Base64Alphabet.find(key[21]) & 15u) != 0) return false;
    std::string text(key);
    text += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    accept = Base64(Sha1(text));
    return true;
}

bool ValidCloseCode(std::uint16_t code) noexcept
{
    return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) ||
        (code >= 3000 && code <= 4999);
}

FrameParseResult ParseClientFrame(std::span<const std::byte> input,
    std::size_t maxPayload, WebSocketFrame& frame)
{
    if (input.size() < 2) return {};
    const auto first = std::to_integer<unsigned int>(input[0]);
    const auto second = std::to_integer<unsigned int>(input[1]);
    const auto opcode = first & 15u;
    if ((first & 0x70u) != 0 || (second & 0x80u) == 0 ||
        (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10))
        return {FrameParseKind::Error, 0, 1002};
    const bool control = opcode >= 8;
    if (control && ((first & 0x80u) == 0 || (second & 127u) > 125)) return {FrameParseKind::Error, 0, 1002};
    std::uint64_t length = second & 127u;
    std::size_t header = 2;
    if (length == 126)
    {
        if (input.size() < 4) return {};
        length = (std::to_integer<std::uint64_t>(input[2]) << 8) | std::to_integer<std::uint64_t>(input[3]);
        if (length < 126) return {FrameParseKind::Error, 0, 1002};
        header = 4;
    }
    else if (length == 127)
    {
        if (input.size() < 10) return {};
        if ((std::to_integer<unsigned int>(input[2]) & 128u) != 0) return {FrameParseKind::Error, 0, 1002};
        length = 0;
        for (std::size_t index = 2; index < 10; ++index)
            length = (length << 8) | std::to_integer<std::uint64_t>(input[index]);
        if (length < 65536) return {FrameParseKind::Error, 0, 1002};
        header = 10;
    }
    if (!control && length > maxPayload) return {FrameParseKind::Error, 0, 1009};
    if (length > (std::numeric_limits<std::size_t>::max)() - header - 4) return {FrameParseKind::Error, 0, 1009};
    const auto payloadSize = static_cast<std::size_t>(length);
    if (input.size() < header + 4 + payloadSize) return {};
    frame.final = (first & 128u) != 0;
    frame.opcode = static_cast<std::uint8_t>(opcode);
    frame.payload.resize(payloadSize);
    for (std::size_t index = 0; index < payloadSize; ++index)
        frame.payload[index] = input[header + 4 + index] ^ input[header + (index % 4)];
    return {FrameParseKind::Complete, header + 4 + payloadSize, 1000};
}

std::vector<std::byte> EncodeServerFrame(std::uint8_t opcode, std::span<const std::byte> payload)
{
    std::vector<std::byte> result;
    result.reserve(payload.size() + 10);
    result.push_back(static_cast<std::byte>(0x80u | opcode));
    if (payload.size() < 126) result.push_back(static_cast<std::byte>(payload.size()));
    else if (payload.size() <= 65535)
    {
        result.push_back(std::byte{126});
        result.push_back(static_cast<std::byte>((payload.size() >> 8) & 255u));
        result.push_back(static_cast<std::byte>(payload.size() & 255u));
    }
    else
    {
        result.push_back(std::byte{127});
        const auto length = static_cast<std::uint64_t>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8) result.push_back(static_cast<std::byte>((length >> shift) & 255u));
    }
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
}
}
