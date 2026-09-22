#pragma once

#include "Net/SendBudgetInternal.h"
#include "ServerCore/Net/Connection.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace ServerCore::Net
{
// Portable owned payload queue. The connection's mutex serializes every method.
// Front() borrows stable storage until that vector is fully consumed or Clear()
// is called. A backend must finish/cancel any kernel borrow before either action.
// Retained bytes include already-sent prefixes, and shared reservations are only
// returned after the corresponding storage has actually been released.
class SendQueue final
{
public:
    explicit SendQueue(std::shared_ptr<SendBudget> budget = nullptr);
    ~SendQueue();
    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;

    [[nodiscard]] Core::Status Enqueue(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] std::span<std::byte> Front() noexcept;
    void Consume(std::size_t bytes) noexcept;
    void Clear() noexcept;
    [[nodiscard]] std::size_t QueuedBytes() const noexcept;
    [[nodiscard]] std::size_t RetainedBytes() const noexcept;

private:
    std::shared_ptr<SendBudget> mBudget;
    std::deque<std::vector<std::byte>> mPayloads;
    std::size_t mOffset = 0;
    std::size_t mQueuedBytes = 0;
    std::size_t mRetainedBytes = 0;
};
}
