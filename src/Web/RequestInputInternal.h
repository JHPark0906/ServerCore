#pragma once
#include "ServerCore/Web/HttpServer.h"
#include "Web/WebProtocol.h"
#include "Web/WebCallbackInternal.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <new>

namespace ServerCore::Web::Detail
{
class RequestBodyState final : public HttpRequestBody
{
    struct Credit
    {
        explicit Credit(std::shared_ptr<void> value) : reservation(std::move(value)) {}
        std::shared_ptr<void> reservation;
        std::atomic<std::size_t> retained{0};
    };
public:
    RequestBodyState(std::size_t maximum, std::shared_ptr<void> reservation, std::function<void()> abort, const void* owner)
        : mCredit(std::make_shared<Credit>(std::move(reservation))), mMaximum(maximum), mAbort(std::move(abort)), mOwner(owner) {}
    Core::Result<std::shared_ptr<const std::vector<std::byte>>> Read() override
    {
        using Result = Core::Result<std::shared_ptr<const std::vector<std::byte>>>;
        const std::lock_guard guard(mMutex);
        if (mError != Core::ErrorCode::Ok) return Result::FromStatus(Core::Status::FailWithoutMessage(mError));
        if (mQueue && !mQueue->empty())
        { auto value = std::move(mQueue->front()); mQueue->pop_front(); return Result::FromValue(std::move(value)); }
        if (mDone) return Result::FromValue({});
        return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock));
    }
    void Cancel() noexcept override
    {
        if (End(Core::ErrorCode::Cancelled)) mAbort();
    }
    std::stop_token GetCancellationToken() const noexcept override { return mCancellation.get_token(); }
    std::size_t RetainedBytes() const noexcept override { return mCredit->retained.load(); }
    std::size_t Available() const noexcept { return mMaximum - mCredit->retained.load(); }
    Core::Result<Core::CompletionSubscription> WaitForReadReady(
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) override
    {
        using Result = Core::Result<Core::CompletionSubscription>;
        if (!callback) return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
        try {
            auto registered = Core::CompletionSubscription::Create(
                [owner = mOwner, callback = std::move(callback)](Core::Status status) {
                    const WebCallbackScope scope(owner);
                    callback(std::move(status));
                });
            if (!registered.IsOk()) return registered;
            auto notification = std::make_shared<WebCompletion>();
            notification->source = registered.Value().GetSource();
            notification->owner = mOwner;
            bool ready;
            {
                const std::lock_guard guard(mMutex);
                if (mReadWait && mReadWait->source.IsPending())
                    return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::AlreadyExists));
                ready = mDone || mError != Core::ErrorCode::Ok || (mQueue && !mQueue->empty());
                if (!ready) mReadWait = notification;
            }
            registered.Value().BindCancellation(cancellation);
            if (ready) WebCompletionScope::Complete(std::move(notification));
            return registered;
        } catch (...) { return Result::FromStatus(Core::Status::AllocationFailure()); }
    }
    void Push(std::span<const std::byte> bytes)
    {
        std::shared_ptr<WebCompletion> ready;
        {
        const std::lock_guard guard(mMutex);
        if (mDone || mError != Core::ErrorCode::Ok || !mQueue || bytes.empty()) return;
        if (bytes.size() > Available()) throw std::bad_alloc();
        auto owned = std::make_unique<std::vector<std::byte>>(bytes.begin(), bytes.end());
        mCredit->retained.fetch_add(bytes.size());
        auto value = std::shared_ptr<const std::vector<std::byte>>(owned.release(), [credit = mCredit](const auto* data) {
            const auto size = data->size();
            delete data;
            credit->retained.fetch_sub(size);
        });
        mQueue->push_back(std::move(value));
        ready = std::move(mReadWait);
        }
        WebCompletionScope::Complete(std::move(ready));
    }
    bool End(Core::ErrorCode error = Core::ErrorCode::Ok) noexcept
    {
        // Detach the admission-allocated container; constructing an empty deque
        // can allocate on MSVC and must not happen in this noexcept close path.
        std::unique_ptr<std::deque<std::shared_ptr<const std::vector<std::byte>>>> discarded;
        std::shared_ptr<WebCompletion> ready;
        {
            const std::lock_guard guard(mMutex);
            if (mDone || mError != Core::ErrorCode::Ok) return false;
            mDone = error == Core::ErrorCode::Ok;
            mError = error;
            if (error != Core::ErrorCode::Ok) discarded = std::move(mQueue);
            ready = std::move(mReadWait);
        }
        if (error != Core::ErrorCode::Ok)
        { const WebCallbackScope callback(mOwner); (void)mCancellation.request_stop(); }
        WebCompletionScope::Complete(std::move(ready));
        return true;
    }
