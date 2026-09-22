#include "Web/HttpObservationInternal.h"
#include "Observability/MetricsInternal.h"
#include "Web/WebCallbackInternal.h"
#include <algorithm>
#include <utility>

namespace ServerCore::Web::Detail
{
using Core::ErrorCode;
using Core::Status;
namespace Metric = Observability::Detail;
Status HttpObservation::Configure(Observability::RequestTraceHandler callback, std::size_t maximum)
{
    if (!callback || maximum == 0 || maximum > 65536)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    // Keep arbitrary callback capture destruction outside the queue mutex.
    auto stored = std::make_shared<const Observability::RequestTraceHandler>(std::move(callback));
    std::shared_ptr<const Observability::RequestTraceHandler> previous;
    {
        const std::lock_guard guard(mMutex);
        if (mStarted || mStopping)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        previous = std::exchange(mCallback, std::move(stored));
        mMaximum = maximum;
    }
    return Status::Ok();
}
Status HttpObservation::Start()
{
    const std::lock_guard guard(mMutex);
    if (mStarted || mStopping)
        return Status::FailWithoutMessage(ErrorCode::Closed);
    mStarted = true;
    if (!mCallback)
        return Status::Ok();
    try
    {
        mWorker = std::thread([this] { Run(); });
        return Status::Ok();
    }
    catch (...)
    {
        mStopping = true;
        return Status::AllocationFailure();
    }
}
void HttpObservation::Stop()
{
    {
        const std::lock_guard guard(mMutex);
        mStopping = true;
    }
    mWake.notify_all();
    if (mWorker.joinable())
        mWorker.join();
}
void HttpObservation::Terminal(Observability::RequestTrace trace) noexcept
{
    Metric::Add(completedRequests);
    if (trace.outcome == ErrorCode::Cancelled)
        Metric::Add(cancelledRequests);
    else if (trace.outcome == ErrorCode::Timeout)
        Metric::Add(timedOutRequests);
    else if (trace.outcome != ErrorCode::Ok)
        Metric::Add(failedRequests);
    Metric::Add(mTotalLatency, trace.elapsedNanoseconds);
    Metric::Maximum(mMaxLatency, trace.elapsedNanoseconds);
    Metric::Add(mLatencyBuckets[Metric::LatencyBucket(trace.elapsedNanoseconds)]);
    try
    {
        const std::lock_guard guard(mMutex);
        if (!mCallback)
            return;
        if (mStopping || mQueue.size() >= mMaximum)
        {
            Metric::Add(mDropped);
            return;
        }
        mQueue.push_back(std::move(trace));
        mWake.notify_one();
    }
    catch (...)
    {
        Metric::Add(mDropped);
    }
}
void HttpObservation::Run() noexcept
{
    const WebCallbackScope callbackScope(mOwner);
    for (;;)
    {
        Observability::RequestTrace trace;
        {
            std::unique_lock guard(mMutex);
            mWake.wait(guard, [this] { return mStopping || !mQueue.empty(); });
            if (mQueue.empty() && mStopping)
                return;
            trace = std::move(mQueue.front());
            mQueue.pop_front();
        }
        try
        {
            (*mCallback)(std::move(trace));
        }
        catch (...)
        {
            Metric::Add(mCallbackErrors);
        }
    }
}
Observability::HttpServerMetricsSnapshot HttpObservation::Snapshot() const noexcept
{
    Observability::HttpServerMetricsSnapshot value;
    value.acceptedConnections = acceptedConnections.load();
    value.closedConnections = closedConnections.load();
    value.rejectedConnections = rejectedConnections.load();
    value.acceptedRequests = acceptedRequests.load();
    value.completedRequests = completedRequests.load();
    value.failedRequests = failedRequests.load();
    value.cancelledRequests = cancelledRequests.load();
    value.timedOutRequests = timedOutRequests.load();
    value.rejectedRequests = rejectedRequests.load();
    value.protocolErrors = protocolErrors.load();
    value.droppedTraceEvents = mDropped.load();
    value.traceCallbackErrors = mCallbackErrors.load();
    value.totalLatencyNanoseconds = mTotalLatency.load();
    value.maxLatencyNanoseconds = mMaxLatency.load();
    for (std::size_t index = 0; index < mLatencyBuckets.size(); ++index)
        value.latencyHistogram.buckets[index] = mLatencyBuckets[index].load();
    const std::lock_guard guard(mMutex);
    value.pendingTraceEvents = mQueue.size();
    return value;
}
RequestObservation::RequestObservation(std::shared_ptr<HttpObservation> owner, std::uint64_t id,
    std::uint64_t connection, std::string_view method)
    : mOwner(std::move(owner))
{
    mTrace.requestId = id;
    mTrace.connectionId = connection;
    mTrace.method = method.substr(0, 64);
    Metric::Add(mOwner->acceptedRequests);
}
void RequestObservation::Finish(ErrorCode result, unsigned status) noexcept
{
    if (mFinished.test_and_set())
        return;
    mTrace.status = status;
    mTrace.outcome = result;
    mTrace.elapsedNanoseconds = Metric::Elapsed(mStarted);
    mOwner->Terminal(std::move(mTrace));
}
}
