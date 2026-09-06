#include "ServerCore/Core/Clock.h"

#include <chrono>
#include <cstdint>

namespace ServerCore::Core
{
std::uint64_t MillisecondsSinceProcessStart() noexcept
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point origin = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin);
    return static_cast<std::uint64_t>(elapsed.count());
}
}
