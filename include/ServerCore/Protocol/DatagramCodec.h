#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ServerCore::Protocol::DatagramCodec
{
// A datagram is independent of TCP FrameCodec: magic, opaque session token,
// big-endian sequence, then one application payload. No fragmentation/reassembly.
// 1200 application bytes leave room for IPv4/IPv6 and UDP headers on usual paths;
// this is a conservative limit, not a guarantee for every network MTU.
inline constexpr std::size_t HeaderBytes = 28;
inline constexpr std::size_t MaximumDatagramBytes = 1200;
inline constexpr std::size_t MaximumPayloadBytes = MaximumDatagramBytes - HeaderBytes;
using Token = std::array<std::byte, 16>;

struct PacketView
{
    Token token{};
    std::uint64_t sequence = 0;
    // Borrowed from Decode's input. Consume before that buffer is reused.
    std::span<const std::byte> payload;
};

// Tokens must be generated unpredictably by the application and delivered over
// its control connection. This codec does not encrypt/authenticate contents or
// protect against an observer who can read that connection.
[[nodiscard]] inline std::string TokenToHex(const Token& token)
{
    constexpr char Hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (std::size_t index = 0; index < token.size(); ++index)
    {
        const auto value = std::to_integer<unsigned>(token[index]);
        result[index * 2] = Hex[value >> 4];
        result[index * 2 + 1] = Hex[value & 15];
    }
    return result;
}

[[nodiscard]] inline std::optional<Token> TokenFromHex(const std::string_view text) noexcept
{
    if (text.size() != 32) return std::nullopt;
    const auto nibble = [](const char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    Token token{};
    for (std::size_t index = 0; index < token.size(); ++index)
    {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        token[index] = static_cast<std::byte>((high << 4) | low);
    }
    return token;
}

// Sequence zero is reserved; reconnect with a new token rather than wrapping.
// Returns zero without writing when the caller's bounds are invalid.
[[nodiscard]] inline std::size_t Encode(const std::span<std::byte> destination,
    const Token& token, const std::uint64_t sequence,
    const std::span<const std::byte> payload) noexcept
{
    if (sequence == 0 || payload.empty() || payload.size() > MaximumPayloadBytes ||
        destination.size() < HeaderBytes + payload.size()) return 0;
    destination[0] = std::byte{'S'};
    destination[1] = std::byte{'M'};
    destination[2] = std::byte{'U'};
    destination[3] = std::byte{'1'};
    std::copy(token.begin(), token.end(), destination.begin() + 4);
    for (std::size_t index = 0; index < 8; ++index)
        destination[20 + index] = static_cast<std::byte>((sequence >> ((7 - index) * 8)) & 255);
    std::copy(payload.begin(), payload.end(), destination.begin() + HeaderBytes);
    return HeaderBytes + payload.size();
}

[[nodiscard]] inline std::optional<PacketView> Decode(
    const std::span<const std::byte> datagram) noexcept
{
    if (datagram.size() <= HeaderBytes || datagram.size() > MaximumDatagramBytes ||
        datagram[0] != std::byte{'S'} || datagram[1] != std::byte{'M'} ||
        datagram[2] != std::byte{'U'} || datagram[3] != std::byte{'1'}) return std::nullopt;
    PacketView packet;
    std::copy_n(datagram.begin() + 4, packet.token.size(), packet.token.begin());
    for (std::size_t index = 0; index < 8; ++index)
        packet.sequence = (packet.sequence << 8) | std::to_integer<unsigned>(datagram[20 + index]);
    if (packet.sequence == 0) return std::nullopt;
    packet.payload = datagram.subspan(HeaderBytes);
    return packet;
}
}
