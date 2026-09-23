#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Observability/Metrics.h"
#include "ServerCore/Observability/RequestTrace.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace ServerCore::Web::Detail
{
class HttpObservation
{
public:
    explicit HttpObservation(const void* owner) noexcept
        : mOwner(owner)
    {
    }
    SERVERCORE_TEST_API Core::Status Configure(Observability::RequestTraceHandler callback, std::size_t maximum);
    SERVERCORE_TEST_API Core::Status Start();
    SERVERCORE_TEST_API void Stop();
    SERVERCORE_TEST_API void Terminal(Observability::RequestTrace trace) noexcept;
    SERVERCORE_TEST_API Observability::HttpServerMetricsSnapshot Snapshot() const noexcept;
    std::atomic<std::uint64_t> acceptedConnections{ 0 }, closedConnections{ 0 },
        rejectedConnections{ 0 };
    std::atomic<std::uint64_t> acceptedRequests{ 0 }, completedRequests{ 0 }, failedRequests{ 0 };
    std::atomic<std::uint64_t> cancelledRequests{ 0 }, timedOutRequests{ 0 }, rejectedRequests{ 0 },
        protocolErrors{ 0 };

private:
    SERVERCORE_TEST_API void Run() noexcept;
    const void* mOwner;
    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::shared_ptr<const Observability::RequestTraceHandler> mCallback;
    std::deque<Observability::RequestTrace> mQueue;
    std::thread mWorker;
    std::size_t mMaximum = 256;
    bool mStarted = false, mStopping = false;
    std::atomic<std::uint64_t> mDropped{ 0 }, mCallbackErrors{ 0 }, mTotalLatency{ 0 },
        mMaxLatency{ 0 };
    std::array<std::atomic<std::uint64_t>, 12> mLatencyBuckets{};
};

class RequestObservation
{
public:
    SERVERCORE_TEST_API RequestObservation(std::shared_ptr<HttpObservation> owner, std::uint64_t id,
        std::uint64_t connection, std::string_view method);
    ~RequestObservation() { Finish(Core::ErrorCode::PlatformError, 0); }
    SERVERCORE_TEST_API void Finish(Core::ErrorCode result, unsigned status) noexcept;

private:
    std::shared_ptr<HttpObservation> mOwner;
    Observability::RequestTrace mTrace;
    const std::chrono::steady_clock::time_point mStarted = std::chrono::steady_clock::now();
    std::atomic_flag mFinished = ATOMIC_FLAG_INIT;
};
}
