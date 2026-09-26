#pragma once

#include "ServerCore/Export.h"

#include "ServerCore/Net/Acceptor.h"

#include <chrono>
#include <cstddef>
#include <memory>

namespace ServerCore::Net
{
class SendBudget;

inline bool ValidSendQueueLimits(const SendQueueLimits& limits) noexcept
{
    return limits.connectionBytes > 0 && limits.connectionBytes <= 1024 * 1024 &&
           limits.totalBytes > 0 && limits.totalBytes <= 512 * 1024 * 1024;
}

// Store one callable, then retain it during each handoff without copying mutable
// capture state. Different accepted connections may invoke it concurrently.
using SharedConnectionHandler =
    std::shared_ptr<const std::function<void(std::shared_ptr<Connection>)>>;

/// <summary>실행 환경 층이 Acceptor가 만들 Connection에 내부 송신 예산을 주입하는 통로다.</summary>
/// <remarks>
/// 공개 Acceptor는 ServerHost의 합산 메모리 정책을 알지 않는다. 이 접근자는 ServerHost 조립
/// 지점에서만 쓰며, 소비자가 Acceptor를 직접 사용할 때의 공개 표면을 늘리지 않는다.
/// </remarks>
class AcceptorAccess
{
public:
    SERVERCORE_TEST_API static void SetSendBudget(
        Acceptor& acceptor, std::shared_ptr<SendBudget> sendBudget);

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
    /// <summary>켜 둔 동안 수락 준비가 자원 부족으로 실패한 것처럼 돈다. 시험 전용이다.</summary>
    /// <remarks>
    /// Windows는 AcceptEx 게시가 WSAENOBUFS로, Linux는 accept4가 ENOBUFS로 실패한 것처럼 된다.
    /// 실제 자원 고갈은 결정적으로 만들 수 없는 경우가 많아, 재시도 경로를 시간 경합 없이 돌리려고 둔다.
    /// </remarks>
    SERVERCORE_TEST_API static void SetAcceptFailureInjection(Acceptor& acceptor, bool enabled);

    /// <summary>주입으로 실패시킨 누적 횟수다.</summary>
    [[nodiscard]] SERVERCORE_TEST_API static std::size_t InjectedAcceptFailureCount(
        const Acceptor& acceptor);

#if defined(_WIN32)
    /// <summary>수락된 소켓을 IoContext에 붙이기 직전에 부를 함수를 건다. 시험 전용이다.</summary>
    /// <remarks>수락 완료를 처리하는 I/O 스레드에서 불린다. 빈 함수를 넘기면 지운다.</remarks>
    SERVERCORE_TEST_API static void SetBeforeAssociateHook(
        Acceptor& acceptor, std::function<void()> hook);

    /// <summary>Stop()이 취소된 AcceptEx 완료를 기다리는 제한을 바꾼다. 시험 전용이다.</summary>
    /// <remarks>0으로 두면 제한이 적용되는 경로가 시간 경합 없이 바로 드러난다.</remarks>
    SERVERCORE_TEST_API static void SetPendingAcceptDrainTimeout(
        Acceptor& acceptor, std::chrono::milliseconds timeout);

    /// <summary>지금 걸려 있는 AcceptEx 요청 수다.</summary>
    [[nodiscard]] SERVERCORE_TEST_API static std::size_t PendingAcceptCount(
        const Acceptor& acceptor);
#endif
#endif
};
}
