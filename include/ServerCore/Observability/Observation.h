#pragma once
#include "ServerCore/Core/Error.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ServerCore::Observability
{
// Fixed-cardinality diagnostics. Unknown is explicit: adapters never infer a
// close/rejection reason that the originating subsystem did not record.
enum class Protocol : std::uint32_t
{
    Unknown,
    Http,
    WebSocket,
    Tcp,
    Udp,
    Task,
    Host,
    Logger
};
enum class Lifecycle : std::uint32_t
{
    Unknown,
    Created,
    Running,
    Draining,
    Stopped
};
enum class EventReason : std::uint32_t
{
    Unknown,
    Normal,
    Cancelled,
    Timeout,
    ProtocolError,
    Capacity,
    Policy,
    TransportError,
    Count
};
inline constexpr std::size_t EventReasonCount = static_cast<std::size_t>(EventReason::Count);
enum ObservationField : std::uint64_t
{
    Connections = 1u << 0,
    PendingWork = 1u << 1,
    ReceiveBytes = 1u << 2,
    SendBytes = 1u << 3,
    RetainedBytes = 1u << 4,
    DrainRemaining = 1u << 5,
    ClosedEvents = 1u << 6,
    RejectedEvents = 1u << 7,
    TimedOutEvents = 1u << 8
};
struct ObservationSnapshot
{
    Protocol protocol = Protocol::Unknown;
    Lifecycle lifecycle = Lifecycle::Unknown;
    std::uint64_t available = 0;
    std::uint64_t connections = 0, pendingWork = 0, receiveBytes = 0, sendBytes = 0;
    std::uint64_t retainedBytes = 0, drainRemaining = 0;
    std::array<std::uint64_t, EventReasonCount> closed{}, rejected{};
    std::uint64_t timedOut = 0;
    [[nodiscard]] bool Has(ObservationField field) const noexcept
    {
        return (available & field) != 0;
    }
};
// Map an observed operation result, not a guessed policy cause. In particular,
// Closed alone does not identify peer-close versus local shutdown.
[[nodiscard]] constexpr EventReason ClassifyReason(Core::ErrorCode code) noexcept
{
    switch (code)
    {
    case Core::ErrorCode::Ok:
        return EventReason::Normal;
    case Core::ErrorCode::Cancelled:
        return EventReason::Cancelled;
    case Core::ErrorCode::Timeout:
        return EventReason::Timeout;
    case Core::ErrorCode::InvalidFormat:
    case Core::ErrorCode::UnknownType:
        return EventReason::ProtocolError;
    case Core::ErrorCode::TooLarge:
    case Core::ErrorCode::WouldBlock:
        return EventReason::Capacity;
    case Core::ErrorCode::PlatformError:
        return EventReason::TransportError;
    default:
        return EventReason::Unknown;
    }
}
}
