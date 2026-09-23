#include "ServerCore/C/RequestLimiter.h"
#include "C/Internal.h"
#include "ServerCore/Runtime/RequestLimiter.h"
namespace C = ServerCore::CDetail;
namespace R = ServerCore::Runtime;
struct sc_request_limiter
{
    R::RequestLimiter value;
};
struct sc_request_permit
{
    R::RequestPermit value;
};
extern "C"
{
    sc_status sc_request_limiter_options_init(sc_request_limiter_options* o, size_t size)
    {
        if (!o || size < sizeof(*o))
            return SC_INVALID_ARGUMENT;
        *o = { SC_ABI_VERSION, sizeof(*o), 1024, 128, 128 * 1024, 100, 200, 1000, 8, 1024, 300000 };
        return SC_OK;
    }
    sc_status sc_request_limiter_create(
        const sc_request_limiter_options* o, sc_request_limiter** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!C::Version(o))
            return SC_INVALID_ARGUMENT;
        return C::Protect(
            [&]() -> sc_status
            {
                auto created = R::RequestLimiter::Create({ o->max_keys, o->max_key_bytes,
                    o->max_retained_key_bytes, o->refill_tokens, o->burst_tokens,
                    std::chrono::milliseconds(o->refill_interval_ms), o->max_concurrent_per_key,
                    o->max_concurrent_total, std::chrono::milliseconds(o->idle_expiry_ms) });
                if (!created.IsOk())
                    return C::Code(created.GetStatus());
                *out = new sc_request_limiter{ std::move(created.Value()) };
                return SC_OK;
            });
    }
    void sc_request_limiter_destroy(sc_request_limiter* limiter)
    {
        delete limiter;
    }
    void sc_request_limiter_close(sc_request_limiter* limiter)
    {
        if (limiter)
            limiter->value.Close();
    }
    sc_status sc_request_limiter_acquire(sc_request_limiter* limiter, sc_bytes key, uint64_t cost,
        sc_request_limit_decision* decision, sc_request_permit** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        if (!limiter || !C::Valid(key) || !C::Version(decision))
            return SC_INVALID_ARGUMENT;
        decision->reason = SC_LIMIT_NONE;
        decision->retry_after_ms = 0;
        return C::Protect(
            [&]() -> sc_status
            {
                auto owner = std::make_unique<sc_request_permit>();
                auto acquired = limiter->value.TryAcquire(C::Text(key), cost);
                if (!acquired.IsOk())
                    return C::Code(acquired.GetStatus());
                auto& value = acquired.Value();
                decision->reason = static_cast<uint32_t>(value.reason);
                decision->retry_after_ms = static_cast<uint64_t>(value.retryAfter.count());
                if (value.permit)
                {
                    owner->value = std::move(*value.permit);
                    *out = owner.release();
                }
                return SC_OK;
            });
    }
    void sc_request_permit_destroy(sc_request_permit* permit)
    {
        delete permit;
    }
    size_t sc_request_limiter_prune(sc_request_limiter* limiter)
    {
        return limiter ? limiter->value.PruneExpired() : 0;
    }
    size_t sc_request_limiter_key_count(const sc_request_limiter* limiter)
    {
        return limiter ? limiter->value.Snapshot().keys : 0;
    }
    size_t sc_request_limiter_active_count(const sc_request_limiter* limiter)
    {
        return limiter ? limiter->value.Snapshot().active : 0;
    }
}
