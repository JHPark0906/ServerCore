#pragma once

#include "Net/DatagramReadinessInternal.h"
#include "ServerCore/Core/Endpoint.h"
#include "ServerCore/Export.h"

#ifdef _WIN32
#include "Net/WinsockInternal.h"
#else
#include "ServerCore/Core/Error.h"
#include <netinet/in.h>
#endif

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ServerCore::Net
{
// Private platform transport. The owning runtime serializes all access.
struct DatagramReceiveResult
{
    Core::Status status;
    Core::IpEndpoint endpoint{};
    std::size_t bytes = 0;
    bool retryable = false;
};

class DatagramSocket final
{
public:
    DatagramSocket() = default;
    SERVERCORE_TEST_API ~DatagramSocket();
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;

    [[nodiscard]] SERVERCORE_TEST_API Core::Status Bind(
        std::string_view address, std::uint16_t port);
    [[nodiscard]] SERVERCORE_TEST_API Core::Status Bind(
        const Core::IpEndpoint& endpoint, bool ipv6Only = true);
    SERVERCORE_TEST_API void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept
    {
#ifdef _WIN32
        return mSocket != INVALID_SOCKET;
#else
        return mSocket >= 0;
#endif
    }
    [[nodiscard]] std::uint16_t Port() const noexcept { return mPort; }
    [[nodiscard]] Core::IpEndpoint LocalEndpoint() const noexcept { return mLocalEndpoint; }
    [[nodiscard]] std::shared_ptr<DatagramReadiness> Readiness() const noexcept
    {
        return mReadiness;
    }
    // An empty datagram succeeds with zero bytes. Oversized datagrams are consumed as TooLarge.
    // Only connection-reset/refused receive errors are retryable platform failures.
    [[nodiscard]] SERVERCORE_TEST_API DatagramReceiveResult Receive(
        std::span<std::byte> buffer) noexcept;
    [[nodiscard]] SERVERCORE_TEST_API Core::Status Send(
        const Core::IpEndpoint& endpoint, std::span<const std::byte> payload) noexcept;

private:
#ifdef _WIN32
    std::shared_ptr<WinsockScope> mWinsock;
    SOCKET mSocket = INVALID_SOCKET;
#else
    int mSocket = -1;
#endif
    std::uint16_t mPort = 0;
    Core::IpEndpoint mLocalEndpoint;
    bool mIpv6Only = true;
    std::shared_ptr<DatagramReadiness> mReadiness;
};

// Empty output succeeds; spans exceeding a 32-bit byte count are rejected.
[[nodiscard]] SERVERCORE_TEST_API Core::Status GenerateDatagramSecret(
    std::span<std::byte> bytes) noexcept;
}
