#pragma once

/// <summary>
/// 전송 층이 플랫폼 헤더와 만나는 유일한 자리다. 이 층 밖으로 나가지 않는다.
/// </summary>
/// <remarks>
/// 왜 이 파일이 있는가:
/// WinSock2.h는 반드시 Windows.h보다 먼저 포함되어야 한다. 반대로 두면 Windows.h가 딸려
/// 들이는 옛 winsock.h가 먼저 잡혀 소켓 타입이 중복 정의되고, 그 오류는 이 파일이 아니라
/// 그것을 포함한 쪽에서 난다. 순서를 지키자는 약속만으로는 소스가 늘어나는 만큼 어긋날
/// 자리도 늘어난다.
///
/// 그래서 이 층의 소스는 플랫폼 헤더를 직접 포함하지 않고 이 헤더 하나만 포함한다.
/// 그것이 위 순서를 지키는 수단이다. 즉 순서를 틀릴 수 있는 자리가 이 파일 안 한 곳뿐이다.
///
/// 아래 인클루드 묶음 사이의 빈 줄에도 뜻이 있다. clang-format은 빈 줄로 나뉜 묶음을
/// 넘나들며 정렬하지 않으므로, 빈 줄이 도구가 이 순서를 재배열하는 것을 막는다.
///
/// 이 헤더는 공개 헤더가 아니다. include/ 아래에 두지 않는 이유는 소비자에게 Windows
/// 매크로 지뢰를 넘기지 않기 위해서다.
/// </remarks>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ServerCore/Core/Error.h"

#include <memory>
#include <string>
#include <string_view>

#include <WinSock2.h>

#include <MSWSock.h>
#include <WS2tcpip.h>

#include <Windows.h>

namespace ServerCore::Net
{
/// <summary>
/// Winsock이 초기화된 채로 유지되는 구간을 나타낸다. 살아 있는 동안 Winsock이 산다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// WSAStartup과 WSACleanup은 짝이 맞아야 하는데, 짝을 손으로 맞추면 이른 반환 하나에
/// 어긋난다. 이 타입이 그 짝을 소멸자에 묶는다.
///
/// 소유한다: 자기 몫의 Winsock 초기화 하나.
/// 소유하지 않는다: 소켓. 이 타입은 소켓을 만들지도 닫지도 않는다.
///
/// 소유권과 수명:
/// AcquireWinsock()이 shared_ptr로 돌려준다. 소켓을 든 것들이 그 shared_ptr를 함께 들고
/// 있으므로, 마지막 소켓 소유자가 사라질 때까지 Winsock이 내려가지 않는다. 이것이
/// "수락기가 먼저 죽어도 연결의 소켓이 유효하다"를 지키는 수단이다.
///
/// 스레드 안전성: 스레드 안전. 만들고 나면 상태가 바뀌지 않는다.
///
/// 약속하지 않는 것:
/// - 프로세스 전체에서 하나뿐이라고 약속하지 않는다. 여럿이 있어도 된다. Winsock 자신이
///   초기화 횟수를 센다.
/// </remarks>
class WinsockScope
{
public:
    /// <summary>이미 초기화에 성공한 뒤에만 만들어진다. AcquireWinsock()만 부른다.</summary>
    /// <remarks>
    /// 공개 생성자인 이유: std::make_shared가 부를 수 있어야 한다. 아무나 부르지 못하게
    /// 하려고 CreationKey를 받는다. 이 타입은 이 파일 밖에서 만들 수 없다.
    /// </remarks>
    struct CreationKey
    {
        explicit CreationKey() = default;
    };

    explicit WinsockScope(CreationKey) noexcept;
    ~WinsockScope();

    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
    WinsockScope(WinsockScope&&) = delete;
    WinsockScope& operator=(WinsockScope&&) = delete;
};

/// <summary>Winsock을 초기화하고 그 수명을 담은 shared_ptr를 돌려준다.</summary>
/// <returns>
/// 실패는 WSAStartup 또는 그 수명 객체 할당이 실패한 경우이며 PlatformError로 온다.
/// WSAStartup 실패의 설명 문자열에는 반환 코드가 숫자 그대로 들어간다. 다만 그 문자열을
/// 만들 메모리조차 부족하거나 수명 객체 할당이 실패하면 설명은 비어 있을 수 있다.
/// </returns>
[[nodiscard]] Core::Result<std::shared_ptr<WinsockScope>> AcquireWinsock();

/// <summary>소켓 오류를 사람이 읽을 문장으로 만든다. 숫자를 지우지 않는다.</summary>
/// <remarks>
/// 코드 규약이 정한 것이다. WSAGetLastError()의 값을 숫자 그대로 남긴다. 이름으로 바꾸면
/// 우리가 모르는 코드가 왔을 때 그 값이 사라진다.
/// </remarks>
/// <param name="what">실패한 호출의 이름. 예를 들어 "bind".</param>
/// <param name="socketError">WSAGetLastError()가 돌려준 값.</param>
[[nodiscard]] std::string FormatSocketError(std::string_view what, int socketError);

/// <summary>소켓 오류를 PlatformError Status로 만든다. 설명 할당 실패 시에도 예외를 던지지 않는다.</summary>
[[nodiscard]] Core::Status MakeSocketFailure(std::string_view what, int socketError) noexcept;

/// <summary>Win32 오류(GetLastError)를 PlatformError Status로 만든다. 설명 할당 실패 시에도 예외를 던지지 않는다.</summary>
/// <remarks>
/// 소켓이 아닌 호출(CreateIoCompletionPort 따위)이 실패했을 때 쓴다. 숫자를 그대로 남기는
/// 규칙은 소켓 쪽과 같다.
/// </remarks>
[[nodiscard]] Core::Status MakeWin32Failure(
    std::string_view what, unsigned long win32Error) noexcept;
}
