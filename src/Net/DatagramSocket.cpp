#include "Net/DatagramSocket.h"
#include "Net/EndpointInternal.h"

#include <limits>
#include <string>
#include <utility>

#include <bcrypt.h>

namespace ServerCore::Net
{
namespace
{
using Core::ErrorCode;
using Core::Status;

class SocketOwner final
{
public:
    explicit SocketOwner(const SOCKET socket) noexcept : mSocket(socket) {}
    ~SocketOwner()
    {
        if (mSocket != INVALID_SOCKET) (void)::closesocket(mSocket);
    }

    SocketOwner(const SocketOwner&) = delete;
    SocketOwner& operator=(const SocketOwner&) = delete;

    [[nodiscard]] SOCKET Get() const noexcept { return mSocket; }
    [[nodiscard]] SOCKET Release() noexcept { return std::exchange(mSocket, INVALID_SOCKET); }

private:
    SOCKET mSocket;
};

[[nodiscard]] Status RandomFailure(const NTSTATUS error) noexcept
{
    try
    {
        return Status::Fail(ErrorCode::PlatformError,
            "BCryptGenRandom failed, NTSTATUS=" + std::to_string(error));
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}
}

DatagramSocket::~DatagramSocket()
{
    Close();
}

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

    try
    {
        auto winsock = AcquireWinsock();
        if (!winsock.IsOk()) return std::move(winsock).TakeStatus();

        // The socket closes before its local Winsock owner on every failed bind.
        SocketOwner socket(::socket(endpoint.Family(), SOCK_DGRAM, IPPROTO_UDP));
        if (socket.Get() == INVALID_SOCKET) return MakeSocketFailure("UDP socket", ::WSAGetLastError());
        if (!SetIpv6Only(socket.Get(),endpoint.Family(),ipv6Only))
            return MakeSocketFailure("UDP setsockopt(IPV6_V6ONLY)",::WSAGetLastError());

        const BOOL exclusive = TRUE;
        const int receiveBytes = 4 * 1024 * 1024;
        const int sendBytes = 1024 * 1024;
        u_long nonblocking = 1;
        if (::setsockopt(socket.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
            return MakeSocketFailure("UDP setsockopt(SO_EXCLUSIVEADDRUSE)", ::WSAGetLastError());
        if (::setsockopt(socket.Get(), SOL_SOCKET, SO_RCVBUF,
                reinterpret_cast<const char*>(&receiveBytes), sizeof(receiveBytes)) == SOCKET_ERROR)
            return MakeSocketFailure("UDP setsockopt(SO_RCVBUF)", ::WSAGetLastError());
        if (::setsockopt(socket.Get(), SOL_SOCKET, SO_SNDBUF,
                reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes)) == SOCKET_ERROR)
            return MakeSocketFailure("UDP setsockopt(SO_SNDBUF)", ::WSAGetLastError());
        if (::ioctlsocket(socket.Get(), FIONBIO, &nonblocking) == SOCKET_ERROR)
            return MakeSocketFailure("UDP ioctlsocket(FIONBIO)", ::WSAGetLastError());
        if (::bind(socket.Get(), endpoint.Address(), endpoint.length) == SOCKET_ERROR)
            return MakeSocketFailure("UDP bind", ::WSAGetLastError());

        int length = sizeof(endpoint.storage);
        if (::getsockname(socket.Get(), endpoint.Address(), &length) == SOCKET_ERROR)
            return MakeSocketFailure("UDP getsockname", ::WSAGetLastError());
        const auto local=FromNativeEndpoint(endpoint.storage,length);
        if (!local.IsValid() || local.port == 0)
            return Status::FailWithoutMessage(ErrorCode::PlatformError);

        auto readiness = DatagramReadiness::Create(static_cast<std::uintptr_t>(socket.Get()));
        if (!readiness.IsOk()) return std::move(readiness).TakeStatus();
        mReadiness = std::move(readiness.Value());
        mWinsock = std::move(winsock.Value());
        mSocket = socket.Release();
        mPort = local.port;
        mLocalEndpoint = local;
        mIpv6Only = ipv6Only;
        return Status::Ok();
    }
    catch (...)
    {
        return Status::AllocationFailure();
    }
}

void DatagramSocket::Close() noexcept
{
    if (mReadiness) mReadiness->Close();
    if (mSocket != INVALID_SOCKET) (void)::closesocket(mSocket);
    mSocket = INVALID_SOCKET;
    mPort = 0;
    mLocalEndpoint = {};
    mReadiness.reset();
    mWinsock.reset();
}

DatagramReceiveResult DatagramSocket::Receive(const std::span<std::byte> buffer) noexcept
{
    if (!IsOpen()) return {Status::FailWithoutMessage(ErrorCode::Closed)};
    if (buffer.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return {Status::FailWithoutMessage(ErrorCode::TooLarge)};

    sockaddr_storage endpoint{};
    int length = sizeof(endpoint);
    char emptyBuffer = 0;
    char* const data = buffer.empty() ? &emptyBuffer : reinterpret_cast<char*>(buffer.data());
    const int count = ::recvfrom(mSocket, data, static_cast<int>(buffer.size()), 0,
        reinterpret_cast<sockaddr*>(&endpoint), &length);
    if (count == SOCKET_ERROR)
    {
        const int error = ::WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return {Status::FailWithoutMessage(ErrorCode::WouldBlock)};
        if (error == WSAEMSGSIZE) return {Status::FailWithoutMessage(ErrorCode::TooLarge)};
        return {MakeSocketFailure("UDP recvfrom", error), {}, 0,
            error == WSAECONNRESET || error == WSAECONNREFUSED};
    }
    const auto remote=FromNativeEndpoint(endpoint,length);
    if (count < 0 || static_cast<std::size_t>(count) > buffer.size() || !remote.IsValid())
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

    const char emptyPayload = 0;
    const char* const data = payload.empty() ? &emptyPayload : reinterpret_cast<const char*>(payload.data());
    const int sent = ::sendto(mSocket, data, static_cast<int>(payload.size()), 0,
        endpoint.Address(), endpoint.length);
    if (sent == SOCKET_ERROR)
    {
        const int error = ::WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAENOBUFS)
            return Status::FailWithoutMessage(ErrorCode::WouldBlock);
        return MakeSocketFailure("UDP sendto", error);
    }
    if (sent < 0 || static_cast<std::size_t>(sent) != payload.size())
        return Status::FailWithoutMessage(ErrorCode::PlatformError);
    return Status::Ok();
}

Core::Status GenerateDatagramSecret(const std::span<std::byte> bytes) noexcept
{
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()))
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    if (bytes.empty()) return Status::Ok();

    const NTSTATUS result = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return result < 0 ? RandomFailure(result) : Status::Ok();
}
}
