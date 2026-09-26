#pragma once
#include "ServerCore/Export.h"

#if !defined(SERVERCORE_ENABLE_TEST_HOOKS)
#error "ExecutionTestAccess is available only while ServerCore test hooks are enabled"
#endif

#include <chrono>
#include <memory>

// Execution 층(TimerScheduler 등) 구현이 포함하는 시험 전용 지점만 둔다. Host 층 구성 요소의 gate는
// TransportTestAccess.h에 둔다. cmake/CheckLayers.cmake가 이 헤더를 Execution 층으로 분류한다.
namespace ServerCore::Runtime::TestAccess
{
/// <summary>반복 타이머가 다음 due를 정하는 순간을 관찰하는 시험 전용 지점이다.</summary>
/// <remarks>
/// 공개 Runtime 계약이 아니다. 이 헤더는 src 아래에만 있고, 시험을 빌드할 때만 ServerCore와
/// ServerCoreTests가 함께 이 선언을 본다. 벽시계 지연을 재지 않고도 재무장 규칙(고정 지연인지
/// 고정 속도 격자인지)을 판정할 수 있게 한다. 스케줄러 mutex를 쥔 채 불리므로 값을 기록하고
/// 곧바로 돌아와야 한다.
/// </remarks>
class ITimerRepeatObserver
{
public:
    virtual ~ITimerRepeatObserver() = default;

    /// <param name="served">방금 끝난 실행이 맡았던 due다.</param>
    /// <param name="next">다음 실행의 due다.</param>
    /// <param name="now">완료를 처리한 시각이다.</param>
    virtual void OnRepeatArmed(std::chrono::steady_clock::time_point served,
        std::chrono::steady_clock::time_point next,
        std::chrono::steady_clock::time_point now) noexcept = 0;
};

/// <summary>반복 타이머 재무장 관찰자를 설치한다. 보관은 weak_ptr로 한다.</summary>
SERVERCORE_TEST_API void InstallTimerRepeatObserver(std::shared_ptr<ITimerRepeatObserver> observer);

/// <summary>같은 관찰자가 아직 설치되어 있을 때만 제거한다.</summary>
SERVERCORE_TEST_API void ClearTimerRepeatObserver(
    const std::shared_ptr<ITimerRepeatObserver>& expected);
}
