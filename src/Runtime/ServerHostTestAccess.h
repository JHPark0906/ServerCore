#pragma once

#if !defined(SERVERCORE_ENABLE_TEST_HOOKS)
#error "ServerHostTestAccess is available only while ServerCore test hooks are enabled"
#endif

#include <memory>

namespace ServerCore::Runtime::TestAccess
{
/// <summary>파싱 worker가 JSON 해석 직전에 멈출 수 있게 하는 시험 전용 동기화 지점이다.</summary>
/// <remarks>
/// 공개 Runtime 계약이 아니다. 이 헤더는 src 아래에만 있고, 시험을 빌드할 때만 ServerCore와
/// ServerCoreTests가 함께 이 선언을 본다. worker가 gate 호출 동안에도 ParseReservation을
/// 붙들어, 전역 parse 예산의 경쟁 상태를 시간 지연 없이 재현할 수 있다.
/// </remarks>
class IBeforeParseGate
{
public:
    virtual ~IBeforeParseGate() = default;

    virtual void WaitBeforeParse() noexcept = 0;
};

/// <summary>다음 parse worker가 만날 gate를 설치한다.</summary>
/// <remarks>보관은 weak_ptr로 하므로 시험이 끝난 뒤 전역 상태가 gate를 소유하지 않는다.</remarks>
void InstallBeforeParseGate(std::shared_ptr<IBeforeParseGate> gate);

/// <summary>같은 gate가 아직 설치되어 있을 때만 제거한다.</summary>
/// <remarks>서로 독립적인 시험이 다른 gate를 설치했으면 그것을 지우지 않는다.</remarks>
void ClearBeforeParseGate(const std::shared_ptr<IBeforeParseGate>& expected);

/// <summary>NetworkSession 수신 callback에서 I/O worker를 멈추는 시험 전용 gate다.</summary>
class IBeforeSessionReceiveGate
{
public:
    virtual ~IBeforeSessionReceiveGate() = default;

    virtual void WaitBeforeSessionReceive() noexcept = 0;
};

/// <summary>다음 NetworkSession 수신 callback들이 만날 gate를 설치한다.</summary>
void InstallBeforeSessionReceiveGate(std::shared_ptr<IBeforeSessionReceiveGate> gate);

/// <summary>같은 수신 gate가 아직 설치되어 있을 때만 제거한다.</summary>
void ClearBeforeSessionReceiveGate(const std::shared_ptr<IBeforeSessionReceiveGate>& expected);

/// <summary>수락 handler가 새 Connection의 Start()로 돌아가기 직전에 멈추는 시험 gate다.</summary>
class IBeforeConnectionStartGate
{
public:
    virtual ~IBeforeConnectionStartGate() = default;

    virtual void WaitBeforeConnectionStart() noexcept = 0;
};

/// <summary>다음 수락 handler가 Connection::Start() 직전에 만날 gate를 설치한다.</summary>
void InstallBeforeConnectionStartGate(std::shared_ptr<IBeforeConnectionStartGate> gate);

/// <summary>같은 Connection 시작 gate가 아직 설치되어 있을 때만 제거한다.</summary>
void ClearBeforeConnectionStartGate(const std::shared_ptr<IBeforeConnectionStartGate>& expected);

/// <summary>Acceptor 시작 성공 뒤 Host가 Running으로 전이하기 직전을 멈추는 시험 gate다.</summary>
class IBeforeHostRunningGate
{
public:
    virtual ~IBeforeHostRunningGate() = default;

    virtual void WaitBeforeHostRunning() noexcept = 0;
};

/// <summary>다음 ServerHost 시작이 Running으로 전이하기 직전에 만날 gate를 설치한다.</summary>
void InstallBeforeHostRunningGate(std::shared_ptr<IBeforeHostRunningGate> gate);

/// <summary>같은 Host 시작 gate가 아직 설치되어 있을 때만 제거한다.</summary>
void ClearBeforeHostRunningGate(const std::shared_ptr<IBeforeHostRunningGate>& expected);

/// <summary>실패한 Start의 소유자 파기와 Stop의 소유자 읽기가 겹치지 않는지 보는 시험 경계다.</summary>
class IFailedStartOwnerGate
{
public:
    virtual ~IFailedStartOwnerGate() = default;

    virtual void WaitBeforeFailedStartOwnerReset() noexcept = 0;
    virtual void OnStopOwnerRead() noexcept = 0;
};

/// <summary>실패 정리의 파기 직전과 Stop의 실행 문맥 검사 직전 경계를 설치한다.</summary>
void InstallFailedStartOwnerGate(std::shared_ptr<IFailedStartOwnerGate> gate);

/// <summary>같은 실패 정리 gate가 아직 설치되어 있을 때만 제거한다.</summary>
void ClearFailedStartOwnerGate(const std::shared_ptr<IFailedStartOwnerGate>& expected);

/// <summary>다음 NetworkSession 종료가 JobRunner Post 실패 fallback을 타게 한다.</summary>
void FailNextFinalizePost() noexcept;

/// <summary>아직 소비되지 않은 finalizer Post 실패 표식을 지운다.</summary>
void ClearFinalizePostFailure() noexcept;
}
