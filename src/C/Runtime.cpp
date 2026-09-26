#include "ServerCore/C/Runtime.h"
#include "C/GameExecutionInternal.h"
#include "C/Internal.h"
#include "C/ObservationInternal.h"
#include "ServerCore/Observability/ServerObservation.h"
#include "ServerCore/Runtime/KeyedExecutor.h"
#include "ServerCore/Runtime/TaskGroup.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#include <condition_variable>
#include <thread>
#include <variant>
#include <vector>

using namespace ServerCore;
using CDetail::Code;
using CDetail::Protect;
using CDetail::Version;
using Clock = std::chrono::steady_clock;
struct sc_runtime_token
{
    std::stop_token token;
};
struct sc_cancellation
{
    std::stop_source source;
};
namespace
{
thread_local unsigned callbackDepth = 0;
thread_local bool reaperThread = false;
struct CallbackScope
{
    CallbackScope() noexcept { ++callbackDepth; }
    ~CallbackScope() { --callbackDepth; }
};
Clock::time_point Deadline(uint32_t milliseconds) noexcept
{
    return milliseconds == UINT32_MAX ? Clock::time_point::max()
                                      : Clock::now() + std::chrono::milliseconds(milliseconds);
}
struct Owner
{
    Owner* next = nullptr;
    virtual ~Owner() = default;
    virtual void RequestStop() noexcept = 0;
};
// Admission reserves the intrusive retirement node itself. Final release never
// allocates/spawns and never destroys a native executor on its own worker.
class Reaper
{
public:
    // No static destructor: joining a DLL-owned thread during Windows loader
    // detach can deadlock. Explicit Shutdown owns process/module quiescence.
    static Reaper& Instance()
    {
        static auto* value = new Reaper;
        return *value;
    }
    bool Acquire()
    {
        std::lock_guard guard(mutex);
        if (closing || live == 256)
            return false;
        ++live;
        return true;
    }
    sc_status AdmissionFailure()
    {
        std::lock_guard guard(mutex);
        return closing ? SC_CLOSED : SC_WOULD_BLOCK;
    }
    void Release()
    {
        {
            std::lock_guard guard(mutex);
            --live;
        }
        wake.notify_all();
    }
    void Retire(Owner* owner) noexcept
    {
        owner->RequestStop();
        {
            std::lock_guard guard(mutex);
            if (tail)
                tail->next = owner;
            else
                head = owner;
            tail = owner;
        }
        // Shutdown and the worker share this condition variable. Waking only
        // a shutdown waiter could leave queued retirement stranded forever.
        wake.notify_all();
    }
    sc_status Shutdown(uint32_t milliseconds)
    {
        if (callbackDepth || reaperThread)
            return SC_INVALID_ARGUMENT;
        const auto deadline = Deadline(milliseconds);
        // Shutdown 호출은 한 번에 하나만 진행한다. 예전에는 std::timed_mutex::try_lock_until로 줄을
        // 세웠는데, GCC 13 libtsan이 그 구현인 pthread_mutex_clocklock을 가로채지 못해 그 잠금을 보지
        // 못하고 풀기를 "unlock of an unlocked mutex"로 보고했다(CAbi.RuntimeShutdown). TSan이 모두
        // 보는 mutex·조건 변수·표지로 같은 규칙(0: 즉시, UINT32_MAX: 무기한, 그 밖: 기한)을 지킨다.
        {
            std::unique_lock gate(lifecycleMutex);
            const auto idle = [this] { return !lifecycleBusy; };
            if (milliseconds == 0)
            {
                if (!idle())
                    return SC_WOULD_BLOCK;
            }
            else if (milliseconds == UINT32_MAX)
                lifecycleIdle.wait(gate, idle);
            else if (!lifecycleIdle.wait_until(gate, deadline, idle))
                return SC_TIMEOUT;
            lifecycleBusy = true;
        }
        struct LifecycleRelease
        {
            Reaper& reaper;
            ~LifecycleRelease()
            {
                {
                    std::lock_guard gate(reaper.lifecycleMutex);
                    reaper.lifecycleBusy = false;
                }
                reaper.lifecycleIdle.notify_all();
            }
        } release{ *this };
        {
            std::unique_lock guard(mutex);
            closing = true;
            wake.notify_all();
            if (live != 0)
            {
                if (milliseconds == 0)
                    return SC_WOULD_BLOCK;
                if (milliseconds == UINT32_MAX)
                    wake.wait(guard, [this] { return live == 0; });
                else if (!wake.wait_until(guard, deadline, [this] { return live == 0; }))
                    return SC_TIMEOUT;
            }
        }
        if (worker.joinable())
            worker.join();
        return SC_OK;
    }

private:
    Reaper()
        : worker([this] { Run(); })
    {
    }
    void Run()
    {
        reaperThread = true;
        for (;;)
        {
            Owner* item;
            {
                std::unique_lock guard(mutex);
                wake.wait(guard, [this] { return head || (closing && live == 0); });
                if (!head)
                    return;
                item = head;
                head = item->next;
                if (!head)
                    tail = nullptr;
            }
            delete item;
            Release();
        }
    }
    std::mutex mutex;
    std::mutex lifecycleMutex;
    std::condition_variable lifecycleIdle;
    bool lifecycleBusy = false;
    std::condition_variable wake;
    Owner* head = nullptr;
    Owner* tail = nullptr;
    size_t live = 0;
    bool closing = false;
    std::thread worker;
};
template <class T> std::shared_ptr<T> MakeOwner()
{
    auto& reaper = Reaper::Instance();
    if (!reaper.Acquire())
        return {};
    T* value;
    try
    {
        value = new T;
    }
    catch (...)
    {
        reaper.Release();
        throw;
    }
    return std::shared_ptr<T>(value, [&reaper](T* owner) { reaper.Retire(owner); });
}
struct ExecutorOwner final : Owner
{
    Runtime::TaskExecutor native;
    void RequestStop() noexcept override { native.RequestStop(); }
};
struct KeyedOwner final : Owner
{
    Runtime::KeyedExecutor native;
    void RequestStop() noexcept override { native.RequestStop(); }
};
struct TimerOwner final : Owner
{
    std::shared_ptr<ExecutorOwner> executor;
    Runtime::TimerScheduler native;
    void RequestStop() noexcept override { native.RequestStop(); }
};
struct GroupOwner final : Owner
{
    std::shared_ptr<ExecutorOwner> executor;
    // 출력 할당에 실패한 수집이 이미 거둔 기록. 다음 수집이 새로 거두기 전에 먼저 돌려준다.
    std::mutex takeMutex;
    std::vector<Runtime::TaskGroupCompletion> untaken;
    Runtime::TaskGroup native;
    void RequestStop() noexcept override { native.RequestCancel(); }
};
struct Work
{
    explicit Work(sc_runtime_work input) noexcept
        : value(input)
    {
    }
    Work(Work&& other) noexcept
        : value(std::exchange(other.value, {}))
    {
    }
    Work(const Work&) = delete;
    ~Work()
    {
        if (value.release)
        {
            const CallbackScope scope;
            try
            {
                value.release(value.context);
            }
            catch (...)
            {
            }
        }
    }
    Core::Status operator()(std::stop_token token)
    {
        const CallbackScope scope;
        const sc_runtime_token borrowed{ token };
        sc_status code;
        try
        {
            code = value.work(value.context, &borrowed);
        }
        catch (...)
        {
            code = SC_PLATFORM_ERROR;
        }
        if (code < SC_OK || code > SC_CANCELLED)
            code = SC_PLATFORM_ERROR;
        return code == SC_OK ? Core::Status::Ok()
                             : Core::Status::FailWithoutMessage(static_cast<Core::ErrorCode>(code));
    }
    sc_runtime_work value;
};
Runtime::TaskOptions TaskOptions(const sc_task_options& options)
{
    Runtime::TaskOptions value;
    value.retainedBytes = options.retained_bytes;
    if (options.parent)
        value.parentToken = options.parent->source.get_token();
    if (options.deadline_ms != UINT32_MAX)
        value.deadline = Deadline(options.deadline_ms);
    return value;
}
template <class T> sc_status Init(T* options, size_t size, T value) noexcept
{
    if (!options || size < sizeof(T))
        return SC_INVALID_ARGUMENT;
    value.abi_version = SC_ABI_VERSION;
    value.struct_size = sizeof(T);
    *options = value;
    return SC_OK;
}
}
struct sc_executor
{
    std::shared_ptr<ExecutorOwner> state;
};
struct sc_keyed_executor
{
    std::shared_ptr<KeyedOwner> state;
};
struct sc_timer_scheduler
{
    std::shared_ptr<TimerOwner> state;
};
struct sc_task_group
{
    std::shared_ptr<GroupOwner> state;
};
struct sc_runtime_task
{
    std::variant<Runtime::TaskHandle, Runtime::KeyedTaskHandle, Runtime::TimerHandle> value;
};
struct sc_group_completions
{
    std::vector<sc_group_completion> values;
};

