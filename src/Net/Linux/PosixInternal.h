#pragma once

#include "ServerCore/Core/Error.h"

#include <cerrno>
#include <string>
#include <string_view>
#include <utility>

namespace ServerCore::Net
{
[[nodiscard]] inline Core::Status MakePosixFailure(
    const std::string_view operation, const int error) noexcept
{
    try
    {
        std::string message(operation);
        message.append(" failed, errno=");
        message.append(std::to_string(error));
        return Core::Status::Fail(Core::ErrorCode::PlatformError, std::move(message));
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}

[[nodiscard]] inline bool IsWouldBlock(const int error) noexcept
{
    return error == EAGAIN || error == EWOULDBLOCK;
}
}
