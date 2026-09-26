#pragma once
#include "ServerCore/Core/Endpoint.h"
#ifdef _WIN32
#include "Net/WinsockInternal.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#include <cstring>

namespace ServerCore::Net
{
#ifdef _WIN32
using SocketAddressLength = int;
using NativeSocket = SOCKET;
#else
using SocketAddressLength = socklen_t;
using NativeSocket = int;
#endif
struct NativeEndpoint
{
    sockaddr_storage storage{};
    SocketAddressLength length = 0;
    [[nodiscard]] const sockaddr* Address() const noexcept
    {
        return reinterpret_cast<const sockaddr*>(&storage);
    }
    [[nodiscard]] sockaddr* Address() noexcept { return reinterpret_cast<sockaddr*>(&storage); }
    [[nodiscard]] int Family() const noexcept { return storage.ss_family; }
};
inline bool ToNativeEndpoint(const Core::IpEndpoint& endpoint, NativeEndpoint& output) noexcept
{
    NativeEndpoint value;
    const auto& ip = endpoint.address;
    if (ip.Family() == Core::IpFamily::V4)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port);
        std::memcpy(&address.sin_addr, ip.Bytes().data(), 4);
        std::memcpy(&value.storage, &address, sizeof address);
        value.length = sizeof address;
    }
    else if (ip.Family() == Core::IpFamily::V6)
    {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(endpoint.port);
        address.sin6_scope_id = ip.ScopeId();
        std::memcpy(&address.sin6_addr, ip.Bytes().data(), 16);
        std::memcpy(&value.storage, &address, sizeof address);
        value.length = sizeof address;
    }
    else
        return false;
    output = value;
    return true;
}
inline Core::IpEndpoint FromNativeEndpoint(
    const sockaddr_storage& storage, SocketAddressLength length) noexcept
{
    if (storage.ss_family == AF_INET &&
        length >= static_cast<SocketAddressLength>(sizeof(sockaddr_in)))
    {
        sockaddr_in address{};
        std::memcpy(&address, &storage, sizeof address);
        auto ip = Core::IpAddress::FromBytes(
            Core::IpFamily::V4, { reinterpret_cast<const std::uint8_t*>(&address.sin_addr), 4 });
        return { ip.Value(), ntohs(address.sin_port) };
    }
    if (storage.ss_family == AF_INET6 &&
        length >= static_cast<SocketAddressLength>(sizeof(sockaddr_in6)))
    {
        sockaddr_in6 address{};
        std::memcpy(&address, &storage, sizeof address);
        auto ip = Core::IpAddress::FromBytes(Core::IpFamily::V6,
            { reinterpret_cast<const std::uint8_t*>(&address.sin6_addr), 16 },
            address.sin6_scope_id);
        return { ip.Value(), ntohs(address.sin6_port) };
    }
    return {};
}
inline Core::IpEndpoint ReadSocketEndpoint(NativeSocket socket, bool remote) noexcept
{
    sockaddr_storage storage{};
    SocketAddressLength length = sizeof storage;
    const auto result = remote
                            ? ::getpeername(socket, reinterpret_cast<sockaddr*>(&storage), &length)
                            : ::getsockname(socket, reinterpret_cast<sockaddr*>(&storage), &length);
    return result == 0 ? FromNativeEndpoint(storage, length) : Core::IpEndpoint{};
}
inline Core::IpEndpoint FromNativeEndpoint(
    const sockaddr* address, SocketAddressLength length) noexcept
{
    if (!address || length < static_cast<SocketAddressLength>(sizeof(address->sa_family)) ||
        length > static_cast<SocketAddressLength>(sizeof(sockaddr_storage)))
        return {};
    sockaddr_storage storage{};
    std::memcpy(&storage, address, static_cast<std::size_t>(length));
    return FromNativeEndpoint(storage, length);
}
inline bool SetIpv6Only(NativeSocket socket, int family, bool only) noexcept
{
    if (family != AF_INET6)
        return true;
    const int value = only ? 1 : 0;
    return ::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
#ifdef _WIN32
               reinterpret_cast<const char*>(&value),
#else
               &value,
#endif
               sizeof value) == 0;
}
}
