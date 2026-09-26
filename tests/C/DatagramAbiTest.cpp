#include "C/AbiSocketTestSupport.h"
#include "Net/DatagramSocket.h"
#include "ServerCore/C/Datagram.h"
#include <chrono>
#include <thread>
#include <vector>
namespace
{
using namespace AbiTest;
using namespace std::chrono_literals;
using ServerCoreTest::ExpectTrue;
using Owner = Handle<sc_udp_transport, sc_udp_transport_destroy>;
using Event = Handle<sc_udp_event, sc_udp_event_destroy>;
constexpr std::string_view Body = R"({"type":"Ping","body":{}})";
std::vector<std::byte> Packet(sc_udp_token token, uint64_t sequence)
{
    std::vector<std::byte> output(1200);
    size_t written = 0;
    ExpectTrue(sc_udp_packet_encode(&token, sequence, Bytes(Body),
                   reinterpret_cast<uint8_t*>(output.data()), output.size(), &written) == SC_OK,
        "C shared packet encode");
    output.resize(written);
    return output;
}
Event Next(sc_udp_transport* owner)
{
    sc_udp_event* out = nullptr;
    ExpectTrue(sc_udp_transport_next(owner, 5000, &out) == SC_OK, "C event arrives");
    return Event(out, sc_udp_event_destroy);
}
/// <summary>metrics를 다시 읽어 조건이 설 때까지 기다린다. 끝내 서지 않으면 마지막 값을 남긴다.</summary>
template <class Condition>
bool AwaitMetrics(sc_udp_transport* owner, sc_udp_metrics& metrics, Condition condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do
    {
        if (sc_udp_transport_get_metrics(owner, &metrics) == SC_OK && condition(metrics))
            return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
/// <summary>
/// 반환 엔드포인트 이전과 시퀀스 점프 상한(UDP-1)이 C 옵션과 metrics로 드러나는지 본다. 기본값은
/// 이전을 막고 점프 상한 1024이며, 0인 상한과 정의되지 않은 값은 만들기에서 거절한다. 막힌 패킷은
/// 사건으로 나오지 않고 각자의 metrics와 rejected_datagrams에 센다. 이전을 켜면 같은 패킷이 들어온다.
/// </summary>
void EndpointAndSequencePolicy()
{
    sc_udp_options options{};
    ExpectTrue(sc_udp_options_init(&options, sizeof(options)) == SC_OK &&
                   options.allow_endpoint_migration == 0 && options.max_sequence_jump == 1024,
        "UDP options default to no migration and a 1024 sequence jump");
    sc_udp_transport* raw = nullptr;
    auto invalid = options;
    invalid.max_sequence_jump = 0;
    ExpectTrue(sc_udp_transport_create(&invalid, &raw) == SC_INVALID_ARGUMENT && !raw,
        "a zero sequence jump is rejected");
    invalid = options;
    invalid.allow_endpoint_migration = 2;
    ExpectTrue(sc_udp_transport_create(&invalid, &raw) == SC_INVALID_ARGUMENT && !raw,
        "an undefined migration flag is rejected");
    invalid = options;
    invalid.reserved2 = 1;
    ExpectTrue(sc_udp_transport_create(&invalid, &raw) == SC_INVALID_ARGUMENT && !raw,
        "a nonzero reserved field is rejected");
    for (const uint32_t migration : { 0u, 1u })
    {
        options.max_sequence_jump = 4;
        options.allow_endpoint_migration = migration;
        raw = nullptr;
        if (!Check(sc_udp_transport_create(&options, &raw) == SC_OK && raw,
                "policy transport created"))
            return;
        Owner owner(raw, sc_udp_transport_destroy);
        if (!Check(sc_udp_transport_start(raw) == SC_OK, "policy transport started"))
            return;
        sc_ip_endpoint local{};
        (void)sc_udp_transport_local_endpoint(raw, &local);
        const auto endpoint = ServerCore::Core::IpEndpoint::Parse("127.0.0.1", local.port).Value();
        ServerCore::Net::DatagramSocket first, second;
        if (!Check(first.Bind("127.0.0.1", 0).IsOk() && second.Bind("127.0.0.1", 0).IsOk(),
                "policy clients bound"))
            return;
        sc_udp_token token{};
        ExpectTrue(sc_udp_transport_register(raw, 9, &token) == SC_OK, "policy session registered");
        ExpectTrue(first.Send(endpoint, Packet(token, 1)).IsOk(), "first packet sent");
        auto accepted = Next(raw);
        if (!accepted)
            return;
        auto metrics = View<sc_udp_metrics>();
        ExpectTrue(first.Send(endpoint, Packet(token, 6)).IsOk(), "jumping packet sent");
        ExpectTrue(AwaitMetrics(raw, metrics, [](const sc_udp_metrics& value)
                       { return value.sequence_jump_datagrams == 1; }),
            "a jump beyond max_sequence_jump is counted");
        ExpectTrue(second.Send(endpoint, Packet(token, 2)).IsOk(), "packet from a new source sent");
        if (migration)
        {
            auto moved = Next(raw);
            auto view = View<sc_udp_event_view>();
            ExpectTrue(moved && sc_udp_event_get(moved.get(), &view) == SC_OK && view.sequence == 2,
                "migration admits the packet from the new source");
            ExpectTrue(sc_udp_transport_get_metrics(raw, &metrics) == SC_OK &&
                           metrics.endpoint_mismatch_datagrams == 0,
                "migration counts no endpoint mismatch");
        }
        else
        {
            ExpectTrue(AwaitMetrics(raw, metrics, [](const sc_udp_metrics& value)
                           { return value.endpoint_mismatch_datagrams == 1; }),
                "a packet from a new source is counted without migration");
            sc_udp_event* none = nullptr;
            ExpectTrue(sc_udp_transport_next(raw, 0, &none) == SC_WOULD_BLOCK && !none,
                "the mismatched packet produces no event");
        }
        ExpectTrue(metrics.rejected_datagrams >= 1, "policy rejections also count as rejected");
        (void)sc_udp_transport_stop(raw);
    }
}
void Ownership()
{
    sc_udp_options options{};
    ExpectTrue(sc_udp_options_init(&options, sizeof(options)) == SC_OK, "initialize UDP options");
    options.max_event_count = 1;
    sc_udp_transport* raw = nullptr;
    if (!Check(sc_udp_transport_create(&options, &raw) == SC_OK, "create UDP owner"))
        return;
    Owner owner(raw, sc_udp_transport_destroy);
    if (!Check(sc_udp_transport_start(raw) == SC_OK, "start owned UDP pump"))
        return;
    sc_ip_endpoint local{};
    ExpectTrue(sc_udp_transport_local_endpoint(raw, &local) == SC_OK, "read actual local endpoint");
    ServerCore::Net::DatagramSocket peer;
    ExpectTrue(peer.Bind("127.0.0.1", 0).IsOk(), "bind raw client");
    auto endpoint = ServerCore::Core::IpEndpoint::Parse("127.0.0.1", local.port).Value();
    sc_udp_token token{};
    ExpectTrue(sc_udp_transport_register(raw, 7, &token) == SC_OK, "C token registration");
    ExpectTrue(peer.Send(endpoint, Packet(token, 1)).IsOk(), "send original packet");
    auto first = Next(raw);
    if (!first)
        return;
    auto view = View<sc_udp_event_view>();
    ExpectTrue(sc_udp_event_get(first.get(), &view) == SC_OK && view.session_id == 7 &&
                   Text(view.payload) == Body,
        "C owned view values");
    ExpectTrue(peer.Send(endpoint, Packet(token, 2)).IsOk(), "send packet while event held");
    auto metrics = View<sc_udp_metrics>();
    auto deadline = std::chrono::steady_clock::now() + 5s;
    do
    {
        ExpectTrue(sc_udp_transport_get_metrics(raw, &metrics) == SC_OK, "read UDP metrics");
        if (metrics.event_queue_drops)
            break;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    ExpectTrue(metrics.event_queue_drops == 1 && metrics.retained_events == 1,
        "held event retains bounded admission credit");
    first.reset();
    ExpectTrue(peer.Send(endpoint, Packet(token, 2)).IsOk(), "retry uncommitted sequence");
    auto second = Next(raw);
    if (!second)
        return;
    ExpectTrue(sc_udp_event_get(second.get(), &view) == SC_OK && view.sequence == 2,
        "overflow did not commit replay sequence");
    ExpectTrue(sc_udp_transport_stop(raw) == SC_OK, "stop UDP quiescent");
    auto closed = Next(raw);
    ExpectTrue(
        closed && sc_udp_event_get(closed.get(), &view) == SC_OK && view.kind == SC_UDP_CLOSED,
        "reserved terminal bypasses full event credit");
    auto observation = View<sc_observation>();
    ExpectTrue(sc_udp_transport_get_observation(raw, &observation) == SC_OK,
        "UDP common observation adapter");
    owner.reset();
    ExpectTrue(sc_udp_event_get(second.get(), &view) == SC_OK && Text(view.payload) == Body,
        "event bytes survive owner destruction");
    EndpointAndSequencePolicy();
}
void CodecValidation()
{
    sc_udp_options options{};
    ExpectTrue(
        sc_udp_options_init(nullptr, sizeof(options)) == SC_INVALID_ARGUMENT, "null init rejected");
    ExpectTrue(sc_udp_options_init(&options, sizeof(options) - 1) == SC_INVALID_ARGUMENT,
        "short init rejected");
    sc_udp_token token{};
    token.bytes[0] = 42;
    auto packet = Packet(token, 9);
    auto stale = View<sc_udp_packet_view>();
    stale.struct_size = 0;
    ExpectTrue(
        sc_udp_packet_decode({ reinterpret_cast<const uint8_t*>(packet.data()), packet.size() },
            &stale) == SC_INVALID_ARGUMENT,
        "packet decode requires an initialized output header");
    auto view = View<sc_udp_packet_view>();
    ExpectTrue(
        sc_udp_packet_decode(
            { reinterpret_cast<const uint8_t*>(packet.data()), packet.size() }, &view) == SC_OK &&
            view.sequence == 9 && view.token.bytes[0] == 42 && Text(view.payload) == Body,
        "shared wire codec round trip");
    std::array<uint8_t, 4> sentinel{ 1, 2, 3, 4 };
    size_t written = 0;
    ExpectTrue(sc_udp_packet_encode(&token, 9, Bytes(Body), sentinel.data(), sentinel.size(),
                   &written) == SC_TOO_LARGE &&
                   sentinel[0] == 1 && written == packet.size(),
        "size probe reports full packet without partial writes");
    // 페이로드가 쓰일 출력 구간과 겹치면 정의된 결과가 없으므로 거절한다(CABI-5). 쓰이지 않는
    // 뒤쪽 공간에 놓인 페이로드는 겹침이 아니다.
    std::array<uint8_t, 256> shared{};
    std::copy(Body.begin(), Body.end(), shared.begin() + packet.size());
    const auto encodeFrom = [&](std::size_t offset)
    {
        written = 0;
        return sc_udp_packet_encode(&token, 9, { shared.data() + offset, Body.size() },
            shared.data(), shared.size(), &written);
    };
    const auto header = packet.size() - Body.size();
    ExpectTrue(encodeFrom(header) == SC_INVALID_ARGUMENT && written == 0,
        "payload already in place after the header is rejected as overlapping");
    ExpectTrue(encodeFrom(10) == SC_INVALID_ARGUMENT && written == 0,
        "payload overlapping the header is rejected");
    ExpectTrue(encodeFrom(packet.size()) == SC_OK && written == packet.size(),
        "payload beyond the written range is not an overlap");
    packet[0] = std::byte{ 0 };
    ExpectTrue(
        sc_udp_packet_decode({ reinterpret_cast<const uint8_t*>(packet.data()), packet.size() },
            &view) == SC_INVALID_FORMAT,
        "bad magic rejected by same codec");
}
const ServerCoreTest::CheckRegistration a{ "CAbi.DatagramOwnedEventsAndBackpressure", Ownership };
const ServerCoreTest::CheckRegistration b{ "CAbi.DatagramCodecAndValidation", CodecValidation };
}
