#include "AbiSocketTestSupport.h"
#include "ServerCore/C/Observability.h"
#include "ServerCore/C/Net.h"
#include "ServerCore/C/Runtime.h"
#include "ServerCore/C/Web.h"

namespace
{
using namespace AbiTest;
using ServerCoreTest::ExpectTrue;
void ObservationContracts()
{
    ServerCoreTest::SocketRuntime sockets;
    if (!Check(sockets.IsReady(), "observation sockets initialize")) return;
    sc_observation observation{};
    ExpectTrue(sc_observation_init(&observation, sizeof(observation)-1) == SC_INVALID_ARGUMENT,
        "C observation output size is version checked");
    ExpectTrue(sc_observation_init(&observation, sizeof(observation)) == SC_OK, "C observation output initializes");
    sc_tcp_options options{}; (void)sc_tcp_options_init(&options, sizeof(options));
    options.port = FreePort(); options.connection_send_bytes = 64;
    sc_tcp_server* raw = nullptr;
    if (!Check(sc_tcp_server_create(&options, &raw) == SC_OK, "observed TCP owner creates")) return;
    Handle<sc_tcp_server, sc_tcp_server_destroy> server(raw, sc_tcp_server_destroy);
    ExpectTrue(sc_tcp_server_get_observation(raw, &observation) == SC_OK && observation.lifecycle == SC_LIFECYCLE_CREATED,
        "C TCP distinguishes unstarted owner");
    if (!Check(sc_tcp_server_start(raw) == SC_OK, "observed TCP listener starts")) return;
    Peer peer; if (!peer.Connect(sc_tcp_server_port(raw))) return;
    sc_tcp_connection* accepted = nullptr;
    if (!Check(sc_tcp_server_accept(raw, 5000, &accepted) == SC_OK, "observed TCP listener accepts")) return;
    Handle<sc_tcp_connection, sc_tcp_connection_destroy> connection(accepted, sc_tcp_connection_destroy);
    ExpectTrue(sc_tcp_server_get_observation(raw, &observation) == SC_OK && observation.connections == 1 &&
        observation.protocol == SC_PROTOCOL_TCP && observation.lifecycle == SC_LIFECYCLE_RUNNING, "actual TCP connection gauge is visible");
    const std::string oversized(128, 'a');
    ExpectTrue(sc_tcp_connection_send(accepted, Bytes(oversized)) == SC_WOULD_BLOCK, "capacity refusal is deterministic");
    ExpectTrue(sc_tcp_server_get_observation(raw, &observation) == SC_OK && observation.rejected[SC_REASON_CAPACITY] == 1,
        "capacity event uses the common fixed reason without text parsing");
    ExpectTrue(sc_tcp_server_stop(raw) == SC_OK, "observed TCP stops");
    ExpectTrue(sc_tcp_server_get_observation(raw, &observation) == SC_OK && observation.lifecycle == SC_LIFECYCLE_STOPPED &&
        observation.connections == 0 && observation.drain_remaining == 0 && observation.closed[SC_REASON_UNKNOWN] == 1,
        "local Close lacking a richer native reason is honestly Unknown");
    sc_executor_options executorOptions{}; (void)sc_executor_options_init(&executorOptions, sizeof(executorOptions));
    sc_executor* executor = nullptr;
    if (!Check(sc_executor_create(&executorOptions, &executor) == SC_OK, "observed executor creates")) return;
    Handle<sc_executor, sc_executor_destroy> executorOwner(executor, sc_executor_destroy);
    ExpectTrue(sc_executor_get_observation(executor, &observation) == SC_OK && observation.protocol == SC_PROTOCOL_TASK &&
        !(observation.available & SC_OBSERVATION_CONNECTIONS), "work executor does not invent connection gauges");
    ExpectTrue(sc_executor_stop(executor) == SC_OK && sc_executor_get_observation(executor, &observation) == SC_OK &&
        observation.lifecycle == SC_LIFECYCLE_STOPPED, "C executor stop is observable");
    sc_web_options webOptions{}; (void)sc_web_options_init(&webOptions, sizeof(webOptions)); webOptions.port = FreePort();
    sc_web_server* web = nullptr;
    if (!Check(sc_web_server_create(&webOptions, &web) == SC_OK, "observed HTTP owner creates")) return;
    Handle<sc_web_server, sc_web_server_destroy> webOwner(web, sc_web_server_destroy);
    ExpectTrue(sc_web_server_route(web, Bytes("POST"), Bytes("/held"), 0) == SC_OK, "observed HTTP route registers");
    if (!Check(sc_web_server_start(web) == SC_OK, "observed HTTP starts")) return;
    Peer http;
    const std::string body(8192, 'b');
    if (!http.Connect(sc_web_server_port(web)) || !http.Send(Http("POST", "/held", body))) return;
    sc_web_event* event = nullptr;
    if (!Check(sc_web_server_next(web, 5000, &event) == SC_OK, "held HTTP event is delivered")) return;
    Handle<sc_web_event, sc_web_event_destroy> eventOwner(event, sc_web_event_destroy);
    ExpectTrue(sc_web_server_stop(web) == SC_OK, "HTTP stops while a consumer retains an event");
    ExpectTrue(sc_web_server_get_observation(web, &observation) == SC_OK && observation.lifecycle == SC_LIFECYCLE_STOPPED &&
        observation.retained_bytes >= body.size() && observation.retained_bytes < body.size() * 2,
        "shared native request storage is retained once, not charged again for handler and C event aliases");
    eventOwner.reset();
    ExpectTrue(sc_web_server_get_observation(web, &observation) == SC_OK && observation.retained_bytes == 0,
        "C event destruction releases wrapper and native context storage before observation returns zero");
}
const ServerCoreTest::CheckRegistration observation("CAbi.ObservationContracts", ObservationContracts);
}
