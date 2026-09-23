#include "ServerCore/Runtime/RequestLimiter.h"
#include "TestHarness.h"
#include <mutex>
#include <thread>
#include <vector>
namespace
{
using namespace ServerCore;
using namespace std::chrono_literals;
using ServerCoreTest::ExpectTrue;
void LimiterBudgetsAndLease()
{
    Runtime::RequestLimiterOptions options;
    options.maxKeys = 2;
    options.maxKeyBytes = 4;
    options.maxRetainedKeyBytes = 5;
    options.burstTokens = 2;
    options.refillTokens = 1;
    options.refillInterval = 24h;
    options.maxConcurrentPerKey = 1;
    options.maxConcurrentTotal = 2;
    auto limiter = Runtime::RequestLimiter::Create(options).Value();
    auto one = limiter.TryAcquire("abc");
    ExpectTrue(one.IsOk() && one.Value().Allowed(), "initial key admitted");
    auto conflict = limiter.TryAcquire("abc");
    ExpectTrue(
        conflict.IsOk() && conflict.Value().reason == Runtime::RequestLimitReason::KeyConcurrency,
        "per-key concurrency bounds work");
    auto table = limiter.TryAcquire("def");
    ExpectTrue(table.IsOk() && table.Value().reason == Runtime::RequestLimitReason::KeyCapacity,
        "key byte budget enforced");
    auto two = limiter.TryAcquire("xy");
    ExpectTrue(two.IsOk() && two.Value().Allowed() && limiter.Snapshot().retainedKeyBytes == 5,
        "exact key budget succeeds");
    ExpectTrue(
        limiter.TryAcquire("z").Value().reason == Runtime::RequestLimitReason::TotalConcurrency,
        "total concurrency denies without new key");
    one.Value().permit.reset();
    auto retry = limiter.TryAcquire("abc");
    ExpectTrue(
        retry.IsOk() && retry.Value().Allowed(), "concurrency denials never spend rate tokens");
    retry.Value().permit.reset();
    auto rate = limiter.TryAcquire("abc");
    ExpectTrue(rate.IsOk() && rate.Value().reason == Runtime::RequestLimitReason::Rate &&
                   rate.Value().retryAfter > 0ms,
        "released permit never refunds spent rate tokens");
    ExpectTrue(limiter.TryAcquire("abcde").GetStatus().Code() == Core::ErrorCode::TooLarge &&
                   limiter.TryAcquire("abc", 3).GetStatus().Code() == Core::ErrorCode::TooLarge,
        "oversized key/cost distinct from transient denial");
    limiter.Close();
    ExpectTrue(limiter.TryAcquire("xy").GetStatus().Code() == Core::ErrorCode::Closed,
        "close prevents new work");
    two.Value().permit.reset();
    ExpectTrue(limiter.Snapshot().active == 0 && limiter.Snapshot().keys == 0,
        "last permits retire closed registry");
}
void LimiterExpiryAndConcurrency()
{
    Runtime::RequestLimiterOptions options;
    options.maxKeys = 1;
    options.burstTokens = 1;
    options.refillTokens = 1;
    options.refillInterval = 10s;
    options.idleExpiry = 1ms;
    auto limited = Runtime::RequestLimiter::Create(options).Value();
    auto spent = limited.TryAcquire("old");
    spent.Value().permit.reset();
    std::this_thread::sleep_for(10ms);
    ExpectTrue(limited.PruneExpired() == 0 && limited.TryAcquire("new").Value().reason ==
                                                  Runtime::RequestLimitReason::KeyCapacity,
        "idle eviction cannot reset a depleted token bucket");
    options.refillInterval = 1ms;
    auto expiry = Runtime::RequestLimiter::Create(options).Value();
    auto held = expiry.TryAcquire("one");
    std::this_thread::sleep_for(10ms);
    ExpectTrue(expiry.PruneExpired() == 0, "active permit prevents eviction");
    held.Value().permit.reset();
    std::this_thread::sleep_for(10ms);
    ExpectTrue(expiry.PruneExpired() == 1 && expiry.TryAcquire("two").Value().Allowed(),
        "idle full bucket expires and returns key capacity");

    options.maxKeys = 16;
    options.maxConcurrentPerKey = 2;
    options.maxConcurrentTotal = 3;
    options.burstTokens = 100;
    options.refillInterval = 1s;
    auto concurrent = Runtime::RequestLimiter::Create(options).Value();
    std::mutex mutex;
    std::vector<Runtime::RequestPermit> permits;
    std::vector<std::thread> workers;
    for (int i = 0; i < 12; ++i)
        workers.emplace_back(
            [&, i]
            {
                auto acquired = concurrent.TryAcquire(i % 2 ? "odd" : "even");
                if (acquired.IsOk() && acquired.Value().Allowed())
                {
                    std::lock_guard lock(mutex);
                    permits.push_back(std::move(*acquired.Value().permit));
                }
            });
    for (auto& worker : workers)
        worker.join();
    ExpectTrue(permits.size() == 3 && concurrent.Snapshot().active == 3,
        "concurrent admission preserves per-key and global bounds");
    permits.clear();
    ExpectTrue(concurrent.Snapshot().active == 0, "moving permits returns each charge once");
}
ServerCoreTest::CheckRegistration a(
    "Runtime.RequestLimiterBudgetsAndLease", LimiterBudgetsAndLease);
ServerCoreTest::CheckRegistration b(
    "Runtime.RequestLimiterExpiryAndConcurrency", LimiterExpiryAndConcurrency);
}
