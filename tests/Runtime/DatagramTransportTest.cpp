#include "ServerCore/Runtime/DatagramTransport.h"

#include "Net/DatagramSocket.h"
#include "SocketTestSupport.h"
#include "TestHarness.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace Codec = ServerCore::Protocol::DatagramCodec;
using ServerCore::Core::ErrorCode;
using ServerCore::Core::Status;
using ServerCore::Protocol::Message;
using ServerCore::Runtime::DatagramMessageView;
using ServerCore::Runtime::DatagramPollBudget;
using ServerCore::Runtime::DatagramTransport;
using ServerCore::Session::SessionId;
using ServerCoreTest::ExpectTrue;

constexpr auto WaitLimit = std::chrono::seconds(5);
constexpr SessionId FirstId = static_cast<SessionId>(1);
constexpr SessionId SecondId = static_cast<SessionId>(2);
constexpr std::string_view Payload = R"({"type":"ApplicationMessage","body":{"value":7}})";
constexpr std::string_view Reply = R"({"type":"ApplicationReply","body":{"value":8}})";
constexpr std::string_view Denied = R"({"type":"DeniedByApplication","body":{}})";

static_assert(!std::is_copy_constructible_v<DatagramTransport>);
static_assert(!std::is_copy_assignable_v<DatagramTransport>);
static_assert(noexcept(std::declval<DatagramTransport&>().Close()));
static_assert(noexcept(std::declval<DatagramTransport&>().SendSerialized(FirstId, {})));

std::span<const std::byte> Bytes(const std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

bool ExpectOk(const Status& status, const std::string_view context)
{
    ExpectTrue(status.IsOk(), context);
    return status.IsOk();
}

bool Register(DatagramTransport& transport, const SessionId id, Codec::Token& token)
{
    const auto registration = transport.RegisterSession(id);
    if (!ExpectOk(registration.GetStatus(), "session registration succeeds"))
        return false;
    token = registration.Value();
    return true;
}

std::vector<std::byte> Packet(const Codec::Token& token, const std::uint64_t sequence,
    const std::string_view payload = Payload)
{
    std::vector<std::byte> bytes(Codec::HeaderBytes + payload.size());
    const auto written = Codec::Encode(bytes, token, sequence, Bytes(payload));
    ExpectTrue(written == bytes.size(), "test packet encoding fills its bounded storage");
    return bytes;
}

// The peer uses raw datagrams so malformed and zero-length input really crosses
// the socket boundary instead of sharing the transport's validation path.
class RawPeer
{
public:
    RawPeer()
    {
        ExpectTrue(mSockets.IsReady(), "raw peer initializes its socket runtime");
        if (!mSockets.IsReady())
            return;
        mSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mSocket == ServerCoreTest::InvalidSocket)
        {
            ExpectTrue(false, "raw peer creates its UDP socket");
            return;
        }
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        mReady =
            ::bind(mSocket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0 &&
            ServerCoreTest::SetSocketNonblocking(mSocket);
        ExpectTrue(mReady, "raw peer binds an ephemeral nonblocking loopback socket");
    }

    ~RawPeer()
    {
        if (mSocket != ServerCoreTest::InvalidSocket)
            ServerCoreTest::CloseSocket(mSocket);
    }
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;

    bool IsReady() const noexcept { return mReady; }

    bool Send(const std::uint16_t port, const std::span<const std::byte> bytes)
    {
        if (!mReady)
            return false;
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        endpoint.sin_port = htons(port);
        const char* const data = bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data());
        const auto length = static_cast<int>(bytes.size());
        const int sent = ServerCoreTest::SendTo(mSocket, data, length, 0,
            reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
        ExpectTrue(sent == length, "raw peer sends the complete local datagram");
        return sent == length;
    }

    bool Receive(std::vector<std::byte>& bytes)
    {
        std::array<std::byte, 65536> buffer{};
        sockaddr_in endpoint{};
        ServerCoreTest::SocketLength endpointLength = sizeof(endpoint);
        const int count = ServerCoreTest::ReceiveFrom(mSocket,
            reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&endpoint), &endpointLength);
        if (count == ServerCoreTest::SocketError)
        {
            ExpectTrue(ServerCoreTest::SocketWouldBlock(ServerCoreTest::LastSocketError()),
                "raw peer empty receive has no unexpected socket failure");
            return false;
        }
        mLastSourcePort = ntohs(endpoint.sin_port);
        bytes.assign(buffer.begin(), buffer.begin() + count);
        return true;
    }

    std::uint16_t LastSourcePort() const noexcept { return mLastSourcePort; }

private:
    ServerCoreTest::SocketRuntime mSockets;
    ServerCoreTest::Socket mSocket = ServerCoreTest::InvalidSocket;
    bool mReady = false;
    std::uint16_t mLastSourcePort = 0;
};

bool WaitFor(const std::function<bool()>& predicate, const std::string_view context,
    const std::function<std::string()>& failureDetails = {})
{
    const auto deadline = std::chrono::steady_clock::now() + WaitLimit;
    do
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (failureDetails)
        ExpectTrue(false, std::string(context) + ": " + failureDetails());
    else
        ExpectTrue(false, context);
    return false;
}

std::string ReceiveMetrics(const DatagramTransport::Metrics& metrics, const std::size_t callbacks)
{
    return "receivedDatagrams=" + std::to_string(metrics.receivedDatagrams) +
           ", receivedBytes=" + std::to_string(metrics.receivedBytes) +
           ", rejectedDatagrams=" + std::to_string(metrics.rejectedDatagrams) +
           ", malformedDatagrams=" + std::to_string(metrics.malformedDatagrams) +
           ", truncatedDatagrams=" + std::to_string(metrics.truncatedDatagrams) +
           ", socketErrors=" + std::to_string(metrics.socketErrors) +
           ", callbacks=" + std::to_string(callbacks);
}

