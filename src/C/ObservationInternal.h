#pragma once
#include "C/Internal.h"
#include "ServerCore/Observability/Observation.h"
#include <algorithm>
namespace ServerCore::CDetail
{
inline void AddObservation(std::uint64_t& value, std::uint64_t amount) noexcept
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    value = amount > maximum - value ? maximum : value + amount;
}
inline sc_status CopyObservation(const Observability::ObservationSnapshot& value, sc_observation* out) noexcept
{
    if (!Version(out)) return SC_INVALID_ARGUMENT;
    out->protocol = static_cast<uint32_t>(value.protocol); out->lifecycle = static_cast<uint32_t>(value.lifecycle);
    out->available = value.available; out->connections = value.connections; out->pending_work = value.pendingWork;
    out->receive_bytes = value.receiveBytes; out->send_bytes = value.sendBytes; out->retained_bytes = value.retainedBytes;
    out->drain_remaining = value.drainRemaining; out->timed_out = value.timedOut;
    std::copy(value.closed.begin(), value.closed.end(), out->closed);
    std::copy(value.rejected.begin(), value.rejected.end(), out->rejected);
    return SC_OK;
}
}
