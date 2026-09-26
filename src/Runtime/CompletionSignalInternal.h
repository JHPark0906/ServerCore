#pragma once
#include "ServerCore/Core/CompletionSubscription.h"
#include <array>
#include <mutex>

namespace ServerCore::Runtime::Detail
{
// Fixed metadata per completion owner; registrations own their callbacks.
// No callback/capture is moved, called or destroyed under this mutex.
class CompletionSignal
{
public:
    Core::Status Observe(Core::CompletionSource source) noexcept
    {
        Core::ErrorCode result;
        {
            const std::lock_guard guard(mMutex);
            if (!mReady)
            {
                for (auto& slot : mSources)
                    if (!slot.IsPending())
                    {
                        slot = source;
                        return Core::Status::Ok();
                    }
                return Core::Status::FailWithoutMessage(Core::ErrorCode::WouldBlock);
            }
            result = mResult;
        }
        (void)source.Complete(result);
        return Core::Status::Ok();
    }
    Core::Result<Core::CompletionSubscription> Subscribe(
        std::function<void(Core::Status)> callback, std::stop_token cancellation)
    {
        auto result = Core::CompletionSubscription::Create(std::move(callback));
        if (!result.IsOk())
            return result;
        auto status = Observe(result.Value().GetSource());
        if (!status.IsOk())
            return Core::Result<Core::CompletionSubscription>::FromStatus(std::move(status));
        result.Value().BindCancellation(cancellation);
        return result;
    }
    void Complete(Core::ErrorCode result = Core::ErrorCode::Ok) noexcept
    {
        std::array<Core::CompletionSource, 16> sources;
        {
            const std::lock_guard guard(mMutex);
            if (mReady)
                return;
            mReady = true;
            mResult = result;
            sources.swap(mSources);
        }
        for (const auto& source : sources)
            (void)source.Complete(result);
    }

private:
    std::mutex mMutex;
    std::array<Core::CompletionSource, 16> mSources;
    bool mReady = false;
    Core::ErrorCode mResult = Core::ErrorCode::WouldBlock;
};
}
