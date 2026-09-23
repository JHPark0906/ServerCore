#include "Net/DatagramSocket.h"

#include "Net/EndpointInternal.h"
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
    const auto parsed=Core::IpEndpoint::Parse(address,port);
    return Bind(parsed.IsOk()?parsed.Value():Core::IpEndpoint{});
}
Core::Status DatagramSocket::Bind(const Core::IpEndpoint& binding,const bool ipv6Only)
{
    if (IsOpen()) return Status::FailWithoutMessage(ErrorCode::AlreadyExists);
    NativeEndpoint endpoint;
    if (!ToNativeEndpoint(binding,endpoint))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    const int descriptor = ::socket(endpoint.Family(), SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (descriptor < 0) return MakePosixFailure("UDP socket", errno);
    // No SO_REUSEADDR/SO_REUSEPORT: only one binding may own this UDP endpoint.
    const int receiveBytes = 4 * 1024 * 1024;
    const int sendBytes = 1024 * 1024;
    if (!SetIpv6Only(descriptor,endpoint.Family(),ipv6Only) ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &receiveBytes, sizeof(receiveBytes)) < 0 ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &sendBytes, sizeof(sendBytes)) < 0 ||
        ::bind(descriptor, endpoint.Address(), endpoint.length) < 0)
    {
        auto failure = MakePosixFailure("UDP bind", errno);
        ::close(descriptor);
        return failure;
    }
    socklen_t length = sizeof(endpoint.storage);
    if (::getsockname(descriptor, endpoint.Address(), &length) < 0)
    {
        auto failure = MakePosixFailure("UDP getsockname", errno);
        ::close(descriptor);
        return failure;
    }
    const auto local=FromNativeEndpoint(endpoint.storage,length);
    if (!local.IsValid() || local.port == 0)
    {
        ::close(descriptor);
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    }
    auto readiness = DatagramReadiness::Create(static_cast<std::uintptr_t>(descriptor));
    if (!readiness.IsOk()) { ::close(descriptor); return std::move(readiness).TakeStatus(); }
    mReadiness = std::move(readiness.Value());
    mSocket = descriptor;
    mPort = local.port;
    mLocalEndpoint = local;
    mIpv6Only = ipv6Only;
    return Status::Ok();
}

void DatagramSocket::Close() noexcept
{
    if (mReadiness) mReadiness->Close();
    if (mSocket >= 0) ::close(mSocket);
    mSocket = -1;
    mPort = 0;
    mLocalEndpoint = {};
    mReadiness.reset();
}

DatagramReceiveResult DatagramSocket::Receive(const std::span<std::byte> buffer) noexcept
{
    if (!IsOpen()) return {Status::FailWithoutMessage(ErrorCode::Closed)};
    if (buffer.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return {Status::FailWithoutMessage(ErrorCode::TooLarge)};
    sockaddr_storage endpoint{};
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
    const auto remote=FromNativeEndpoint(endpoint,message.msg_namelen);
    if (static_cast<std::size_t>(count) > buffer.size() || !remote.IsValid())
        return {Status::FailWithoutMessage(ErrorCode::PlatformError)};
    return {Status::Ok(), remote, static_cast<std::size_t>(count)};
}

Core::Status DatagramSocket::Send(const Core::IpEndpoint& destination,
    const std::span<const std::byte> payload) noexcept
{
    if (!IsOpen()) return Status::FailWithoutMessage(ErrorCode::Closed);
    NativeEndpoint endpoint;
    if (!ToNativeEndpoint(destination,endpoint) ||
        destination.address.Family()!=mLocalEndpoint.address.Family() ||
        (mIpv6Only&&destination.address.IsV4Mapped()))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    if (payload.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    const std::byte empty{};
    ssize_t count;
    do
    {
        count = ::sendto(mSocket, payload.empty() ? &empty : payload.data(), payload.size(), MSG_NOSIGNAL,
            endpoint.Address(), endpoint.length);
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