const DatagramTransport::Admission Allow = [](const DatagramMessageView&, const Message&)
{ return true; };
const DatagramTransport::Receiver Ignore = [](SessionId, const Message&) {};

bool MakeReady(DatagramTransport& transport, RawPeer& peer, const Codec::Token& token,
    const SessionId id = FirstId)
{
    if (!peer.Send(transport.Port(), Packet(token, 1)))
        return false;
    std::size_t received = 0;
    return WaitFor(
        [&]
        {
            transport.Poll(Allow,
                [&](SessionId actualId, const Message&)
                {
                    ExpectTrue(actualId == id,
                        "token routes the received message to its registered session");
                    ++received;
                });
            return received == 1 && transport.IsReady(id);
        },
        "valid local packet makes its session ready");
}

void BindLifecycleAndWinsockIsolation()
{
    DatagramTransport live;
    if (!ExpectOk(live.Bind("127.0.0.1", 0), "listener accepts an ephemeral port"))
        return;
    ExpectTrue(live.Port() != 0, "listener reports the assigned port without exposing its value");
    ExpectTrue(live.Bind("127.0.0.1", 0).Code() == ErrorCode::AlreadyExists,
        "binding an open transport is rejected");
    Codec::Token liveToken{};
    if (!Register(live, FirstId, liveToken))
        return;
    {
        DatagramTransport other;
        ExpectTrue(other.Bind("not-an-ipv4", 0).Code() == ErrorCode::InvalidArgument,
            "invalid numeric address is rejected");
        ExpectTrue(other.Bind("", 0).Code() == ErrorCode::InvalidArgument,
            "empty numeric address is rejected");
        ExpectTrue(other.Port() == 0, "failed bind does not publish a port");
        ExpectTrue(other.Bind("127.0.0.1", live.Port()).Code() == ErrorCode::PlatformError,
            "exclusive bind refuses a second listener on the same endpoint");
        if (!ExpectOk(other.Bind("127.0.0.1", 0), "failed bind leaves transport reusable"))
            return;
        const auto oldPort = other.Port();
        other.Close();
        other.Close();
        ExpectTrue(other.Port() == 0, "repeated close keeps the port cleared");
        if (!ExpectOk(other.Bind("127.0.0.1", oldPort), "closed UDP endpoint can be rebound"))
            return;
    }
    RawPeer peer;
    if (!peer.IsReady() || !MakeReady(live, peer, liveToken))
        return;
    ExpectTrue(
        live.IsReady(FirstId), "another transport's destruction leaves the live socket usable");
}

void SocketEndpointBoundaryConformance()
{
    ServerCore::Net::DatagramSocket socket;
    ServerCore::Core::IpEndpoint endpoint{};
    ExpectTrue(socket.Receive({}).status.Code() == ErrorCode::Closed &&
                   socket.Send(endpoint, {}).Code() == ErrorCode::Closed,
        "closed socket status precedes buffer and endpoint validation");

    constexpr char embeddedNul[] = "127.0.0.1\0ignored";
    const std::array invalidAddresses{ std::string_view{}, std::string_view{ "localhost" },
        std::string_view{ ":::1" }, std::string_view{ "127.1" }, std::string_view{ "127.000.0.1" },
        std::string_view{ "256.0.0.1" }, std::string_view{ "127.0.0.1 " },
        std::string_view{ " 127.0.0.1" }, std::string_view{ embeddedNul, sizeof(embeddedNul) - 1 },
        std::string_view{ "1234567890123456" } };
    for (const auto address : invalidAddresses)
    {
        ExpectTrue(socket.Bind(address, 0).Code() == ErrorCode::InvalidArgument,
            "numeric IPv4 validation rejects malformed or embedded-NUL addresses");
        ExpectTrue(!socket.IsOpen() && socket.Port() == 0,
            "an invalid endpoint publishes neither a socket nor a port");
    }

    const std::string adjacent = "127.0.0.1not-part-of-the-address";
    if (!ExpectOk(socket.Bind(std::string_view(adjacent).substr(0, 9), 0),
            "a non-NUL-terminated IPv4 view binds using exactly its declared bytes"))
        return;
    ExpectTrue(socket.Bind(std::string_view{}, 0).Code() == ErrorCode::AlreadyExists,
        "an already bound socket takes precedence over a new invalid address");
    ExpectTrue(socket.Send(endpoint, {}).Code() == ErrorCode::InvalidArgument,
        "a bound IPv4 socket rejects an incompatible endpoint family");
    const auto port = socket.Port();
    socket.Close();
    socket.Close();
    ExpectTrue(!socket.IsOpen() && socket.Port() == 0, "close resets the raw socket and port");
    ExpectOk(
        socket.Bind("127.0.0.1", port), "closed endpoint can bind again after invalid operations");
}

