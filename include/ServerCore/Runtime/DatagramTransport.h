#pragma once
#include "ServerCore/Export.h"

#include "ServerCore/Core/CompletionSubscription.h"
#include "ServerCore/Core/Endpoint.h"
#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/BinaryMessage.h"
#include "ServerCore/Protocol/DatagramCodec.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Runtime/TaskExecutor.h"
#include "ServerCore/Session/Session.h"

#include <chrono>
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

struct DatagramTransportOptions
{
    std::size_t maxRegisteredSessions = 256;
    Protocol::PayloadMode payloadMode = Protocol::PayloadMode::Json;
    struct SendRate
    {
        // Both zero disables this bucket. Otherwise positive, burst >= 1200;
        // bytes count the entire DatagramCodec packet, excluding OS headers.
        std::uint64_t bytesPerInterval = 0, burstBytes = 0;
        std::chrono::milliseconds interval{ 1000 };
    };
    SendRate totalSendRate{}, peerSendRate{};
};

struct DatagramMessageView
{
    Session::SessionId session = Session::SessionId::Invalid;
    Core::IpEndpoint remoteEndpoint;
    std::uint64_t sequence = 0;
    Protocol::PayloadMode payloadMode = Protocol::PayloadMode::Json;
    // Validated JSON envelope or BinaryMessage envelope (including type bytes).
    // Borrowed only for the synchronous callback; no DatagramCodec header here.
    std::span<const std::byte> payload;
};
struct DatagramReceiveOptions
{
    DatagramPollBudget budget{};
    std::size_t retainedCallbackBytes = 0;
    std::stop_token cancellation{};
};

/// Nonblocking IPv4/IPv6 transport for token-addressed messages using DatagramCodec.
/// The reliable control channel owns session identity and distributes tokens securely.
/// Call UnregisterSession when that session ends. Tokens are bearer capabilities, not encryption.
/// Manual Poll owns no worker. Optional readiness uses one blocking monitor per
/// binding; StartReceiving adds one TimerScheduler and uses a supplied executor.
/// No retransmission, reliable delivery, encryption or automatic send queue.
/// Destruction requires an external control context, after receive/readiness
/// callbacks have returned. Close may be requested inside a callback, but that
/// callback must not destroy the owner or its last shared ownership reference.
class DatagramTransport final
{
public:
    using Admission = std::function<bool(Session::SessionId, const Protocol::Message&)>;
    using Receiver = std::function<void(Session::SessionId, const Protocol::Message&)>;
    using BinaryAdmission = std::function<bool(Session::SessionId, Protocol::BinaryMessageView)>;
    using BinaryReceiver = std::function<void(Session::SessionId, Protocol::BinaryMessageView)>;
    using DatagramAdmission = std::function<bool(const DatagramMessageView&)>;
    using DatagramReceiver = std::function<void(const DatagramMessageView&)>;

    struct Metrics
    {
        std::uint64_t receivedDatagrams = 0;
        std::uint64_t receivedBytes = 0;
        std::uint64_t sentDatagrams = 0;
        std::uint64_t sentBytes = 0;
        std::uint64_t rejectedDatagrams = 0;
        std::uint64_t sendWouldBlock = 0;
        std::uint64_t socketErrors = 0;
        std::uint64_t truncatedDatagrams = 0, malformedDatagrams = 0;
        std::uint64_t unknownTokenDatagrams = 0, replayedDatagrams = 0;
        std::uint64_t invalidPayloadDatagrams = 0, admissionRejectedDatagrams = 0;
        std::uint64_t staleDatagrams = 0, callbackFailures = 0;
        std::uint64_t sendRateLimited = 0, sendLimitedGlobal = 0, sendLimitedPeer = 0,
                      sendNotReady = 0;
        std::uint64_t pumpBatches = 0, pumpFailures = 0, pollContentions = 0;
        std::size_t registeredSessions = 0, pendingBatches = 0;
        bool bound = false, receiving = false;
    };

    SERVERCORE_API DatagramTransport();
    SERVERCORE_API ~DatagramTransport();
    DatagramTransport(const DatagramTransport&) = delete;
    DatagramTransport& operator=(const DatagramTransport&) = delete;

    /// Before the first successful Bind only. Positive registry cap <= 65,536; exhaustion returns WouldBlock.
    [[nodiscard]] SERVERCORE_API Core::Status Configure(const DatagramTransportOptions& options);

