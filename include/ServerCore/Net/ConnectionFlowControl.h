#pragma once

#include "ServerCore/Export.h"

#include "ServerCore/Core/CompletionSubscription.h"
#include "ServerCore/Core/Error.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>

namespace ServerCore::Net
{
class Connection;
class SendCapacityState;

// Compatibility spelling; lifetime is shared with Session and other layers.
using SendCapacitySubscription = Core::CompletionSubscription;

// Optional extension: existing Connection implementations need not change their
// virtual interface. All TCP connections created by ServerCore implement this.
class ConnectionFlowControl
{
public:
    virtual ~ConnectionFlowControl() = default;
    // Stops new receive operations. One already submitted/in progress receive
    // (at most 16 KiB) may still reach the observer. Resume never duplicates I/O.
    // Once CloseAfterSend shuts down the send side, the transport drains and
    // discards input regardless of pause; nothing more reaches the observer.
    virtual Core::Status PauseReceive() = 0;
    virtual Core::Status ResumeReceive() = 0;
    [[nodiscard]] virtual bool IsReceivePaused() const noexcept = 0;
    [[nodiscard]] virtual std::size_t RetainedSendBytes() const noexcept = 0;
    // Asynchronous, advisory availability, not a reservation. Retry Send on Ok;
    // another producer may consume capacity first. One pending wait per connection;
    // a second is AlreadyExists. A size that can never fit is TooLarge.
    // Accepted waits complete once with Ok, Closed or Cancelled. Invalid calls
    // return an error without calling the callback. It may run inline or on an
    // I/O/cancelling thread, outside transport locks, and may overlap the receive
    // observer. Keep it short and dispatch application state to its own executor.
    // Exceptions are contained. Reset the subscription before destroying captures.
    virtual Core::Result<SendCapacitySubscription> WaitForSendCapacity(std::size_t requiredBytes,
        std::function<void(Core::Status)> callback, std::stop_token cancellation = {}) = 0;
};

[[nodiscard]] SERVERCORE_API std::shared_ptr<ConnectionFlowControl> GetConnectionFlowControl(
    const std::shared_ptr<Connection>& connection) noexcept;

// Payload storage limits, not total process/OS socket memory. Configure before
// Acceptor::Start. Defaults preserve the existing 1 MiB connection bound.
struct SendQueueLimits
{
    std::size_t connectionBytes = 1024 * 1024;
    std::size_t totalBytes = 256 * 1024 * 1024;
};
}
