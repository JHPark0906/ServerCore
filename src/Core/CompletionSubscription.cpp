#include "ServerCore/Core/CompletionSubscription.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace ServerCore::Core
{
class CompletionSubscription::State : public std::enable_shared_from_this<State>
{
public:
    explicit State(std::function<void(Status)> callback) : mCallback(std::make_unique<std::function<void(Status)>>(std::move(callback))) {}
    void BindCancellation(std::stop_token first, std::stop_token second) noexcept
    {
        mFirst.emplace(first, CancelCallback{weak_from_this()});
        mSecond.emplace(second, CancelCallback{weak_from_this()});
    }
    bool Complete(ErrorCode code) noexcept
    {
        std::unique_ptr<std::function<void(Status)>> callback;
        {
            const std::lock_guard guard(mMutex);
            if (mDone) return false;
            mDone = true;
            mInvoking = true;
            mThread = std::this_thread::get_id();
            callback = std::move(mCallback);
        }
        try { (*callback)(code == ErrorCode::Ok ? Status::Ok() : Status::FailWithoutMessage(code)); }
        catch (...) { }
        callback = {};
        {
            const std::lock_guard guard(mMutex);
            mInvoking = false;
            mThread = {};
        }
        mChanged.notify_all();
        return true;
    }
    void Reset() noexcept
    {
        std::unique_ptr<std::function<void(Status)>> discarded;
        {
            std::unique_lock guard(mMutex);
            mDone = true;
            discarded = std::move(mCallback);
            if (mThread != std::this_thread::get_id())
                mChanged.wait(guard, [this] { return !mInvoking; });
        }
        mFirst.reset();
        mSecond.reset();
    }
    bool IsPending() const noexcept
    {
        const std::lock_guard guard(mMutex);
        return !mDone;
    }
private:
    struct CancelCallback
    {
        std::weak_ptr<State> state;
        void operator()() const noexcept
        { if (auto value = state.lock()) (void)value->Complete(ErrorCode::Cancelled); }
    };
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::unique_ptr<std::function<void(Status)>> mCallback;
    bool mDone = false, mInvoking = false;
    std::thread::id mThread;
    std::optional<std::stop_callback<CancelCallback>> mFirst, mSecond;
};

CompletionSubscription::CompletionSubscription(std::shared_ptr<State> state) noexcept : mState(std::move(state)) {}
CompletionSubscription::~CompletionSubscription() { Reset(); }
CompletionSubscription::CompletionSubscription(CompletionSubscription&& other) noexcept : mState(std::move(other.mState)) {}
CompletionSubscription& CompletionSubscription::operator=(CompletionSubscription&& other) noexcept
{
    if (this != &other) { Reset(); mState = std::move(other.mState); }
    return *this;
}
Result<CompletionSubscription> CompletionSubscription::Create(std::function<void(Status)> callback)
{
    if (!callback) return Result<CompletionSubscription>::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    try { return Result<CompletionSubscription>::FromValue(CompletionSubscription(std::make_shared<State>(std::move(callback)))); }
    catch (...) { return Result<CompletionSubscription>::FromStatus(Status::AllocationFailure()); }
}
CompletionSource CompletionSubscription::GetSource() const noexcept { return CompletionSource(mState); }
void CompletionSubscription::BindCancellation(std::stop_token first, std::stop_token second) noexcept
{ if (const auto state = mState) state->BindCancellation(first, second); }
void CompletionSubscription::Reset() noexcept { if (auto state = std::move(mState)) state->Reset(); }
bool CompletionSubscription::Cancel() noexcept { const auto state = mState; return state && state->Complete(ErrorCode::Cancelled); }
bool CompletionSubscription::IsPending() const noexcept { return mState && mState->IsPending(); }
CompletionSource::CompletionSource(std::weak_ptr<CompletionSubscription::State> state) noexcept : mState(std::move(state)) {}
bool CompletionSource::Complete(ErrorCode code) const noexcept
{ const auto state = mState.lock(); return state && state->Complete(code); }
bool CompletionSource::IsPending() const noexcept
{ const auto state = mState.lock(); return state && state->IsPending(); }
}