void SocketEmptyAndTruncatedDatagrams()
{
    ServerCore::Net::DatagramSocket socket;
    if (!ExpectOk(socket.Bind("127.0.0.1", 0), "raw receive boundary socket binds"))
        return;
    RawPeer peer;
    if (!peer.IsReady())
        return;
    ServerCore::Net::DatagramReceiveResult received{ Status::Ok() };
    const auto receive = [&](const std::span<std::byte> storage)
    {
        return WaitFor(
            [&]
            {
                received = socket.Receive(storage);
                return received.status.Code() != ErrorCode::WouldBlock;
            },
            "raw receive completes within its deadline");
    };

    if (!peer.Send(socket.Port(), {}) || !receive({}))
        return;
    ExpectTrue(received.status.IsOk() && received.bytes == 0 && !received.retryable,
        "empty datagram succeeds even with an empty receive buffer");
    ExpectTrue(received.endpoint.address.Family() == ServerCore::Core::IpFamily::V4 &&
                   received.endpoint.port != 0,
        "an empty datagram still identifies its reply endpoint");
    if (!received.status.IsOk())
        return;
    if (!ExpectOk(socket.Send(received.endpoint, {}), "raw UDP permits an empty reply"))
        return;
    std::vector<std::byte> reply;
    if (!WaitFor([&] { return peer.Receive(reply); }, "peer receives an empty reply"))
        return;
    ExpectTrue(reply.empty(), "an empty reply contains no synthesized byte");

    const std::array oversized{ std::byte{ 'A' }, std::byte{ 'B' } };
    const std::array marker{ std::byte{ 'R' } };
    std::array<std::byte, 1> buffer{};
    if (!peer.Send(socket.Port(), oversized) || !peer.Send(socket.Port(), marker) ||
        !receive(buffer))
        return;
    ExpectTrue(
        received.status.Code() == ErrorCode::TooLarge && received.bytes == 0 && !received.retryable,
        "truncation is a consumed size error, not a successful prefix or retryable socket failure");
    if (!receive(buffer))
        return;
    ExpectTrue(received.status.IsOk() && received.bytes == marker.size() && buffer == marker,
        "receiving after truncation starts at the next whole datagram");

    if (!peer.Send(socket.Port(), marker) || !receive({}))
        return;
    ExpectTrue(received.status.Code() == ErrorCode::TooLarge && received.bytes == 0,
        "nonempty datagram cannot masquerade as empty when the receive buffer is empty");
    if (!peer.Send(socket.Port(), {}) || !peer.Send(socket.Port(), marker) || !receive(buffer))
        return;
    ExpectTrue(received.status.IsOk() && received.bytes == 0,
        "empty input remains a complete datagram when the receive buffer has storage");
    if (!receive(buffer))
        return;
    ExpectTrue(received.status.IsOk() && received.bytes == marker.size() && buffer == marker,
        "empty and truncated receives preserve the next payload boundary");
    ExpectTrue(socket.Receive(buffer).status.Code() == ErrorCode::WouldBlock,
        "the datagram queue is empty after each packet has been consumed once");
    ExpectOk(ServerCore::Net::GenerateDatagramSecret({}),
        "empty secret generation succeeds without output");
}

void RegistrationLifecycle()
{
    DatagramTransport transport;
    auto closed = transport.RegisterSession(FirstId);
    ExpectTrue(closed.GetStatus().Code() == ErrorCode::Closed,
        "closed transport rejects session registration");
    ExpectTrue(transport.SendSerialized(FirstId, Bytes(Reply)).Code() == ErrorCode::Closed,
        "unknown session send is closed");
    if (!ExpectOk(transport.Bind("127.0.0.1", 0), "registration listener binds"))
        return;
    auto invalid = transport.RegisterSession(SessionId::Invalid);
    ExpectTrue(invalid.GetStatus().Code() == ErrorCode::InvalidArgument,
        "invalid session identity is rejected");
    Codec::Token first{};
    Codec::Token second{};
    if (!Register(transport, FirstId, first) || !Register(transport, SecondId, second))
        return;
    ExpectTrue(first != second, "different registrations receive different tokens");
    auto duplicate = transport.RegisterSession(FirstId);
    ExpectTrue(duplicate.GetStatus().Code() == ErrorCode::InvalidArgument,
        "duplicate session identity is rejected");
    ExpectTrue(!transport.IsReady(FirstId) &&
                   transport.SendSerialized(FirstId, Bytes(Reply)).Code() == ErrorCode::WouldBlock,
        "registration does not invent a return endpoint");
    RawPeer peer;
    if (!peer.IsReady() || !MakeReady(transport, peer, first))
        return;
    transport.UnregisterSession(FirstId);
    transport.UnregisterSession(FirstId);
    ExpectTrue(!transport.IsReady(FirstId) &&
                   transport.SendSerialized(FirstId, Bytes(Reply)).Code() == ErrorCode::Closed,
        "unregistration clears readiness and is safe to repeat");
    Codec::Token replacement{};
    if (!Register(transport, FirstId, replacement))
        return;
    ExpectTrue(replacement != first && !transport.IsReady(FirstId),
        "same identity receives a fresh unready registration");
    const auto before = transport.SnapshotMetrics();
    if (!peer.Send(transport.Port(), Packet(first, 100)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, Ignore);
                return transport.SnapshotMetrics().rejectedDatagrams > before.rejectedDatagrams;
            },
            "old token is rejected after unregistration"))
        return;
    ExpectTrue(!transport.IsReady(FirstId), "old token cannot activate its replacement");
    if (!MakeReady(transport, peer, replacement))
        return;
    const auto metrics = transport.SnapshotMetrics();
    transport.Close();
    ExpectTrue(!transport.IsReady(FirstId) && !transport.IsReady(SecondId),
        "close clears every registration");
    ExpectTrue(transport.SnapshotMetrics().receivedDatagrams == metrics.receivedDatagrams &&
                   transport.SnapshotMetrics().rejectedDatagrams == metrics.rejectedDatagrams,
        "close preserves lifetime metrics");
}

