#include "ServerCore/Net/ConnectionFlowControl.h"

#include "Net/SendBudgetInternal.h"
#include "Net/SendCapacityInternal.h"
#include "ServerCore/Net/Connection.h"

#include <condition_variable>
#include <optional>
#include <thread>
#include <utility>

namespace ServerCore::Net
{
using Core::ErrorCode;
using Core::Status;

class SendCapacitySubscription::State : public std::enable_shared_from_this<State>
{
public:
    State(std::size_t bytes, std::function<void(Status)> callback)
        : requiredBytes(bytes), mCallback(std::move(callback)) {}

    void AttachCancellation(std::stop_token token)
    {
        mCancellation.emplace(token, CancelCallback{weak_from_this()});
    }

    bool Complete(ErrorCode code) noexcept
    {
        std::function<void(Status)> callback;
        {
            const std::lock_guard guard(mMutex);
            if (mDone) return false;
            mDone = true;
            mInvoking = true;
            mCallbackThread = std::this_thread::get_id();
            callback = std::move(mCallback);
        }
        try { callback(code == ErrorCode::Ok ? Status::Ok() : Status::FailWithoutMessage(code)); }
        catch (...) { /* Application exceptions cannot escape a transport completion. */ }
        // Release captures before publishing quiescence; their destructors can
        // reenter Reset on this same callback thread without waiting on itself.
        callback = {};
        {
            const std::lock_guard guard(mMutex);
            mInvoking = false;
            mCallbackThread = {};
        }
        mChanged.notify_all();
        return true;
    }

    void Reset() noexcept
    {
        std::function<void(Status)> discarded;
        {
            std::unique_lock guard(mMutex);
            mDone = true;
            discarded = std::move(mCallback);
            if (mCallbackThread != std::this_thread::get_id())
                mChanged.wait(guard, [this] { return !mInvoking; });
        }
        mCancellation.reset();
    }

    bool IsPending() const noexcept
    {
        const std::lock_guard guard(mMutex);
        return !mDone;
    }

    const std::size_t requiredBytes;
private:
    struct CancelCallback
    {
        std::weak_ptr<State> state;
        void operator()() const noexcept
        {
            if (auto retained = state.lock()) (void)retained->Complete(ErrorCode::Cancelled);
        }
    };
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    std::function<void(Status)> mCallback;
    bool mDone = false;
    bool mInvoking = false;
    std::thread::id mCallbackThread;
    std::optional<std::stop_callback<CancelCallback>> mCancellation;
};

SendCapacitySubscription::SendCapacitySubscription(std::shared_ptr<State> state) noexcept : mState(std::move(state)) {}
SendCapacitySubscription::~SendCapacitySubscription() { Reset(); }
SendCapacitySubscription::SendCapacitySubscription(SendCapacitySubscription&& other) noexcept : mState(std::move(other.mState)) {}
SendCapacitySubscription& SendCapacitySubscription::operator=(SendCapacitySubscription&& other) noexcept
{
    if (this != &other) { Reset(); mState = std::move(other.mState); }
    return *this;
}
void SendCapacitySubscription::Reset() noexcept
{
    if (auto state = std::move(mState)) state->Reset();
}
bool SendCapacitySubscription::Cancel() noexcept
{
    const auto state = mState;
    return state && state->Complete(ErrorCode::Cancelled);
}
bool SendCapacitySubscription::IsPending() const noexcept { return mState && mState->IsPending(); }

std::shared_ptr<ConnectionFlowControl> GetConnectionFlowControl(const std::shared_ptr<Connection>& connection) noexcept
{
    return std::dynamic_pointer_cast<ConnectionFlowControl>(connection);
}

SendCapacityState::SendCapacityState(std::shared_ptr<SendBudget> budget) : mBudget(std::move(budget)) {}
SendCapacityState::~SendCapacityState() { if (mRegistration) mBudget->Unregister(mRegistration); }
void SendCapacityState::SetRetainedBytes(std::size_t bytes) noexcept { mRetainedBytes.store(bytes, std::memory_order_release); }
void SendCapacityState::Close() noexcept
{
    mClosed.store(true, std::memory_order_release);
    mBudget->MarkChanged();
}

void SendCapacityState::Notify() noexcept
{
    std::shared_ptr<SendCapacitySubscription::State> pending;
    {
        const std::lock_guard guard(mMutex);
        pending = mPending.lock();
    }
    if (!pending || !pending->IsPending()) return;
    if (mClosed.load(std::memory_order_acquire)) { (void)pending->Complete(ErrorCode::Closed); return; }
    // Release publishes local retained storage before shared accounting. Read
    // shared accounting first so observing released bytes also sees local release.
    const auto shared = mBudget->UsedBytes();
    const auto local = mRetainedBytes.load(std::memory_order_acquire);
    if (local <= mBudget->ConnectionLimitBytes() && pending->requiredBytes <= mBudget->ConnectionLimitBytes() - local &&
        shared <= mBudget->LimitBytes() && pending->requiredBytes <= mBudget->LimitBytes() - shared)
        (void)pending->Complete(ErrorCode::Ok);
}

Core::Result<SendCapacitySubscription> SendCapacityState::Wait(std::size_t bytes,
    std::function<void(Status)> callback, std::stop_token cancellation)
{
    using Result = Core::Result<SendCapacitySubscription>;
    if (!callback || bytes == 0) return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::InvalidArgument));
    if (bytes > mBudget->ConnectionLimitBytes() || bytes > mBudget->LimitBytes())
        return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::TooLarge));
    try
    {
        auto node = std::make_shared<SendCapacitySubscription::State>(bytes, std::move(callback));
        {
            const std::lock_guard guard(mMutex);
            if (mClosed.load(std::memory_order_acquire)) return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
            if (const auto current = mPending.lock(); current && current->IsPending())
                return Result::FromStatus(Status::FailWithoutMessage(ErrorCode::AlreadyExists));
            if (!mRegistration) mRegistration = mBudget->Register(shared_from_this());
            mPending = node;
        }
        // Publish before attaching: an already-cancelled token may invoke the
        // callback inline, and that callback may register a successor wait.
        node->AttachCancellation(cancellation);
        Notify();
        return Result::FromValue(SendCapacitySubscription(std::move(node)));
    }
    catch (...) { return Result::FromStatus(Status::AllocationFailure()); }
}
}
