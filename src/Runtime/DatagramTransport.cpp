#include "ServerCore/Runtime/DatagramTransport.h"

#include "Net/DatagramSocket.h"

#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace ServerCore::Runtime
{
namespace
{
namespace Codec = Protocol::DatagramCodec;
using Core::ErrorCode;
using Core::Result;
using Core::Status;
using Session::SessionId;

struct TokenHash
{
    std::size_t operator()(const Codec::Token& token) const noexcept
    {
        std::size_t value = static_cast<std::size_t>(1469598103934665603ull);
        for (const auto byte : token)
            value = (value ^ std::to_integer<unsigned>(byte)) *
                static_cast<std::size_t>(1099511628211ull);
        return value;
    }
};

void ConsumeBytes(std::size_t& consumed, const std::size_t count) noexcept
{
    const auto remaining = std::numeric_limits<std::size_t>::max() - consumed;
    consumed += count < remaining ? count : remaining;
}
}

struct DatagramTransport::Impl
{
    struct Peer
    {
        Codec::Token token{};
        sockaddr_in endpoint{};
        std::uint64_t registrationGeneration = 0;
        std::uint64_t receivedSequence = 0;
        std::uint64_t sentSequence = 0;
        bool ready = false;
    };

    mutable std::mutex mutex;
    Net::DatagramSocket socket;
    std::uint64_t bindingGeneration = 0;
    std::uint64_t registrationGeneration = 0;
    std::unordered_map<SessionId, Peer> peers;
    std::unordered_map<Codec::Token, SessionId, TokenHash> tokens;
    Metrics metrics;
};

DatagramTransport::DatagramTransport() : mImpl(std::make_unique<Impl>()) {}
DatagramTransport::~DatagramTransport() { Close(); }

Status DatagramTransport::Bind(const std::string_view address, const std::uint16_t port)
{
    const std::lock_guard guard(mImpl->mutex);
    if (mImpl->socket.IsOpen()) return Status::FailWithoutMessage(ErrorCode::AlreadyExists);
    if (mImpl->bindingGeneration == std::numeric_limits<std::uint64_t>::max())
        return Status::FailWithoutMessage(ErrorCode::Closed);
    auto status = mImpl->socket.Bind(address, port);
    if (status.IsOk()) ++mImpl->bindingGeneration;
    return status;
}

void DatagramTransport::Close() noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    mImpl->socket.Close();
    mImpl->tokens.clear();
    mImpl->peers.clear();
}

std::uint16_t DatagramTransport::Port() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    return mImpl->socket.Port();
}

Result<Codec::Token> DatagramTransport::RegisterSession(const SessionId id)
{
    try
    {
        const std::lock_guard guard(mImpl->mutex);
        if (!mImpl->socket.IsOpen())
            return Result<Codec::Token>::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
        if (!Session::IsValid(id) || mImpl->peers.contains(id))
            return Result<Codec::Token>::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        if (mImpl->registrationGeneration == std::numeric_limits<std::uint64_t>::max())
            return Result<Codec::Token>::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));

        Impl::Peer peer;
        do
        {
            auto status = Net::GenerateDatagramSecret(peer.token);
            if (!status.IsOk()) return Result<Codec::Token>::FromStatus(std::move(status));
        } while (mImpl->tokens.contains(peer.token));
        peer.registrationGeneration = mImpl->registrationGeneration + 1;

        mImpl->peers.emplace(id, peer);
        try
        {
            mImpl->tokens.emplace(peer.token, id);
        }
        catch (...)
        {
            mImpl->peers.erase(id);
            throw;
        }
        mImpl->registrationGeneration = peer.registrationGeneration;
        return Result<Codec::Token>::FromValue(peer.token);
    }
    catch (const std::bad_alloc&)
    {
        return Result<Codec::Token>::FromStatus(Status::AllocationFailure());
    }
}

void DatagramTransport::UnregisterSession(const SessionId id) noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto peer = mImpl->peers.find(id);
    if (peer == mImpl->peers.end()) return;
    mImpl->tokens.erase(peer->second.token);
    mImpl->peers.erase(peer);
}

bool DatagramTransport::IsReady(const SessionId id) const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto peer = mImpl->peers.find(id);
    return peer != mImpl->peers.end() && peer->second.ready;
}

