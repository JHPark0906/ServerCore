#pragma once

#include "ServerCore/Net/IoContext.h"

#include <cstdint>
#include <functional>

namespace ServerCore::Net
{
// epoll carries monotonic registration identities, never pointers or file descriptors.
// A fetched event retains its callback, so removing a registration cannot invalidate
// an event already fetched by a worker. Targets still check their own closed state.
class IoContextAccess
{
public:
    using Callback = std::function<void(std::uint32_t)>;
    [[nodiscard]] static Core::Result<std::uint64_t> Register(
        IoContext& context, int descriptor, std::uint32_t events, Callback callback);
    [[nodiscard]] static Core::Status Rearm(IoContext& context, int descriptor,
        std::uint64_t registration, std::uint32_t events) noexcept;
    static void Remove(IoContext& context, int descriptor, std::uint64_t registration) noexcept;
};
}