void GenericRoundTripAndSendSequence()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() || !ExpectOk(transport.Bind("127.0.0.1", 0), "roundtrip listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token))
        return;
    if (!peer.Send(transport.Port(), Packet(token, 1)))
        return;
    std::size_t received = 0;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow,
                    [&](SessionId id, const Message& message)
                    {
                        ExpectTrue(id == FirstId && message.Type() == "ApplicationMessage",
                            "transport accepts a generic application type without game message "
                            "knowledge");
                        ExpectTrue(message.Body() && message.Body()->TryObject(),
                            "generic JSON body survives parsing");
                        ++received;
                    });
                return received == 1;
            },
            "generic message reaches its receiver"))
        return;
    std::array<std::byte, Codec::MaximumPayloadBytes + 1> tooLarge{};
    ExpectTrue(transport.SendSerialized(FirstId, {}).Code() == ErrorCode::TooLarge,
        "ready session rejects empty payload");
    ExpectTrue(transport.SendSerialized(FirstId, tooLarge).Code() == ErrorCode::TooLarge,
        "ready session rejects payload over its datagram limit");
    ExpectTrue(transport.SnapshotMetrics().sentDatagrams == 0,
        "rejected sends are not successful datagrams");
    for (std::uint64_t expectedSequence = 1; expectedSequence <= 2; ++expectedSequence)
    {
        if (!ExpectOk(transport.SendSerialized(FirstId, Bytes(Reply)),
                "valid reply is accepted by the OS"))
            return;
        std::vector<std::byte> response;
        if (!WaitFor([&] { return peer.Receive(response); }, "raw peer receives transport reply"))
            return;
        const auto packet = Codec::Decode(response);
        ExpectTrue(packet && packet->token == token && packet->sequence == expectedSequence,
            "successful sends alone advance the exact outbound sequence");
        if (packet)
            ExpectTrue(std::string_view(reinterpret_cast<const char*>(packet->payload.data()),
                           packet->payload.size()) == Reply,
                "send preserves already serialized payload bytes");
        ExpectTrue(
            peer.LastSourcePort() == transport.Port(), "reply originates from the listener socket");
    }
    const auto metrics = transport.SnapshotMetrics();
    ExpectTrue(metrics.receivedDatagrams == 1 &&
                   metrics.receivedBytes == Codec::HeaderBytes + Payload.size(),
        "receive metrics count complete application datagrams");
    ExpectTrue(
        metrics.sentDatagrams == 2 && metrics.sentBytes == 2 * (Codec::HeaderBytes + Reply.size()),
        "send metrics include codec headers and accepted payloads");
}

void InvalidPacketsCannotRebindOrPoisonSequence()
{
    DatagramTransport transport;
    RawPeer original;
    RawPeer attacker;
    if (!original.IsReady() || !attacker.IsReady() ||
        !ExpectOk(transport.Bind("127.0.0.1", 0), "validation listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token) || !MakeReady(transport, original, token))
        return;
    auto foreign = token;
    foreign[0] ^= std::byte{ 0xff };
    const auto before = transport.SnapshotMetrics();
    // 잘못된 본문과 application 거절은 이미 정해진 엔드포인트에서 보낸다. 다른 출발지에서 오면 본문을
    // 보기 전에 엔드포인트 불일치로 먼저 거절되어 이 두 경로를 지나지 않는다.
    if (!attacker.Send(transport.Port(), Packet(foreign, 1000)) ||
        !attacker.Send(transport.Port(), Packet(token, 1)) ||
        !original.Send(transport.Port(), Packet(token, 1001, "{")) ||
        !original.Send(transport.Port(), Packet(token, 1002, Denied)))
        return;
    std::size_t delivered = 0;
    const DatagramTransport::Admission admit =
        [](const DatagramMessageView&, const Message& message)
    { return message.Type() != "DeniedByApplication"; };
    if (!WaitFor(
            [&]
            {
                transport.Poll(admit, [&](SessionId, const Message&) { ++delivered; });
                return transport.SnapshotMetrics().rejectedDatagrams >=
                       before.rejectedDatagrams + 4;
            },
            "unknown, replayed, malformed and application-denied datagrams are rejected"))
        return;
    ExpectTrue(delivered == 0, "rejected input never reaches the application receiver");
    const auto rejected = transport.SnapshotMetrics();
    ExpectTrue(rejected.invalidPayloadDatagrams == before.invalidPayloadDatagrams + 1 &&
                   rejected.admissionRejectedDatagrams == before.admissionRejectedDatagrams + 1,
        "malformed and application-denied packets take their own rejection paths");
    if (!ExpectOk(transport.SendSerialized(FirstId, Bytes(Reply)),
            "established return path remains sendable"))
        return;
    std::vector<std::byte> response;
    if (!WaitFor([&] { return original.Receive(response); },
            "rejection preserves the original return endpoint"))
        return;
    ExpectTrue(!attacker.Receive(response),
        "rejected packets receive no reply and cannot steal the return endpoint");
    if (!original.Send(transport.Port(), Packet(token, 2)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(admit, [&](SessionId, const Message&) { ++delivered; });
                return delivered == 1;
            },
            "invalid high sequence does not poison the next valid low sequence"))
        return;
    // 이전이 기본으로 꺼져 있으므로, 토큰과 새 시퀀스가 맞아도 다른 출발지의 패킷은 반환 경로를
    // 빼앗지 못한다. 이전을 켰을 때의 교체는 EndpointMigrationIsOptIn이 본다.
    const auto beforeMismatch = transport.SnapshotMetrics().endpointMismatchDatagrams;
    if (!attacker.Send(transport.Port(), Packet(token, 3)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(admit, [&](SessionId, const Message&) { ++delivered; });
                return transport.SnapshotMetrics().endpointMismatchDatagrams == beforeMismatch + 1;
            },
            "valid newer packet from another endpoint is rejected by default"))
        return;
    ExpectTrue(delivered == 1, "rejected second-endpoint packet reaches no receiver");
    if (!ExpectOk(
            transport.SendSerialized(FirstId, Bytes(Reply)), "original return path stays sendable"))
        return;
    if (!WaitFor([&] { return original.Receive(response); },
            "original endpoint keeps receiving replies"))
        return;
    ExpectTrue(!attacker.Receive(response), "second endpoint does not receive the reply");
}

