#include "TestHarness.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/PeriodicRunner.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using ServerCore::Core::ErrorCode;
using ServerCore::Runtime::JobRunner;
using ServerCore::Runtime::PeriodicRunner;

bool WaitFor(std::condition_variable& wake, std::unique_lock<std::mutex>& guard,
    const std::function<bool()>& condition)
{
    return wake.wait_for(guard, 2s, condition);
}

void JobRunnerDrainsAcceptedJobsOnStop()
{
    JobRunner runner;
    std::vector<int> order;

    const ServerCore::Core::Status first = runner.Post([&order]() { order.push_back(1); });
    const ServerCore::Core::Status second = runner.Post([&order]() { order.push_back(2); });
    ServerCoreTest::ExpectTrue(first.IsOk() && second.IsOk(), "jobs are accepted before Stop");

    runner.Stop();
    std::thread worker([&runner]() { runner.RunUntilStopped(); });
    worker.join();

    ServerCoreTest::ExpectEqual(std::size_t{ 2 }, order.size(), "accepted jobs drain after Stop");
    if (order.size() == 2)
    {
        ServerCoreTest::ExpectEqual(1, order[0], "first accepted job keeps its order");
        ServerCoreTest::ExpectEqual(2, order[1], "second accepted job keeps its order");
    }

    const ServerCore::Core::Status rejected = runner.Post([]() {});
    ServerCoreTest::ExpectTrue(
        !rejected.IsOk() && rejected.Code() == ErrorCode::Closed, "Post after Stop reports Closed");
}

void JobRunnerReportsThreadAffinity()
{
    JobRunner runner;
    std::mutex mutex;
    std::condition_variable wake;
    bool executed = false;
    bool ranOnRunnerThread = false;

    const ServerCore::Core::Status posted = runner.Post(
        [&]()
        {
            {
                const std::lock_guard<std::mutex> guard(mutex);
                ranOnRunnerThread = runner.IsCurrentThread();
                executed = true;
            }
            wake.notify_one();
        });
    ServerCoreTest::ExpectTrue(posted.IsOk(), "affinity probe is accepted");

    std::thread worker([&runner]() { runner.RunUntilStopped(); });
    {
        std::unique_lock<std::mutex> guard(mutex);
        ServerCoreTest::ExpectTrue(
            WaitFor(wake, guard, [&executed]() { return executed; }), "probe runs");
    }

    runner.Stop();
    worker.join();

    ServerCoreTest::ExpectTrue(ranOnRunnerThread, "job runs on JobRunner thread");
    ServerCoreTest::ExpectTrue(
        !runner.IsCurrentThread(), "affinity is false after RunUntilStopped returns");
}

void PeriodicRunnerInvokesThroughJobRunner()
{
    JobRunner jobRunner;
    std::thread worker([&jobRunner]() { jobRunner.RunUntilStopped(); });

    std::mutex mutex;
    std::condition_variable wake;
    std::size_t callCount = 0;
    bool everyCallUsedRunner = true;
    PeriodicRunner periodic(jobRunner, 20ms,
        [&]()
        {
            {
                const std::lock_guard<std::mutex> guard(mutex);
                everyCallUsedRunner = everyCallUsedRunner && jobRunner.IsCurrentThread();
                ++callCount;
            }
            wake.notify_all();
        });

    const ServerCore::Core::Status started = periodic.Start();
    ServerCoreTest::ExpectTrue(started.IsOk(), "periodic runner starts");
    {
        std::unique_lock<std::mutex> guard(mutex);
        ServerCoreTest::ExpectTrue(WaitFor(wake, guard, [&callCount]() { return callCount >= 2; }),
            "periodic callback runs more than once");
    }

    periodic.Stop();

    bool drained = false;
    const ServerCore::Core::Status sentinelPosted = jobRunner.Post(
        [&]()
        {
            {
                const std::lock_guard<std::mutex> guard(mutex);
                drained = true;
            }
            wake.notify_all();
        });
    ServerCoreTest::ExpectTrue(sentinelPosted.IsOk(), "post-stop sentinel is accepted");
    {
        std::unique_lock<std::mutex> guard(mutex);
        ServerCoreTest::ExpectTrue(
            WaitFor(wake, guard, [&drained]() { return drained; }), "queued work drains");
        const std::size_t callsAfterStop = callCount;
        guard.unlock();
        std::this_thread::sleep_for(80ms);
        guard.lock();
        ServerCoreTest::ExpectEqual(
            callsAfterStop, callCount, "Stop prevents new periodic callbacks");
    }

    jobRunner.Stop();
    worker.join();
    ServerCoreTest::ExpectTrue(everyCallUsedRunner, "every periodic callback uses JobRunner");
}

