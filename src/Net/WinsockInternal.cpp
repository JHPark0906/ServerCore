#include "Net/WinsockInternal.h"

#include <new>
#include <string>

namespace ServerCore::Net
{
WinsockScope::WinsockScope(CreationKey) noexcept
{
    // 초기화 자체는 AcquireWinsock()이 이미 했다. 이 타입은 그 짝인 WSACleanup을 소멸자에
    // 묶어 두는 일만 한다.
}

WinsockScope::~WinsockScope()
{
    // WSACleanup의 실패는 알릴 곳이 없다. 소멸자는 값을 돌려주지 못하고, 실패했다면 이미
    // Winsock이 내려간 뒤다. 그래서 반환값을 일부러 버린다.
    (void)::WSACleanup();
}

Core::Result<std::shared_ptr<WinsockScope>> AcquireWinsock()
{
    WSADATA data{};

    // 2.2를 요구한다. 이 층이 쓰는 것(겹침 소켓, AcceptEx, 완료 포트)이 전부 그 안에 있다.
    const int startupResult = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (startupResult != 0)
    {
        // WSAStartup은 오류를 반환값으로 준다. WSAGetLastError()가 아니다.
        return Core::Result<std::shared_ptr<WinsockScope>>::FromStatus(
            MakeSocketFailure("WSAStartup", startupResult));
    }

    try
    {
        return Core::Result<std::shared_ptr<WinsockScope>>::FromValue(
            std::make_shared<WinsockScope>(WinsockScope::CreationKey{}));
    }
    catch (const std::bad_alloc&)
    {
        (void)::WSACleanup();
        return Core::Result<std::shared_ptr<WinsockScope>>::FromStatus(
            Core::Status::AllocationFailure());
    }
}

std::string FormatSocketError(std::string_view what, int socketError)
{
    // 숫자를 그대로 남긴다. 코드 규약이 정한 것이고, 이름으로 바꾸면 우리가 모르는 코드가
    // 왔을 때 그 값이 사라진다.
    std::string message;
    message.append(what);
    message.append(" failed, WSAGetLastError()=");
    message.append(std::to_string(socketError));
    return message;
}

Core::Status MakeSocketFailure(const std::string_view what, const int socketError) noexcept
{
    try
    {
        return Core::Status::Fail(
            Core::ErrorCode::PlatformError, FormatSocketError(what, socketError));
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}

Core::Status MakeWin32Failure(const std::string_view what, const unsigned long win32Error) noexcept
{
    try
    {
        std::string message;
        message.append(what);
        message.append(" failed, GetLastError()=");
        message.append(std::to_string(win32Error));
        return Core::Status::Fail(Core::ErrorCode::PlatformError, std::move(message));
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}
}
