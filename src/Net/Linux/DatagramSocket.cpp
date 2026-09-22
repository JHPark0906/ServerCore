#include "Net/DatagramSocket.h"

#include "Net/Ipv4EndpointInternal.h"
#include "Net/Linux/PosixInternal.h"

#include <limits>

#include <arpa/inet.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ServerCore::Net
{
namespace
{
using Core::ErrorCode;
using Core::Status;
}

DatagramSocket::~DatagramSocket() { Close(); }

Core::Status DatagramSocket::Bind(const std::string_view address, const std::uint16_t port)
{
    if (IsOpen()) return Status::FailWithoutMessage(ErrorCode::AlreadyExists);
    sockaddr_in endpoint{};
    if (!TryParseIpv4Endpoint(address, port, endpoint))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    const int descriptor = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (descriptor < 0) return MakePosixFailure("UDP socket", errno);
    // No SO_REUSEADDR/SO_REUSEPORT: only one binding may own this UDP endpoint.
    const int receiveBytes = 4 * 1024 * 1024;
    const int sendBytes = 1024 * 1024;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &receiveBytes, sizeof(receiveBytes)) < 0 ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &sendBytes, sizeof(sendBytes)) < 0 ||
        ::bind(descriptor, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0)
    {
        auto failure = MakePosixFailure("UDP bind", errno);
        ::close(descriptor);
        return failure;
    }
    socklen_t length = sizeof(endpoint);
    if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&endpoint), &length) < 0)
    {
        auto failure = MakePosixFailure("UDP getsockname", errno);
        ::close(descriptor);
        return failure;
    }
    if (length != sizeof(endpoint) || endpoint.sin_family != AF_INET || endpoint.sin_port == 0)
    {
        ::close(descriptor);
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    }
    mSocket = descriptor;
    mPort = ntohs(endpoint.sin_port);
    return Status::Ok();
}

void DatagramSocket::Close() noexcept
{
    if (mSocket >= 0) ::close(mSocket);
    mSocket = -1;
    mPort = 0;
}

DatagramReceiveResult DatagramSocket::Receive(const std::span<std::byte> buffer) noexcept
{
    if (!IsOpen()) return {Status::FailWithoutMessage(ErrorCode::Closed)};
    if (buffer.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return {Status::FailWithoutMessage(ErrorCode::TooLarge)};
    sockaddr_in endpoint{};
    std::byte empty{};
    iovec vector{buffer.empty() ? &empty : buffer.data(), buffer.size()};
    msghdr message{};
    message.msg_name = &endpoint;
    message.msg_namelen = sizeof(endpoint);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    ssize_t count;
    do { count = ::recvmsg(mSocket, &message, 0); } while (count < 0 && errno == EINTR);
    if (count < 0)
    {
        const int error = errno;
        if (IsWouldBlock(error)) return {Status::FailWithoutMessage(ErrorCode::WouldBlock)};
        return {MakePosixFailure("UDP recvmsg", error), {}, 0, error == ECONNRESET || error == ECONNREFUSED};
    }
    // recvfrom alone would silently accept a prefix. recvmsg reports MSG_TRUNC
    // after consuming the entire datagram, preserving the Windows error contract.
    if ((message.msg_flags & MSG_TRUNC) != 0)
        return {Status::FailWithoutMessage(ErrorCode::TooLarge)};
    if (static_cast<std::size_t>(count) > buffer.size() || message.msg_namelen != sizeof(endpoint) ||
        endpoint.sin_family != AF_INET)
        return {Status::FailWithoutMessage(ErrorCode::PlatformError)};
    return {Status::Ok(), endpoint, static_cast<std::size_t>(count)};
}

Core::Status DatagramSocket::Send(const sockaddr_in& endpoint,
    const std::span<const std::byte> payload) noexcept
{
    if (!IsOpen()) return Status::FailWithoutMessage(ErrorCode::Closed);
    if (endpoint.sin_family != AF_INET) return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    if (payload.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    const std::byte empty{};
    ssize_t count;
    do
    {
        count = ::sendto(mSocket, payload.empty() ? &empty : payload.data(), payload.size(), MSG_NOSIGNAL,
            reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
    {
        const int error = errno;
        if (IsWouldBlock(error) || error == ENOBUFS)
            return Status::FailWithoutMessage(ErrorCode::WouldBlock);
        return MakePosixFailure("UDP sendto", error);
    }
    if (static_cast<std::size_t>(count) != payload.size())
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    return Status::Ok();
}

Core::Status GenerateDatagramSecret(const std::span<std::byte> bytes) noexcept
{
    if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)())
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    std::size_t filled = 0;
    while (filled < bytes.size())
    {
        const ssize_t count = ::getrandom(bytes.data() + filled, bytes.size() - filled, 0);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return MakePosixFailure("getrandom", errno);
        }
        if (count == 0) return Status::FailWithoutMessage(ErrorCode::PlatformError);
        filled += static_cast<std::size_t>(count);
    }
    return Status::Ok();
}
}
