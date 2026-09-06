#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Runtime/JobRunner.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace ServerCore::Runtime
{
/// <summary>
/// 일정한 간격으로 작업을 예약하고, 실제 작업은 JobRunner의 직렬 스레드에서 실행한다.
/// </summary>
/// <remarks>
/// 타이머 스레드는 시간만 재며 게임 상태를 만지지 않는다. 콜백은 항상 주어진 JobRunner에서
/// 실행되므로 세션 목록처럼 직렬화가 필요한 상태를 안전하게 다룰 수 있다.
/// PeriodicRunner는 JobRunner의 원시 주소를 저장하지 않고, 생성 때 얻은 수명 손잡이를
/// 저장한다. 따라서 생성 뒤 JobRunner가 정상 수명 절차를 따라 사라져도 타이머 스레드는
/// 해제된 JobRunner를 건드리지 않고 예약을 멈춘다. 단, JobRunner&를 넘기는 생성 시점에는
/// 그 JobRunner가 살아 있어야 하며, JobRunner 자신의 실행 스레드는 객체를 없애기 전에
/// 호출자가 join해야 한다.
///
/// 밀린 주기는 몰아서 실행하지 않는다. 타이머가 한 주기 이상 늦었거나 앞선 콜백이 아직
/// 큐에 있거나 실행 중이면 그 시각의 콜백은 버리고 SkippedCount에 더한다. 이 규칙은 밀린
/// 스냅숏이 한꺼번에 나가며 지연을 키우는 것을 막는다.
///
/// 콜백은 예외를 던지지 않아야 한다. 콜백은 JobRunner의 직렬 실행 문맥 일부이므로 예외 뒤에
/// 같은 큐의 작업을 계속 실행하면 상태가 어디까지 바뀌었는지 알 수 없다. 구현이 던지면 계약
/// 위반으로 프로세스를 끝낸다.
///
/// Start는 한 번만 성공할 수 있다. Stop은 동시 호출해도 모두 같은 타이머 스레드의 join이
/// 끝난 뒤 돌아온다. 이미 JobRunner에 들어간 콜백은 JobRunner 자신의 Stop 규칙에 따라
/// 실행될 수 있다.
/// </remarks>
class PeriodicRunner
{
public:
    using Period = std::chrono::milliseconds;
    using Callback = std::function<void()>;

    /// <summary>작업 투입 손잡이·주기·실행할 콜백을 정한다. 실제 타이머 스레드는 Start에서 만든다.</summary>
    PeriodicRunner(JobRunner::Lease runner, Period period, Callback callback);

    /// <summary>독립 JobRunner를 쓰는 편의 생성자다.</summary>
    /// <remarks>
    /// ServerHost가 소유한 실행자에는 GetJobRunner()이 돌려주는 Lease를 넘긴다. 이 overload는
    /// 호출자가 JobRunner의 종료와 실행 스레드 join을 직접 소유하는 경우에만 쓴다.
    /// </remarks>
    PeriodicRunner(JobRunner& runner, Period period, Callback callback);
    ~PeriodicRunner();

    PeriodicRunner(const PeriodicRunner&) = delete;
    PeriodicRunner& operator=(const PeriodicRunner&) = delete;

    /// <summary>타이머 스레드를 시작한다.</summary>
    /// <returns>0 이하 주기나 빈 콜백은 InvalidArgument, 중복 시작은 AlreadyExists다.</returns>
    Core::Status Start();

    /// <summary>새 주기 예약을 멈추고 타이머 스레드의 join이 끝날 때까지 기다린다. 스레드 안전하다.</summary>
    /// <remarks>
    /// 여러 호출자가 동시에 Stop을 부르면 한 호출자가 join하고 나머지는 그 join 완료를 기다린다.
    /// 따라서 이 함수가 돌아온 뒤에는 타이머 스레드가 더는 PeriodicRunner 상태를 실행하지 않는다.
    /// 이미 JobRunner에 들어간 콜백은 실행될 수 있지만, Stop은 timer join 뒤 JobRunner Lease를
    /// 놓는다. 큐의 콜백이 이 객체의 상태를 소유해도 상태가 같은 큐를 역으로 소유하지 않게 해,
    /// 실행되지 않은 콜백이 소유 순환으로 남는 것을 막는다.
    /// 타이머 스레드 자신이 부르는 것은 계약 위반이다.
    /// </remarks>
    void Stop();

    /// <summary>실행하지 않고 버린 주기 수다.</summary>
    [[nodiscard]] std::uint64_t SkippedCount() const noexcept;

    /// <summary>새 예약을 만드는 타이머 루프가 실행 중인지 답한다.</summary>
    /// <remarks>동시 실행 중의 순간값이다. 종료 동기화에는 Stop의 반환을 쓴다.</remarks>
    [[nodiscard]] bool IsRunning() const noexcept;

private:
    static void RecordSkippedPeriods(const JobRunner::Lease& runner, std::uint64_t count) noexcept;

    class State;
    std::shared_ptr<State> mState;
};
}
