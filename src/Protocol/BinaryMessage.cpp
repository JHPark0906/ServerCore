#include "ServerCore/Protocol/BinaryMessage.h"

#include <algorithm>
#include <new>

namespace ServerCore::Protocol
{
Core::Result<BinaryMessageView> DecodeBinaryMessage(const std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < BinaryMessageHeaderSize)
        return Core::Result<BinaryMessageView>::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidFormat));
    std::uint32_t type = 0;
    for (unsigned i = 0; i != 4; ++i)
        type |= std::to_integer<std::uint32_t>(bytes[i]) << (i * 8);
    if (type == 0)
        return Core::Result<BinaryMessageView>::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidFormat));
    return Core::Result<BinaryMessageView>::FromValue(
        { type, bytes.subspan(BinaryMessageHeaderSize) });
}

Core::Result<std::vector<std::byte>> EncodeBinaryMessage(const std::uint32_t type,
    const std::span<const std::byte> payload, const std::size_t maximumBytes)
{
    using Result = Core::Result<std::vector<std::byte>>;
    if (type == 0)
        return Result::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
    if (maximumBytes < BinaryMessageHeaderSize ||
        payload.size() > maximumBytes - BinaryMessageHeaderSize)
        return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::TooLarge));
    try
    {
        std::vector<std::byte> bytes(BinaryMessageHeaderSize + payload.size());
        for (unsigned i = 0; i != 4; ++i)
            bytes[i] = static_cast<std::byte>((type >> (i * 8)) & 0xffu);
        std::copy(payload.begin(), payload.end(), bytes.begin() + BinaryMessageHeaderSize);
        return Result::FromValue(std::move(bytes));
    }
    catch (const std::bad_alloc&)
    {
        return Result::FromStatus(Core::Status::AllocationFailure());
    }
}
}
