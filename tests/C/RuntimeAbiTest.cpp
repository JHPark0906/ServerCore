#include "AbiAllocationFailure.h"
#include "ServerCore/C/Runtime.h"
#include "TestHarness.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using ServerCoreTest::ExpectEqual;
using ServerCoreTest::ExpectTrue;
template <class Predicate> bool Await(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
struct WorkState
{
    std::atomic<unsigned> calls{ 0 }, released{ 0 };
    std::atomic<bool> entered{ false }, proceed{ false };
};
sc_status Block(void* pointer, const sc_runtime_token* token)
{
    auto& state = *static_cast<WorkState*>(pointer);
    ++state.calls;
    state.entered = true;
    while (!state.proceed && !sc_runtime_token_requested(token))
        std::this_thread::sleep_for(1ms);
    return SC_OK;
}
sc_status Count(void* pointer, const sc_runtime_token*)
{
    ++static_cast<WorkState*>(pointer)->calls;
    return SC_OK;
}
void Release(void* pointer)
{
    ++static_cast<WorkState*>(pointer)->released;
}
sc_status Finish(void*, const sc_runtime_token*)
{
    return SC_OK;
}
/// <summary>자식 count개를 받아 모두 끝날 때까지 기다린 그룹을 만든다. 실패하면 null이다.</summary>
sc_task_group* FinishedGroup(sc_executor* executor, std::size_t maxChildren, std::size_t count)
{
    sc_task_group_options options{};
    (void)sc_task_group_options_init(&options, sizeof options);
    options.max_children = maxChildren;
    sc_task_group* group = nullptr;
    if (sc_task_group_create(executor, &options, &group) != SC_OK)
        return nullptr;
    sc_task_options childOptions{};
    (void)sc_task_options_init(&childOptions, sizeof childOptions);
    for (std::size_t index = 0; index < count; ++index)
    {
        std::uint64_t id = 0;
        sc_runtime_task* child = nullptr;
        (void)sc_task_group_submit(group, { nullptr, Finish, nullptr }, &childOptions, &id, &child);
        sc_runtime_task_destroy(child);
    }
    (void)sc_task_group_wait(group, 3000);
    return group;
}
void DestroyGroup(sc_task_group* group)
{
    (void)sc_task_group_stop(group);
    sc_task_group_destroy(group);
}
std::size_t CompletionCount(const sc_group_completions* completions)
{
    std::size_t count = 0;
    (void)sc_group_completions_data(completions, &count);
    return count;
}
/// <summary>
/// 완료 기록 수집은 max_children이 아니라 실제로 거둔 기록 수만큼 할당하고, 할당이 실패해도 기록을
/// 잃지 않는다(CABI-4). 후자는 수집 호출의 할당 지점마다 실패를 주입해 본다. 실패한 호출 뒤의 재수집이
/// 두 기록을 모두 돌려주어야 하고, 주입이 한 번은 일어나고 끝내 주입 없는 성공에 이르러야 한다.
/// </summary>
void GroupCompletionAllocation()
{
    sc_executor_options executorOptions{};
    (void)sc_executor_options_init(&executorOptions, sizeof executorOptions);
    sc_executor* executor = nullptr;
    if (sc_executor_create(&executorOptions, &executor) != SC_OK)
    {
        ExpectTrue(false, "completion allocation executor starts");
        return;
    }
    if (auto* group = FinishedGroup(executor, SIZE_MAX, 1))
    {
        sc_group_completions* taken = nullptr;
        ExpectEqual(sc_status{ SC_OK }, sc_task_group_take_completions(group, &taken),
            "completions are collected under an unbounded child limit");
        ExpectEqual(std::size_t{ 1 }, CompletionCount(taken),
            "unbounded child limit returns the finished child");
        sc_group_completions_destroy(taken);
        DestroyGroup(group);
    }
    else
        ExpectTrue(false, "unbounded child limit group starts");
    if (AbiTest::AllocationFailureReachesLibrary())
    {
        std::size_t injected = 0;
        bool completed = false;
        for (std::size_t skip = 0; skip < 4096 && !completed; ++skip)
        {
            auto* group = FinishedGroup(executor, 4, 2);
            if (!group)
            {
                ExpectTrue(false, "allocation sweep group starts");
                break;
            }
            sc_status status = SC_OK;
            sc_group_completions* taken = nullptr;
            bool escaped = false;
            const bool fired = AbiTest::InjectAllocationFailure(
                skip,
                [&] { status = AbiTest::Opaque(&sc_task_group_take_completions)(group, &taken); },
                escaped);
            const bool produced = taken != nullptr;
            std::size_t total = CompletionCount(taken);
            sc_group_completions_destroy(taken);
            ExpectTrue(!escaped && (status == SC_OK) == produced,
                "failed completion collection reports a status without output");
            if (status != SC_OK)
            {
                sc_group_completions* retry = nullptr;
                ExpectEqual(sc_status{ SC_OK }, sc_task_group_take_completions(group, &retry),
                    "completion collection succeeds after an allocation failure");
                total += CompletionCount(retry);
                sc_group_completions_destroy(retry);
            }
            ExpectEqual(
                std::size_t{ 2 }, total, "no completion record is lost to an allocation failure");
            DestroyGroup(group);
            if (fired)
                ++injected;
            else
                completed = true;
        }
        ExpectTrue(injected != 0, "completion sweep injected at least one allocation failure");
        ExpectTrue(completed, "completion sweep reached an attempt without injected failure");
    }
    (void)sc_executor_stop(executor);
    sc_executor_destroy(executor);
}
void RuntimeWorkAndNotifications()
{
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT }, sc_executor_options_init(nullptr, 0),
        "runtime initializer validates storage");
    sc_executor_options options{};
    ExpectEqual(sc_status{ SC_OK }, sc_executor_options_init(&options, sizeof options),
        "runtime defaults initialize");
    options.worker_count = 1;
    options.max_pending_tasks = 1;
    options.max_retained_bytes = 8;
    sc_executor* executor = nullptr;
    ExpectEqual(sc_status{ SC_OK }, sc_executor_create(&options, &executor), "C executor starts");
    if (!executor)
        return;
    sc_task_options taskOptions{};
    (void)sc_task_options_init(&taskOptions, sizeof taskOptions);
    taskOptions.retained_bytes = 4;
    WorkState blocked, queued, rejected;
    sc_runtime_task *first = nullptr, *second = nullptr, *third = nullptr;
    ExpectEqual(sc_status{ SC_OK },
        sc_executor_submit(executor, { &blocked, Block, Release }, &taskOptions, &first),
        "C task starts");
    ExpectTrue(Await([&] { return blocked.entered.load(); }), "C callback enters native worker");
    ExpectEqual(sc_status{ SC_WOULD_BLOCK }, sc_runtime_task_wait(first, 0),
        "zero task wait polls without timeout error");
    ExpectEqual(sc_status{ SC_OK },
        sc_executor_submit(executor, { &queued, Count, Release }, &taskOptions, &second),
        "C pending task admitted");
    ExpectEqual(sc_status{ SC_WOULD_BLOCK },
        sc_executor_submit(executor, { &rejected, Count, Release }, &taskOptions, &third),
        "C pending count/bytes bounded");
    ExpectEqual(
        1u, rejected.released.load(), "rejection transfers and releases context exactly once");
    ExpectEqual(0u, rejected.calls.load(), "rejected work never called");
    sc_notifier* notifier = nullptr;
    sc_subscription* subscription = nullptr;
    ExpectEqual(sc_status{ SC_OK }, sc_notifier_create(4, &notifier), "runtime notifier starts");
    ExpectEqual(sc_status{ SC_OK }, sc_runtime_task_subscribe(first, notifier, 19, &subscription),
        "task completion subscribes");
    uint64_t key = 0;
    ExpectEqual(sc_status{ SC_WOULD_BLOCK }, sc_notifier_next(notifier, 0, &key),
        "unfinished task does not signal terminal readiness");
    blocked.proceed = true;
    ExpectEqual(sc_status{ SC_OK }, sc_notifier_next(notifier, 3000, &key),
        "true terminal task wakes notifier");
    ExpectEqual(uint64_t{ 19 }, key, "runtime notifier preserves key");
    ExpectEqual(
        sc_status{ SC_OK }, sc_runtime_task_result(first), "notification sees terminal status");
    ExpectEqual(1u, blocked.released.load(), "notification follows callback release");
    ExpectEqual(sc_status{ SC_OK }, sc_runtime_task_wait(second, 3000),
        "queued task runs after capacity release");
    sc_subscription_destroy(subscription);
    sc_notifier_destroy(notifier);
    sc_runtime_task_destroy(first);
    sc_runtime_task_destroy(second);
    ExpectEqual(sc_status{ SC_OK }, sc_executor_stop(executor), "C executor stops quiescently");
    sc_executor_destroy(executor);

    WorkState invalid;
    ExpectEqual(sc_status{ SC_INVALID_ARGUMENT },
        sc_executor_submit(nullptr, { &invalid, Count, Release }, nullptr, nullptr),
        "invalid ABI submission rejected");
    ExpectEqual(
        1u, invalid.released.load(), "invalid ABI submission still retires transferred context");
}
void RuntimeTimersAndGroups()
{
    sc_executor_options options{};
    (void)sc_executor_options_init(&options, sizeof options);
    sc_executor* executor = nullptr;
    ExpectEqual(
        sc_status{ SC_OK }, sc_executor_create(&options, &executor), "timer/group parent starts");
    if (!executor)
        return;
    sc_timer_scheduler_options schedulerOptions{};
    (void)sc_timer_scheduler_options_init(&schedulerOptions, sizeof schedulerOptions);
    sc_timer_scheduler* scheduler = nullptr;
    ExpectEqual(sc_status{ SC_OK },
        sc_timer_scheduler_create(executor, &schedulerOptions, &scheduler), "C scheduler starts");
    sc_task_group_options groupOptions{};
    (void)sc_task_group_options_init(&groupOptions, sizeof groupOptions);
    groupOptions.max_children = 2;
    groupOptions.deadline_ms = 500;
    sc_task_group* group = nullptr;
    ExpectEqual(sc_status{ SC_OK }, sc_task_group_create(executor, &groupOptions, &group),
        "C deadline group starts");
    sc_executor_destroy(executor);
    WorkState timerState, groupState;
    sc_timer_options timerOptions{};
    (void)sc_timer_options_init(&timerOptions, sizeof timerOptions);
    timerOptions.delay_ms = 60000;
    timerOptions.repeat_ms = 2;
    sc_runtime_task* timer = nullptr;
    ExpectEqual(sc_status{ SC_OK },
        sc_timer_scheduler_schedule(
            scheduler, { &timerState, Count, Release }, &timerOptions, &timer),
        "dependent scheduler keeps parent alive");
    ExpectEqual(sc_status{ SC_OK }, sc_runtime_task_reschedule(timer, 0),
        "C timer can reschedule before dispatch");
    ExpectTrue(Await([&] { return timerState.calls.load() >= 3; }),
        "C repeating timer retains callable identity");
    sc_runtime_task_cancel(timer);
    ExpectEqual(sc_status{ SC_CANCELLED }, sc_runtime_task_wait(timer, 3000),
        "C timer cancel reaches terminal");
    ExpectEqual(
        1u, timerState.released.load(), "repeated callback released only after final retirement");
    sc_task_options childOptions{};
    (void)sc_task_options_init(&childOptions, sizeof childOptions);
    sc_runtime_task* child = nullptr;
    uint64_t id = 0;
    ExpectEqual(sc_status{ SC_OK },
        sc_task_group_submit(group, { &groupState, Block, Release }, &childOptions, &id, &child),
        "dependent group keeps parent alive");
    ExpectEqual(sc_status{ SC_WOULD_BLOCK }, sc_task_group_wait(group, 0),
        "zero group wait closes admission and polls");
    ExpectEqual(sc_status{ SC_OK }, sc_task_group_wait(group, 3000),
        "group deadline terminates cooperative child");
    ExpectEqual(sc_status{ SC_TIMEOUT }, sc_runtime_task_result(child),
        "group deadline propagates child Timeout");
    sc_group_completions* completed = nullptr;
    ExpectEqual(sc_status{ SC_OK }, sc_task_group_take_completions(group, &completed),
        "C group completions are collected once");
    size_t count = 0;
    const auto* data = sc_group_completions_data(completed, &count);
    ExpectTrue(count == 1 && data && data[0].id == id && data[0].status == SC_TIMEOUT,
        "completion records preserve child id and result");
    sc_group_completions_destroy(completed);
    ExpectEqual(1u, groupState.released.load(), "group terminal follows child context release");
    sc_runtime_task_destroy(child);
    sc_runtime_task_destroy(timer);
    ExpectEqual(
        sc_status{ SC_OK }, sc_task_group_stop(group), "group joins before deferred release");
    ExpectEqual(sc_status{ SC_OK }, sc_timer_scheduler_stop(scheduler),
        "scheduler joins before deferred release");
    sc_task_group_destroy(group);
    sc_timer_scheduler_destroy(scheduler);
    GroupCompletionAllocation();
}
struct DropOwner
{
    std::atomic<bool>* allowed;
    std::atomic<unsigned>* released;
    void* owner;
    void (*destroy)(void*);
    sc_status (*stop)(void*);
    std::atomic<bool>* rejected;
};
sc_status DropFromWorker(void* pointer, const sc_runtime_token*)
{
    auto& value = *static_cast<DropOwner*>(pointer);
    while (!value.allowed->load())
        std::this_thread::sleep_for(1ms);
    *value.rejected = value.stop(value.owner) == SC_INVALID_ARGUMENT &&
                      sc_runtime_shutdown(0) == SC_INVALID_ARGUMENT;
    value.destroy(value.owner);
    return SC_OK;
}
void DropContext(void* pointer)
{
    auto* value = static_cast<DropOwner*>(pointer);
    if (sc_runtime_shutdown(0) != SC_INVALID_ARGUMENT)
        *value->rejected = false;
    ++*value->released;
    delete value;
}
void RuntimeShutdown()
{
    sc_executor_options options{};
    (void)sc_executor_options_init(&options, sizeof options);
    sc_executor* executor = nullptr;
    ExpectEqual(sc_status{ SC_OK }, sc_executor_create(&options, &executor),
        "shutdown test executor starts");
    if (!executor)
        return;
    sc_task_group_options groupOptions{};
    (void)sc_task_group_options_init(&groupOptions, sizeof groupOptions);
    std::vector<sc_task_group*> groups;
    for (size_t index = 0; index < 255; ++index)
    {
        sc_task_group* group = nullptr;
        ExpectEqual(sc_status{ SC_OK }, sc_task_group_create(executor, &groupOptions, &group),
            "bounded cleanup owner slot admitted");
        if (group)
            groups.push_back(group);
    }
    sc_task_group* excess = nullptr;
    ExpectEqual(sc_status{ SC_WOULD_BLOCK }, sc_task_group_create(executor, &groupOptions, &excess),
        "live plus retiring runtime owner count is bounded at 256");
    for (auto* group : groups)
        sc_task_group_destroy(group);
    ExpectEqual(sc_status{ SC_WOULD_BLOCK }, sc_runtime_shutdown(0),
        "shutdown poll closes admission without blocking on live owner");
    sc_executor* rejected = nullptr;
    ExpectEqual(sc_status{ SC_CLOSED }, sc_executor_create(&options, &rejected),
        "shutdown closes runtime creation permanently");
    ExpectEqual(sc_status{ SC_TIMEOUT }, sc_runtime_shutdown(5),
        "bounded shutdown times out while owner remains alive");
    std::atomic<bool> entered{ false };
    std::atomic<sc_status> blockingResult{ SC_PLATFORM_ERROR };
    std::thread blocking(
        [&]
        {
            entered = true;
            blockingResult = sc_runtime_shutdown(UINT32_MAX);
        });
    (void)Await([&] { return entered.load(); });
    std::this_thread::sleep_for(10ms);
    const auto before = std::chrono::steady_clock::now();
    ExpectEqual(sc_status{ SC_WOULD_BLOCK }, sc_runtime_shutdown(0),
        "shutdown poll does not wait for another shutdown caller");
    ExpectTrue(std::chrono::steady_clock::now() - before < 250ms,
        "concurrent shutdown poll returns promptly");
    ExpectEqual(sc_status{ SC_TIMEOUT }, sc_runtime_shutdown(5),
        "concurrent finite shutdown respects original deadline");
    sc_executor_destroy(executor);
    blocking.join();
    ExpectEqual(sc_status{ SC_OK }, blockingResult.load(),
        "blocking shutdown joins after last owner retires");
    ExpectEqual(sc_status{ SC_OK }, sc_runtime_shutdown(3000),
        "explicit control-thread shutdown joins deferred cleanup");
    ExpectEqual(sc_status{ SC_OK }, sc_runtime_shutdown(0), "completed shutdown is idempotent");
}
void RuntimeDeferredOwnerDestruction()
{
    // Every owner kind can be the last reference released on its own worker.
    for (unsigned kind = 0; kind < 4; ++kind)
    {
        sc_executor_options eo{};
        (void)sc_executor_options_init(&eo, sizeof eo);
        sc_executor* executor = nullptr;
        ExpectEqual(sc_status{ SC_OK }, sc_executor_create(&eo, &executor),
            "self-drop parent executor starts");
        if (!executor)
            continue;
        std::atomic<bool> allowed{ false }, rejected{ false };
        std::atomic<unsigned> released{ 0 };
        auto* context = new DropOwner{ &allowed, &released, nullptr, nullptr, nullptr, &rejected };
        sc_task_options options{};
        (void)sc_task_options_init(&options, sizeof options);
        sc_runtime_task* task = nullptr;
        sc_status admitted = SC_PLATFORM_ERROR;
        if (kind == 0)
        {
            context->owner = executor;
            context->destroy = [](void* p) { sc_executor_destroy(static_cast<sc_executor*>(p)); };
            context->stop = [](void* p) { return sc_executor_stop(static_cast<sc_executor*>(p)); };
            admitted = sc_executor_submit(
                executor, { context, DropFromWorker, DropContext }, &options, &task);
        }
        else if (kind == 1)
        {
            sc_keyed_executor_options ko{};
            (void)sc_keyed_executor_options_init(&ko, sizeof ko);
            sc_keyed_executor* keyed = nullptr;
            ExpectEqual(sc_status{ SC_OK }, sc_keyed_executor_create(&ko, &keyed),
                "self-drop keyed executor starts");
            context->owner = keyed;
            context->destroy = [](void* p)
            { sc_keyed_executor_destroy(static_cast<sc_keyed_executor*>(p)); };
            context->stop = [](void* p)
            { return sc_keyed_executor_stop(static_cast<sc_keyed_executor*>(p)); };
            admitted = sc_keyed_executor_submit(
                keyed, 1, { context, DropFromWorker, DropContext }, &options, &task);
            sc_executor_destroy(executor);
        }
        else if (kind == 2)
        {
            sc_timer_scheduler_options so{};
            (void)sc_timer_scheduler_options_init(&so, sizeof so);
            sc_timer_scheduler* scheduler = nullptr;
            ExpectEqual(sc_status{ SC_OK }, sc_timer_scheduler_create(executor, &so, &scheduler),
                "self-drop scheduler starts");
            sc_timer_options timerOptions{};
            (void)sc_timer_options_init(&timerOptions, sizeof timerOptions);
            context->owner = scheduler;
            context->destroy = [](void* p)
            { sc_timer_scheduler_destroy(static_cast<sc_timer_scheduler*>(p)); };
            context->stop = [](void* p)
            { return sc_timer_scheduler_stop(static_cast<sc_timer_scheduler*>(p)); };
            admitted = sc_timer_scheduler_schedule(
                scheduler, { context, DropFromWorker, DropContext }, &timerOptions, &task);
            sc_executor_destroy(executor);
        }
        else
        {
            sc_task_group_options go{};
            (void)sc_task_group_options_init(&go, sizeof go);
            sc_task_group* group = nullptr;
            ExpectEqual(sc_status{ SC_OK }, sc_task_group_create(executor, &go, &group),
                "self-drop group starts");
            context->owner = group;
            context->destroy = [](void* p)
            { sc_task_group_destroy(static_cast<sc_task_group*>(p)); };
            context->stop = [](void* p)
            { return sc_task_group_stop(static_cast<sc_task_group*>(p)); };
            uint64_t id = 0;
            admitted = sc_task_group_submit(
                group, { context, DropFromWorker, DropContext }, &options, &id, &task);
            sc_executor_destroy(executor);
        }
        ExpectEqual(sc_status{ SC_OK }, admitted, "self-drop work admitted");
        allowed = true;
        if (task)
        {
            const auto terminal = sc_runtime_task_wait(task, 3000);
            ExpectTrue(terminal == SC_OK || terminal == SC_CANCELLED,
                "last-owner worker release reaches terminal without self-join");
            sc_runtime_task_destroy(task);
        }
        ExpectTrue(Await([&] { return released.load() == 1; }),
            "last-owner cleanup releases callback exactly once");
        ExpectTrue(rejected, "explicit stop from own work is rejected while Drop is safe");
    }
}
const ServerCoreTest::CheckRegistration work(
    "CAbi.RuntimeWorkAndNotifications", RuntimeWorkAndNotifications);
const ServerCoreTest::CheckRegistration timers(
    "CAbi.RuntimeTimersAndGroups", RuntimeTimersAndGroups);
const ServerCoreTest::CheckRegistration destruction(
    "CAbi.RuntimeDeferredOwnerDestruction", RuntimeDeferredOwnerDestruction);
const ServerCoreTest::CheckRegistration shutdown("CAbi.RuntimeShutdown", RuntimeShutdown);
}
