#pragma once

#include <cstddef>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
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
        if (mReady) ::WSACleanup();
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

[[nodiscard]] inline bool SetSocketTimeouts(const Socket socket, const unsigned milliseconds) noexcept
{
#ifdef _WIN32
    const DWORD timeout = milliseconds;
#else
    const timeval timeout{ static_cast<time_t>(milliseconds / 1000u),
        static_cast<suseconds_t>((milliseconds % 1000u) * 1000u) };
#endif
    return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
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
        received = ::recvfrom(socket, data, static_cast<std::size_t>(size), flags,
            endpoint, endpointLength);
    } while (received == -1 && errno == EINTR);
    return static_cast<int>(received);
#endif
}
}
