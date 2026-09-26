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
    /// <summary>
    /// 반환 엔드포인트가 이미 정해진 세션에 다른 출발지에서 토큰·시퀀스가 맞는 패킷이 오면 반환
    /// 엔드포인트를 그 출발지로 옮길지 정한다. 기본값 false에서는 그 패킷을 거절하고
    /// endpointMismatchDatagrams로 센다(Runtime.DatagramEndpointMigrationIsOptIn).
    /// </summary>
    /// <remarks>
    /// 이 전송은 경로 검증(nonce echo)을 하지 않고 토큰은 MAC 없이 평문으로 오간다. 켜면 토큰을 아는
    /// 쪽이 출발지를 위조한 패킷 하나로 이 세션의 송신을 제3자에게 돌릴 수 있다(반사·증폭). NAT
    /// 재바인딩을 견뎌야 할 때만 켜고, 송신률 제한(peerSendRate)과 admission의 remoteEndpoint 검사를
    /// 함께 쓴다. 끈 상태에서도 첫 유효 패킷의 출발지가 반환 엔드포인트가 된다.
    /// </remarks>
    bool allowEndpointMigration = false;
    /// <summary>
    /// 패킷 하나가 수신 시퀀스를 앞으로 옮길 수 있는 최대 폭. 1 이상이어야 한다. 넘는 패킷은
    /// 거절하고 수신 시퀀스를 바꾸지 않으며 sequenceJumpDatagrams로 센다
    /// (Runtime.DatagramSequenceJumpIsBounded).
    /// </summary>
    /// <remarks>
    /// 정당한 클라이언트도 이 수보다 많은 패킷을 연달아 잃으면 재등록 전까지 거절되므로 예상 최대
    /// 손실 구간보다 크게 잡는다. 1024는 60Hz에서 약 17초다.
    /// </remarks>
    std::uint64_t maxSequenceJump = 1024;
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
/// The first admitted datagram fixes a session's return endpoint; later sources are rejected
/// unless DatagramTransportOptions::allowEndpointMigration is set.
/// Manual Poll owns no worker. Optional readiness uses one blocking monitor per
/// binding; StartReceiving adds one TimerScheduler and uses a supplied executor.
/// No retransmission, reliable delivery, encryption or automatic send queue.
/// Destruction requires an external control context, after receive/readiness
/// callbacks have returned. Close may be requested inside a callback, but that
/// callback must not destroy the owner or its last shared ownership reference.
/// <remarks>
/// 재정렬 창이 없다. 세션마다 가장 큰 커밋 시퀀스 하나만 기억하므로, 그보다 작거나 같은 시퀀스로 늦게
/// 도착한 패킷은 재전송이든 네트워크 재정렬이든 모두 replayedDatagrams로 버려지고 그 입력은 잃는다.
/// 순서가 뒤바뀌어도 되는 입력이면 애플리케이션이 중복 전송이나 최신 상태 전송으로 견뎌야 한다.
/// 모든 세션의 송신(sendto)과 수신(recvmsg)이 전송 하나의 mutex 아래에서 실행되므로, 전송 하나의
/// 처리량은 소켓 호출 하나씩으로 직렬화된다.
/// </remarks>
class DatagramTransport final
{
public:
    /// <summary>
    /// 첫 인자는 세션·출발지 엔드포인트·시퀀스를 담은 수신 뷰다. 엔드포인트 이전을 켠 전송은 여기서
    /// view.remoteEndpoint를 RemoteEndpoint(view.session)와 견주어 이전을 거부할 수 있다.
    /// </summary>
    using Admission = std::function<bool(const DatagramMessageView&, const Protocol::Message&)>;
    using Receiver = std::function<void(Session::SessionId, const Protocol::Message&)>;
    /// <summary>Admission과 같은 수신 뷰를 받는 Binary 판정이다.</summary>
    using BinaryAdmission =
        std::function<bool(const DatagramMessageView&, Protocol::BinaryMessageView)>;
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
        /// <summary>이전이 꺼진 세션에 다른 출발지에서 온 패킷. rejectedDatagrams에도 센다.</summary>
        std::uint64_t endpointMismatchDatagrams = 0;
        /// <summary>maxSequenceJump를 넘는 시퀀스 점프. rejectedDatagrams에도 센다.</summary>
        std::uint64_t sequenceJumpDatagrams = 0;
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
    [[nodiscard]] SERVERCORE_API Core::Status Bind(
        const Core::IpEndpoint& endpoint, bool ipv6Only = true);
    [[nodiscard]] SERVERCORE_API Core::IpEndpoint LocalEndpoint() const noexcept;
    /// Committed return endpoint (first admitted source, or the latest one when migration is
    /// allowed); NotFound for unknown session, WouldBlock before first packet.
    [[nodiscard]] SERVERCORE_API Core::Result<Core::IpEndpoint> RemoteEndpoint(
        Session::SessionId id) const;
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
    /// A batch the executor rejects (e.g. retainedCallbackBytes above its byte budget) is not
    /// validated here: receiving stops at the first batch and counts one pumpFailures
    /// (Runtime.DatagramPumpCountsBatchFailures).
    SERVERCORE_API Core::Status StartReceiving(std::shared_ptr<TaskExecutor> executor,
        DatagramAdmission admission, DatagramReceiver receiver,
        const DatagramReceiveOptions& options = {});
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

    /// Decode, token, replay, endpoint and sequence-jump checks precede one JSON parse. Admission
    /// receives the datagram view (session, source endpoint, sequence) and runs before committing
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
    SERVERCORE_API void PollDatagrams(const DatagramAdmission& admission,
        const DatagramReceiver& receiver, DatagramPollBudget budget = {}) noexcept;
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
    SERVERCORE_API void PollPayload(
        const PayloadAdmission&, const PayloadReceiver&, DatagramPollBudget) noexcept;
    [[nodiscard]] SERVERCORE_API Core::Status SendPayload(
        Session::SessionId, std::span<const std::byte>) noexcept;
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
}