// UDP-1: 토큰과 시퀀스가 맞는 패킷이라도 다른 출발지에서 오면 기본값에서는 반환 엔드포인트를 옮기지
// 않는다. 옮기면 출발지를 위조한 패킷 하나로 서버 송신을 제3자에게 돌릴 수 있다(반사).
void EndpointMigrationIsOptIn()
{
    DatagramTransport transport;
    RawPeer original;
    RawPeer other;
    if (!original.IsReady() || !other.IsReady() ||
        !ExpectOk(transport.Bind("127.0.0.1", 0), "migration listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token) || !MakeReady(transport, original, token))
        return;
    const auto before = transport.SnapshotMetrics();
    std::size_t delivered = 0;
    const DatagramTransport::Receiver count = [&](SessionId, const Message&) { ++delivered; };
    if (!other.Send(transport.Port(), Packet(token, 2)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, count);
                return transport.SnapshotMetrics().rejectedDatagrams ==
                       before.rejectedDatagrams + 1;
            },
            "valid packet from a second endpoint is rejected while migration is off"))
        return;
    ExpectTrue(delivered == 0, "rejected second-endpoint packet never reaches the receiver");
    if (!ExpectOk(
            transport.SendSerialized(FirstId, Bytes(Reply)), "established path stays sendable"))
        return;
    std::vector<std::byte> response;
    if (!WaitFor(
            [&] { return original.Receive(response); }, "reply keeps the established endpoint"))
        return;
    ExpectTrue(!other.Receive(response), "second endpoint receives no reply");
    if (!original.Send(transport.Port(), Packet(token, 2)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, count);
                return delivered == 1;
            },
            "rejected second-endpoint packet does not consume its sequence"))
        return;
    ExpectTrue(transport.SnapshotMetrics().endpointMismatchDatagrams ==
                   before.endpointMismatchDatagrams + 1,
        "second-endpoint rejection is counted as an endpoint mismatch");

    // 이전을 켜면 admission이 수신 뷰의 출발지를 보고 이전을 거부하거나 허락한다.
    ServerCore::Runtime::DatagramTransportOptions options;
    ExpectTrue(!options.allowEndpointMigration, "endpoint migration is off by default");
    options.allowEndpointMigration = true;
    DatagramTransport migrating;
    if (!ExpectOk(migrating.Configure(options), "migration can be enabled") ||
        !ExpectOk(migrating.Bind("127.0.0.1", 0), "migrating listener binds"))
        return;
    Codec::Token migratingToken{};
    if (!Register(migrating, FirstId, migratingToken) ||
        !MakeReady(migrating, original, migratingToken))
        return;
    bool allowMove = false;
    std::size_t moveChecks = 0;
    const DatagramTransport::Admission guard = [&](const DatagramMessageView& view, const Message&)
    {
        const auto established = migrating.RemoteEndpoint(view.session);
        const bool moved = established.IsOk() && established.Value() != view.remoteEndpoint;
        if (moved)
            ++moveChecks;
        return !moved || allowMove;
    };
    delivered = 0;
    const auto beforeGuard = migrating.SnapshotMetrics();
    if (!other.Send(migrating.Port(), Packet(migratingToken, 2)))
        return;
    if (!WaitFor(
            [&]
            {
                migrating.Poll(guard, count);
                return migrating.SnapshotMetrics().admissionRejectedDatagrams ==
                       beforeGuard.admissionRejectedDatagrams + 1;
            },
            "admission sees the new source endpoint and can refuse migration"))
        return;
    ExpectTrue(moveChecks == 1 && delivered == 0, "refused migration delivers nothing");
    allowMove = true;
    if (!other.Send(migrating.Port(), Packet(migratingToken, 2)))
        return;
    if (!WaitFor(
            [&]
            {
                migrating.Poll(guard, count);
                return delivered == 1;
            },
            "refused migration left the sequence for an allowed retry"))
        return;
    if (!ExpectOk(migrating.SendSerialized(FirstId, Bytes(Reply)), "migrated path is sendable"))
        return;
    if (!WaitFor([&] { return other.Receive(response); }, "migrated endpoint receives the reply"))
        return;
    ExpectTrue(!original.Receive(response), "previous endpoint no longer receives replies");
}