void PeriodicRunnerCountsSkippedPeriods()
{
    JobRunner jobRunner;
    std::thread worker([&jobRunner]() { jobRunner.RunUntilStopped(); });

    std::mutex mutex;
    std::condition_variable wake;
    bool firstCallbackStarted = false;
    PeriodicRunner periodic(jobRunner, 10ms,
        [&]()
        {
            {
                const std::lock_guard<std::mutex> guard(mutex);
                firstCallbackStarted = true;
            }
            wake.notify_one();
            std::this_thread::sleep_for(80ms);
        });

    ServerCoreTest::ExpectTrue(periodic.Start().IsOk(), "slow periodic runner starts");
    {
        std::unique_lock<std::mutex> guard(mutex);
        ServerCoreTest::ExpectTrue(
            WaitFor(wake, guard, [&firstCallbackStarted]() { return firstCallbackStarted; }),
            "slow callback starts");
    }

    std::this_thread::sleep_for(50ms);
    periodic.Stop();
    ServerCoreTest::ExpectEqual(periodic.SkippedCount(), jobRunner.PeriodicSkippedCount(),
        "JobRunner aggregates skipped periods from its periodic leases");
    jobRunner.Stop();
    worker.join();

    ServerCoreTest::ExpectTrue(periodic.SkippedCount() != 0,
        "periods blocked by an outstanding callback are counted as skipped");
}

