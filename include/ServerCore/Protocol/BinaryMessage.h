#pragma once

#include "ServerCore/Core/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ServerCore::Protocol
{
/// Explicit endpoint protocol selection; there is no content sniffing or automatic downgrade.
enum class PayloadMode { Json, Binary };

/// Binary envelope: four little-endian type bytes followed by uninterpreted application bytes.
/// Type zero is reserved. Views remain valid only while their source storage remains alive.
struct BinaryMessageView
{
    std::uint32_t type = 0;
    std::span<const std::byte> payload;
};

inline constexpr std::size_t BinaryMessageHeaderSize = 4;

[[nodiscard]] Core::Result<BinaryMessageView> DecodeBinaryMessage(
    std::span<const std::byte> bytes) noexcept;
[[nodiscard]] Core::Result<std::vector<std::byte>> EncodeBinaryMessage(
    std::uint32_t type, std::span<const std::byte> payload, std::size_t maximumBytes);
}