// UDP-1: 시퀀스 점프에 상한이 없으면 토큰을 본 관찰자가 sequence = UINT64_MAX 패킷 하나로 그 세션의
// UDP 수신을 재등록 전까지 막는다.
void SequenceJumpIsBounded()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() || !ExpectOk(transport.Bind("127.0.0.1", 0), "jump listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token) || !MakeReady(transport, peer, token))
        return;
    const auto before = transport.SnapshotMetrics();
    std::size_t delivered = 0;
    const DatagramTransport::Receiver count = [&](SessionId, const Message&) { ++delivered; };
    if (!peer.Send(transport.Port(), Packet(token, std::numeric_limits<std::uint64_t>::max())))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, count);
                const auto metrics = transport.SnapshotMetrics();
                return metrics.receivedDatagrams == before.receivedDatagrams + 1;
            },
            "maximum-sequence packet is received"))
        return;
    ExpectTrue(delivered == 0 &&
                   transport.SnapshotMetrics().rejectedDatagrams == before.rejectedDatagrams + 1,
        "sequence jump beyond the cap is rejected");
    const auto afterJump = delivered;
    if (!peer.Send(transport.Port(), Packet(token, 2)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, count);
                return delivered == afterJump + 1;
            },
            "rejected jump does not block the next sequence"))
        return;

    // 상한과 같은 점프는 받고, 하나 더 큰 점프는 거절한다.
    const auto cap = ServerCore::Runtime::DatagramTransportOptions{}.maxSequenceJump;
    if (!peer.Send(transport.Port(), Packet(token, 2 + cap)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, count);
                return delivered == afterJump + 2;
            },
            "jump equal to the cap is accepted"))
        return;
    if (!peer.Send(transport.Port(), Packet(token, 2 + cap + cap + 1)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, count);
                return transport.SnapshotMetrics().sequenceJumpDatagrams ==
                       before.sequenceJumpDatagrams + 2;
            },
            "jump one past the cap is rejected"))
        return;
    ExpectTrue(delivered == afterJump + 2, "rejected jumps deliver nothing");
    ServerCore::Runtime::DatagramTransportOptions zero;
    zero.maxSequenceJump = 0;
    DatagramTransport unconfigured;
    ExpectTrue(unconfigured.Configure(zero).Code() == ErrorCode::InvalidArgument,
        "zero sequence-jump cap is invalid");
}

void ReceiverCallbacksCanSendUnregisterAndClose()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() || !ExpectOk(transport.Bind("127.0.0.1", 0), "reentrant listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token) || !peer.Send(transport.Port(), Packet(token, 1)))
        return;
    std::size_t calls = 0;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow,
                    [&](SessionId id, const Message&)
                    {
                        ExpectTrue(transport.IsReady(id), "receiver observes committed readiness");
                        ExpectOk(transport.SendSerialized(id, Bytes(Reply)),
                            "receiver may send through the same transport");
                        transport.UnregisterSession(id);
                        ExpectTrue(
                            transport.SendSerialized(id, Bytes(Reply)).Code() == ErrorCode::Closed,
                            "receiver unregistration immediately closes the session's send path");
                        transport.Close();
                        ++calls;
                    });
                return calls == 1;
            },
            "receiver lifecycle operations do not deadlock"))
        return;
    ExpectTrue(transport.Port() == 0 && !transport.IsReady(FirstId),
        "receiver close leaves no listener or ready registration");
    std::vector<std::byte> reply;
    WaitFor([&] { return peer.Receive(reply); },
        "reply accepted before callback close reaches the local peer");
}

void AdmissionCallbacksRespectRegistrationLifetime()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() || !ExpectOk(transport.Bind("127.0.0.1", 0), "admission listener binds"))
        return;
    Codec::Token oldToken{};
    if (!Register(transport, FirstId, oldToken) || !MakeReady(transport, peer, oldToken))
        return;
    if (!peer.Send(transport.Port(), Packet(oldToken, 100)))
        return;
    Codec::Token replacement{};
    std::size_t admitted = 0;
    std::size_t delivered = 0;
    if (!WaitFor(
            [&]
            {
                transport.Poll(
                    [&](const DatagramMessageView& view, const Message&)
                    {
                        const auto id = view.session;
                        ExpectOk(transport.SendSerialized(id, Bytes(Reply)),
                            "admission callback may use an existing return path");
                        transport.UnregisterSession(id);
                        Register(transport, id, replacement);
                        ++admitted;
                        return true;
                    },
                    [&](SessionId, const Message&) { ++delivered; });
                return admitted == 1;
            },
            "admission callback can replace its registration"))
        return;
    ExpectTrue(delivered == 0 && replacement != oldToken && !transport.IsReady(FirstId),
        "old admitted packet cannot commit readiness or sequence to its replacement");
    if (!MakeReady(transport, peer, replacement))
        return;
    if (!peer.Send(transport.Port(), Packet(replacement, 200)))
        return;
    Codec::Token reopened{};
    admitted = 0;
    if (!WaitFor(
            [&]
            {
                transport.Poll(
                    [&](const DatagramMessageView& view, const Message&)
                    {
                        const auto id = view.session;
                        transport.Close();
                        ExpectOk(transport.Bind("127.0.0.1", 0),
                            "admission callback can reopen the listener");
                        Register(transport, id, reopened);
                        ++admitted;
                        return true;
                    },
                    [&](SessionId, const Message&) { ++delivered; });
                return admitted == 1;
            },
            "admission callback can close and reopen the transport"))
        return;
    ExpectTrue(delivered == 0 && reopened != replacement && !transport.IsReady(FirstId),
        "a packet from the closed socket cannot activate a newly bound registration");
    MakeReady(transport, peer, reopened);
}

