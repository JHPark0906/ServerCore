#pragma once
#include "ServerCore/C/Types.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace AbiTest
{
inline sc_bytes Bytes(std::string_view value)
{
    return { reinterpret_cast<const uint8_t*>(value.data()), value.size() };
}
inline std::string Text(sc_bytes value)
{
    return value.len ? std::string(reinterpret_cast<const char*>(value.data), value.len)
                     : std::string{};
}
template <class T> T View()
{
    T value{};
    value.abi_version = SC_ABI_VERSION;
    value.struct_size = sizeof(value);
    return value;
}
// decltype(Destroy)를 쓰지 않는다. MSVC는 noexcept 함수를 받은 이 매개변수의 decltype을 함수
// 타입으로 내어 unique_ptr가 컴파일되지 않는다.
template <class T, void (*Destroy)(T*)> using Handle = std::unique_ptr<T, void (*)(T*)>;
inline bool Check(bool condition, std::string_view message)
{
    ServerCoreTest::ExpectTrue(condition, message);
    return condition;
}
inline std::uint16_t FreePort()
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == ServerCoreTest::InvalidSocket)
        return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ServerCoreTest::SocketLength length = sizeof(address);
    const bool valid =
        ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ServerCoreTest::CloseSocket(socket);
    return valid ? ntohs(address.sin_port) : 0;
}
class Peer
{
public:
    ~Peer() { Close(); }
    Peer() = default;
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    bool Connect(std::uint16_t port)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!Check(socket != ServerCoreTest::InvalidSocket, "C ABI raw peer opens"))
            return false;
        if (!Check(ServerCoreTest::SetSocketTimeouts(socket, 5000), "C ABI peer timeouts install"))
            return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        return Check(
            ::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "C ABI raw peer connects");
    }
    void Close()
    {
        if (socket != ServerCoreTest::InvalidSocket)
            ServerCoreTest::CloseSocket(std::exchange(socket, ServerCoreTest::InvalidSocket));
    }
    bool Send(std::string_view bytes)
    {
        while (!bytes.empty())
        {
            const auto count =
                ServerCoreTest::Send(socket, bytes.data(), static_cast<int>(bytes.size()));
            if (!Check(count > 0, "C ABI peer writes complete bytes"))
                return false;
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }
    std::string Read(std::size_t count)
    {
        if (!Check(count <= 1024 * 1024, "C ABI peer read allocation is bounded"))
            return {};
        std::string bytes(count, '\0');
        std::size_t offset = 0;
        while (offset < count)
        {
            const auto received = ServerCoreTest::Receive(
                socket, bytes.data() + offset, static_cast<int>(count - offset));
            if (!Check(received > 0, "C ABI peer reads the complete expected payload"))
            {
                bytes.resize(offset);
                return bytes;
            }
            offset += static_cast<std::size_t>(received);
        }
        return bytes;
    }
    std::string Head()
    {
        std::string bytes;
        while (!bytes.ends_with("\r\n\r\n") && bytes.size() < 65536)
        {
            const auto part = Read(1);
            if (part.empty())
                break;
            bytes += part;
        }
        return bytes;
    }
    std::string UntilClosed(bool* closed = nullptr)
    {
        std::string bytes;
        std::array<char, 1024> buffer{};
        if (closed)
            *closed = false;
        while (bytes.size() < 1024 * 1024)
        {
            const auto count =
                ServerCoreTest::Receive(socket, buffer.data(), static_cast<int>(buffer.size()));
            if (count <= 0)
            {
                const auto error = count < 0 ? ServerCoreTest::LastSocketError() : 0;
                const bool terminal = count == 0 ||
#ifdef _WIN32
                                      error == WSAECONNRESET || error == WSAECONNABORTED ||
                                      error == WSAENOTCONN;
#else
                                      error == ECONNRESET || error == ENOTCONN;
#endif
                if (closed)
                    *closed = terminal;
                return bytes;
            }
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return bytes;
    }
    bool Masked(unsigned opcode, std::string_view payload)
    {
        if (!Check(payload.size() < 126, "C ABI fixture uses bounded short WebSocket frames"))
            return false;
        std::string wire;
        wire += static_cast<char>(0x80u | opcode);
        wire += static_cast<char>(0x80u | payload.size());
        const std::array<unsigned char, 4> mask{ 0x21, 0x43, 0x65, 0x87 };
        for (const auto value : mask)
            wire += static_cast<char>(value);
        for (std::size_t index = 0; index < payload.size(); ++index)
            wire += static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % 4]);
        return Send(wire);
    }
    std::pair<unsigned, std::string> Frame()
    {
        const auto head = Read(2);
        if (head.size() != 2)
            return {};
        const auto first = static_cast<unsigned char>(head[0]);
        const auto second = static_cast<unsigned char>(head[1]);
        std::size_t length = second & 127u;
        if (!Check((second & 128u) == 0, "server WebSocket frames are unmasked"))
            return {};
        if (length == 126)
        {
            const auto extended = Read(2);
            if (extended.size() != 2)
                return {};
            length = static_cast<unsigned char>(extended[0]) * 256u +
                     static_cast<unsigned char>(extended[1]);
        }
        if (!Check(length != 127, "C ABI fixture frame fits its bounded short reader"))
            return {};
        return { first & 15u, Read(length) };
    }

private:
    ServerCoreTest::Socket socket = ServerCoreTest::InvalidSocket;
};
inline std::string Http(
    std::string_view method, std::string_view target, std::string_view body = {})
{
    return std::string(method) + " " + std::string(target) +
           " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}
inline std::string Upgrade(std::string_view path)
{
    return "GET " + std::string(path) +
           " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
}
}
