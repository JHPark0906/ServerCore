#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Export.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ServerCore::Protocol
{
enum class ByteOrder
{
    BigEndian,
    LittleEndian
};
inline constexpr std::size_t MaximumBinaryBuffer = 64 * 1024 * 1024;
// Borrowed input. Every failed read leaves Position unchanged. No alignment or
// native representation is assumed; length prefixes are uint32 in the chosen order.
class BinaryReader
{
public:
    SERVERCORE_API static Core::Result<BinaryReader> Create(std::span<const std::byte> input,
        ByteOrder order = ByteOrder::BigEndian, std::size_t maximumBytes = 1024 * 1024) noexcept;
    SERVERCORE_API Core::Result<std::uint64_t> ReadUnsigned(std::size_t width) noexcept;
    SERVERCORE_API Core::Result<std::int64_t> ReadSigned(std::size_t width) noexcept;
    SERVERCORE_API Core::Result<bool> ReadBool() noexcept;
    SERVERCORE_API Core::Result<float> ReadFloat32() noexcept;
    SERVERCORE_API Core::Result<double> ReadFloat64() noexcept;
    SERVERCORE_API Core::Result<std::span<const std::byte>> ReadBytes(std::size_t length) noexcept;
    SERVERCORE_API Core::Result<std::span<const std::byte>> ReadLengthPrefixed(
        std::size_t maximumLength) noexcept;
    SERVERCORE_API Core::Result<std::string_view> ReadUtf8(std::size_t maximumLength) noexcept;
    SERVERCORE_API Core::Status Skip(std::size_t length) noexcept;
    std::size_t Position() const noexcept { return mPosition; }
    std::size_t Remaining() const noexcept { return mInput.size() - mPosition; }

private:
    BinaryReader(std::span<const std::byte> input, ByteOrder order)
        : mInput(input)
        , mOrder(order)
    {
    }
    std::span<const std::byte> mInput;
    ByteOrder mOrder;
    std::size_t mPosition = 0;
};
// Single-owner builder. Failures preserve bytes. Bytes() is invalidated by the
// next successful mutation; caller serialization is required for both classes.
class BinaryWriter
{
public:
    SERVERCORE_API static Core::Result<BinaryWriter> Create(
        std::size_t maximumBytes = 1024 * 1024, ByteOrder order = ByteOrder::BigEndian) noexcept;
    SERVERCORE_API Core::Status WriteUnsigned(std::uint64_t value, std::size_t width);
    SERVERCORE_API Core::Status WriteSigned(std::int64_t value, std::size_t width);
    Core::Status WriteBool(bool value) { return WriteUnsigned(value ? 1 : 0, 1); }
    SERVERCORE_API Core::Status WriteFloat32(float value);
    SERVERCORE_API Core::Status WriteFloat64(double value);
    SERVERCORE_API Core::Status WriteBytes(std::span<const std::byte> value);
    SERVERCORE_API Core::Status WriteLengthPrefixed(
        std::span<const std::byte> value, std::size_t maximumLength);
    SERVERCORE_API Core::Status WriteUtf8(std::string_view value, std::size_t maximumLength);
    std::span<const std::byte> Bytes() const noexcept { return mBytes; }
    void Clear() noexcept { mBytes.clear(); }

private:
    BinaryWriter(std::size_t limit, ByteOrder order)
        : mLimit(limit)
        , mOrder(order)
    {
    }
    Core::Status Append(std::span<const std::byte> value, bool prefix);
    std::vector<std::byte> mBytes;
    std::size_t mLimit;
    ByteOrder mOrder;
};
struct ProtocolOffer
{
    std::uint32_t version = 0;
    std::uint64_t features = 0;
};
// Explicit application hook, not a new wire handshake. Server order wins among
// versions with all required features. Lists contain 1..64 distinct nonzero versions.
// Incompatible offers return Unimplemented; existing envelopes are never changed.
SERVERCORE_API Core::Result<ProtocolOffer> NegotiateProtocol(std::span<const ProtocolOffer> server,
    std::span<const ProtocolOffer> peer, std::uint64_t requiredFeatures = 0) noexcept;
}