void ServerCore::CDetail::EnterRuntimeCallback() noexcept
{
    ++callbackDepth;
}
void ServerCore::CDetail::LeaveRuntimeCallback() noexcept
{
    --callbackDepth;
}

ServerCore::CDetail::TimerSchedulerLease ServerCore::CDetail::RetainTimerScheduler(
    const sc_timer_scheduler* value) noexcept
{
    return value ? TimerSchedulerLease{ value->state, &value->state->native }
                 : TimerSchedulerLease{};
}

extern "C"
{
    sc_status sc_executor_get_observation(const sc_executor* executor, sc_observation* out) noexcept
    {
        if (!executor)
            return SC_INVALID_ARGUMENT;
        return CDetail::CopyObservation(Observability::Observe(executor->state->native), out);
    }
    sc_status sc_executor_options_init(sc_executor_options* value, size_t size) noexcept
    {
        return Init(value, size, { 0, 0, 2, 128, 4 * 1024 * 1024 });
    }
    sc_status sc_keyed_executor_options_init(sc_keyed_executor_options* value, size_t size) noexcept
    {
        return Init(value, size, { 0, 0, 2, 1024, 4096, 16 * 1024 * 1024, 128, 4 * 1024 * 1024 });
    }
    sc_status sc_task_options_init(sc_task_options* value, size_t size) noexcept
    {
        return Init(value, size, { 0, 0, 0, nullptr, UINT32_MAX });
    }
    sc_status sc_timer_scheduler_options_init(
        sc_timer_scheduler_options* value, size_t size) noexcept
    {
        return Init(value, size, { 0, 0, 1024, 4 * 1024 * 1024 });
    }
    sc_status sc_timer_options_init(sc_timer_options* value, size_t size) noexcept
    {
        return Init(value, size, { 0, 0, 0, nullptr, 0, 0 });
    }
    sc_status sc_task_group_options_init(sc_task_group_options* value, size_t size) noexcept
    {
        return Init(value, size, { 0, 0, 128, 4 * 1024 * 1024, nullptr, UINT32_MAX });
    }
    sc_status sc_cancellation_create(sc_cancellation** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]
            {
                *out = new sc_cancellation;
                return SC_OK;
            });
    }
    void sc_cancellation_request(sc_cancellation* value) noexcept
    {
        if (value)
            (void)value->source.request_stop();
    }
    int sc_cancellation_requested(const sc_cancellation* value) noexcept
    {
        return value && value->source.stop_requested();
    }
    void sc_cancellation_destroy(sc_cancellation* value) noexcept
    {
        delete value;
    }
    int sc_runtime_token_requested(const sc_runtime_token* value) noexcept
    {
        return value && value->token.stop_requested();
    }
    sc_status sc_runtime_shutdown(uint32_t milliseconds) noexcept
    {
        if (callbackDepth || reaperThread)
            return SC_INVALID_ARGUMENT;
        return Protect([&] { return Reaper::Instance().Shutdown(milliseconds); });
    }
    sc_status sc_executor_create(const sc_executor_options* options, sc_executor** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!Version(options))
                    return SC_INVALID_ARGUMENT;
                auto state = MakeOwner<ExecutorOwner>();
                if (!state)
                    return Reaper::Instance().AdmissionFailure();
                auto status = state->native.Start({ options->worker_count,
                    options->max_pending_tasks, options->max_retained_bytes });
                if (!status.IsOk())
                    return Code(status);
                *out = new sc_executor{ std::move(state) };
                return SC_OK;
            });
    }
    sc_status sc_keyed_executor_create(
        const sc_keyed_executor_options* options, sc_keyed_executor** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!Version(options))
                    return SC_INVALID_ARGUMENT;
                auto state = MakeOwner<KeyedOwner>();
                if (!state)
                    return Reaper::Instance().AdmissionFailure();
                auto status = state->native.Start({ options->worker_count, options->max_keys,
                    options->max_outstanding_tasks, options->max_retained_bytes,
                    options->max_outstanding_tasks_per_key, options->max_retained_bytes_per_key });
                if (!status.IsOk())
                    return Code(status);
                *out = new sc_keyed_executor{ std::move(state) };
                return SC_OK;
            });
    }
    sc_status sc_timer_scheduler_create(sc_executor* executor,
        const sc_timer_scheduler_options* options, sc_timer_scheduler** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!executor || !Version(options))
                    return SC_INVALID_ARGUMENT;
                auto state = MakeOwner<TimerOwner>();
                if (!state)
                    return Reaper::Instance().AdmissionFailure();
                state->executor = executor->state;
                auto status = state->native.Start(
                    state->executor->native, { options->max_timers, options->max_retained_bytes });
                if (!status.IsOk())
                    return Code(status);
                *out = new sc_timer_scheduler{ std::move(state) };
                return SC_OK;
            });
    }
    sc_status sc_task_group_create(
        sc_executor* executor, const sc_task_group_options* options, sc_task_group** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!executor || !Version(options))
                    return SC_INVALID_ARGUMENT;
                auto state = MakeOwner<GroupOwner>();
                if (!state)
                    return Reaper::Instance().AdmissionFailure();
                state->executor = executor->state;
                Runtime::TaskGroupOptions configuration;
                configuration.maxChildren = options->max_children;
                configuration.maxRetainedBytes = options->max_retained_bytes;
                if (options->parent)
                    configuration.parentToken = options->parent->source.get_token();
                if (options->deadline_ms != UINT32_MAX)
                    configuration.deadline = Deadline(options->deadline_ms);
                auto status = state->native.Start(state->executor->native, configuration);
                if (!status.IsOk())
                    return Code(status);
                *out = new sc_task_group{ std::move(state) };
                return SC_OK;
            });
    }
    sc_status sc_executor_submit(sc_executor* executor, sc_runtime_work callback,
        const sc_task_options* options, sc_runtime_task** out) noexcept
    {
        Work owned(callback);
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!executor || !callback.work || !Version(options))
                    return SC_INVALID_ARGUMENT;
                auto handle = std::make_unique<sc_runtime_task>();
                auto result =
                    executor->state->native.Submit(std::move(owned), TaskOptions(*options));
                if (!result.IsOk())
                    return Code(result.GetStatus());
                handle->value = std::move(result.Value());
                *out = handle.release();
                return SC_OK;
            });
    }
    sc_status sc_keyed_executor_submit(sc_keyed_executor* executor, uint64_t key,
        sc_runtime_work callback, const sc_task_options* options, sc_runtime_task** out) noexcept
    {
        Work owned(callback);
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!executor || !callback.work || !Version(options))
                    return SC_INVALID_ARGUMENT;
                auto handle = std::make_unique<sc_runtime_task>();
                auto result =
                    executor->state->native.Submit(key, std::move(owned), TaskOptions(*options));
                if (!result.IsOk())
                    return Code(result.GetStatus());
                handle->value = std::move(result.Value());
                *out = handle.release();
                return SC_OK;
            });
    }
    sc_status sc_timer_scheduler_schedule(sc_timer_scheduler* scheduler, sc_runtime_work callback,
        const sc_timer_options* options, sc_runtime_task** out) noexcept
    {
        Work owned(callback);
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!scheduler || !callback.work || !Version(options))
                    return SC_INVALID_ARGUMENT;
                auto handle = std::make_unique<sc_runtime_task>();
                Runtime::TimerOptions configuration;
                configuration.due = Clock::now() + std::chrono::milliseconds(options->delay_ms);
                configuration.repeatInterval = std::chrono::milliseconds(options->repeat_ms);
                configuration.retainedBytes = options->retained_bytes;
                if (options->parent)
                    configuration.parentToken = options->parent->source.get_token();
                auto result = scheduler->state->native.Schedule(std::move(owned), configuration);
                if (!result.IsOk())
                    return Code(result.GetStatus());
                handle->value = std::move(result.Value());
                *out = handle.release();
                return SC_OK;
            });
    }
    sc_status sc_task_group_submit(sc_task_group* group, sc_runtime_work callback,
        const sc_task_options* options, uint64_t* id, sc_runtime_task** out) noexcept
    {
        Work owned(callback);
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (id)
            *id = 0;
        return Protect(
            [&]() -> sc_status
            {
                if (!group || !id || !callback.work || !Version(options))
                    return SC_INVALID_ARGUMENT;
                auto handle = std::make_unique<sc_runtime_task>();
                auto result = group->state->native.Submit(std::move(owned), TaskOptions(*options));
                if (!result.IsOk())
                    return Code(result.GetStatus());
                *id = result.Value().id;
                handle->value = std::move(result.Value().task);
                *out = handle.release();
                return SC_OK;
            });
    }
    void sc_executor_request_stop(sc_executor* value) noexcept
    {
        if (value)
            value->state->native.RequestStop();
    }
    sc_status sc_executor_stop(sc_executor* value) noexcept
    {
        return Protect(
            [&] { return value ? Code(value->state->native.Stop()) : SC_INVALID_ARGUMENT; });
    }
    void sc_executor_destroy(sc_executor* value) noexcept
    {
        delete value;
    }
    void sc_keyed_executor_request_stop(sc_keyed_executor* value) noexcept
    {
        if (value)
            value->state->native.RequestStop();
    }
    sc_status sc_keyed_executor_stop(sc_keyed_executor* value) noexcept
    {
        return Protect(
            [&] { return value ? Code(value->state->native.Stop()) : SC_INVALID_ARGUMENT; });
    }
    void sc_keyed_executor_destroy(sc_keyed_executor* value) noexcept
    {
        delete value;
    }
    void sc_timer_scheduler_request_stop(sc_timer_scheduler* value) noexcept
    {
        if (value)
            value->state->native.RequestStop();
    }
    sc_status sc_timer_scheduler_stop(sc_timer_scheduler* value) noexcept
    {
        return Protect(
            [&] { return value ? Code(value->state->native.Stop()) : SC_INVALID_ARGUMENT; });
    }
    void sc_timer_scheduler_destroy(sc_timer_scheduler* value) noexcept
    {
        delete value;
    }
    void sc_task_group_close(sc_task_group* value) noexcept
    {
        if (value)
            value->state->native.CloseAdmission();
    }
    void sc_task_group_cancel(sc_task_group* value) noexcept
    {
        if (value)
            value->state->native.RequestCancel();
    }
    sc_status sc_task_group_result(const sc_task_group* value) noexcept
    {
        return value ? Code(value->state->native.GetStatus()) : SC_INVALID_ARGUMENT;
    }
    sc_status sc_task_group_wait(sc_task_group* value, uint32_t milliseconds) noexcept
    {
        return Protect(
            [&]() -> sc_status
            {
                if (!value)
                    return SC_INVALID_ARGUMENT;
                if (milliseconds == 0)
                {
                    value->state->native.CloseAdmission();
                    return Code(value->state->native.GetStatus());
                }
                return Code(value->state->native.WaitUntil(Deadline(milliseconds)));
            });
    }
    sc_status sc_task_group_stop(sc_task_group* value) noexcept
    {
        return Protect(
            [&] { return value ? Code(value->state->native.Stop()) : SC_INVALID_ARGUMENT; });
    }
    void sc_task_group_destroy(sc_task_group* value) noexcept
    {
        delete value;
    }
    sc_status sc_task_group_subscribe(
        sc_task_group* value, sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!value)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return Protect(
            [&]
            {
                return CDetail::SubscribeCompletion([&](auto callback)
                    { return value->state->native.WaitForCompletion(std::move(callback)); },
                    notifier, key, out);
            });
    }
    sc_status sc_task_group_take_completions(
        sc_task_group* value, sc_group_completions** out) noexcept
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        return Protect(
            [&]() -> sc_status
            {
                if (!value)
                    return SC_INVALID_ARGUMENT;
                auto& state = *value->state;
                const std::lock_guard guard(state.takeMutex);
                // 네이티브 수집은 기록을 소비한다. 출력은 max_children이 아니라 거둔 수만큼 할당하고, 그
                // 할당이 실패하면 기록을 버리지 않고 남겨 다음 호출이 돌려준다(CAbi.RuntimeTimersAndGroups).
                auto records = std::move(state.untaken);
                state.untaken.clear();
                if (records.empty())
                {
                    auto completed = state.native.TakeCompletions();
                    if (!completed.IsOk())
                        return Code(completed.GetStatus());
                    records = std::move(completed.Value());
                }
                try
                {
                    auto result = std::make_unique<sc_group_completions>();
                    result->values.reserve(records.size());
                    for (const auto& item : records)
                        result->values.push_back({ item.id, static_cast<sc_status>(item.code) });
                    *out = result.release();
                    return SC_OK;
                }
                catch (...)
                {
                    state.untaken = std::move(records);
                    return SC_PLATFORM_ERROR;
                }
            });
    }
    const sc_group_completion* sc_group_completions_data(
        const sc_group_completions* value, size_t* count) noexcept
    {
        if (count)
            *count = value ? value->values.size() : 0;
        return value ? value->values.data() : nullptr;
    }
    void sc_group_completions_destroy(sc_group_completions* value) noexcept
    {
        delete value;
    }
    void sc_runtime_task_cancel(sc_runtime_task* value) noexcept
    {
        if (value)
            std::visit([](const auto& task) { (void)task.RequestCancel(); }, value->value);
    }
    sc_status sc_runtime_task_result(const sc_runtime_task* value) noexcept
    {
        return Protect(
            [&]() -> sc_status
            {
                return value ? std::visit([](const auto& task) { return Code(task.GetStatus()); },
                                   value->value)
                             : SC_INVALID_ARGUMENT;
            });
    }
    uint32_t sc_runtime_task_finished(const sc_runtime_task* value) noexcept
    {
        return value && std::visit([](const auto& task) { return task.IsFinished(); }, value->value)
                   ? 1u
                   : 0u;
    }
    sc_status sc_runtime_task_wait(sc_runtime_task* value, uint32_t milliseconds) noexcept
    {
        if (milliseconds == 0)
            return sc_runtime_task_result(value);
        return Protect(
            [&]() -> sc_status
            {
                return value ? std::visit([&](const auto& task)
                                   { return Code(task.WaitUntil(Deadline(milliseconds))); },
                                   value->value)
                             : SC_INVALID_ARGUMENT;
            });
    }
    sc_status sc_runtime_task_reschedule(sc_runtime_task* value, uint32_t milliseconds) noexcept
    {
        if (!value)
            return SC_INVALID_ARGUMENT;
        const auto* timer = std::get_if<Runtime::TimerHandle>(&value->value);
        return timer
                   ? Code(timer->Reschedule(Clock::now() + std::chrono::milliseconds(milliseconds)))
                   : SC_INVALID_ARGUMENT;
    }
    sc_status sc_runtime_task_subscribe(
        sc_runtime_task* value, sc_notifier* notifier, uint64_t key, sc_subscription** out) noexcept
    {
        if (!value)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return Protect(
            [&]
            {
                return CDetail::SubscribeCompletion(
                    [&](auto callback)
                    {
                        return std::visit([&](const auto& task)
                            { return task.WaitForCompletion(std::move(callback)); }, value->value);
                    },
                    notifier, key, out);
            });
    }
    void sc_runtime_task_destroy(sc_runtime_task* value) noexcept
    {
        delete value;
    }
}