void PeriodicRunnerStopsSafelyAfterJobRunnerDestruction()
{
    std::mutex mutex;
    std::condition_variable wake;
    std::size_t callbackCount = 0;
    std::unique_ptr<PeriodicRunner> periodic;

    {
        auto runner = std::make_unique<JobRunner>();
        periodic = std::make_unique<PeriodicRunner>(*runner, 100ms,
            [&]()
            {
                {
                    const std::lock_guard<std::mutex> guard(mutex);
                    ++callbackCount;
                }
                wake.notify_all();
            });

        ServerCoreTest::ExpectTrue(
            periodic->Start().IsOk(), "periodic runner starts before owner destruction");

        // JobRunner에는 실행 스레드가 없으므로 여기서 소멸해도 별도 join이 필요 없다. PeriodicRunner는
        // 이 뒤 100ms 후에 예약을 시도하지만, 해제된 runner 주소를 읽지 않고 lease의 Closed를 본다.
        runner.reset();
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (periodic->IsRunning() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(5ms);
    }

    ServerCoreTest::ExpectTrue(
        !periodic->IsRunning(), "destroyed JobRunner closes the periodic lease");
    {
        const std::lock_guard<std::mutex> guard(mutex);
        ServerCoreTest::ExpectEqual(
            std::size_t{ 0 }, callbackCount, "destroyed JobRunner receives no periodic callback");
    }

    periodic->Stop();

    // JobRunner를 돌리지 않으면 주기 callback이 큐에서 State를 소유한 채 남는다. Stop이 State의
    // Lease를 놓지 않으면 그 State와 JobRunner 큐가 서로를 소유해 callback capture도 해제되지 않는다.
    auto undrainedRunner = std::make_unique<JobRunner>();
    auto callbackLifetime = std::make_shared<int>(0);
    const std::weak_ptr<int> observedCallbackLifetime = callbackLifetime;
    auto undrainedPeriodic = std::make_unique<PeriodicRunner>(
        *undrainedRunner, 10ms, [callbackLifetime]() { (void)callbackLifetime; });
    callbackLifetime.reset();

    ServerCoreTest::ExpectTrue(
        undrainedPeriodic->Start().IsOk(), "periodic runner starts without a JobRunner worker");
    const auto queuedDeadline = std::chrono::steady_clock::now() + 2s;
    while (
        undrainedRunner->PendingCount() == 0 && std::chrono::steady_clock::now() < queuedDeadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, undrainedRunner->PendingCount(),
        "one periodic callback remains queued in the undrained JobRunner");

    undrainedRunner.reset();
    undrainedPeriodic->Stop();
    undrainedPeriodic.reset();
    ServerCoreTest::ExpectTrue(observedCallbackLifetime.expired(),
        "Stop releases the JobRunner lease and breaks the undrained callback ownership cycle");
}

void PeriodicRunnerConcurrentStopsShareCompletedJoin()
{
    JobRunner jobRunner;
    std::thread worker([&jobRunner]() { jobRunner.RunUntilStopped(); });

    std::mutex mutex;
    std::condition_variable wake;
    std::size_t callbackCount = 0;
    std::atomic<int> stopReturnCount = 0;
    PeriodicRunner periodic(jobRunner, 10ms,
        [&]()
        {
            {
                const std::lock_guard<std::mutex> guard(mutex);
                ++callbackCount;
            }
            wake.notify_all();
        });

    ServerCoreTest::ExpectTrue(periodic.Start().IsOk(), "concurrent-stop periodic runner starts");
    {
        std::unique_lock<std::mutex> guard(mutex);
        ServerCoreTest::ExpectTrue(
            WaitFor(wake, guard, [&callbackCount]() { return callbackCount != 0; }),
            "concurrent-stop periodic callback starts");
    }

    std::thread firstStop(
        [&]()
        {
            periodic.Stop();
            stopReturnCount.fetch_add(1, std::memory_order_release);
        });
    std::thread secondStop(
        [&]()
        {
            periodic.Stop();
            stopReturnCount.fetch_add(1, std::memory_order_release);
        });

    firstStop.join();
    secondStop.join();

    ServerCoreTest::ExpectEqual(
        2, stopReturnCount.load(std::memory_order_acquire), "both concurrent Stop calls return");
    ServerCoreTest::ExpectTrue(
        !periodic.IsRunning(), "concurrent Stop leaves no timer loop running");

    // Stop 이전에 JobRunner로 들어간 콜백은 실행될 수 있다는 계약이므로, 먼저 그 실행자도
    // 배수해 놓고 이후에는 타이머가 새 콜백을 만들지 않는지만 확인한다.
    jobRunner.Stop();
    worker.join();

    std::size_t callsAfterStops = 0;
    {
        const std::lock_guard<std::mutex> guard(mutex);
        callsAfterStops = callbackCount;
    }
    std::this_thread::sleep_for(50ms);
    {
        const std::lock_guard<std::mutex> guard(mutex);
        ServerCoreTest::ExpectEqual(callsAfterStops, callbackCount,
            "completed concurrent Stops prevent new periodic callbacks");
    }
}

ServerCoreTest::CheckRegistration gJobRunnerDrainsAcceptedJobsOnStop(
    "Runtime.JobRunnerDrainsAcceptedJobsOnStop", &JobRunnerDrainsAcceptedJobsOnStop);
ServerCoreTest::CheckRegistration gJobRunnerReportsThreadAffinity(
    "Runtime.JobRunnerReportsThreadAffinity", &JobRunnerReportsThreadAffinity);
ServerCoreTest::CheckRegistration gPeriodicRunnerInvokesThroughJobRunner(
    "Runtime.PeriodicRunnerInvokesThroughJobRunner", &PeriodicRunnerInvokesThroughJobRunner);
ServerCoreTest::CheckRegistration gPeriodicRunnerCountsSkippedPeriods(
    "Runtime.PeriodicRunnerCountsSkippedPeriods", &PeriodicRunnerCountsSkippedPeriods);
ServerCoreTest::CheckRegistration gPeriodicRunnerStopsSafelyAfterJobRunnerDestruction(
    "Runtime.PeriodicRunnerStopsSafelyAfterJobRunnerDestruction",
    &PeriodicRunnerStopsSafelyAfterJobRunnerDestruction);
ServerCoreTest::CheckRegistration gPeriodicRunnerConcurrentStopsShareCompletedJoin(
    "Runtime.PeriodicRunnerConcurrentStopsShareCompletedJoin",
    &PeriodicRunnerConcurrentStopsShareCompletedJoin);
}
