#pragma once
#include "C/Internal.h"
#include "ServerCore/Observability/Observation.h"
#include <algorithm>
namespace ServerCore::CDetail
{
// CopyObservation은 프로토콜·수명·가용 비트를 그대로 옮기고 사유별 배열을 통째로 복사한다.
// 번호나 배열 길이가 갈리면 잘못된 값이 나가거나 out->closed를 넘어 쓰므로 여기서 멈춘다.
static_assert(SC_PROTOCOL_UNKNOWN == static_cast<int>(Observability::Protocol::Unknown) &&
              SC_PROTOCOL_HTTP == static_cast<int>(Observability::Protocol::Http) &&
              SC_PROTOCOL_WEBSOCKET == static_cast<int>(Observability::Protocol::WebSocket) &&
              SC_PROTOCOL_TCP == static_cast<int>(Observability::Protocol::Tcp) &&
              SC_PROTOCOL_UDP == static_cast<int>(Observability::Protocol::Udp) &&
              SC_PROTOCOL_TASK == static_cast<int>(Observability::Protocol::Task) &&
              SC_PROTOCOL_HOST == static_cast<int>(Observability::Protocol::Host) &&
              SC_PROTOCOL_LOGGER == static_cast<int>(Observability::Protocol::Logger));
static_assert(SC_LIFECYCLE_UNKNOWN == static_cast<int>(Observability::Lifecycle::Unknown) &&
              SC_LIFECYCLE_CREATED == static_cast<int>(Observability::Lifecycle::Created) &&
              SC_LIFECYCLE_RUNNING == static_cast<int>(Observability::Lifecycle::Running) &&
              SC_LIFECYCLE_DRAINING == static_cast<int>(Observability::Lifecycle::Draining) &&
              SC_LIFECYCLE_STOPPED == static_cast<int>(Observability::Lifecycle::Stopped));
static_assert(SC_REASON_UNKNOWN == static_cast<int>(Observability::EventReason::Unknown) &&
              SC_REASON_NORMAL == static_cast<int>(Observability::EventReason::Normal) &&
              SC_REASON_CANCELLED == static_cast<int>(Observability::EventReason::Cancelled) &&
              SC_REASON_TIMEOUT == static_cast<int>(Observability::EventReason::Timeout) &&
              SC_REASON_PROTOCOL == static_cast<int>(Observability::EventReason::ProtocolError) &&
              SC_REASON_CAPACITY == static_cast<int>(Observability::EventReason::Capacity) &&
              SC_REASON_POLICY == static_cast<int>(Observability::EventReason::Policy) &&
              SC_REASON_TRANSPORT == static_cast<int>(Observability::EventReason::TransportError) &&
              static_cast<std::size_t>(SC_REASON_COUNT) == Observability::EventReasonCount);
static_assert(static_cast<std::uint64_t>(SC_OBSERVATION_CONNECTIONS) ==
                  static_cast<std::uint64_t>(Observability::Connections) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_PENDING_WORK) ==
                  static_cast<std::uint64_t>(Observability::PendingWork) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_RECEIVE_BYTES) ==
                  static_cast<std::uint64_t>(Observability::ReceiveBytes) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_SEND_BYTES) ==
                  static_cast<std::uint64_t>(Observability::SendBytes) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_RETAINED_BYTES) ==
                  static_cast<std::uint64_t>(Observability::RetainedBytes) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_DRAIN_REMAINING) ==
                  static_cast<std::uint64_t>(Observability::DrainRemaining) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_CLOSED) ==
                  static_cast<std::uint64_t>(Observability::ClosedEvents) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_REJECTED) ==
                  static_cast<std::uint64_t>(Observability::RejectedEvents) &&
              static_cast<std::uint64_t>(SC_OBSERVATION_TIMED_OUT) ==
                  static_cast<std::uint64_t>(Observability::TimedOutEvents));
inline void AddObservation(std::uint64_t& value, std::uint64_t amount) noexcept
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    value = amount > maximum - value ? maximum : value + amount;
}
inline sc_status CopyObservation(
    const Observability::ObservationSnapshot& value, sc_observation* out) noexcept
{
    if (!OutputVersion(out))
        return SC_INVALID_ARGUMENT;
    out->protocol = static_cast<uint32_t>(value.protocol);
    out->lifecycle = static_cast<uint32_t>(value.lifecycle);
    out->available = value.available;
    out->connections = value.connections;
    out->pending_work = value.pendingWork;
    out->receive_bytes = value.receiveBytes;
    out->send_bytes = value.sendBytes;
    out->retained_bytes = value.retainedBytes;
    out->drain_remaining = value.drainRemaining;
    out->timed_out = value.timedOut;
    std::copy(value.closed.begin(), value.closed.end(), out->closed);
    std::copy(value.rejected.begin(), value.rejected.end(), out->rejected);
    return SC_OK;
}
}
