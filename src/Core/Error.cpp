#include "ServerCore/Core/Error.h"

#include <utility>

namespace ServerCore::Core
{
// 여기서 만드는 설명 문자열은 영문이다. 주석은 한국어지만 이것은 실행 중에 콘솔이나 로그로
// 나가는 값이고, 콘솔 코드페이지가 UTF-8이 아니면 한글이 깨진다. 단언의 마지막 한 줄을
// 영문으로 둔 것과 같은 이유다.

Status Status::Ok() noexcept
{
    return Status{};
}

Status Status::Fail(ErrorCode code, std::string message)
{
    SERVERCORE_ASSERT(code != ErrorCode::Ok, "Fail() was given ErrorCode::Ok");

    Status status;
    status.mCode = code;
    status.mMessage = std::move(message);
    return status;
}

Status Status::FailWithoutMessage(const ErrorCode code) noexcept
{
    SERVERCORE_ASSERT(code != ErrorCode::Ok, "FailWithoutMessage() was given ErrorCode::Ok");

    Status status;
    status.mCode = code;
    return status;
}

Status Status::AllocationFailure() noexcept
{
    return FailWithoutMessage(ErrorCode::PlatformError);
}

Status Status::Unimplemented(std::string_view what)
{
    std::string message("not implemented: ");
    message.append(what);
    return Fail(ErrorCode::Unimplemented, std::move(message));
}

ErrorCode Status::Code() const noexcept
{
    return mCode;
}

bool Status::IsOk() const noexcept
{
    return mCode == ErrorCode::Ok;
}

const std::string& Status::Message() const noexcept
{
    return mMessage;
}
}
