#pragma once

#include "ServerCore/Net/ConnectionFlowControl.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace ServerCore::Net
{
class SendBudget;

class SendCapacityState : public std::enable_shared_from_this<SendCapacityState>
{
public:
    explicit SendCapacityState(std::shared_ptr<SendBudget> budget);
    ~SendCapacityState();
    void SetRetainedBytes(std::size_t bytes) noexcept;
    void Close() noexcept;
    void Notify() noexcept;
    Core::Result<SendCapacitySubscription> Wait(std::size_t bytes,
        std::function<void(Core::Status)> callback, std::stop_token cancellation);
private:
    std::shared_ptr<SendBudget> mBudget;
    std::atomic<std::size_t> mRetainedBytes{0};
    std::atomic<bool> mClosed{false};
    std::mutex mMutex;
    std::weak_ptr<SendCapacitySubscription::State> mPending;
    std::uint64_t mRegistration = 0;
};
}
