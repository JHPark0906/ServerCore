#include "AbiAllocationFailure.h"
#include "AbiSocketTestSupport.h"
#include "ServerCore/C/GameExecution.h"
#include <atomic>
#include <cstdlib>
#include <new>
#include <string_view>

#if SC_TEST_ALLOCATION_HOOK
namespace
{
/// <summary>이 스레드에 실패 주입이 걸려 있는가.</summary>
constinit thread_local bool tAllocationArmed = false;
/// <summary>걸려 있을 때 실패 전에 더 통과시킬 할당 수.</summary>
constinit thread_local std::size_t tAllocationSkip = 0;
/// <summary>걸린 뒤 실패를 실제로 일으켰는가.</summary>
constinit thread_local bool tAllocationFired = false;
}

/// <summary>
/// 시험 실행 파일 전체의 전역 operator new 대체다. 주입이 걸려 있지 않으면 malloc으로 내려가는
/// 표준 동작과 같다. 배열·nothrow 형태의 기본 구현은 이 함수를 부르므로 함께 주입 대상이 된다.
/// 정렬 형태는 대체하지 않는다.
/// </summary>
void* operator new(std::size_t size)
{
    if (tAllocationArmed)
    {
        if (tAllocationSkip == 0)
        {
            tAllocationArmed = false;
            tAllocationFired = true;
            throw std::bad_alloc();
        }
        --tAllocationSkip;
    }
    for (;;)
    {
        if (void* memory = std::malloc(size != 0 ? size : 1))
            return memory;
        const auto handler = std::get_new_handler();
        if (!handler)
            throw std::bad_alloc();
        handler();
    }
}
void operator delete(void* memory) noexcept
{
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
#endif

namespace AbiTest
{
#if SC_TEST_ALLOCATION_HOOK
FailNthAllocation::FailNthAllocation(std::size_t skip) noexcept
{
    tAllocationSkip = skip;
    tAllocationFired = false;
    tAllocationArmed = true;
}
FailNthAllocation::~FailNthAllocation()
{
    tAllocationArmed = false;
}
bool FailNthAllocation::Fired() const noexcept
{
    return tAllocationFired;
}
bool AllocationFailureReachesLibrary()
{
    sc_status status = SC_OK;
    sc_notifier* notifier = nullptr;
    bool escaped = false;
    const bool fired = InjectAllocationFailure(
        0, [&] { status = Opaque(&sc_notifier_create)(1, &notifier); }, escaped);
    if (notifier)
        sc_notifier_destroy(notifier);
    return Check(fired && !escaped && status == SC_PLATFORM_ERROR && !notifier,
        "allocation failure injection reaches the ServerCore binary");
}
#else
FailNthAllocation::FailNthAllocation(std::size_t) noexcept {}
FailNthAllocation::~FailNthAllocation() = default;
bool FailNthAllocation::Fired() const noexcept
{
    return false;
}
bool AllocationFailureReachesLibrary()
{
    return false;
}
#endif
}

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
    if (!tick || tick->abi_version != SC_ABI_VERSION || tick->struct_size < sizeof(sc_tick_info) ||
        !tick->index || sc_tick_token_requested(token))
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
using Notifier = Handle<sc_notifier, sc_notifier_destroy>;
/// <summary>
/// 구독 호출 하나를 할당 실패 지점마다 한 번씩 되풀이한다.
/// </summary>
/// <remarks>
/// 시도마다 확인하는 것: C++ 예외가 ABI 밖으로 새지 않는다. 주입한 실패는 성공이 아닌 상태 코드로
/// 돌아오고 출력은 비어 있다. 실패한 시도는 키를 점유하지 않는다 — 모든 시도가 같은 키를 쓰므로,
/// 점유가 남으면 마지막 시도가 AlreadyExists로 붉어진다. 주입이 한 번도 일어나지 않았거나 끝내
/// 성공에 이르지 못하면 그것도 실패로 적는다. 그 경로가 실제로 돌았음을 이 두 단언이 보장한다.
/// </remarks>
template <class Subscribe>
void SweepSubscribeAllocationFailures(std::string_view what, Subscribe&& subscribe)
{
    if (!AllocationFailureReachesLibrary())
        return;
    sc_notifier* rawNotifier = nullptr;
    if (!Check(sc_notifier_create(4, &rawNotifier) == SC_OK && rawNotifier,
            "allocation sweep notifier created"))
        return;
    Notifier notifier(rawNotifier, sc_notifier_destroy);
    const std::string name(what);
    std::size_t injected = 0;
    bool completed = false;
    for (std::size_t skip = 0; skip < 4096 && !completed; ++skip)
    {
        sc_status status = SC_OK;
        sc_subscription* subscription = nullptr;
        bool escaped = false;
        const bool fired = InjectAllocationFailure(
            skip, [&] { status = subscribe(notifier.get(), 7, &subscription); }, escaped);
        const bool produced = subscription != nullptr;
        sc_subscription_destroy(subscription);
        if (!Check(!escaped, name + " lets no C++ exception cross the ABI"))
            return;
        if (!Check((status == SC_OK) == produced, name + " output matches its status"))
            return;
        if (fired)
        {
            ++injected;
            if (!Check(status != SC_OK, name + " reports an injected allocation failure"))
                return;
        }
        else
        {
            ExpectEqual(sc_status{ SC_OK }, status,
                name + " succeeds with the same key after every failed attempt");
            completed = true;
        }
    }
    ExpectTrue(injected != 0, name + " sweep injected at least one allocation failure");
    ExpectTrue(completed, name + " sweep reached an attempt without injected failure");
}
sc_status IdleTick(void*, const sc_tick_info*, const sc_tick_token*)
{
    return SC_OK;
}
void TickSubscribeAllocationFailure()
{
    RuntimeFixture runtime;
    if (!Check(runtime.scheduler != nullptr, "allocation sweep tick scheduler created"))
        return;
    sc_tick_options options{};
    (void)sc_tick_options_init(&options, sizeof options);
    options.interval_ms = 60 * 60 * 1000; // 시험 동안 끝나지 않아 완료 등록 경로가 일정하다.
    sc_tick* raw = nullptr;
    if (!Check(sc_tick_start(runtime.scheduler.get(), &options, { nullptr, IdleTick, nullptr },
                   &raw) == SC_OK &&
                   raw,
            "allocation sweep tick started"))
        return;
    Tick tick(raw, sc_tick_destroy);
    SweepSubscribeAllocationFailures("sc_tick_subscribe",
        [&](sc_notifier* notifier, uint64_t key, sc_subscription** out)
        { return Opaque(&sc_tick_subscribe)(tick.get(), notifier, key, out); });
    sc_tick_cancel(tick.get());
    (void)sc_tick_wait(tick.get(), 3000);
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
    auto metrics = View<sc_tick_metrics>();
    ExpectTrue(sc_tick_get_metrics(tick.get(), &metrics) == SC_OK && metrics.executed == 3,
        "C metrics match completed ticks");
    auto stale = View<sc_tick_metrics>();
    stale.struct_size = 0;
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT }, sc_tick_get_metrics(tick.get(), &stale),
        "tick metrics require an initialized output header");
    auto rejected = std::make_shared<TickState>();
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_tick_start(nullptr, nullptr, { new TickOwner(rejected), OnTick, ReleaseTick }, nullptr),
        "invalid tick submission rejected");
    ExpectEqual(
        1u, rejected->released.load(), "rejected tick still consumes transferred callback context");
    TickSubscribeAllocationFailure();
}
void OutboundBatchAllocationFailure();
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
    SweepSubscribeAllocationFailures("sc_outbound_subscribe",
        [&](sc_notifier* notifier, uint64_t key, sc_subscription** out)
        { return Opaque(&sc_outbound_subscribe)(queue.get(), notifier, key, out); });
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
    auto metrics = View<sc_outbound_metrics>();
    auto stale = View<sc_outbound_metrics>();
    stale.abi_version = 1;
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT }, sc_outbound_get_metrics(queue.get(), &stale),
        "outbound metrics reject an ABI 1 output header");
    ExpectTrue(sc_outbound_get_metrics(queue.get(), &metrics) == SC_OK && metrics.sent == 3 &&
                   !metrics.pending && !metrics.retained_bytes,
        "C queue accounting covers all three accepted messages");
    ExpectEqual(std::uint64_t{ 0 }, metrics.deferred_wakes,
        "no wake is deferred while the shared scheduler has free slots");
    queue.reset();
    (void)sc_tcp_server_stop(server.get());
    OutboundBatchAllocationFailure();
}
/// <summary>시도마다 새로 여는 TCP 연결과 그 위의 송신 큐다.</summary>
struct OutboundTarget
{
    Handle<sc_tcp_server, sc_tcp_server_destroy> server{ nullptr, sc_tcp_server_destroy };
    Peer peer;
    Handle<sc_tcp_connection, sc_tcp_connection_destroy> connection{ nullptr,
        sc_tcp_connection_destroy };
    Queue queue{ nullptr, sc_outbound_destroy };
    ~OutboundTarget()
    {
        queue.reset();
        if (server)
            (void)sc_tcp_server_stop(server.get());
    }
    bool Open(sc_timer_scheduler* scheduler)
    {
        sc_tcp_options options{};
        (void)sc_tcp_options_init(&options, sizeof options);
        options.port = FreePort();
        sc_tcp_server* rawServer = nullptr;
        if (sc_tcp_server_create(&options, &rawServer) != SC_OK)
            return false;
        server.reset(rawServer);
        if (sc_tcp_server_start(server.get()) != SC_OK ||
            !peer.Connect(sc_tcp_server_port(server.get())))
            return false;
        sc_tcp_connection* rawConnection = nullptr;
        if (sc_tcp_server_accept(server.get(), 3000, &rawConnection) != SC_OK)
            return false;
        connection.reset(rawConnection);
        sc_outbound_options limits{};
        (void)sc_outbound_options_init(&limits, sizeof limits);
        sc_outbound_queue* rawQueue = nullptr;
        if (sc_outbound_create(scheduler, connection.get(), &limits, &rawQueue) != SC_OK)
            return false;
        queue.reset(rawQueue);
        return true;
    }
};
/// <summary>
/// 일괄 전송이 성공이 아닌 상태를 돌려주면 어떤 수신자도 항목을 받지 않았다(CABI-4). 호출의 할당
/// 지점마다 실패를 주입해, 실패한 호출에서는 enqueued 수가 그대로이고 성공한 호출에서는 결과 칸의
/// 성공 수만큼 늘었는지 본다. 주입이 한 번은 일어나고 끝내 주입 없는 성공에 이르러야 한다.
/// 시도마다 새 큐를 쓴다. 송신 펌프의 할당 실패는 큐를 끝내므로, 큐를 나눠 쓰면 그 뒤의 할당
/// 지점이 닫힌 큐에서만 돌아 검사되지 않는다.
/// </summary>
void OutboundBatchAllocationFailure()
{
    if (!AllocationFailureReachesLibrary())
        return;
    ServerCoreTest::SocketRuntime sockets;
    RuntimeFixture runtime;
    if (!Check(sockets.IsReady() && runtime.scheduler != nullptr, "batch sweep runtime ready"))
        return;
    sc_outbound_item_options item{};
    (void)sc_outbound_item_options_init(&item, sizeof item);
    std::size_t injected = 0;
    bool completed = false;
    for (std::size_t skip = 0; skip < 4096 && !completed; ++skip)
    {
        OutboundTarget target;
        if (!Check(target.Open(runtime.scheduler.get()), "batch sweep queue opened"))
            return;
        Queue& queue = target.queue;
        sc_outbound_queue* recipients[]{ queue.get(), queue.get() };
        auto before = View<sc_outbound_metrics>(), after = View<sc_outbound_metrics>();
        (void)sc_outbound_get_metrics(queue.get(), &before);
        sc_status results[2]{ SC_WOULD_BLOCK, SC_WOULD_BLOCK };
        sc_status status = SC_OK;
        bool escaped = false;
        const bool fired = InjectAllocationFailure(
            skip, [&]
            { status = Opaque(&sc_outbound_batch)(recipients, 2, Bytes("q"), &item, results, 2); },
            escaped);
        (void)sc_outbound_get_metrics(queue.get(), &after);
        if (!Check(!escaped, "sc_outbound_batch lets no C++ exception cross the ABI"))
            return;
        const std::uint64_t admitted = after.enqueued - before.enqueued;
        if (status != SC_OK)
            ExpectEqual(std::uint64_t{ 0 }, admitted, "a failed batch admitted no recipient");
        else
        {
            const std::uint64_t reported =
                (results[0] == SC_OK ? 1u : 0u) + (results[1] == SC_OK ? 1u : 0u);
            ExpectEqual(reported, admitted, "batch result slots match admissions");
        }
        if (fired)
            ++injected;
        else
            completed = true;
    }
    ExpectTrue(injected != 0, "batch sweep injected at least one allocation failure");
    ExpectTrue(completed, "batch sweep reached an attempt without injected failure");
}
const ServerCoreTest::CheckRegistration ticks("CAbi.LogicalTicksOwnership", LogicalTicksOwnership);
const ServerCoreTest::CheckRegistration outbound(
    "CAbi.OutboundOwnershipAndBatch", OutboundOwnershipAndBatch);
}
