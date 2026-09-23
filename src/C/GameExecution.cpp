#include "ServerCore/C/GameExecution.h"
#include "C/GameExecutionInternal.h"
#include "C/Internal.h"
#include "ServerCore/Runtime/OutboundQueue.h"
#include "ServerCore/Runtime/TickRunner.h"
#include <utility>
namespace R = ServerCore::Runtime;
namespace C = ServerCore::CDetail;
namespace Core = ServerCore::Core;
using Clock = std::chrono::steady_clock;
struct sc_tick_token
{
    std::stop_token value;
};
struct sc_tick
{
    C::TimerSchedulerLease scheduler;
    std::optional<R::TickHandle> value;
    ~sc_tick()
    {
        if (value)
            (void)value->RequestCancel();
    }
};
struct sc_outbound_queue
{
    C::TimerSchedulerLease scheduler;
    C::TcpConnectionLease connection;
    std::size_t maxBytes;
    std::shared_ptr<R::OutboundQueue> value;
};
namespace
{
struct CallbackScope
{
    CallbackScope() { C::EnterRuntimeCallback(); }
    ~CallbackScope() { C::LeaveRuntimeCallback(); }
};
struct Work
{
    explicit Work(sc_tick_work input)
        : value(input)
    {
    }
    Work(Work&& other) noexcept
        : value(std::exchange(other.value, {}))
    {
    }
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
    sc_tick_work value;
    Core::Status Run(const R::TickInfo& tick, std::stop_token token)
    {
        const CallbackScope scope;
        const sc_tick_info info{ tick.index,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(tick.lateness).count()),
            tick.skipped };
        const sc_tick_token borrowed{ token };
        sc_status result;
        try
        {
            result = value.work(value.context, &info, &borrowed);
        }
        catch (...)
        {
            result = SC_PLATFORM_ERROR;
        }
        if (result < SC_OK || result > SC_CANCELLED)
            result = SC_PLATFORM_ERROR;
        return result == SC_OK
                   ? Core::Status::Ok()
                   : Core::Status::FailWithoutMessage(static_cast<Core::ErrorCode>(result));
    }
};
template <class T> sc_status Init(T* out, size_t size, T value)
{
    if (!out || size < sizeof(T))
        return SC_INVALID_ARGUMENT;
    value.abi_version = SC_ABI_VERSION;
    value.struct_size = sizeof(T);
    *out = value;
    return SC_OK;
}
bool Valid(const sc_outbound_item_options* value)
{
    return C::Version(value) && value->has_latest_key <= 1;
}
R::OutboundEnqueueOptions Options(const sc_outbound_item_options& value)
{
    R::OutboundEnqueueOptions result;
    if (value.has_latest_key)
        result.latestKey = value.latest_key;
    if (value.expiry_ms != UINT32_MAX)
        result.expires = Clock::now() + std::chrono::milliseconds(value.expiry_ms);
    return result;
}
}
extern "C"
{
    sc_status sc_tick_options_init(sc_tick_options* out, size_t size)
    {
        return Init(out, size, { 0, 0, 16, SC_TICK_SKIP, 4, 0 });
    }
    sc_status sc_tick_start(sc_timer_scheduler* scheduler, const sc_tick_options* options,
        sc_tick_work callback, sc_tick** out)
    {
        Work owned(callback);
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!scheduler || !C::Version(options) || !callback.work ||
            options->lag_policy > SC_TICK_CATCH_UP)
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto lease = C::RetainTimerScheduler(scheduler);
                if (!lease.scheduler)
                    return SC_INVALID_ARGUMENT;
                R::TickOptions native;
                native.interval = std::chrono::milliseconds(options->interval_ms);
                native.lagPolicy = static_cast<R::TickLagPolicy>(options->lag_policy);
                native.maxCatchUp = options->max_catch_up;
                native.retainedBytes = options->retained_bytes;
                // Allocate public output before scheduling work, preventing an output
                // allocation failure from leaving an unreachable recurring timer alive.
                auto output = std::make_unique<sc_tick>();
                auto value = R::ScheduleTicks(
                    *lease.scheduler,
                    [work = std::move(owned), keep = lease.owner](
                        const R::TickInfo& tick, std::stop_token token) mutable
                    {
                        (void)keep;
                        return work.Run(tick, token);
                    },
                    native);
                if (!value.IsOk())
                    return C::Code(value.GetStatus());
                output->scheduler = std::move(lease);
                output->value.emplace(std::move(value.Value()));
                *out = output.release();
                return SC_OK;
            });
    }
    int sc_tick_token_requested(const sc_tick_token* value)
    {
        return value && value->value.stop_requested();
    }
    void sc_tick_cancel(sc_tick* value)
    {
        if (value)
            (void)value->value->RequestCancel();
    }
    sc_status sc_tick_result(const sc_tick* value)
    {
        return value ? C::Code(value->value->GetStatus()) : SC_INVALID_ARGUMENT;
    }
    int sc_tick_finished(const sc_tick* value)
    {
        return value && value->value->IsFinished();
    }
    sc_status sc_tick_wait(const sc_tick* value, uint32_t timeout)
    {
        if (!value)
            return SC_INVALID_ARGUMENT;
        if (!timeout)
            return sc_tick_result(value);
        return C::Protect(
            [&]
            {
                return C::Code(timeout == UINT32_MAX
                                   ? value->value->Wait()
                                   : value->value->WaitUntil(
                                         Clock::now() + std::chrono::milliseconds(timeout)));
            });
    }
    sc_status sc_tick_get_metrics(const sc_tick* value, sc_tick_metrics* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        const auto metrics = value->value->Metrics();
        *out = {
            metrics.executed, metrics.skipped,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(metrics.lastLateness).count()),
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(metrics.maxLateness).count())
        };
        return SC_OK;
    }
    sc_status sc_tick_subscribe(
        const sc_tick* value, sc_notifier* notifier, uint64_t key, sc_subscription** out)
    {
        if (!value)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::SubscribeCompletion([&](auto callback)
            { return value->value->WaitForCompletion(std::move(callback)); }, notifier, key, out);
    }
    void sc_tick_destroy(sc_tick* value)
    {
        delete value;
    }
    sc_status sc_outbound_options_init(sc_outbound_options* out, size_t size)
    {
        return Init(out, size, { 0, 0, 128, 4 * 1024 * 1024, 64 });
    }
    sc_status sc_outbound_item_options_init(sc_outbound_item_options* out, size_t size)
    {
        return Init(out, size, { 0, 0, 0, UINT32_MAX, 0 });
    }
    sc_status sc_outbound_create(sc_timer_scheduler* scheduler, sc_tcp_connection* connection,
        const sc_outbound_options* options, sc_outbound_queue** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Version(options))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto lease = C::RetainTimerScheduler(scheduler);
                auto peer = C::RetainTcpConnection(connection);
                if (!lease.scheduler || !peer.connection)
                    return SC_INVALID_ARGUMENT;
                auto value = R::OutboundQueue::ForConnection(*lease.scheduler, peer.connection,
                    { options->max_messages, options->max_retained_bytes,
                        options->max_pump_messages });
                if (!value.IsOk())
                    return C::Code(value.GetStatus());
                *out = new sc_outbound_queue{ std::move(lease), std::move(peer),
                    options->max_retained_bytes, std::move(value.Value()) };
                return SC_OK;
            });
    }
    sc_status sc_outbound_enqueue(
        sc_outbound_queue* value, sc_bytes bytes, const sc_outbound_item_options* options)
    {
        if (!value || !C::Valid(bytes) || !Valid(options))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto payload = R::OutboundPayload::CopyBytes(C::Bytes(bytes), value->maxBytes);
                if (!payload.IsOk())
                    return C::Code(payload.GetStatus());
                return C::Code(
                    value->value->Enqueue(std::move(payload.Value()), Options(*options)));
            });
    }
    sc_status sc_outbound_batch(sc_outbound_queue* const* recipients, size_t count, sc_bytes bytes,
        const sc_outbound_item_options* options, sc_status* results, size_t resultCount)
    {
        if ((count && (!recipients || !results)) || resultCount < count || !C::Valid(bytes) ||
            !Valid(options))
            return SC_INVALID_ARGUMENT;
        if (count > 4096)
            return SC_TOO_LARGE;
        return C::Protect(
            [&]() -> sc_status
            {
                std::vector<std::shared_ptr<R::OutboundQueue>> targets;
                targets.reserve(count);
                size_t max = 0;
                for (size_t index = 0; index < count; ++index)
                {
                    targets.push_back(recipients[index] ? recipients[index]->value : nullptr);
                    if (recipients[index])
                        max = (std::max)(max, recipients[index]->maxBytes);
                }
                if (!count)
                    return SC_OK;
                if (!max)
                {
                    for (size_t index = 0; index < count; ++index)
                        results[index] = SC_INVALID_ARGUMENT;
                    return SC_OK;
                }
                auto payload = R::OutboundPayload::CopyBytes(C::Bytes(bytes), max);
                if (!payload.IsOk())
                    return C::Code(payload.GetStatus());
                auto accepted =
                    R::BatchEnqueue(targets, std::move(payload.Value()), Options(*options));
                if (!accepted.IsOk())
                    return C::Code(accepted.GetStatus());
                for (size_t index = 0; index < count; ++index)
                    results[index] = static_cast<sc_status>(accepted.Value()[index]);
                return SC_OK;
            });
    }
    void sc_outbound_begin_drain(sc_outbound_queue* value)
    {
        if (value)
            value->value->BeginDrain();
    }
    void sc_outbound_close(sc_outbound_queue* value)
    {
        if (value)
            value->value->Close();
    }
    int sc_outbound_finished(const sc_outbound_queue* value)
    {
        return value && value->value->IsFinished();
    }
    sc_status sc_outbound_result(const sc_outbound_queue* value)
    {
        return value ? C::Code(value->value->GetStatus()) : SC_INVALID_ARGUMENT;
    }
    sc_status sc_outbound_get_metrics(const sc_outbound_queue* value, sc_outbound_metrics* out)
    {
        if (!value || !out)
            return SC_INVALID_ARGUMENT;
        const auto m = value->value->Metrics();
        *out = { m.pending, m.retainedBytes, m.enqueued, m.sent, m.replaced, m.expired,
            m.discarded };
        return SC_OK;
    }
    sc_status sc_outbound_subscribe(
        const sc_outbound_queue* value, sc_notifier* notifier, uint64_t key, sc_subscription** out)
    {
        if (!value)
        {
            if (out)
                *out = nullptr;
            return SC_INVALID_ARGUMENT;
        }
        return C::SubscribeCompletion([&](auto callback)
            { return value->value->WaitForCompletion(std::move(callback)); }, notifier, key, out);
    }
    void sc_outbound_destroy(sc_outbound_queue* value)
    {
        delete value;
    }
}
