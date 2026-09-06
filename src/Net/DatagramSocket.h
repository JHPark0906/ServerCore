#pragma once

#include "Net/WinsockInternal.h"

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
    sockaddr_in endpoint{};
    std::size_t bytes = 0;
    bool retryable = false;
};

class DatagramSocket final
{
public:
    DatagramSocket() = default;
    ~DatagramSocket();
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;

    [[nodiscard]] Core::Status Bind(std::string_view address, std::uint16_t port);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return mSocket != INVALID_SOCKET; }
    [[nodiscard]] std::uint16_t Port() const noexcept { return mPort; }
    // An empty datagram succeeds with zero bytes. Oversized datagrams are consumed as TooLarge.
    // Only connection-reset/refused receive errors are retryable platform failures.
    [[nodiscard]] DatagramReceiveResult Receive(std::span<std::byte> buffer) noexcept;
    [[nodiscard]] Core::Status Send(const sockaddr_in& endpoint,
        std::span<const std::byte> payload) noexcept;

private:
    std::shared_ptr<WinsockScope> mWinsock;
    SOCKET mSocket = INVALID_SOCKET;
    std::uint16_t mPort = 0;
};

// Empty output succeeds; spans exceeding ULONG's byte-count limit are rejected.
[[nodiscard]] Core::Status GenerateDatagramSecret(std::span<std::byte> bytes) noexcept;
}