void CallbackExceptionsEndPoll()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() || !ExpectOk(transport.Bind("127.0.0.1", 0), "exception listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token))
        return;
    if (!peer.Send(transport.Port(), Packet(token, 100)) ||
        !peer.Send(transport.Port(), Packet(token, 1)))
        return;
    std::size_t calls = 0;
    if (!WaitFor(
            [&]
            {
                transport.Poll([](const DatagramMessageView&, const Message&) -> bool
                    { throw std::runtime_error("admission test"); },
                    [&](SessionId, const Message&) { ++calls; });
                return transport.SnapshotMetrics().rejectedDatagrams != 0;
            },
            "admission exception is contained"))
        return;
    ExpectTrue(transport.SnapshotMetrics().receivedDatagrams == 1 &&
                   transport.SnapshotMetrics().rejectedDatagrams == 1 && calls == 0 &&
                   !transport.IsReady(FirstId),
        "admission exception ends the poll without committing the packet");
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, [&](SessionId, const Message&) { ++calls; });
                return calls == 1;
            },
            "lower valid sequence is usable after admission exception"))
        return;
    const auto before = transport.SnapshotMetrics();
    if (!peer.Send(transport.Port(), Packet(token, 10)) ||
        !peer.Send(transport.Port(), Packet(token, 9)))
        return;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow,
                    [](SessionId, const Message&) { throw std::runtime_error("receiver test"); });
                return transport.SnapshotMetrics().rejectedDatagrams > before.rejectedDatagrams;
            },
            "receiver exception is contained"))
        return;
    ExpectTrue(transport.SnapshotMetrics().receivedDatagrams == before.receivedDatagrams + 1 &&
                   transport.IsReady(FirstId),
        "receiver exception ends the poll after committing its packet");
    if (!peer.Send(transport.Port(), Packet(token, 11)))
        return;
    calls = 0;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow, [&](SessionId, const Message&) { ++calls; });
                return calls != 0;
            },
            "receiver exception leaves transport usable"))
        return;
    ExpectTrue(
        calls == 1 && transport.SnapshotMetrics().rejectedDatagrams == before.rejectedDatagrams + 2,
        "sequence committed before receiver exception rejects the older queued packet");
}

void DatagramBudgetCountsRejectedAndEmptyPackets()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() ||
        !ExpectOk(transport.Bind("127.0.0.1", 0), "packet budget listener binds"))
        return;
    std::array<std::byte, Codec::MaximumDatagramBytes + 1> oversized{};
    // One byte beyond the Poll receive buffer tests socket truncation without
    // depending on the delivery of a fragmented, multi-kilobyte datagram.
    std::array<std::byte, Codec::MaximumDatagramBytes + 2> truncated{};
    const std::array<std::byte, 1> malformed{ std::byte{ 'X' } };
    if (!peer.Send(transport.Port(), {}) || !peer.Send(transport.Port(), malformed) ||
        !peer.Send(transport.Port(), oversized) || !peer.Send(transport.Port(), truncated))
        return;
    std::size_t calls = 0;
    if (!WaitFor(
            [&]
            {
                const auto before = transport.SnapshotMetrics();
                transport.Poll(
                    Allow, [&](SessionId, const Message&) { ++calls; },
                    DatagramPollBudget{ 1, 65536 });
                const auto after = transport.SnapshotMetrics();
                ExpectTrue(after.rejectedDatagrams - before.rejectedDatagrams <= 1,
                    "one packet budget includes empty, malformed and truncated attempts");
                return after.rejectedDatagrams == 4;
            },
            "bounded polls eventually reject every invalid queued datagram",
            [&] { return ReceiveMetrics(transport.SnapshotMetrics(), calls); }))
        return;
    const auto metrics = transport.SnapshotMetrics();
    ExpectTrue(metrics.receivedDatagrams == 3 &&
                   metrics.receivedBytes == oversized.size() + malformed.size(),
        "empty and malformed datagrams are received in full before the truncated datagram");
    ExpectTrue(metrics.malformedDatagrams == 3 && metrics.truncatedDatagrams == 1 &&
                   metrics.socketErrors == 0,
        "three malformed datagrams and one socket-truncated datagram have distinct metrics: " +
            ReceiveMetrics(metrics, calls));
    ExpectTrue(calls == 0, "invalid budget traffic never reaches a receiver");
    Codec::Token token{};
    if (!Register(transport, FirstId, token))
        return;
    MakeReady(transport, peer, token);
}

void ByteBudgetAndEmptyCallbacksPreservePendingPackets()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() || !ExpectOk(transport.Bind("127.0.0.1", 0), "byte budget listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token))
        return;
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
        if (!peer.Send(transport.Port(), Packet(token, sequence)))
            return;
    std::size_t calls = 0;
    const DatagramTransport::Receiver receive = [&](SessionId, const Message&) { ++calls; };
    transport.Poll({}, receive);
    transport.Poll(Allow, {});
    transport.Poll(Allow, receive, DatagramPollBudget{ 0, 1024 });
    transport.Poll(Allow, receive, DatagramPollBudget{ 10, 0 });
    ExpectTrue(transport.SnapshotMetrics().receivedDatagrams == 0 && calls == 0,
        "empty callbacks and zero budgets consume no queued datagrams");
    if (!WaitFor(
            [&]
            {
                const auto before = transport.SnapshotMetrics();
                transport.Poll(Allow, receive, DatagramPollBudget{ 100, 1 });
                const auto after = transport.SnapshotMetrics();
                ExpectTrue(after.receivedDatagrams - before.receivedDatagrams <= 1 &&
                               after.receivedBytes - before.receivedBytes <=
                                   Codec::HeaderBytes + Payload.size(),
                    "byte budget allows its final whole packet but stops before another receive");
                return calls == 3;
            },
            "later bounded polls deliver every pending valid datagram"))
        return;
    ExpectTrue(
        transport.SnapshotMetrics().receivedBytes == 3 * (Codec::HeaderBytes + Payload.size()),
        "byte budget counts full encoded datagrams");
}

