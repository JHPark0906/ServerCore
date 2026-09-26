#pragma once

#include "ServerCore/Net/IoContext.h"

#include "Net/WinsockInternal.h"

#include <atomic>
#include <functional>
#include <memory>

namespace ServerCore::Net
{
/// <summary>
/// 전송 층 구현이 IoContext의 완료 포트에 닿는 유일한 통로다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 수락기와 연결은 자기 소켓을 완료 포트에 이어 붙여야 하는데, 완료 포트 핸들은 Windows
/// 타입이라 공개 헤더에 나올 수 없다. 공개 메서드로 열면 소비자가 만질 수 있는 것이 되고,
/// 그러면 "이 층이 플랫폼을 아는 유일한 층"이 깨진다. 그래서 통로를 이름 하나로 좁히고 그
/// 이름만 IoContext의 friend로 둔다.
///
/// 소유한다: 아무것도 소유하지 않는다. 상태가 없는 함수 묶음이다.
///
/// 스레드 안전성: 스레드 안전. IoContext의 잠금을 잡고 돈다.
///
/// 약속하지 않는 것:
/// - 이 헤더는 공개 계약이 아니다. include/ 아래에 없으므로 소비자가 볼 수 없고, 여기 있는
///   것은 예고 없이 바뀐다.
/// </remarks>
class IoContextAccess
{
public:
    /// <summary>소켓을 완료 포트에 이어 붙인다. 그 뒤로 그 소켓의 완료가 여기로 온다.</summary>
    /// <remarks>
    /// 완료 키를 쓰지 않고 0으로 둔다. 완료를 누구에게 배분할지는 OVERLAPPED 쪽에서
    /// 되찾으므로 키에 담을 것이 없고, 두 군데에 같은 뜻을 두면 어긋날 자리가 생긴다.
    /// </remarks>
    /// <param name="socket">이어 붙일 소켓. 겹침 소켓이어야 한다.</param>
    /// <returns>돌고 있지 않으면 Closed. 붙이지 못하면 PlatformError.</returns>
    [[nodiscard]] static Core::Status AssociateSocket(IoContext& context, SOCKET socket);

    /// <summary>IoContext가 돌고 있는지를 담은 공유 표시를 준다.</summary>
    /// <remarks>
    /// IoContext가 먼저 소멸해도 이 표시는 남아 거짓을 읽힌다. Acceptor::Stop()이 IoContext 객체에
    /// 닿지 않고 완료를 처리할 스레드가 있는지 확인하는 데 쓴다. Stop()이 시작되면 거짓이 된다.
    /// </remarks>
    [[nodiscard]] static std::shared_ptr<const std::atomic<bool>> RunningFlag(
        IoContext& context) noexcept;

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
    /// <summary>Stop()이 종료 신호를 다 보낸 직후, worker를 기다리기 전에 부를 함수를 건다. 시험 전용이다.</summary>
    /// <remarks>Stop()을 부른 스레드에서 불린다. 빈 함수를 넘기면 지운다.</remarks>
    SERVERCORE_TEST_API static void SetAfterStopSignalHook(
        IoContext& context, std::function<void()> hook);
#endif
};
}
