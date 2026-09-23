#pragma once

#include "ServerCore/Export.h"

#include "Net/SendBudgetInternal.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/ConnectionFlowControl.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace ServerCore::Net
{
// Portable owned payload queue. The connection's mutex serializes queue access;
// WaitForCapacity uses its independent state and runs outside transport locks.
// Destruction also runs outside transport locks and completes terminal waits.
// Front() borrows stable storage until that vector is fully consumed or Clear()
// is called. A backend must finish/cancel any kernel borrow before either action.
// Retained bytes include already-sent prefixes, and shared reservations are only
// returned after the corresponding storage has actually been released.
class SendQueue final
{
public:
    SERVERCORE_TEST_API explicit SendQueue(std::shared_ptr<SendBudget> budget = nullptr);
    SERVERCORE_TEST_API ~SendQueue();
    SendQueue(const SendQueue&) = delete;
    SendQueue& operator=(const SendQueue&) = delete;

    [[nodiscard]] SERVERCORE_TEST_API Core::Status Enqueue(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] SERVERCORE_TEST_API bool Empty() const noexcept;
    [[nodiscard]] SERVERCORE_TEST_API std::span<std::byte> Front() noexcept;
    SERVERCORE_TEST_API void Consume(std::size_t bytes) noexcept;
    SERVERCORE_TEST_API void Clear() noexcept;
    [[nodiscard]] SERVERCORE_TEST_API std::size_t QueuedBytes() const noexcept;
    [[nodiscard]] SERVERCORE_TEST_API std::size_t RetainedBytes() const noexcept;
    [[nodiscard]] SERVERCORE_TEST_API std::shared_ptr<SendBudget> Budget() const noexcept;
    // Mark send admission closed under the transport lock, then use
    // SendNotificationScope outside the lock to dispatch terminal notifications.
    SERVERCORE_TEST_API void CloseCapacityWaits() noexcept;
    SERVERCORE_TEST_API Core::Result<SendCapacitySubscription> WaitForCapacity(std::size_t requiredBytes,
        std::function<void(Core::Status)> callback, std::stop_token cancellation);

private:
    std::shared_ptr<SendBudget> mBudget;
    std::shared_ptr<SendCapacityState> mCapacity;
    std::deque<std::vector<std::byte>> mPayloads;
    std::size_t mOffset = 0;
    std::size_t mQueuedBytes = 0;
    std::size_t mRetainedBytes = 0;
};
}
