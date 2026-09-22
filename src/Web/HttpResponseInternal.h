#pragma once

#include "ServerCore/Web/HttpServer.h"
#include "ServerCore/Net/Connection.h"
#include "Web/ResponseEncoding.h"

#include <atomic>
#include <mutex>

namespace ServerCore::Web::Detail
{
class RequestObservation;
std::int64_t WebNow() noexcept;

// Shared by HTTP and WebSocket sends on one transport. Monotonic admission
// accounting lets the maintenance thread distinguish draining from refilling.
class TrackedSender
{
public:
    explicit TrackedSender(std::shared_ptr<Net::Connection> value)
        : connection(std::move(value)), flow(Net::GetConnectionFlowControl(connection)) {}
    Core::Status Send(std::span<const std::byte> bytes);
    const std::shared_ptr<Net::Connection> connection;
    const std::shared_ptr<Net::ConnectionFlowControl> flow;
    std::atomic<std::uint64_t> admitted{0};
    std::atomic<std::int64_t> lastWrite{0};
};

class HttpResponseState final : public HttpResponseWriter
{
public:
    HttpResponseState(std::shared_ptr<TrackedSender> sender, const HttpServerOptions& options,
        bool head, bool close, bool connect, const void* callbackOwner, std::shared_ptr<void> reservation,
        std::shared_ptr<RequestObservation> observation = {}, HttpHeaders policyHeaders = {});
    Core::Status Start(const HttpResponseHead& head) override;
    Core::Status Write(std::span<const std::byte> bytes) override;
    Core::Status Finish() override;
    Core::Status Complete(const HttpResponse& response) override;
    void Abort() noexcept override;
    std::stop_token GetCancellationToken() const noexcept override;
    bool IsHeadRequest() const noexcept override { return mHead; }
    std::size_t MaxWriteBytes() const noexcept override;
    std::size_t RetainedSendBytes() const noexcept override;
    Core::Result<Net::SendCapacitySubscription> WaitForWriteCapacity(
        std::size_t bytes, std::function<void(Core::Status)> callback) override;
    void Cancel(Core::ErrorCode reason = Core::ErrorCode::Cancelled) noexcept;
    bool IsFinished() const noexcept;
    bool IsAborted() const noexcept;
    bool ShouldClose() const noexcept;
    bool HasStarted() const noexcept;
    bool TimedOut(std::int64_t now) const noexcept;
private:
    enum class Phase { New, Streaming, Finished, Aborted };
    bool MarkCancelled() noexcept;
    void Report(Core::ErrorCode reason) noexcept;
    void AbortWithReason(Core::ErrorCode reason) noexcept;
    Core::Status SendTransaction(std::string wire, Phase next, ResponseBodyMode mode,
        std::uint64_t length, std::uint64_t written, bool close, unsigned status);
    const std::shared_ptr<TrackedSender> mSender;
    const HttpServerOptions mOptions;
    const bool mHead;
    const bool mRequestClose;
    const bool mConnect;
    const void* const mCallbackOwner;
    const std::int64_t mCreated;
    std::stop_source mCancellation;
    std::stop_source mWaitCancellation;
    mutable std::mutex mMutex;
    Phase mPhase = Phase::New;
    ResponseBodyMode mMode = ResponseBodyMode::None;
    std::uint64_t mLength = 0;
    std::uint64_t mWritten = 0;
    std::int64_t mLastWrite;
    bool mClose = false;
    bool mSending = false;
    bool mWaitsRetired = false;
    std::size_t mBlockedWireBytes = 0;
    std::shared_ptr<void> mReservation;
    const std::shared_ptr<RequestObservation> mObservation;
    const HttpHeaders mPolicyHeaders;
    unsigned mStatus = 0;
};
}
