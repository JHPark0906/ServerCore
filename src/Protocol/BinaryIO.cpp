#include "ServerCore/Protocol/BinaryIO.h"
#include "Core/Utf8Internal.h"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <new>

namespace ServerCore::Protocol
{
namespace
{
using Core::ErrorCode;
Core::Status Fail(ErrorCode code) noexcept
{
    return Core::Status::FailWithoutMessage(code);
}
template <class T> Core::Result<T> Error(ErrorCode code) noexcept
{
    return Core::Result<T>::FromStatus(Fail(code));
}
bool Width(std::size_t width) noexcept
{
    return width == 1 || width == 2 || width == 4 || width == 8;
}
bool Order(ByteOrder order) noexcept
{
    return order == ByteOrder::BigEndian || order == ByteOrder::LittleEndian;
}
void Store(std::byte* dest, std::uint64_t value, std::size_t width, ByteOrder order) noexcept
{
    for (std::size_t i = 0; i < width; ++i)
        dest[i] =
            std::byte((value >> (8 * (order == ByteOrder::BigEndian ? width - i - 1 : i))) & 255);
}
}
Core::Result<BinaryReader> BinaryReader::Create(
    std::span<const std::byte> input, ByteOrder order, std::size_t maximumBytes) noexcept
{
    if (!Order(order) || !maximumBytes || maximumBytes > MaximumBinaryBuffer)
        return Error<BinaryReader>(ErrorCode::InvalidArgument);
    if (input.size() > maximumBytes)
        return Error<BinaryReader>(ErrorCode::TooLarge);
    return Core::Result<BinaryReader>::FromValue(BinaryReader(input, order));
}
Core::Result<std::uint64_t> BinaryReader::ReadUnsigned(std::size_t width) noexcept
{
    if (!Width(width))
        return Error<std::uint64_t>(ErrorCode::InvalidArgument);
    if (width > Remaining())
        return Error<std::uint64_t>(ErrorCode::InvalidFormat);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value |= std::to_integer<std::uint64_t>(mInput[mPosition + i])
                 << (8 * (mOrder == ByteOrder::BigEndian ? width - i - 1 : i));
    mPosition += width;
    return Core::Result<std::uint64_t>::FromValue(value);
}
Core::Result<std::int64_t> BinaryReader::ReadSigned(std::size_t width) noexcept
{
    auto result = ReadUnsigned(width);
    if (!result.IsOk())
        return Core::Result<std::int64_t>::FromStatus(std::move(result).TakeStatus());
    auto bits = result.Value();
    if (width < 8 && (bits & (std::uint64_t{ 1 } << (width * 8 - 1))))
        bits |= ~((std::uint64_t{ 1 } << (width * 8)) - 1);
    return Core::Result<std::int64_t>::FromValue(std::bit_cast<std::int64_t>(bits));
}
Core::Result<bool> BinaryReader::ReadBool() noexcept
{
    if (!Remaining() || std::to_integer<unsigned>(mInput[mPosition]) > 1)
        return Error<bool>(ErrorCode::InvalidFormat);
    return Core::Result<bool>::FromValue(mInput[mPosition++] == std::byte{ 1 });
}
Core::Result<float> BinaryReader::ReadFloat32() noexcept
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    auto result = ReadUnsigned(4);
    if (!result.IsOk())
        return Core::Result<float>::FromStatus(std::move(result).TakeStatus());
    return Core::Result<float>::FromValue(
        std::bit_cast<float>(static_cast<std::uint32_t>(result.Value())));
}
Core::Result<double> BinaryReader::ReadFloat64() noexcept
{
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    auto result = ReadUnsigned(8);
    if (!result.IsOk())
        return Core::Result<double>::FromStatus(std::move(result).TakeStatus());
    return Core::Result<double>::FromValue(std::bit_cast<double>(result.Value()));
}
Core::Result<std::span<const std::byte>> BinaryReader::ReadBytes(std::size_t length) noexcept
{
    if (length > Remaining())
        return Error<std::span<const std::byte>>(ErrorCode::InvalidFormat);
    auto bytes = mInput.subspan(mPosition, length);
    mPosition += length;
    return Core::Result<std::span<const std::byte>>::FromValue(bytes);
}
Core::Result<std::span<const std::byte>> BinaryReader::ReadLengthPrefixed(
    std::size_t maximumLength) noexcept
{
    auto saved = *this;
    auto length = saved.ReadUnsigned(4);
    if (!length.IsOk())
        return Error<std::span<const std::byte>>(length.GetStatus().Code());
    if (length.Value() > maximumLength)
        return Error<std::span<const std::byte>>(ErrorCode::TooLarge);
    auto bytes = saved.ReadBytes(static_cast<std::size_t>(length.Value()));
    if (bytes.IsOk())
        *this = saved;
    return bytes;
}
Core::Result<std::string_view> BinaryReader::ReadUtf8(std::size_t maximumLength) noexcept
{
    auto saved = *this;
    auto bytes = saved.ReadLengthPrefixed(maximumLength);
    if (!bytes.IsOk())
        return Error<std::string_view>(bytes.GetStatus().Code());
    auto view = bytes.Value().empty()
                    ? std::string_view{}
                    : std::string_view(reinterpret_cast<const char*>(bytes.Value().data()),
                          bytes.Value().size());
    if (!Core::Detail::IsValidUtf8(view))
        return Error<std::string_view>(ErrorCode::InvalidFormat);
    *this = saved;
    return Core::Result<std::string_view>::FromValue(view);
}
Core::Status BinaryReader::Skip(std::size_t length) noexcept
{
    if (length > Remaining())
        return Fail(ErrorCode::InvalidFormat);
    mPosition += length;
    return Core::Status::Ok();
}
Core::Result<BinaryWriter> BinaryWriter::Create(std::size_t maximumBytes, ByteOrder order) noexcept
{
    if (!Order(order) || !maximumBytes || maximumBytes > MaximumBinaryBuffer)
        return Error<BinaryWriter>(ErrorCode::InvalidArgument);
    return Core::Result<BinaryWriter>::FromValue(BinaryWriter(maximumBytes, order));
}
Core::Status BinaryWriter::Append(std::span<const std::byte> value, bool prefix)
{
    const std::size_t header = prefix ? 4 : 0;
    if (header > mLimit - mBytes.size() || value.size() > mLimit - mBytes.size() - header)
        return Fail(ErrorCode::TooLarge);
    // Rebind a self-view after vector growth; external valid spans are unchanged.
    const auto source = reinterpret_cast<std::uintptr_t>(value.data());
    const auto base = reinterpret_cast<std::uintptr_t>(mBytes.data());
    const bool aliases = !value.empty() && source >= base && source - base < mBytes.size();
    const auto offset = aliases ? static_cast<std::size_t>(source - base) : 0;
    if (aliases && value.size() > mBytes.size() - offset)
        return Fail(ErrorCode::InvalidArgument);
    const auto old = mBytes.size();
    try
    {
        mBytes.resize(old + header + value.size());
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    if (prefix)
        Store(mBytes.data() + old, value.size(), 4, mOrder);
    if (!value.empty())
        std::memmove(mBytes.data() + old + header, aliases ? mBytes.data() + offset : value.data(),
            value.size());
    return Core::Status::Ok();
}
Core::Status BinaryWriter::WriteUnsigned(std::uint64_t value, std::size_t width)
{
    if (!Width(width) || (width < 8 && value >= (std::uint64_t{ 1 } << (width * 8))))
        return Fail(ErrorCode::InvalidArgument);
    std::array<std::byte, 8> bytes{};
    Store(bytes.data(), value, width, mOrder);
    return Append(std::span(bytes).first(width), false);
}
Core::Status BinaryWriter::WriteSigned(std::int64_t value, std::size_t width)
{
    if (!Width(width))
        return Fail(ErrorCode::InvalidArgument);
    if (width < 8)
    {
        auto upper = std::int64_t{ 1 } << (width * 8 - 1);
        if (value < -upper || value >= upper)
            return Fail(ErrorCode::InvalidArgument);
    }
    std::array<std::byte, 8> bytes{};
    Store(bytes.data(), static_cast<std::uint64_t>(value), width, mOrder);
    return Append(std::span(bytes).first(width), false);
}
Core::Status BinaryWriter::WriteFloat32(float value)
{
    return WriteUnsigned(std::bit_cast<std::uint32_t>(value), 4);
}
Core::Status BinaryWriter::WriteFloat64(double value)
{
    return WriteUnsigned(std::bit_cast<std::uint64_t>(value), 8);
}
Core::Status BinaryWriter::WriteBytes(std::span<const std::byte> value)
{
    return Append(value, false);
}
Core::Status BinaryWriter::WriteLengthPrefixed(
    std::span<const std::byte> value, std::size_t maximumLength)
{
    if (value.size() > maximumLength || value.size() > UINT32_MAX)
        return Fail(ErrorCode::TooLarge);
    return Append(value, true);
}
Core::Status BinaryWriter::WriteUtf8(std::string_view value, std::size_t maximumLength)
{
    if (value.size() > maximumLength)
        return Fail(ErrorCode::TooLarge);
    if (!Core::Detail::IsValidUtf8(value))
        return Fail(ErrorCode::InvalidArgument);
    return WriteLengthPrefixed(std::as_bytes(std::span(value.data(), value.size())), maximumLength);
}
Core::Result<ProtocolOffer> NegotiateProtocol(std::span<const ProtocolOffer> server,
    std::span<const ProtocolOffer> peer, std::uint64_t requiredFeatures) noexcept
{
    auto valid = [](std::span<const ProtocolOffer> offers)
    {
        if (offers.empty() || offers.size() > 64)
            return false;
        for (std::size_t i = 0; i < offers.size(); ++i)
        {
            if (!offers[i].version)
                return false;
            for (std::size_t j = 0; j < i; ++j)
                if (offers[j].version == offers[i].version)
                    return false;
        }
        return true;
    };
    if (!valid(server) || !valid(peer))
        return Error<ProtocolOffer>(ErrorCode::InvalidArgument);
    for (const auto& local : server)
        for (const auto& remote : peer)
            if (local.version == remote.version &&
                ((local.features & remote.features) & requiredFeatures) == requiredFeatures)
                return Core::Result<ProtocolOffer>::FromValue(
                    { local.version, local.features & remote.features });
    return Error<ProtocolOffer>(ErrorCode::Unimplemented);
}
}
