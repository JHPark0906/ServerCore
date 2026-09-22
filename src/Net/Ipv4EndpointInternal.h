#pragma once

#ifdef _WIN32
#include "Net/WinsockInternal.h"
#else
#include <arpa/inet.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ServerCore::Net
{
// TCP and UDP share numeric IPv4 syntax. Port policy belongs to each caller:
// TCP currently rejects zero, while UDP uses it for an ephemeral binding.
// Never pass a borrowed string_view directly to the OS's NUL-terminated parser.
[[nodiscard]] inline bool TryParseIpv4Endpoint(const std::string_view address,
    const std::uint16_t port, sockaddr_in& endpoint) noexcept
{
    std::array<char, INET_ADDRSTRLEN> text{};
    if (address.empty() || address.size() >= text.size() ||
        address.find('\0') != std::string_view::npos)
        return false;
    std::memcpy(text.data(), address.data(), address.size());

    sockaddr_in parsed{};
    parsed.sin_family = AF_INET;
    parsed.sin_port = htons(port);
    if (::inet_pton(AF_INET, text.data(), &parsed.sin_addr) != 1) return false;
    endpoint = parsed;
    return true;
}
}
