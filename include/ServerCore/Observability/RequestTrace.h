#pragma once
#include "ServerCore/Core/Error.h"
#include <cstdint>
#include <functional>
#include <string>

namespace ServerCore::Observability
{
// Owned metadata for one terminal routed HTTP response. IDs are local to one
// server lifetime. No URL/path/query, headers, credentials or body are collected.
// method contains at most the first 64 bytes of the validated method token.
// Ok means fully admitted to the local send queue, not remote receipt.
struct RequestTrace
{
    std::uint64_t requestId = 0, connectionId = 0;
    std::string method;
    unsigned status = 0; // Zero if no response headers were committed.
    Core::ErrorCode outcome = Core::ErrorCode::Ok;
    std::uint64_t elapsedNanoseconds = 0;
};
using RequestTraceHandler = std::function<void(RequestTrace)>;
}
