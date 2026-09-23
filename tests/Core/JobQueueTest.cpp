#include "TestHarness.h"

#include "ServerCore/Core/JobQueue.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using ServerCore::Core::JobQueue;

// Small noexcept-movable callables may be moved inline by the STL. Such a move
// is user code and must be able to query the queue without taking its own lock twice.
struct ReentrantJob
{
    JobQueue* queue;
    int* moves;
    ReentrantJob(JobQueue& owner, int& count) : queue(&owner), moves(&count) {}
    ReentrantJob(const ReentrantJob&) = delete;
    ReentrantJob(ReentrantJob&& other) noexcept : queue(other.queue), moves(other.moves)
    {
        (void)queue->PendingCount();
        ++*moves;
    }
    void operator()() { (void)queue->PendingCount(); }
};

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

struct OwnedJobResource
{
    int* destructions;
    ~OwnedJobResource() { ++*destructions; }
};

void JobQueueOwnsMoveOnlyJobs()
{
    int destructions = 0;
    int executions = 0;
    {
        JobQueue queue;
        auto resource = std::make_unique<OwnedJobResource>(&destructions);
        const auto* identity = resource.get();
        JobQueue::Job job([owned = std::move(resource), identity, &executions] {
            ServerCoreTest::ExpectTrue(owned.get() == identity,
                "the queued job retains the exact exclusively owned resource");
            ++executions;
        });
        ServerCoreTest::ExpectTrue(queue.Post(std::move(job)).IsOk() && !resource,
            "a job with a unique_ptr capture transfers into the queue");
        ServerCoreTest::ExpectEqual(0, destructions, "pending work retains its owned resource");
        ServerCoreTest::ExpectEqual(std::size_t{1}, queue.DrainOnce(), "the owned job executes once");
        ServerCoreTest::ExpectEqual(1, executions, "draining invokes the owned callback");
        ServerCoreTest::ExpectEqual(1, destructions,
            "draining releases the executed job's resource exactly once");
        ServerCoreTest::ExpectTrue(queue.Post(
            [owned = std::make_unique<OwnedJobResource>(&destructions), &executions] {
                if (owned) ++executions;
            }).IsOk(), "a second move-only job may remain queued until destruction");
        ServerCoreTest::ExpectEqual(1, destructions,
            "a job that has not run still owns its captured resource");
    }
    ServerCoreTest::ExpectEqual(1, executions, "destroying a queue does not invoke pending work");
    ServerCoreTest::ExpectEqual(2, destructions,
        "destroying a queue releases pending move-only captures exactly once");
    JobQueue queue;
    int moves = 0;
    ServerCoreTest::ExpectTrue(queue.Post(ReentrantJob(queue, moves)).IsOk(),
        "moving a small callable can query its queue without deadlocking");
    ServerCoreTest::ExpectEqual(std::size_t{1}, queue.DrainOnce(), "the reentrant callable runs once");
    ServerCoreTest::ExpectTrue(moves > 0, "the callable's move constructor was exercised");
}

void JobQueueRejectsEmptyLegacyJobs()
{
    JobQueue queue;
    const auto invalid = ServerCore::Core::ErrorCode::InvalidArgument;
    std::function<void()> empty;
    const std::function<void()> constEmpty;
    ServerCoreTest::ExpectTrue(queue.Post(empty).Code() == invalid &&
                                  queue.Post(constEmpty).Code() == invalid &&
                                  queue.Post(std::move(empty)).Code() == invalid,
        "empty legacy std::function lvalues and rvalues remain InvalidArgument");
    ServerCoreTest::ExpectTrue(queue.Post(JobQueue::Job{}).Code() == invalid &&
                                  queue.Post(nullptr).Code() == invalid &&
                                  queue.Post({}).Code() == invalid,
        "empty canonical jobs, nullptr and braces resolve without overload ambiguity");
    ServerCoreTest::ExpectEqual(std::size_t{0}, queue.PendingCount(),
        "empty job rejection leaves the queue unchanged");
    int executions = 0;
    std::function<void()> legacy = [&executions] { ++executions; };
    ServerCoreTest::ExpectTrue(queue.Post(legacy).IsOk() && queue.Post(std::move(legacy)).IsOk(),
        "nonempty legacy std::function remains accepted by copy and move");
    ServerCoreTest::ExpectEqual(std::size_t{2}, queue.DrainOnce(),
        "accepted legacy jobs use the same queue and execution path");
    ServerCoreTest::ExpectEqual(2, executions, "both accepted legacy callbacks run");
}

ServerCoreTest::CheckRegistration gJobQueueRunsSnapshotOnly(
    "Core.JobQueueRunsSnapshotOnly", &JobQueueRunsSnapshotOnly);
ServerCoreTest::CheckRegistration gJobQueueAcceptsConcurrentPosts(
    "Core.JobQueueAcceptsConcurrentPosts", &JobQueueAcceptsConcurrentPosts);
ServerCoreTest::CheckRegistration gJobQueueOwnsMoveOnlyJobs(
    "Core.JobQueueOwnsMoveOnlyJobs", &JobQueueOwnsMoveOnlyJobs);
ServerCoreTest::CheckRegistration gJobQueueRejectsEmptyLegacyJobs(
    "Core.JobQueueRejectsEmptyLegacyJobs", &JobQueueRejectsEmptyLegacyJobs);
}
