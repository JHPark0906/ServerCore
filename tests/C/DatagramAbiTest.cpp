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
    sc_udp_packet_view view{};
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
    packet[0] = std::byte{ 0 };
    ExpectTrue(
        sc_udp_packet_decode({ reinterpret_cast<const uint8_t*>(packet.data()), packet.size() },
            &view) == SC_INVALID_FORMAT,
        "bad magic rejected by same codec");
}
const ServerCoreTest::CheckRegistration a{ "CAbi.DatagramOwnedEventsAndBackpressure", Ownership };
const ServerCoreTest::CheckRegistration b{ "CAbi.DatagramCodecAndValidation", CodecValidation };
}
