#pragma once
#include "ServerCore/Export.h"

#if !defined(SERVERCORE_ENABLE_TEST_HOOKS)
#error "TransportTestAccess is available only while ServerCore test hooks are enabled"
#endif

#include <memory>

// Host 층 구성 요소(DatagramTransport 등)의 시험 전용 gate만 둔다. Execution 층 구현이 포함하는
// 지점은 ExecutionTestAccess.h에 둔다.
namespace ServerCore::Runtime::TestAccess
{
/// <summary>DatagramTransport 수신 펌프의 두 스레드를 원하는 순서로 세우는 시험 전용 gate다.</summary>
/// <remarks>
/// 공개 Runtime 계약이 아니다. 이 헤더는 src 아래에만 있고, 시험을 빌드할 때만 ServerCore와
/// ServerCoreTests가 함께 이 선언을 본다. 모니터 스레드의 readable 콜백과 타이머 스레드의 완료
/// 콜백이 서로 겹치는 좁은 창을 시간 지연 없이 재현한다. 어느 잠금도 쥐지 않은 자리에서 불린다.
/// </remarks>
class IReceivePumpGate
{
public:
    virtual ~IReceivePumpGate() = default;

    /// <summary>readable 콜백이 배치 타이머의 완료 구독을 받은 직후에 불린다.</summary>
    virtual void AfterCompletionSubscribed() noexcept = 0;

    /// <summary>재무장이 새 readable 구독을 게시한 직후, 교체된 구독을 정리하기 전에 불린다.</summary>
    virtual void AfterRearmPublished() noexcept = 0;
};

/// <summary>수신 펌프 gate를 설치한다. 보관은 weak_ptr로 한다.</summary>
SERVERCORE_TEST_API void InstallReceivePumpGate(std::shared_ptr<IReceivePumpGate> gate);

/// <summary>같은 gate가 아직 설치되어 있을 때만 제거한다.</summary>
SERVERCORE_TEST_API void ClearReceivePumpGate(const std::shared_ptr<IReceivePumpGate>& expected);

/// <summary>OutboundQueue 펌프가 다음 깨움 타이머를 걸기 직전에 끼어드는 시험 전용 gate다.</summary>
/// <remarks>
/// 펌프가 다음 시각을 정한 잠금과 타이머를 게시하는 잠금 사이의 창에서 용량 알림이나 Enqueue가
/// 도착하는 경우를 시간 지연 없이 재현한다. 어느 잠금도 쥐지 않은 펌프 스레드에서 불린다.
/// </remarks>
class IOutboundArmGate
{
public:
    virtual ~IOutboundArmGate() = default;

    virtual void BeforeArm() noexcept = 0;
};

/// <summary>OutboundQueue 재무장 gate를 설치한다. 보관은 weak_ptr로 한다.</summary>
SERVERCORE_TEST_API void InstallOutboundArmGate(std::shared_ptr<IOutboundArmGate> gate);

/// <summary>같은 gate가 아직 설치되어 있을 때만 제거한다.</summary>
SERVERCORE_TEST_API void ClearOutboundArmGate(const std::shared_ptr<IOutboundArmGate>& expected);
}
