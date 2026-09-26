#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

// Raw peer sockets deliberately do not use ServerCore's implementation: an
// integration test must exercise the operating system's actual socket boundary.
namespace ServerCoreTest
{
#ifdef _WIN32
using Socket = SOCKET;
using SocketLength = int;
inline constexpr Socket InvalidSocket = INVALID_SOCKET;
inline constexpr int SocketError = SOCKET_ERROR;
inline constexpr int ShutdownBoth = SD_BOTH;
#else
using Socket = int;
using SocketLength = socklen_t;
inline constexpr Socket InvalidSocket = -1;
inline constexpr int SocketError = -1;
inline constexpr int ShutdownBoth = SHUT_RDWR;
#endif

class SocketRuntime final
{
public:
    SocketRuntime() noexcept
    {
#ifdef _WIN32
        WSADATA data{};
        mReady = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }

    ~SocketRuntime()
    {
#ifdef _WIN32
        if (mReady)
            ::WSACleanup();
#endif
    }

    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
    [[nodiscard]] bool IsReady() const noexcept { return mReady; }

private:
    bool mReady = true;
};

inline void CloseSocket(const Socket socket) noexcept
{
#ifdef _WIN32
    (void)::closesocket(socket);
#else
    // Retrying close after EINTR can close a reused descriptor on Linux.
    (void)::close(socket);
#endif
}

[[nodiscard]] inline int LastSocketError() noexcept
{
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

[[nodiscard]] inline bool SocketWouldBlock(const int error) noexcept
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

[[nodiscard]] inline bool SetSocketTimeouts(
    const Socket socket, const unsigned milliseconds) noexcept
{
#ifdef _WIN32
    const DWORD timeout = milliseconds;
#else
    const timeval timeout{ static_cast<time_t>(milliseconds / 1000u),
        static_cast<suseconds_t>((milliseconds % 1000u) * 1000u) };
#endif
    return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout)) == 0 &&
           ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout)) == 0;
}

[[nodiscard]] inline bool SetSocketNonblocking(const Socket socket) noexcept
{
#ifdef _WIN32
    u_long enabled = 1;
    return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags != -1 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

[[nodiscard]] inline int Send(
    const Socket socket, const char* data, const int size, const int flags = 0) noexcept
{
#ifdef _WIN32
    return ::send(socket, data, size, flags);
#else
    ssize_t sent;
    do
    {
        sent = ::send(socket, data, static_cast<std::size_t>(size), flags | MSG_NOSIGNAL);
    } while (sent == -1 && errno == EINTR);
    return static_cast<int>(sent);
#endif
}

[[nodiscard]] inline int Receive(
    const Socket socket, char* data, const int size, const int flags = 0) noexcept
{
#ifdef _WIN32
    return ::recv(socket, data, size, flags);
#else
    ssize_t received;
    do
    {
        received = ::recv(socket, data, static_cast<std::size_t>(size), flags);
    } while (received == -1 && errno == EINTR);
    return static_cast<int>(received);
#endif
}

[[nodiscard]] inline int SendTo(const Socket socket, const char* data, const int size,
    const int flags, const sockaddr* endpoint, const SocketLength endpointLength) noexcept
{
#ifdef _WIN32
    return ::sendto(socket, data, size, flags, endpoint, endpointLength);
#else
    ssize_t sent;
    do
    {
        sent = ::sendto(socket, data, static_cast<std::size_t>(size), flags | MSG_NOSIGNAL,
            endpoint, endpointLength);
    } while (sent == -1 && errno == EINTR);
    return static_cast<int>(sent);
#endif
}

[[nodiscard]] inline int ReceiveFrom(const Socket socket, char* data, const int size,
    const int flags, sockaddr* endpoint, SocketLength* endpointLength) noexcept
{
#ifdef _WIN32
    return ::recvfrom(socket, data, size, flags, endpoint, endpointLength);
#else
    ssize_t received;
    do
    {
        received = ::recvfrom(
            socket, data, static_cast<std::size_t>(size), flags, endpoint, endpointLength);
    } while (received == -1 && errno == EINTR);
    return static_cast<int>(received);
#endif
}

/// <summary>::1에 UDP 소켓을 묶을 수 있는지 본다.</summary>
/// <remarks>IPv6가 없는 컨테이너에서는 socket이나 bind가 실패한다. 그런 환경에서 IPv6 검사는
/// 실패하지도 조용히 통과하지도 않고 ServerCoreTest::Skip으로 건너뛴다(BUILD-4).</remarks>
[[nodiscard]] inline bool Ipv6LoopbackAvailable() noexcept
{
    const SocketRuntime runtime;
    if (!runtime.IsReady())
        return false;
    const Socket probe = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == InvalidSocket)
        return false;
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    const bool bound =
        ::inet_pton(AF_INET6, "::1", &address.sin6_addr) == 1 &&
        ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    CloseSocket(probe);
    return bound;
}

/// <summary>운영체제에게 127.0.0.1의 서로 다른 빈 TCP 포트 count개를 받아 돌려준다. 실패하면 빈 목록이다.</summary>
/// <remarks>
/// Acceptor와 ServerHost는 포트 0("정하지 않음")을 받지 않는다(Transport.ListenRejectsInvalidEndpoint,
/// Runtime.ServerHostRejectsInvalidOptions가 고정). 그래서 시험은 임시 소켓을 0에 묶어 운영체제가 준 번호를
/// 읽고 닫은 뒤 그 번호로 연다. 임시 소켓을 모두 묶어 둔 채 번호를 읽으므로 한 번에 받은 포트끼리는 서로
/// 다르다. 닫은 뒤 다시 열기까지의 짧은 틈에 다른 프로세스가 같은 번호를 가져갈 수는 있다(TOCTOU).
/// 그래도 고정 블록과 달리 여러 빌드 트리가 동시에 시험을 돌려도 결정적으로 부딪히지는 않는다.
/// </remarks>
[[nodiscard]] inline std::vector<std::uint16_t> FreeLoopbackTcpPorts(const std::size_t count)
{
    const SocketRuntime runtime;
    std::vector<Socket> probes;
    std::vector<std::uint16_t> ports;
    bool ok = runtime.IsReady();
    for (std::size_t index = 0; ok && index < count; ++index)
    {
        const Socket probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe == InvalidSocket)
        {
            ok = false;
            break;
        }
        probes.push_back(probe);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        SocketLength length = sizeof address;
        ok = ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0 &&
             ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0;
        if (ok)
            ports.push_back(ntohs(address.sin_port));
    }
    for (const Socket probe : probes)
        CloseSocket(probe);
    if (!ok)
        ports.clear();
    return ports;
}

/// <summary>FreeLoopbackTcpPorts(1)의 첫 번호다. 실패하면 0이고, 0은 Listen·Configure가 거절한다.</summary>
[[nodiscard]] inline std::uint16_t FreeLoopbackTcpPort()
{
    const auto ports = FreeLoopbackTcpPorts(1);
    return ports.empty() ? std::uint16_t{ 0 } : ports.front();
}
}