    /// Numeric IPv4/IPv6; port zero asks the OS for an ephemeral port. IPv6-only by default.
    [[nodiscard]] SERVERCORE_API Core::Status Bind(std::string_view address, std::uint16_t port);
    [[nodiscard]] SERVERCORE_API Core::Status Bind(const Core::IpEndpoint& endpoint, bool ipv6Only = true);
    [[nodiscard]] SERVERCORE_API Core::IpEndpoint LocalEndpoint() const noexcept;
    /// Last admitted datagram source; NotFound for unknown session, WouldBlock before first packet.
    [[nodiscard]] SERVERCORE_API Core::Result<Core::IpEndpoint> RemoteEndpoint(Session::SessionId id) const;
    /// One-shot advisory readiness, not packet ownership. At most 16 pending
    /// subscriptions per binding; cancellation and Reset never consume input.
    /// Callbacks run outside transport locks on the monitor/cancelling caller.
    /// Close wakes pending waits with Closed. Re-arm after consuming input.
    SERVERCORE_API Core::Result<Core::CompletionSubscription> WaitForReadable(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {});
    /// Optional serial receive pump. It retains executor ownership and schedules
    /// one bounded batch at a time. Executor pressure uses TimerScheduler's 1 ms
    /// bounded retry; no per-packet task queue. Do not call manual Poll meanwhile.
    /// Admission/receiver captures must fit the caller-declared retained bytes.
    SERVERCORE_API Core::Status StartReceiving(std::shared_ptr<TaskExecutor> executor, DatagramAdmission admission,
        DatagramReceiver receiver, const DatagramReceiveOptions& options = {});
    SERVERCORE_API void RequestStopReceiving() noexcept;
    /// Quiescence boundary; from the supplied executor/receive callback requests
    /// cancellation but returns InvalidArgument instead of waiting on itself.
    SERVERCORE_API Core::Status StopReceiving();
    /// Clears registrations; lifetime metrics are retained. Safe to repeat.
    SERVERCORE_API void Close() noexcept;
    [[nodiscard]] SERVERCORE_API std::uint16_t Port() const noexcept;
    [[nodiscard]] SERVERCORE_API Core::Result<Protocol::DatagramCodec::Token> RegisterSession(
        Session::SessionId id);
    SERVERCORE_API void UnregisterSession(Session::SessionId id) noexcept;
    [[nodiscard]] SERVERCORE_API bool IsReady(Session::SessionId id) const noexcept;
    [[nodiscard]] SERVERCORE_API std::size_t RegisteredSessionCount() const noexcept;
    [[nodiscard]] SERVERCORE_API Core::Result<Protocol::DatagramCodec::Token> GetToken(
        Session::SessionId id) const;
    [[nodiscard]] SERVERCORE_API Core::Status SendBinary(
        Session::SessionId id, std::uint32_t type, std::span<const std::byte> payload) noexcept;
    /// Binary mode only. Same token/replay/endpoint admission rules as Poll; views are callback-local.
    SERVERCORE_API void PollBinary(const BinaryAdmission& admission, const BinaryReceiver& receiver,
        DatagramPollBudget budget = {}) noexcept;

    /// Sends an already serialized UTF-8 JSON envelope without queuing or parsing it again.
    /// Payload excludes the TCP length prefix and UDP header; this transport adds its own header.
    /// Use Protocol::SerializeMessage or PreparedMessage::Bytes to construct the payload.
    /// Ok means OS acceptance, not delivery. Sequence advances only on success and never wraps.
    /// A platform failure does not unregister the session or close its reliable control channel.
    [[nodiscard]] SERVERCORE_API Core::Status SendSerialized(
        Session::SessionId id, std::span<const std::byte> payload) noexcept;

    /// Compatibility alias; preserves the same status, size and sequence rules.
    [[deprecated("Use SendSerialized(id, payload); payload must already contain a serialized JSON "
                 "envelope")]] [[nodiscard]] SERVERCORE_API Core::Status
    Send(Session::SessionId id, std::span<const std::byte> payload) noexcept;

    /// Decode, token and replay checks precede one JSON parse. Admission runs before committing
    /// the sequence, endpoint and ready state; a false result cannot rebind or consume a sequence.
    /// Receiver runs after commit. Both callbacks run synchronously without the state mutex and
    /// may call SendSerialized, UnregisterSession or Close. Message references last only for the callback.
    /// Binding lifetime, registration identity and sequence are rechecked after admission.
    /// Rebinding the socket ends the current Poll without consuming packets from the new binding.
    /// Serialize Poll calls on the application's execution context; other methods are thread safe.
    /// Empty callbacks consume no packets. Callback exceptions are counted as rejection and end Poll.
    /// Admission exceptions leave the packet uncommitted; Receiver exceptions do not undo its commit.
    /// The byte budget is checked before each receive and may be exceeded by the final datagram.
    SERVERCORE_API void Poll(const Admission& admission, const Receiver& receiver,
        DatagramPollBudget budget = {}) noexcept;
    /// Same codec/token/replay/admission pipeline, with verified raw envelopes.
    SERVERCORE_API void PollDatagrams(const DatagramAdmission& admission, const DatagramReceiver& receiver,
        DatagramPollBudget budget = {}) noexcept;
    [[nodiscard]] SERVERCORE_API Metrics SnapshotMetrics() const noexcept;

private:
    enum class PayloadDecision
    {
        Allow,
        InvalidPayload,
        Reject
    };
    using PayloadAdmission = std::function<PayloadDecision(const DatagramMessageView&)>;
    using PayloadReceiver = DatagramReceiver;
    SERVERCORE_API void PollPayload(const PayloadAdmission&, const PayloadReceiver&, DatagramPollBudget) noexcept;
    [[nodiscard]] SERVERCORE_API Core::Status SendPayload(Session::SessionId, std::span<const std::byte>) noexcept;
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
}