Status DatagramTransport::Send(const SessionId id, const std::span<const std::byte> payload) noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto found = mImpl->peers.find(id);
    if (found == mImpl->peers.end()) return Status::FailWithoutMessage(ErrorCode::Closed);
    auto& peer = found->second;
    if (!peer.ready || !mImpl->socket.IsOpen())
        return Status::FailWithoutMessage(ErrorCode::WouldBlock);
    if (peer.sentSequence == std::numeric_limits<std::uint64_t>::max())
        return Status::FailWithoutMessage(ErrorCode::Closed);

    std::array<std::byte, Codec::MaximumDatagramBytes> bytes{};
    const auto count = Codec::Encode(bytes, peer.token, peer.sentSequence + 1, payload);
    if (count == 0) return Status::FailWithoutMessage(ErrorCode::TooLarge);
    auto status = mImpl->socket.Send(peer.endpoint, std::span(bytes).first(count));
    if (!status.IsOk())
    {
        if (status.Code() == ErrorCode::WouldBlock) ++mImpl->metrics.sendWouldBlock;
        else ++mImpl->metrics.socketErrors;
        return status;
    }

    ++peer.sentSequence;
    ++mImpl->metrics.sentDatagrams;
    mImpl->metrics.sentBytes += static_cast<std::uint64_t>(count);
    return Status::Ok();
}

void DatagramTransport::Poll(const Admission& admission, const Receiver& receiver,
    const DatagramPollBudget budget) noexcept
{
    if (!admission || !receiver || budget.maximumDatagrams == 0 || budget.maximumBytes == 0) return;

    std::uint64_t bindingGeneration = 0;
    {
        const std::lock_guard guard(mImpl->mutex);
        if (!mImpl->socket.IsOpen()) return;
        bindingGeneration = mImpl->bindingGeneration;
    }

    std::size_t consumed = 0;
    for (std::size_t attempt = 0; attempt < budget.maximumDatagrams && consumed < budget.maximumBytes;
        ++attempt)
    {
        try
        {
            std::array<std::byte, Codec::MaximumDatagramBytes + 1> bytes{};
            Codec::PacketView packet;
            sockaddr_in endpoint{};
            SessionId id = SessionId::Invalid;
            std::uint64_t registrationGeneration = 0;
            {
                const std::lock_guard guard(mImpl->mutex);
                if (!mImpl->socket.IsOpen() || mImpl->bindingGeneration != bindingGeneration) return;
                auto received = mImpl->socket.Receive(bytes);
                if (!received.status.IsOk())
                {
                    if (received.status.Code() == ErrorCode::WouldBlock) return;
                    if (received.status.Code() == ErrorCode::TooLarge)
                    {
                        ++mImpl->metrics.rejectedDatagrams;
                        ConsumeBytes(consumed, bytes.size());
                        continue;
                    }
                    ++mImpl->metrics.socketErrors;
                    if (received.retryable) continue;
                    return;
                }
                if (received.bytes > bytes.size())
                {
                    ++mImpl->metrics.socketErrors;
                    return;
                }
                ++mImpl->metrics.receivedDatagrams;
                mImpl->metrics.receivedBytes += static_cast<std::uint64_t>(received.bytes);
                ConsumeBytes(consumed, received.bytes);

                const auto decoded = Codec::Decode(std::span(bytes).first(received.bytes));
                if (!decoded || received.endpoint.sin_family != AF_INET)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    continue;
                }
                packet = *decoded;
                const auto token = mImpl->tokens.find(packet.token);
                if (token == mImpl->tokens.end())
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    continue;
                }
                const auto peer = mImpl->peers.find(token->second);
                if (peer == mImpl->peers.end() || packet.sequence <= peer->second.receivedSequence)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    continue;
                }
                id = token->second;
                registrationGeneration = peer->second.registrationGeneration;
                endpoint = received.endpoint;
            }

            auto message = Protocol::ParseMessage(packet.payload);
            if (!message.IsOk() || !admission(id, message.Value()))
            {
                const std::lock_guard guard(mImpl->mutex);
                ++mImpl->metrics.rejectedDatagrams;
                continue;
            }

            {
                const std::lock_guard guard(mImpl->mutex);
                if (!mImpl->socket.IsOpen() || mImpl->bindingGeneration != bindingGeneration)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    return;
                }
                const auto found = mImpl->peers.find(id);
                if (found == mImpl->peers.end() ||
                    found->second.registrationGeneration != registrationGeneration ||
                    found->second.token != packet.token || packet.sequence <= found->second.receivedSequence)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    continue;
                }
                auto& peer = found->second;
                peer.receivedSequence = packet.sequence;
                peer.endpoint = endpoint;
                peer.ready = true;
            }
            receiver(id, message.Value());
        }
        catch (...)
        {
            const std::lock_guard guard(mImpl->mutex);
            ++mImpl->metrics.rejectedDatagrams;
            return;
        }
    }
}

DatagramTransport::Metrics DatagramTransport::SnapshotMetrics() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    return mImpl->metrics;
}
}
