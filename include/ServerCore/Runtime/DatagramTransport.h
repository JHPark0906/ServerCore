#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/DatagramCodec.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Session/Session.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace ServerCore::Runtime
{
/// Per-call receive work limits; rejected packets and socket errors also consume attempts.
struct DatagramPollBudget
{
    std::size_t maximumDatagrams = 4096;
    std::size_t maximumBytes = 1024 * 1024;
};

/// Nonblocking IPv4 transport for token-addressed JSON messages using DatagramCodec.
/// The reliable control channel owns session identity and distributes tokens securely.
/// Call UnregisterSession when that session ends. Tokens are bearer capabilities, not encryption.
/// No threads, retransmission queues, game message types, or scheduling policy are owned here.
class DatagramTransport final
{
public:
    using Admission = std::function<bool(Session::SessionId, const Protocol::Message&)>;
    using Receiver = std::function<void(Session::SessionId, const Protocol::Message&)>;

    struct Metrics
    {
        std::uint64_t receivedDatagrams = 0;
        std::uint64_t receivedBytes = 0;
        std::uint64_t sentDatagrams = 0;
        std::uint64_t sentBytes = 0;
        std::uint64_t rejectedDatagrams = 0;
        std::uint64_t sendWouldBlock = 0;
        std::uint64_t socketErrors = 0;
    };

    DatagramTransport();
    ~DatagramTransport();
    DatagramTransport(const DatagramTransport&) = delete;
    DatagramTransport& operator=(const DatagramTransport&) = delete;

    /// Numeric IPv4 only; port zero asks the OS for an ephemeral port.
    [[nodiscard]] Core::Status Bind(std::string_view address, std::uint16_t port);
    /// Clears registrations; lifetime metrics are retained. Safe to repeat.
    void Close() noexcept;
    [[nodiscard]] std::uint16_t Port() const noexcept;
    [[nodiscard]] Core::Result<Protocol::DatagramCodec::Token> RegisterSession(Session::SessionId id);
    void UnregisterSession(Session::SessionId id) noexcept;
    [[nodiscard]] bool IsReady(Session::SessionId id) const noexcept;

    /// Sends an already serialized payload without copying into a queue or parsing it again.
    /// Ok means OS acceptance, not delivery. Sequence advances only on success and never wraps.
    /// A platform failure does not unregister the session or close its reliable control channel.
    [[nodiscard]] Core::Status Send(Session::SessionId id, std::span<const std::byte> payload) noexcept;

    /// Decode, token and replay checks precede one JSON parse. Admission runs before committing
    /// the sequence, endpoint and ready state; a false result cannot rebind or consume a sequence.
    /// Receiver runs after commit. Both callbacks run synchronously without the state mutex and
    /// may call Send, UnregisterSession or Close. Message references last only for the callback.
    /// Binding lifetime, registration identity and sequence are rechecked after admission.
    /// Rebinding the socket ends the current Poll without consuming packets from the new binding.
    /// Serialize Poll calls on the application's execution context; other methods are thread safe.
    /// Empty callbacks consume no packets. Callback exceptions are counted as rejection and end Poll.
    /// Admission exceptions leave the packet uncommitted; Receiver exceptions do not undo its commit.
    /// The byte budget is checked before each receive and may be exceeded by the final datagram.
    void Poll(const Admission& admission, const Receiver& receiver,
        DatagramPollBudget budget = {}) noexcept;
    [[nodiscard]] Metrics SnapshotMetrics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
}