void TruncatedDatagramsRecover()
{
    DatagramTransport transport;
    RawPeer peer;
    if (!peer.IsReady() || !ExpectOk(transport.Bind("127.0.0.1", 0), "truncation listener binds"))
        return;
    Codec::Token token{};
    if (!Register(transport, FirstId, token))
        return;
    auto shortHeader = Packet(token, 100);
    shortHeader.resize(Codec::HeaderBytes);
    auto oversized = Packet(token, 101);
    oversized.resize(Codec::MaximumDatagramBytes + 1);
    auto truncated = Packet(token, 102);
    // Poll receives at most MaximumDatagramBytes + 1 bytes, so +2 must be
    // reported as truncation while avoiding a large-datagram delivery dependency.
    truncated.resize(Codec::MaximumDatagramBytes + 2);
    if (!peer.Send(transport.Port(), shortHeader) || !peer.Send(transport.Port(), oversized) ||
        !peer.Send(transport.Port(), truncated) || !peer.Send(transport.Port(), Packet(token, 1)))
        return;
    std::size_t calls = 0;
    if (!WaitFor(
            [&]
            {
                transport.Poll(Allow,
                    [&](SessionId id, const Message& message)
                    {
                        ExpectTrue(id == FirstId && message.Type() == "ApplicationMessage",
                            "recovery delivers only the intact application message");
                        ++calls;
                    });
                return calls != 0 && transport.SnapshotMetrics().rejectedDatagrams >= 3;
            },
            "valid datagram recovers after short, oversized and truncated input",
            [&] { return ReceiveMetrics(transport.SnapshotMetrics(), calls); }))
        return;
    const auto metrics = transport.SnapshotMetrics();
    ExpectTrue(calls == 1 && metrics.rejectedDatagrams == 3,
        "malformed datagrams are discarded whole without consuming the valid low sequence: " +
            ReceiveMetrics(metrics, calls));
    ExpectTrue(metrics.receivedDatagrams == 3 && metrics.malformedDatagrams == 2 &&
                   metrics.truncatedDatagrams == 1 && metrics.socketErrors == 0,
        "short header and protocol oversize differ from socket truncation without socket errors: " +
            ReceiveMetrics(metrics, calls));
}

const ServerCoreTest::CheckRegistration gBindLifecycleAndWinsockIsolation{
    "Runtime.DatagramBindLifecycleAndWinsockIsolation", &BindLifecycleAndWinsockIsolation
};
const ServerCoreTest::CheckRegistration gSocketEndpointBoundaryConformance{
    "Runtime.DatagramSocketEndpointBoundaryConformance", &SocketEndpointBoundaryConformance
};
const ServerCoreTest::CheckRegistration gSocketEmptyAndTruncatedDatagrams{
    "Runtime.DatagramSocketEmptyAndTruncatedDatagrams", &SocketEmptyAndTruncatedDatagrams
};
const ServerCoreTest::CheckRegistration gRegistrationLifecycle{
    "Runtime.DatagramRegistrationLifecycle", &RegistrationLifecycle
};
const ServerCoreTest::CheckRegistration gGenericRoundTripAndSendSequence{
    "Runtime.DatagramGenericRoundTripAndSendSequence", &GenericRoundTripAndSendSequence
};
const ServerCoreTest::CheckRegistration gInvalidPacketsCannotRebindOrPoisonSequence{
    "Runtime.DatagramInvalidPacketsCannotRebindOrPoisonSequence",
    &InvalidPacketsCannotRebindOrPoisonSequence
};
const ServerCoreTest::CheckRegistration gEndpointMigrationIsOptIn{
    "Runtime.DatagramEndpointMigrationIsOptIn", &EndpointMigrationIsOptIn
};
const ServerCoreTest::CheckRegistration gSequenceJumpIsBounded{
    "Runtime.DatagramSequenceJumpIsBounded", &SequenceJumpIsBounded
};
const ServerCoreTest::CheckRegistration gReceiverCallbacksCanSendUnregisterAndClose{
    "Runtime.DatagramReceiverCallbacksCanSendUnregisterAndClose",
    &ReceiverCallbacksCanSendUnregisterAndClose
};
const ServerCoreTest::CheckRegistration gAdmissionCallbacksRespectRegistrationLifetime{
    "Runtime.DatagramAdmissionCallbacksRespectRegistrationLifetime",
    &AdmissionCallbacksRespectRegistrationLifetime
};
const ServerCoreTest::CheckRegistration gCallbackExceptionsEndPoll{
    "Runtime.DatagramCallbackExceptionsEndPoll", &CallbackExceptionsEndPoll
};
const ServerCoreTest::CheckRegistration gDatagramBudgetCountsRejectedAndEmptyPackets{
    "Runtime.DatagramBudgetCountsRejectedAndEmptyPackets",
    &DatagramBudgetCountsRejectedAndEmptyPackets
};
const ServerCoreTest::CheckRegistration gByteBudgetAndEmptyCallbacksPreservePendingPackets{
    "Runtime.DatagramByteBudgetAndEmptyCallbacksPreservePendingPackets",
    &ByteBudgetAndEmptyCallbacksPreservePendingPackets
};
const ServerCoreTest::CheckRegistration gTruncatedDatagramsRecover{
    "Runtime.DatagramTruncatedDatagramsRecover", &TruncatedDatagramsRecover
};
}
