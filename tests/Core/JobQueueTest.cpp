#include "TestHarness.h"

#include "ServerCore/Core/JobQueue.h"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace
{
using ServerCore::Core::JobQueue;

void JobQueueRunsSnapshotOnly()
{
    JobQueue queue;
    std::vector<int> order;

    const ServerCore::Core::Status outerPosted = queue.Post([&queue, &order]() {
        order.push_back(1);
        const ServerCore::Core::Status innerPosted = queue.Post([&order]() { order.push_back(2); });
        ServerCoreTest::ExpectTrue(innerPosted.IsOk(), "job posted while draining is accepted");
    });
    ServerCoreTest::ExpectTrue(outerPosted.IsOk(), "initial job is accepted");

    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, queue.DrainOnce(), "first drain takes its initial snapshot");
    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, order.size(), "nested job waits for the next drain");
    ServerCoreTest::ExpectEqual(1, order[0], "first snapshot keeps insertion order");

    ServerCoreTest::ExpectEqual(std::size_t{ 1 }, queue.DrainOnce(), "second drain takes nested job");
    ServerCoreTest::ExpectEqual(std::size_t{ 2 }, order.size(), "nested job eventually executes");
    ServerCoreTest::ExpectEqual(2, order[1], "nested job runs after the first snapshot");
}

void JobQueueAcceptsConcurrentPosts()
{
    JobQueue queue;
    std::atomic<int> executed = 0;
    std::atomic<bool> everyPostSucceeded = true;
    constexpr int ProducerCount = 4;
    constexpr int JobsPerProducer = 64;

    std::vector<std::thread> producers;
    producers.reserve(ProducerCount);
    for (int producer = 0; producer < ProducerCount; ++producer)
    {
        producers.emplace_back([&queue, &executed, &everyPostSucceeded]() {
            for (int job = 0; job < JobsPerProducer; ++job)
            {
                const ServerCore::Core::Status posted = queue.Post([&executed]() { ++executed; });
                if (!posted.IsOk())
                {
                    everyPostSucceeded.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& producer : producers)
    {
        producer.join();
    }

    ServerCoreTest::ExpectTrue(everyPostSucceeded.load(std::memory_order_relaxed), "all producers post");
    ServerCoreTest::ExpectEqual(
        static_cast<std::size_t>(ProducerCount * JobsPerProducer),
        queue.PendingCount(),
        "concurrent posts all remain pending");
    ServerCoreTest::ExpectEqual(
        static_cast<std::size_t>(ProducerCount * JobsPerProducer),
        queue.DrainOnce(),
        "one drain executes all producer jobs");
    ServerCoreTest::ExpectEqual(
        ProducerCount * JobsPerProducer, executed.load(std::memory_order_relaxed), "all posted jobs execute");
}

ServerCoreTest::CheckRegistration gJobQueueRunsSnapshotOnly(
    "Core.JobQueueRunsSnapshotOnly", &JobQueueRunsSnapshotOnly);
ServerCoreTest::CheckRegistration gJobQueueAcceptsConcurrentPosts(
    "Core.JobQueueAcceptsConcurrentPosts", &JobQueueAcceptsConcurrentPosts);
}
