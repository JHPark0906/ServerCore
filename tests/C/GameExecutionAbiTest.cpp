#include "AbiSocketTestSupport.h"
#include "ServerCore/C/GameExecution.h"
#include <atomic>

namespace
{
using namespace AbiTest;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
using Executor = Handle<sc_executor, sc_executor_destroy>;
using Scheduler = Handle<sc_timer_scheduler, sc_timer_scheduler_destroy>;
using Tick = Handle<sc_tick, sc_tick_destroy>;
using Queue = Handle<sc_outbound_queue, sc_outbound_destroy>;
struct RuntimeFixture
{
    Executor executor{ nullptr, sc_executor_destroy };
    Scheduler scheduler{ nullptr, sc_timer_scheduler_destroy };
    RuntimeFixture()
    {
        sc_executor_options options{};
        (void)sc_executor_options_init(&options, sizeof options);
        sc_executor* raw = nullptr;
        (void)sc_executor_create(&options, &raw);
        executor.reset(raw);
        sc_timer_scheduler_options timers{};
        (void)sc_timer_scheduler_options_init(&timers, sizeof timers);
        sc_timer_scheduler* clock = nullptr;
        (void)sc_timer_scheduler_create(raw, &timers, &clock);
        scheduler.reset(clock);
    }
    ~RuntimeFixture()
    {
        if (scheduler)
            (void)sc_timer_scheduler_stop(scheduler.get());
        if (executor)
            (void)sc_executor_stop(executor.get());
    }
};
struct TickState
{
    std::atomic<unsigned> calls{ 0 }, released{ 0 };
    std::atomic<int> shutdown{ SC_OK };
};
using TickOwner = std::shared_ptr<TickState>;
sc_status OnTick(void* raw, const sc_tick_info* tick, const sc_tick_token* token)
{
    const auto state = *static_cast<TickOwner*>(raw);
    if (!tick || !tick->index || sc_tick_token_requested(token))
        return SC_INVALID_ARGUMENT;
    state->shutdown = sc_runtime_shutdown(0);
    return ++state->calls == 3 ? SC_CLOSED : SC_OK;
}
void ReleaseTick(void* raw)
{
    auto* owner = static_cast<TickOwner*>(raw);
    ++(*owner)->released;
    delete owner;
}
void LogicalTicksOwnership()
{
    RuntimeFixture runtime;
    if (!Check(runtime.executor && runtime.scheduler, "C tick scheduler created"))
        return;
    auto state = std::make_shared<TickState>();
    sc_tick_options options{};
    (void)sc_tick_options_init(&options, sizeof options);
    options.interval_ms = 2;
    sc_tick* raw = nullptr;
    if (!Check(sc_tick_start(runtime.scheduler.get(), &options,
                   { new TickOwner(state), OnTick, ReleaseTick }, &raw) == SC_OK &&
                   raw,
            "C logical ticks admitted"))
        return;
    Tick tick(raw, sc_tick_destroy);
    runtime.executor.reset(); // Dependent scheduler/tick owns the executor state.
    ExpectEqual(sc_status{ SC_CLOSED }, sc_tick_wait(tick.get(), 3000),
        "callback error ends recurring ticks");
    // Explicit stop also guarantees safe assertion-failure cleanup if the timed wait failed.
    sc_tick_cancel(tick.get());
    (void)sc_timer_scheduler_stop(runtime.scheduler.get());
    ExpectTrue(sc_tick_finished(tick.get()) != 0, "tick terminal observes native callback cleanup");
    ExpectEqual(3u, state->calls.load(), "logical tick callback identity persists");
    ExpectEqual(
        1u, state->released.load(), "foreign context released exactly once after final tick");
    ExpectEqual(int{ SC_INVALID_ARGUMENT }, state->shutdown.load(),
        "tick callbacks participate in runtime self-shutdown guard");
    sc_tick_metrics metrics{};
    ExpectTrue(sc_tick_get_metrics(tick.get(), &metrics) == SC_OK && metrics.executed == 3,
        "C metrics match completed ticks");
    auto rejected = std::make_shared<TickState>();
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_tick_start(nullptr, nullptr, { new TickOwner(rejected), OnTick, ReleaseTick }, nullptr),
        "invalid tick submission rejected");
    ExpectEqual(
        1u, rejected->released.load(), "rejected tick still consumes transferred callback context");
}
void OutboundOwnershipAndBatch()
{
    ServerCoreTest::SocketRuntime sockets;
    if (!Check(sockets.IsReady(), "outbound raw peer socket runtime ready"))
        return;
    RuntimeFixture runtime;
    if (!Check(runtime.scheduler != nullptr, "outbound scheduler ready"))
        return;
    sc_tcp_options options{};
    (void)sc_tcp_options_init(&options, sizeof options);
    options.port = FreePort();
    sc_tcp_server* rawServer = nullptr;
    if (!Check(sc_tcp_server_create(&options, &rawServer) == SC_OK && rawServer,
            "outbound TCP server created"))
        return;
    Handle<sc_tcp_server, sc_tcp_server_destroy> server(rawServer, sc_tcp_server_destroy);
    if (!Check(sc_tcp_server_start(server.get()) == SC_OK, "outbound TCP server started"))
        return;
    Peer peer;
    if (!peer.Connect(sc_tcp_server_port(server.get())))
        return;
    sc_tcp_connection* rawConnection = nullptr;
    if (!Check(sc_tcp_server_accept(server.get(), 3000, &rawConnection) == SC_OK && rawConnection,
            "outbound connection accepted"))
        return;
    Handle<sc_tcp_connection, sc_tcp_connection_destroy> connection(
        rawConnection, sc_tcp_connection_destroy);
    sc_outbound_options limits{};
    (void)sc_outbound_options_init(&limits, sizeof limits);
    limits.max_retained_bytes = 8;
    sc_outbound_queue* rawQueue = nullptr;
    if (!Check(sc_outbound_create(runtime.scheduler.get(), connection.get(), &limits, &rawQueue) ==
                       SC_OK &&
                   rawQueue,
            "C outbound queue created"))
        return;
    Queue queue(rawQueue, sc_outbound_destroy);
    connection.reset(); // Queue retains public connection ownership, not just socket internals.
    sc_outbound_item_options item{};
    (void)sc_outbound_item_options_init(&item, sizeof item);
    ExpectEqual(sc_status{ SC_TOO_LARGE },
        sc_outbound_enqueue(queue.get(), Bytes("123456789"), &item),
        "per-queue wire byte cap checked before admission");
    std::string payload = "copy";
    ExpectEqual(sc_status{ SC_OK }, sc_outbound_enqueue(queue.get(), Bytes(payload), &item),
        "queue accepts copied raw TCP bytes");
    payload.assign(4, 'x');
    ExpectEqual(std::string("copy"), peer.Read(4),
        "input mutation and original connection destruction cannot change queued bytes");
    sc_outbound_queue* recipients[]{ queue.get(), nullptr, queue.get() };
    sc_status results[3]{};
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_outbound_batch(recipients, 3, Bytes("z"), &item, results, 2),
        "short result span rejects batch before sends");
    ExpectEqual(sc_status{ SC_OK }, sc_outbound_batch(recipients, 3, Bytes("z"), &item, results, 3),
        "bounded batch returns per-recipient results");
    ExpectTrue(results[0] == SC_OK && results[1] == SC_INVALID_ARGUMENT && results[2] == SC_OK,
        "duplicate recipient receives each admitted item");
    ExpectEqual(
        std::string("zz"), peer.Read(2), "batch has no hidden all-or-nothing send semantics");
    sc_outbound_begin_drain(queue.get());
    ExpectTrue(sc_outbound_finished(queue.get()) && sc_outbound_result(queue.get()) == SC_OK,
        "drain finishes after transport admission");
    ExpectEqual(sc_status{ SC_CLOSED }, sc_outbound_enqueue(queue.get(), Bytes("x"), &item),
        "drain permanently closes staging admission");
    sc_outbound_metrics metrics{};
    ExpectTrue(sc_outbound_get_metrics(queue.get(), &metrics) == SC_OK && metrics.sent == 3 &&
                   !metrics.pending && !metrics.retained_bytes,
        "C queue accounting covers all three accepted messages");
    queue.reset();
    (void)sc_tcp_server_stop(server.get());
}
const ServerCoreTest::CheckRegistration ticks("CAbi.LogicalTicksOwnership", LogicalTicksOwnership);
const ServerCoreTest::CheckRegistration outbound(
    "CAbi.OutboundOwnershipAndBatch", OutboundOwnershipAndBatch);
}
