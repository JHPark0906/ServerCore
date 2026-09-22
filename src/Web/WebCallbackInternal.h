#pragma once

namespace ServerCore::Web::Detail
{
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
}
