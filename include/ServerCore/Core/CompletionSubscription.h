#pragma once

#include "ServerCore/Export.h"

#include "ServerCore/Core/Error.h"
#include <functional>
#include <memory>
#include <stop_token>

namespace ServerCore::Core
{
class CompletionSource;

// One-shot, movable registration. Reset/destruction suppresses pending delivery
// and waits for an active callback (except from that callback itself). Captures
// are destroyed before quiescence. Serialize operations on the same handle.
class CompletionSubscription
{
public:
    class State;
    CompletionSubscription() noexcept = default;
    SERVERCORE_API ~CompletionSubscription();
    SERVERCORE_API CompletionSubscription(CompletionSubscription&&) noexcept;
    SERVERCORE_API CompletionSubscription& operator=(CompletionSubscription&&) noexcept;
    CompletionSubscription(const CompletionSubscription&) = delete;
    CompletionSubscription& operator=(const CompletionSubscription&) = delete;
    SERVERCORE_API static Result<CompletionSubscription> Create(
        std::function<void(Status)> callback);
    [[nodiscard]] SERVERCORE_API CompletionSource GetSource() const noexcept;
    // The owner calls this ONCE after publishing the source and before exposing
    // the subscription. Already stopped tokens may invoke the callback inline.
    SERVERCORE_API void BindCancellation(
        std::stop_token first = {}, std::stop_token second = {}) noexcept;
    SERVERCORE_API void Reset() noexcept;
    SERVERCORE_API bool Cancel() noexcept;
    [[nodiscard]] SERVERCORE_API bool IsPending() const noexcept;

private:
    explicit CompletionSubscription(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> mState;
};

// Weak, copyable producer end. It neither retains captures nor keeps a dropped
// subscription alive. Complete may invoke inline; call it outside owner locks.
// First completion wins; callback exceptions are contained.
class CompletionSource
{
public:
    CompletionSource() noexcept = default;
    SERVERCORE_API bool Complete(ErrorCode code = ErrorCode::Ok) const noexcept;
    [[nodiscard]] SERVERCORE_API bool IsPending() const noexcept;

private:
    friend class CompletionSubscription;
    explicit CompletionSource(std::weak_ptr<CompletionSubscription::State> state) noexcept;
    std::weak_ptr<CompletionSubscription::State> mState;
};
}
