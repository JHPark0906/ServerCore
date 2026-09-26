#pragma once

#include "ServerCore/Net/IoContext.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace ServerCore::Net
{
class EpollState;

// epoll carries monotonic registration identities, never pointers or file descriptors.
// A fetched event retains its callback, so removing a registration cannot invalidate
// an event already fetched by a worker. Targets still check their own closed state.
class IoContextAccess
{
public:
    using Callback = std::function<void(std::uint32_t)>;
    // Shared ownership of an IoContext's epoll state. Registrants keep it instead
    // of IoContext&, so calls after the IoContext is destroyed stay defined.
    using EventSet = std::shared_ptr<EpollState>;
    [[nodiscard]] static EventSet Share(IoContext& context) noexcept;
    [[nodiscard]] static Core::Result<std::uint64_t> Register(
        const EventSet& eventSet, int descriptor, std::uint32_t events, Callback callback);
    [[nodiscard]] static Core::Status Rearm(const EventSet& eventSet, int descriptor,
        std::uint64_t registration, std::uint32_t events) noexcept;
    static void Remove(
        const EventSet& eventSet, int descriptor, std::uint64_t registration) noexcept;
};
}
