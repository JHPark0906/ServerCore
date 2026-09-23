#include "ServerCore/C/Datagram.h"
#include "C/EndpointInternal.h"
#include "C/Internal.h"
#include "C/ObservationInternal.h"
#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Runtime/DatagramTransport.h"
#include <atomic>
#include <cstring>

namespace C = ServerCore::CDetail;
namespace R = ServerCore::Runtime;
namespace P = ServerCore::Protocol;
namespace Core = ServerCore::Core;
using SessionId = ServerCore::Session::SessionId;
struct sc_udp_event
{
    std::shared_ptr<C::Budget::Lease> charge;
    uint32_t kind = SC_UDP_MESSAGE, mode = SC_UDP_JSON, type = 0;
    sc_status status = SC_OK;
    uint64_t session = 0, sequence = 0;
    Core::IpEndpoint endpoint;
    std::string payload;
};
namespace
{
struct UdpState : std::enable_shared_from_this<UdpState>
{
    UdpState(const sc_udp_options& value)
        : address(C::Text(value.listen_address))
        , options(value)
        , budget(std::make_shared<C::Budget>(value.max_event_count, value.max_event_bytes))
        , executor(std::make_shared<R::TaskExecutor>())
        , terminal(std::make_unique<sc_udp_event>())
    {
        terminal->kind = SC_UDP_CLOSED;
        terminal->status = SC_CLOSED;
    }
    ~UdpState() { (void)Stop(); }
    bool Admit(const R::DatagramMessageView& view)
    {
        std::unique_ptr<sc_udp_event> previous;
        {
            const std::lock_guard guard(candidateMutex);
            previous = std::move(candidate);
        }
        previous.reset();
        if (stopping.load())
            return false;
        auto payload = view.payload;
        uint32_t type = 0;
        if (view.payloadMode == P::PayloadMode::Binary)
        {
            const auto decoded = P::DecodeBinaryMessage(payload);
            if (!decoded.IsOk())
                return false;
            type = decoded.Value().type;
            payload = decoded.Value().payload;
        }
        auto charge = budget->Acquire(sizeof(sc_udp_event) + payload.size());
        if (!charge)
        {
            ++queueDrops;
            return false;
        }
        auto event = std::make_unique<sc_udp_event>();
        event->charge = std::move(charge);
        event->mode = view.payloadMode == P::PayloadMode::Json ? SC_UDP_JSON : SC_UDP_BINARY;
        event->type = type;
        event->session = static_cast<uint64_t>(view.session);
        event->sequence = view.sequence;
        event->endpoint = view.remoteEndpoint;
        if (!payload.empty())
            event->payload.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
        {
            const std::lock_guard guard(candidateMutex);
            candidate = std::move(event);
        }
        return true;
    }
    void Receive(const R::DatagramMessageView&)
    {
        std::unique_ptr<sc_udp_event> event;
        {
            const std::lock_guard guard(candidateMutex);
            event = std::move(candidate);
        }
        if (event && !events.Push(std::move(event)))
            ++queueDrops;
    }
    sc_status Start()
    {
        const std::lock_guard guard(lifecycle);
        if (used || stopping.load())
            return SC_CLOSED;
        used = true;
        R::DatagramTransportOptions configuration;
        configuration.maxRegisteredSessions = options.max_sessions;
        configuration.payloadMode =
            options.payload_mode == SC_UDP_JSON ? P::PayloadMode::Json : P::PayloadMode::Binary;
        configuration.totalSendRate = { options.total_send_rate.bytes_per_interval,
            options.total_send_rate.burst_bytes,
            std::chrono::milliseconds(options.total_send_rate.interval_ms) };
        configuration.peerSendRate = { options.peer_send_rate.bytes_per_interval,
            options.peer_send_rate.burst_bytes,
            std::chrono::milliseconds(options.peer_send_rate.interval_ms) };
        auto status = transport.Configure(configuration);
        if (!status.IsOk())
            return C::Code(status);
        auto endpoint = Core::IpEndpoint::Parse(address, options.port);
        if (!endpoint.IsOk())
            return C::Code(endpoint.GetStatus());
        status = transport.Bind(endpoint.Value(), options.ipv6_only != 0);
        if (!status.IsOk())
            return C::Code(status);
        status = executor->Start({ 1, 1, 4096 });
        if (status.IsOk())
        {
            R::DatagramReceiveOptions receiveOptions;
            receiveOptions.budget = { options.max_datagrams_per_batch,
                options.max_bytes_per_batch };
            receiveOptions.retainedCallbackBytes = sizeof(UdpState*) * 2;
            status = transport.StartReceiving(
                executor,
                [weak = weak_from_this()](const auto& view)
                {
                    auto self = weak.lock();
                    return self && self->Admit(view);
                },
                [weak = weak_from_this()](const auto& view)
                {
                    if (auto self = weak.lock())
                        self->Receive(view);
                },
                receiveOptions);
        }
        if (!status.IsOk())
        {
            transport.Close();
            (void)executor->Stop();
        }
        return C::Code(status);
    }
    sc_status Stop()
    {
        const std::lock_guard guard(lifecycle);
        if (stopping.exchange(true))
            return SC_OK;
        auto status = transport.StopReceiving();
        transport.Close();
        const auto stopped = executor->Stop();
        std::unique_ptr<sc_udp_event> retired;
        {
            const std::lock_guard candidateGuard(candidateMutex);
            retired = std::move(candidate);
        }
        events.Finish(std::move(terminal));
        return !status.IsOk() ? C::Code(status) : C::Code(stopped);
    }
    const std::string address;
    const sc_udp_options options;
    std::mutex lifecycle, candidateMutex;
    std::atomic<bool> stopping{ false };
    bool used = false;
    std::atomic<uint64_t> queueDrops{ 0 };
    std::shared_ptr<C::Budget> budget;
    std::shared_ptr<R::TaskExecutor> executor;
    R::DatagramTransport transport;
    C::PullQueue<sc_udp_event> events;
    std::unique_ptr<sc_udp_event> candidate, terminal;
};
}
struct sc_udp_transport
{
    std::shared_ptr<UdpState> state;
};
extern "C"
{
    sc_status sc_udp_options_init(sc_udp_options* options, size_t size)
    {
        if (!options || size < sizeof(*options))
            return SC_INVALID_ARGUMENT;
        *options = {};
        options->abi_version = SC_ABI_VERSION;
        options->struct_size = sizeof(*options);
        options->listen_address = C::View("127.0.0.1");
        options->ipv6_only = 1;
        options->max_sessions = 256;
        options->max_event_count = 1024;
        options->max_event_bytes = 4 * 1024 * 1024;
        options->max_datagrams_per_batch = 256;
        options->max_bytes_per_batch = 256 * 1024;
        options->total_send_rate.interval_ms = options->peer_send_rate.interval_ms = 1000;
        return SC_OK;
    }
    sc_status sc_udp_transport_create(const sc_udp_options* options, sc_udp_transport** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Version(options) || !C::Valid(options->listen_address) ||
            options->listen_address.len > 64 || options->reserved || options->ipv6_only > 1 ||
            options->payload_mode > 1 || !options->max_sessions || options->max_sessions > 65536 ||
            !options->max_event_count || options->max_event_count > 65536 ||
            !options->max_event_bytes || options->max_event_bytes > 1024 * 1024 * 1024 ||
            !options->max_datagrams_per_batch || options->max_datagrams_per_batch > 65536 ||
            !options->max_bytes_per_batch || options->max_bytes_per_batch > 64 * 1024 * 1024 ||
            options->total_send_rate.reserved || options->peer_send_rate.reserved)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto value = std::make_unique<sc_udp_transport>();
                value->state = std::make_shared<UdpState>(*options);
                *out = value.release();
                return SC_OK;
            });
    }
    sc_status sc_udp_transport_start(sc_udp_transport* value)
    {
        if (!value)
            return SC_INVALID_ARGUMENT;
        return C::Protect([&] { return value->state->Start(); });
    }
    sc_status sc_udp_transport_stop(sc_udp_transport* value)
    {
        if (!value)
            return SC_INVALID_ARGUMENT;
        return C::Protect([&] { return value->state->Stop(); });
    }
    void sc_udp_transport_destroy(sc_udp_transport* value)
    {
        if (value)
        {
            (void)sc_udp_transport_stop(value);
            delete value;
        }
    }
    sc_status sc_udp_transport_local_endpoint(const sc_udp_transport* value, sc_ip_endpoint* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        const auto endpoint = value->state->transport.LocalEndpoint();
        if (!endpoint.IsValid())
            return SC_CLOSED;
        *out = C::FromEndpoint(endpoint);
        return SC_OK;
    }
    sc_status sc_udp_transport_remote_endpoint(
        const sc_udp_transport* value, uint64_t session, sc_ip_endpoint* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto endpoint =
                    value->state->transport.RemoteEndpoint(static_cast<SessionId>(session));
                if (!endpoint.IsOk())
                    return C::Code(endpoint.GetStatus());
                *out = C::FromEndpoint(endpoint.Value());
                return SC_OK;
            });
    }
    sc_status sc_udp_transport_register(
        sc_udp_transport* value, uint64_t session, sc_udp_token* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto token =
                    value->state->transport.RegisterSession(static_cast<SessionId>(session));
                if (!token.IsOk())
                    return C::Code(token.GetStatus());
                std::memcpy(out->bytes, token.Value().data(), 16);
                return SC_OK;
            });
    }
    void sc_udp_transport_unregister(sc_udp_transport* value, uint64_t session)
    {
        if (!value)
            return;
        value->state->transport.UnregisterSession(static_cast<SessionId>(session));
    }
    sc_status sc_udp_transport_send_json(sc_udp_transport* value, uint64_t session, sc_bytes bytes)
    {
        if (!value || !C::Valid(bytes))
            return SC_INVALID_ARGUMENT;
        return C::Code(value->state->transport.SendSerialized(
            static_cast<SessionId>(session), C::Bytes(bytes)));
    }
    sc_status sc_udp_transport_send_binary(
        sc_udp_transport* value, uint64_t session, uint32_t type, sc_bytes bytes)
    {
        if (!value || !C::Valid(bytes))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]
            {
                return C::Code(value->state->transport.SendBinary(
                    static_cast<SessionId>(session), type, C::Bytes(bytes)));
            });
    }
    sc_status sc_udp_transport_next(sc_udp_transport* value, uint32_t timeout, sc_udp_event** out)
    {
        if (!value)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect([&] { return value->state->events.Next(timeout, out); });
    }
    sc_status sc_udp_transport_subscribe(
        sc_udp_transport* value, sc_notifier* notifier, uint64_t key, sc_subscription** out)
    {
        if (!value)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::Protect([&] { return value->state->events.Subscribe(notifier, key, out); });
    }
    sc_status sc_udp_event_get(const sc_udp_event* value, sc_udp_event_view* out)
    {
        if (!value || !C::Version(out))
            return SC_INVALID_ARGUMENT;
        out->kind = value->kind;
        out->payload_mode = value->mode;
        out->status = value->status;
        out->binary_type = value->type;
        out->session_id = value->session;
        out->sequence = value->sequence;
        out->remote_endpoint = C::FromEndpoint(value->endpoint);
        out->payload = C::View(value->payload);
        return SC_OK;
    }
    void sc_udp_event_destroy(sc_udp_event* value)
    {
        delete value;
    }
    sc_status sc_udp_transport_get_metrics(const sc_udp_transport* value, sc_udp_metrics* out)
    {
        if (!value || !C::Version(out))
            return SC_INVALID_ARGUMENT;
        const auto m = value->state->transport.SnapshotMetrics();
        out->received_datagrams = m.receivedDatagrams;
        out->received_bytes = m.receivedBytes;
        out->sent_datagrams = m.sentDatagrams;
        out->sent_bytes = m.sentBytes;
        out->rejected_datagrams = m.rejectedDatagrams;
        out->send_would_block = m.sendWouldBlock;
        out->socket_errors = m.socketErrors;
        out->truncated_datagrams = m.truncatedDatagrams;
        out->malformed_datagrams = m.malformedDatagrams;
        out->unknown_token_datagrams = m.unknownTokenDatagrams;
        out->replayed_datagrams = m.replayedDatagrams;
        out->invalid_payload_datagrams = m.invalidPayloadDatagrams;
        out->admission_rejected_datagrams = m.admissionRejectedDatagrams;
        out->stale_datagrams = m.staleDatagrams;
        out->callback_failures = m.callbackFailures;
        out->send_rate_limited = m.sendRateLimited;
        out->send_limited_global = m.sendLimitedGlobal;
        out->send_limited_peer = m.sendLimitedPeer;
        out->send_not_ready = m.sendNotReady;
        out->pump_batches = m.pumpBatches;
        out->pump_failures = m.pumpFailures;
        out->poll_contentions = m.pollContentions;
        out->registered_sessions = m.registeredSessions;
        out->pending_batches = m.pendingBatches;
        out->bound = m.bound ? 1u : 0u;
        out->receiving = m.receiving ? 1u : 0u;
        out->event_queue_drops = value->state->queueDrops.load();
        const std::lock_guard guard(value->state->budget->mutex);
        out->retained_events = value->state->budget->count;
        out->retained_event_bytes = value->state->budget->bytes;
        return SC_OK;
    }
    sc_status sc_udp_transport_get_observation(const sc_udp_transport* value, sc_observation* out)
    {
        if (!value)
            return SC_INVALID_ARGUMENT;
        auto snapshot = ServerCore::Observability::Observe(value->state->transport);
        {
            const std::lock_guard guard(value->state->budget->mutex);
            snapshot.retainedBytes = value->state->budget->bytes;
            snapshot.available |= ServerCore::Observability::RetainedBytes;
        }
        return C::CopyObservation(snapshot, out);
    }
    sc_status sc_udp_packet_encode(const sc_udp_token* token, uint64_t sequence, sc_bytes payload,
        uint8_t* output, size_t capacity, size_t* written)
    {
        if (!token || !written || !C::Valid(payload) || (!output && capacity))
            return SC_INVALID_ARGUMENT;
        *written = 0;
        if (!sequence || payload.len == 0)
            return SC_INVALID_ARGUMENT;
        if (payload.len > P::DatagramCodec::MaximumPayloadBytes)
            return SC_TOO_LARGE;
        *written = P::DatagramCodec::HeaderBytes + payload.len;
        if (capacity < *written)
            return SC_TOO_LARGE;
        P::DatagramCodec::Token native;
        std::memcpy(native.data(), token->bytes, 16);
        return P::DatagramCodec::Encode({ reinterpret_cast<std::byte*>(output), capacity }, native,
                   sequence, C::Bytes(payload))
                   ? SC_OK
                   : SC_INVALID_ARGUMENT;
    }
    sc_status sc_udp_packet_decode(sc_bytes packet, sc_udp_packet_view* out)
    {
        if (!out || !C::Valid(packet))
            return SC_INVALID_ARGUMENT;
        const auto decoded = P::DatagramCodec::Decode(C::Bytes(packet));
        if (!decoded)
            return SC_INVALID_FORMAT;
        std::memcpy(out->token.bytes, decoded->token.data(), 16);
        out->sequence = decoded->sequence;
        out->payload = { reinterpret_cast<const uint8_t*>(decoded->payload.data()),
            decoded->payload.size() };
        return SC_OK;
    }
}
