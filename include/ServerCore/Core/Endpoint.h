#pragma once

#include "ServerCore/Export.h"
#include "ServerCore/Core/Error.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ServerCore::Core
{
enum class IpFamily : std::uint8_t { Unspecified = 0, V4 = 4, V6 = 6 };
// Numeric addresses only: no DNS, brackets, whitespace or interface-name zones.
// IPv6 accepts a decimal %scope ID. IPv4-mapped IPv6 stays IPv6 until Normalized.
class IpAddress
{
public:
    SERVERCORE_API static Result<IpAddress> Parse(std::string_view text) noexcept;
    SERVERCORE_API static Result<IpAddress> FromBytes(IpFamily family, std::span<const std::uint8_t> bytes,
        std::uint32_t scopeId = 0) noexcept;
    [[nodiscard]] IpFamily Family() const noexcept { return mFamily; }
    [[nodiscard]] std::span<const std::uint8_t> Bytes() const noexcept
    { return {mBytes.data(), mFamily == IpFamily::V4 ? 4u : mFamily == IpFamily::V6 ? 16u : 0u}; }
    [[nodiscard]] std::uint32_t ScopeId() const noexcept { return mScopeId; }
    [[nodiscard]] bool IsValid() const noexcept { return mFamily != IpFamily::Unspecified; }
    [[nodiscard]] SERVERCORE_API bool IsV4Mapped() const noexcept;
    [[nodiscard]] SERVERCORE_API IpAddress Normalized() const noexcept;
    [[nodiscard]] SERVERCORE_API std::string ToString() const;
    friend bool operator==(const IpAddress&, const IpAddress&) = default;
private:
    friend class IpNetwork;
    IpFamily mFamily = IpFamily::Unspecified;
    std::array<std::uint8_t,16> mBytes{};
    std::uint32_t mScopeId = 0;
};
struct IpEndpoint
{
    IpAddress address;
    std::uint16_t port = 0;
    SERVERCORE_API static Result<IpEndpoint> Parse(std::string_view address, std::uint16_t port) noexcept;
    [[nodiscard]] bool IsValid() const noexcept { return address.IsValid(); }
    [[nodiscard]] SERVERCORE_API std::string ToString() const;
    friend bool operator==(const IpEndpoint&, const IpEndpoint&) = default;
};
// CIDR host bits are masked on parsing. Zones are not accepted in CIDR text.
// Contains compares address family and prefix bits, ignoring an address's scope
// ID. Normalize mapped addresses explicitly when defining a trust policy.
class IpNetwork
{
public:
    SERVERCORE_API static Result<IpNetwork> Parse(std::string_view cidr) noexcept;
    [[nodiscard]] SERVERCORE_API bool Contains(const IpAddress& address) const noexcept;
    [[nodiscard]] const IpAddress& Address() const noexcept { return mAddress; }
    [[nodiscard]] std::uint8_t PrefixLength() const noexcept { return mPrefix; }
private:
    IpAddress mAddress;
    std::uint8_t mPrefix = 0;
};
}
