#include "TestHarness.h"

#include "ServerCore/Core/Clock.h"

#include <cstdint>

namespace
{
void MillisecondsSinceProcessStartIsMonotonic()
{
    const std::uint64_t first = ServerCore::Core::MillisecondsSinceProcessStart();
    const std::uint64_t second = ServerCore::Core::MillisecondsSinceProcessStart();
    ServerCoreTest::ExpectTrue(second >= first,
        "process-relative millisecond readings never go backwards");
}

const ServerCoreTest::CheckRegistration gMillisecondsSinceProcessStartIsMonotonic{
    "Clock.MillisecondsSinceProcessStartIsMonotonic", MillisecondsSinceProcessStartIsMonotonic
};
}
