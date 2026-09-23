#pragma once
#include "ServerCore/Core/CompletionSubscription.h"

namespace ServerCore::Web::Detail
{
// A wait allocates its link when registering, so parser-side delivery never
// allocates. Outer transport/parser scopes flush only after releasing locks.
struct WebCompletion {
    Core::CompletionSource source;
    const void* owner = nullptr;
    Core::ErrorCode code = Core::ErrorCode::Ok;
    std::shared_ptr<WebCompletion> next;
};
class WebCompletionScope;
inline thread_local WebCompletionScope* activeCompletionScope = nullptr;
class WebCompletionScope {
public:
    WebCompletionScope() noexcept : mPrevious(activeCompletionScope) { activeCompletionScope = this; }
    ~WebCompletionScope() {
        activeCompletionScope = mPrevious;
        while (mFirst) {
            auto item = std::move(mFirst); mFirst = std::move(item->next);
            Complete(std::move(item));
        }
    }
    static void Complete(std::shared_ptr<WebCompletion> item) noexcept;
private:
    WebCompletionScope* const mPrevious;
    std::shared_ptr<WebCompletion> mFirst;
    WebCompletion* mLast = nullptr;
};
class WebCallbackScope;
inline thread_local const WebCallbackScope* activeWebScope = nullptr;
class WebCallbackScope
{
public:
    explicit WebCallbackScope(const void* owner) noexcept : mOwner(owner), mPrevious(activeWebScope) { activeWebScope = this; }
    ~WebCallbackScope() { activeWebScope = mPrevious; }
    static bool Contains(const void* owner) noexcept
    {
        for (auto scope = activeWebScope; scope; scope = scope->mPrevious)
            if (scope->mOwner == owner) return true;
        return false;
    }
private:
    const void* mOwner;
    const WebCallbackScope* mPrevious;
};
inline void WebCompletionScope::Complete(std::shared_ptr<WebCompletion> item) noexcept {
    if (!item) return;
    if (auto* scope = activeCompletionScope) {
        auto* tail = item.get();
        if (scope->mLast) scope->mLast->next = std::move(item);
        else scope->mFirst = std::move(item);
        scope->mLast = tail;
    } else {
        const WebCallbackScope callback(item->owner);
        (void)item->source.Complete(item->code);
    }
}
}
