#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace ServerCore::Net
{
class SendCapacityState;
// Counts retained payload storage, including a partially sent vector's prefix.
// The runtime injects one budget into all connections belonging to a host.
class SendBudget
{
public:
    explicit SendBudget(std::size_t limitBytes, std::size_t connectionLimitBytes = 1024 * 1024) noexcept;
    [[nodiscard]] bool TryReserve(std::size_t byteCount) noexcept;
    void Release(std::size_t byteCount) noexcept;
    [[nodiscard]] std::size_t LimitBytes() const noexcept;
    [[nodiscard]] std::size_t UsedBytes() const noexcept;
    [[nodiscard]] std::size_t ConnectionLimitBytes() const noexcept;
    std::uint64_t Register(const std::shared_ptr<SendCapacityState>& state);
    void Unregister(std::uint64_t id) noexcept;
    // Invoke only outside ALL connection locks. Release itself only updates
    // accounting, since the caller may still hold a transport mutex.
    void Notify() noexcept;
    void MarkChanged() noexcept;

private:
    std::size_t mLimitBytes;
    std::size_t mConnectionLimitBytes;
    std::atomic<std::size_t> mUsedBytes{ 0 };
    std::mutex mWaitersMutex;
    std::map<std::uint64_t, std::weak_ptr<SendCapacityState>> mWaiters;
    std::uint64_t mNextId = 1;
    std::atomic<bool> mChanged{false};
    std::atomic_flag mNotifying = ATOMIC_FLAG_INIT;
};

// Declare before a transport lock so every return path dispatches AFTER unlock.
class SendNotificationScope
{
public:
    explicit SendNotificationScope(std::shared_ptr<SendBudget> budget) noexcept : mBudget(std::move(budget)) {}
    ~SendNotificationScope() { if (mBudget) mBudget->Notify(); }
private:
    std::shared_ptr<SendBudget> mBudget;
};
}