private:
    std::shared_ptr<Credit> mCredit;
    const std::size_t mMaximum;
    const std::function<void()> mAbort;
    const void* const mOwner;
    mutable std::mutex mMutex;
    std::unique_ptr<std::deque<std::shared_ptr<const std::vector<std::byte>>>> mQueue =
        std::make_unique<std::deque<std::shared_ptr<const std::vector<std::byte>>>>();
    std::stop_source mCancellation;
    std::shared_ptr<WebCompletion> mReadWait;
    Core::ErrorCode mError = Core::ErrorCode::Ok;
    bool mDone = false;
};

class RequestDecisionState final : public HttpRequestDecision
{
public:
    enum class Phase { Pending, Allowed, Rejected, Aborted };
    struct Result { Phase phase; HttpResponse response; HttpHeaders attributes; };
    RequestDecisionState(std::size_t headers, std::size_t body, std::int64_t started, std::chrono::milliseconds timeout, const void* owner)
        : mMaxHeaders(headers), mMaxBody(body), mStarted(started), mTimeout(timeout), mOwner(owner) {}
    Core::Status Allow(HttpHeaders headers, HttpHeaders attributes) override
    {
        try
        {
        if (Expired()) { Abort(); return Fail(Core::ErrorCode::Closed); }
        if (headers.size() > 128 || attributes.size() > 128 - headers.size()) return Fail(Core::ErrorCode::TooLarge);
        std::size_t bytes = 0;
        for (const auto* fields : {&headers, &attributes})
            for (const auto& [name, value] : *fields)
            {
                if (!IsToken(name) || !ValidHeaderValue(value)) return Fail(Core::ErrorCode::InvalidArgument);
                if (name.size() > mMaxHeaders - bytes) return Fail(Core::ErrorCode::TooLarge);
                bytes += name.size();
                if (value.size() > mMaxHeaders - bytes) return Fail(Core::ErrorCode::TooLarge);
                bytes += value.size();
                if (bytes > mMaxHeaders - (std::min)(mMaxHeaders, std::size_t{4})) return Fail(Core::ErrorCode::TooLarge);
                bytes += 4;
            }
        // Validate framing ownership before storing any policy output.
        HttpResponse check; check.headers = headers;
        std::string wire;
        if (!SerializeResponse(check, false, false, mMaxHeaders, mMaxBody, wire)) return Fail(Core::ErrorCode::InvalidArgument);
        const std::lock_guard guard(mMutex);
        if (mPhase != Phase::Pending) return Fail(Core::ErrorCode::Closed);
        // Copies discard caller spare capacity before this deferred decision
        // retains metadata. All allocations precede the terminal transition.
        HttpHeaders compactHeaders, compactAttributes;
        compactHeaders.reserve(headers.size()); compactAttributes.reserve(attributes.size());
        for (const auto& [name, value] : headers) compactHeaders.emplace_back(name, value);
        for (const auto& [name, value] : attributes) compactAttributes.emplace_back(name, value);
        mResponse.headers = std::move(compactHeaders); mAttributes = std::move(compactAttributes); mPhase = Phase::Allowed;
        return Core::Status::Ok();
        }
        catch (...) { return Core::Status::AllocationFailure(); }
    }
    Core::Status Reject(const HttpResponse& response) override
    {
        try
        {
        if (Expired()) { Abort(); return Fail(Core::ErrorCode::Closed); }
        if (response.status < 200 || response.status == 101) return Fail(Core::ErrorCode::InvalidArgument);
        if (response.body.size() > mMaxBody) return Fail(Core::ErrorCode::TooLarge);
        std::string wire;
        if (!SerializeResponse(response, false, true, mMaxHeaders, mMaxBody, wire)) return Fail(Core::ErrorCode::InvalidArgument);
        HttpResponse copy = response;
        const std::lock_guard guard(mMutex);
        if (mPhase != Phase::Pending) return Fail(Core::ErrorCode::Closed);
        mResponse = std::move(copy); mPhase = Phase::Rejected;
        return Core::Status::Ok();
        }
        catch (...) { return Core::Status::AllocationFailure(); }
    }
    void Abort() noexcept override
    {
        { const std::lock_guard guard(mMutex); if (mPhase != Phase::Pending) return; mPhase = Phase::Aborted; }
        const WebCallbackScope callback(mOwner);
        (void)mCancellation.request_stop();
    }
    std::stop_token GetCancellationToken() const noexcept override { return mCancellation.get_token(); }
    Result Take()
    { const std::lock_guard guard(mMutex); return {mPhase, std::move(mResponse), std::move(mAttributes)}; }
private:
    bool Expired() const noexcept
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return now - mStarted >= mTimeout.count();
    }
    static Core::Status Fail(Core::ErrorCode error) noexcept { return Core::Status::FailWithoutMessage(error); }
    const std::size_t mMaxHeaders, mMaxBody;
    const std::int64_t mStarted;
    const std::chrono::milliseconds mTimeout;
    const void* const mOwner;
    std::mutex mMutex;
    Phase mPhase = Phase::Pending;
    HttpResponse mResponse;
    HttpHeaders mAttributes;
    std::stop_source mCancellation;
};
}
