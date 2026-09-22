#include "Web/HttpResponseInternal.h"
#include "Web/WebCallbackInternal.h"
#include "Web/WebProtocol.h"
#include "Web/HttpObservationInternal.h"

#include <algorithm>
#include <utility>

namespace ServerCore::Web::Detail
{
using Core::Status;
using Core::ErrorCode;
namespace
{
Status Fail(ErrorCode code) noexcept { return Status::FailWithoutMessage(code); }
std::span<const std::byte> Bytes(std::string_view text) noexcept { return std::as_bytes(std::span(text.data(), text.size())); }
}

std::int64_t WebNow() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Status TrackedSender::Send(std::span<const std::byte> bytes)
{
    auto result = connection->Send(bytes);
    if (result.IsOk())
    {
        admitted.fetch_add(bytes.size(), std::memory_order_relaxed);
        lastWrite.store(WebNow(), std::memory_order_relaxed);
    }
    return result;
}

HttpResponseState::HttpResponseState(std::shared_ptr<TrackedSender> sender, const HttpServerOptions& options,
    bool head, bool close, bool connect, const void* callbackOwner, std::shared_ptr<void> reservation,
    std::shared_ptr<RequestObservation> observation, HttpHeaders policyHeaders)
    : mSender(std::move(sender)), mOptions(options), mHead(head), mRequestClose(close), mConnect(connect),
      mCallbackOwner(callbackOwner), mCreated(WebNow()), mLastWrite(mCreated), mReservation(std::move(reservation)),
      mObservation(std::move(observation)), mPolicyHeaders(std::move(policyHeaders)) {}

Status HttpResponseState::SendTransaction(std::string wire, Phase next, ResponseBodyMode mode,
    std::uint64_t length, std::uint64_t written, bool close, unsigned status)
{
    // The caller selected an exclusive transaction, then released mMutex. Send
    // can invoke transport notifications inline; no response lock may be held.
    Status result = Status::Ok();
    try { if (!wire.empty()) result = mSender->Send(Bytes(wire)); }
    catch (...) { result = Status::AllocationFailure(); }
    bool drain = false;
    {
        const std::lock_guard guard(mMutex);
        mSending = false;
        if (mPhase == Phase::Aborted) return Fail(ErrorCode::Closed);
        if (result.IsOk())
        {
            mBlockedWireBytes = 0;
            mPhase = next;
            mMode = mode;
            mLength = length;
            mWritten = written;
            mClose = close;
            if (status != 0) mStatus = status;
            mLastWrite = WebNow();
            if (next == Phase::Finished) { mReservation.reset(); drain = close; }
        }
        else if (result.Code() == ErrorCode::WouldBlock) mBlockedWireBytes = wire.size();
    }
    if (result.IsOk() && next == Phase::Finished)
    {
        Report(ErrorCode::Ok);
        // Capacity belongs to this response, not to a later pipelined request.
        // Keep successful completion separate from cancellation of user work.
        const WebCallbackScope callback(mCallbackOwner);
        (void)mWaitCancellation.request_stop();
        const std::lock_guard guard(mMutex);
        mWaitsRetired = true;
    }
    if (drain) mSender->connection->CloseAfterSend();
    if (!result.IsOk() && result.Code() != ErrorCode::WouldBlock) AbortWithReason(result.Code());
    return result;
}

Status HttpResponseState::Start(const HttpResponseHead& value)
{
    std::string wire;
    ResponseBodyMode mode;
    std::uint64_t length;
    bool close;
    try
    {
        const std::lock_guard guard(mMutex);
        if (mPhase == Phase::Finished || mPhase == Phase::Aborted) return Fail(ErrorCode::Closed);
        if (mSending) return Fail(ErrorCode::WouldBlock);
        if (mPhase != Phase::New) return Fail(ErrorCode::AlreadyExists);
        if (mConnect && value.status >= 200 && value.status < 300) return Fail(ErrorCode::InvalidArgument);
        auto head = value;
        for (const auto& field : mPolicyHeaders)
            if (std::none_of(head.headers.begin(), head.headers.end(), [&](const auto& entry) { return EqualInsensitive(entry.first, field.first); }))
                head.headers.push_back(field);
        head.close = head.close || mRequestClose;
        auto prepared = PrepareResponseHead(head, mHead, mOptions.maxHeaderBytes);
        if (!prepared.IsOk()) return std::move(prepared).TakeStatus();
        wire = std::move(prepared.Value().wire);
        mode = prepared.Value().mode;
        length = prepared.Value().length;
        close = head.close;
        mSending = true;
    }
    catch (...) { return Status::AllocationFailure(); }
    return SendTransaction(std::move(wire), Phase::Streaming, mode, length, 0, close, value.status);
}

Status HttpResponseState::Write(std::span<const std::byte> bytes)
{
    std::string wire;
    ResponseBodyMode mode;
    std::uint64_t length, written;
    bool close;
    try
    {
        const std::lock_guard guard(mMutex);
        if (mPhase == Phase::Finished || mPhase == Phase::Aborted) return Fail(ErrorCode::Closed);
        if (mSending) return Fail(ErrorCode::WouldBlock);
        if (mPhase != Phase::Streaming) return Fail(ErrorCode::InvalidArgument);
        if (bytes.empty()) return Status::Ok();
        if (mMode == ResponseBodyMode::None) return Fail(ErrorCode::InvalidArgument);
        if (bytes.size() > MaxWriteBytes()) return Fail(ErrorCode::TooLarge);
        if (mMode == ResponseBodyMode::FixedLength && bytes.size() > mLength - mWritten)
            return Fail(ErrorCode::InvalidArgument);
        if (mMode == ResponseBodyMode::Chunked)
        {
            auto encoded = EncodeChunk(bytes, mOptions.maxStreamChunkBytes);
            if (!encoded.IsOk()) return std::move(encoded).TakeStatus();
            wire = std::move(encoded.Value());
        }
        else wire.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        mode = mMode; length = mLength; written = mWritten + bytes.size(); close = mClose;
        mSending = true;
    }
    catch (...) { return Status::AllocationFailure(); }
    return SendTransaction(std::move(wire), Phase::Streaming, mode, length, written, close, 0);
}

Status HttpResponseState::Finish()
{
    std::string wire;
    ResponseBodyMode mode;
    std::uint64_t length, written;
    bool close;
    try
    {
        const std::lock_guard guard(mMutex);
        if (mPhase == Phase::Finished || mPhase == Phase::Aborted) return Fail(ErrorCode::Closed);
        if (mSending) return Fail(ErrorCode::WouldBlock);
        if (mPhase != Phase::Streaming) return Fail(ErrorCode::InvalidArgument);
        if (mMode == ResponseBodyMode::FixedLength && mWritten != mLength) return Fail(ErrorCode::InvalidArgument);
        if (mMode == ResponseBodyMode::Chunked) wire = "0\r\n\r\n";
        mode = mMode; length = mLength; written = mWritten; close = mClose;
        mSending = true;
    }
    catch (...) { return Status::AllocationFailure(); }
    return SendTransaction(std::move(wire), Phase::Finished, mode, length, written, close, 0);
}

Status HttpResponseState::Complete(const HttpResponse& response)
{
    std::string wire;
    bool close;
    try
    {
        const std::lock_guard guard(mMutex);
        if (mPhase == Phase::Finished || mPhase == Phase::Aborted) return Fail(ErrorCode::Closed);
        if (mSending) return Fail(ErrorCode::WouldBlock);
        if (mPhase != Phase::New) return Fail(ErrorCode::AlreadyExists);
        if (response.body.size() > mOptions.maxResponseBodyBytes) return Fail(ErrorCode::TooLarge);
        if (mConnect && response.status >= 200 && response.status < 300) return Fail(ErrorCode::InvalidArgument);
        close = response.close || mRequestClose;
        auto actual = response;
        for (const auto& field : mPolicyHeaders)
            if (std::none_of(actual.headers.begin(), actual.headers.end(), [&](const auto& entry) { return EqualInsensitive(entry.first, field.first); }))
                actual.headers.push_back(field);
        if (!SerializeResponse(actual, mHead, close, mOptions.maxHeaderBytes, mOptions.maxResponseBodyBytes, wire))
            return Fail(ErrorCode::InvalidArgument);
        mSending = true;
    }
    catch (...) { return Status::AllocationFailure(); }
    return SendTransaction(std::move(wire), Phase::Finished, ResponseBodyMode::None, 0, 0, close, response.status);
}

bool HttpResponseState::MarkCancelled() noexcept
{
    const std::lock_guard guard(mMutex);
    if (mPhase == Phase::Finished || mPhase == Phase::Aborted) return false;
    mPhase = Phase::Aborted;
    mReservation.reset();
    return true;
}
void HttpResponseState::Report(ErrorCode reason) noexcept
{
    unsigned status;
    { const std::lock_guard guard(mMutex); status = mStatus; }
    if (mObservation) mObservation->Finish(reason, status);
}
void HttpResponseState::Cancel(ErrorCode reason) noexcept
{
    if (!MarkCancelled()) return;
    Report(reason);
    const WebCallbackScope callback(mCallbackOwner);
    (void)mWaitCancellation.request_stop();
    (void)mCancellation.request_stop();
}
void HttpResponseState::Abort() noexcept
{
    AbortWithReason(ErrorCode::Cancelled);
}
void HttpResponseState::AbortWithReason(ErrorCode reason) noexcept
{
    if (!MarkCancelled()) return;
    Report(reason);
    const WebCallbackScope callback(mCallbackOwner);
    (void)mWaitCancellation.request_stop();
    (void)mCancellation.request_stop();
    mSender->connection->Close();
}
std::stop_token HttpResponseState::GetCancellationToken() const noexcept { return mCancellation.get_token(); }
std::size_t HttpResponseState::MaxWriteBytes() const noexcept
{
    const auto maximum = (std::min)(mOptions.maxTotalSendQueueCapacityBytes, Net::SendQueueLimitBytes);
    return (std::min)(mOptions.maxStreamChunkBytes, maximum > 32 ? maximum - 32 : std::size_t{0});
}
std::size_t HttpResponseState::RetainedSendBytes() const noexcept { return mSender->flow->RetainedSendBytes(); }
bool HttpResponseState::IsFinished() const noexcept { const std::lock_guard guard(mMutex); return mPhase == Phase::Finished && mWaitsRetired; }
bool HttpResponseState::IsAborted() const noexcept { const std::lock_guard guard(mMutex); return mPhase == Phase::Aborted; }
bool HttpResponseState::ShouldClose() const noexcept { const std::lock_guard guard(mMutex); return mClose; }
bool HttpResponseState::HasStarted() const noexcept { const std::lock_guard guard(mMutex); return mPhase != Phase::New; }
bool HttpResponseState::TimedOut(std::int64_t now) const noexcept
{
    const std::lock_guard guard(mMutex);
    return (mPhase == Phase::New && now - mCreated >= mOptions.handlerTimeout.count()) ||
        (mPhase == Phase::Streaming && now - mLastWrite >= mOptions.streamIdleTimeout.count());
}
Core::Result<Net::SendCapacitySubscription> HttpResponseState::WaitForWriteCapacity(
    std::size_t bytes, std::function<void(Status)> callback)
{
    using Result = Core::Result<Net::SendCapacitySubscription>;
    std::size_t required;
    {
        const std::lock_guard guard(mMutex);
        if (!callback) return Result::FromStatus(Fail(ErrorCode::InvalidArgument));
        if (mPhase == Phase::Finished || mPhase == Phase::Aborted) return Result::FromStatus(Fail(ErrorCode::Closed));
        if (bytes > MaxWriteBytes()) return Result::FromStatus(Fail(ErrorCode::TooLarge));
        // A transport notification can reenter while Send is still returning.
        // Capacity cannot complete that application-level transaction for us.
        if (mSending) return Result::FromStatus(Fail(ErrorCode::WouldBlock));
        required = ((mPhase == Phase::New || bytes == 0) && mBlockedWireBytes != 0) ? mBlockedWireBytes :
            (mPhase == Phase::New ? (std::min)(mOptions.maxHeaderBytes, mOptions.maxTotalSendQueueCapacityBytes) :
             (bytes == 0 ? std::size_t{5} : bytes + (mMode == ResponseBodyMode::Chunked ? 32 : 0)));
    }
    const auto owner = mCallbackOwner;
    try
    {
        return mSender->flow->WaitForSendCapacity(required,
            [owner, callback = std::move(callback)](Status status) mutable {
                const WebCallbackScope scope(owner);
                callback(std::move(status));
            }, mWaitCancellation.get_token());
    }
    catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
}
}
