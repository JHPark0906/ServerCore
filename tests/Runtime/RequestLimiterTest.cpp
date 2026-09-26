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
/// <summary>steady_clock이 기준 시각을 지날 때까지 바쁘게 기다린다. 기다린 길이로 판정하지 않는다.</summary>
void UntilClockPasses(std::chrono::steady_clock::time_point mark)
{
    while (std::chrono::steady_clock::now() <= mark)
    {
    }
}
// EXEC-4: 키 표가 가득 찼을 때, 활성 permit이 없고 버킷이 가득 찬 항목은 새로 만든 항목과 구별되지
// 않는데도 idleExpiry(기본 5분)가 지나기 전에는 비우지 않아 새 키를 거절했다. 채움 속도를 1ns당 한
// 토큰으로 두어, 시계가 한 번 움직이면 버킷이 가득 차게 한다.
void LimiterEvictsFullBucketsUnderPressure()
{
    Runtime::RequestLimiterOptions options;
    options.maxKeys = 4;
    options.burstTokens = 1;
    options.refillTokens = 1000000000;
    options.refillInterval = 1ms;
    options.maxConcurrentPerKey = 1;
    options.maxConcurrentTotal = 8;
    auto limiter = Runtime::RequestLimiter::Create(options).Value();
    for (const auto key : { "a", "b", "c", "d" })
    {
        auto acquired = limiter.TryAcquire(key);
        ExpectTrue(acquired.IsOk() && acquired.Value().Allowed(), "table fills with spent keys");
    }
    UntilClockPasses(std::chrono::steady_clock::now());
    auto fresh = limiter.TryAcquire("e");
    ExpectTrue(fresh.IsOk() && fresh.Value().Allowed(),
        "idle full buckets yield their slots to a new key under pressure");

    // 활성 permit을 쥔 항목은 비우지 않고, permit을 돌려받은 뒤에는 다시 비울 수 있다.
    std::vector<Runtime::RequestPermit> held;
    if (fresh.IsOk() && fresh.Value().Allowed())
        held.push_back(std::move(*fresh.Value().permit));
    for (const auto key : { "f", "g", "h" })
    {
        auto acquired = limiter.TryAcquire(key);
        if (acquired.IsOk() && acquired.Value().Allowed())
            held.push_back(std::move(*acquired.Value().permit));
    }
    ExpectTrue(held.size() == 4 && limiter.Snapshot().keys == 4, "table full of active keys");
    UntilClockPasses(std::chrono::steady_clock::now());
    auto blocked = limiter.TryAcquire("i");
    ExpectTrue(blocked.IsOk() && blocked.Value().reason == Runtime::RequestLimitReason::KeyCapacity,
        "active keys are never evicted");
    if (!held.empty())
        held.pop_back();
    UntilClockPasses(std::chrono::steady_clock::now());
    auto released = limiter.TryAcquire("i");
    ExpectTrue(released.IsOk() && released.Value().Allowed(),
        "a returned permit makes its full bucket evictable again");

    // 버킷이 비어 있는 항목은 압박 중에도 비우지 않는다. 비우면 소진한 속도 한도가 초기화된다.
    options.refillInterval = 24h;
    options.refillTokens = 1;
    auto depleted = Runtime::RequestLimiter::Create(options).Value();
    for (const auto key : { "a", "b", "c", "d" })
        (void)depleted.TryAcquire(key);
    UntilClockPasses(std::chrono::steady_clock::now());
    auto denied = depleted.TryAcquire("e");
    ExpectTrue(denied.IsOk() && denied.Value().reason == Runtime::RequestLimitReason::KeyCapacity,
        "depleted buckets keep their slots under pressure");
    ExpectTrue(depleted.TryAcquire("a").Value().reason == Runtime::RequestLimitReason::Rate,
        "a kept depleted bucket still limits its key");
}
ServerCoreTest::CheckRegistration a(
    "Runtime.RequestLimiterBudgetsAndLease", LimiterBudgetsAndLease);
ServerCoreTest::CheckRegistration b(
    "Runtime.RequestLimiterExpiryAndConcurrency", LimiterExpiryAndConcurrency);
ServerCoreTest::CheckRegistration c(
    "Runtime.RequestLimiterEvictsFullBucketsUnderPressure", LimiterEvictsFullBucketsUnderPressure);
}
