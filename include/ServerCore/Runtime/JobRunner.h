#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/JobQueue.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace ServerCore::Runtime
{
class PeriodicRunner;

/// <summary>
/// 하나의 상태를 여러 스레드가 동시에 건드리지 않도록, 그 상태를 만지는 일을 한 스레드에서
/// 차례로 실행하는 실행자다.
/// </summary>
/// <remarks>
/// 왜 방이나 구역이 아니라 이것인가:
/// 방과 구역의 정책은 게임이 소유한다. 이 실행자는 게임 개념과 독립적인 직렬 실행만 제공한다.
///
/// 소유권과 수명: 자신이 스레드를 만들지 않는다. 어떤 스레드가 RunUntilStopped 안으로
/// 들어와야 일이 돈다. 그 스레드를 만드는 것은 ServerHost다.
/// JobRunner를 없애기 전에는 그 외부 스레드가 RunUntilStopped에서 나온 것을 호출자가 join으로
/// 확인해야 한다. 이 객체는 남의 std::thread를 소유하지 않으므로 소멸자에서 그것을 join할 수
/// 없다. 소멸자는 새 작업을 닫아 PeriodicRunner의 lease가 정지한 공유 상태만 보게 한다.
///
/// 스레드 안전성:
/// - Post는 스레드 안전하다.
/// - RunUntilStopped는 한 스레드만 들어간다. 둘이 들어가면 직렬 실행이라는 성질이 깨진다.
///
/// 약속하지 않는 것:
/// - 작업이 언제 실행되는지 약속하지 않는다. 앞선 작업이 오래 걸리면 그만큼 밀린다.
/// - 공평함을 약속하지 않는다. 넣은 순서대로 실행되지만 넣는 쪽들 사이의 균형은 보지 않는다.
/// - Stop은 새 작업 수락을 먼저 닫고, 그 전에 수락한 작업을 전부 실행한 뒤 돌아온다.
///   따라서 Stop 뒤 Post는 Closed를 돌려주며, 이미 들어 있던 작업은 버리지 않는다.
/// - Stop 뒤에 다시 시작하는 것은 지원하지 않는다. 서버 수명 하나에 실행자 하나를 쓴다.
/// </remarks>
class JobRunner
{
private:
    class SharedState;

public:
    /// <summary>
    /// 실행자 수명에 묶인 작업 투입 손잡이다.
    /// </summary>
    /// <remarks>
    /// 이것은 JobRunner 외피 자체를 소유하지 않는다. 대신 실제 큐·정지 상태를 담은 공유 상태를
    /// 보관한다. JobRunner 소멸자는 그 상태에 정지를 요청하므로, 외피가 사라진 뒤에는 Post가
    /// Closed를 돌려주고 IsStopRequested는 true를 돌려준다. 따라서 손잡이를 가진 기반 시설은
    /// 해제된 JobRunner 주소를 읽지 않는다.
    ///
    /// ServerHost는 이 타입만 게임 백엔드에 돌려준다. 독립 JobRunner를 직접 소유한 코드는
    /// 필요하면 Post를 바로 부를 수 있고, 오래 보관하거나 수명 경계를 넘길 때는 Lease를 쓴다.
    /// </remarks>
    class Lease
    {
    public:
        Lease(const Lease&) = default;
        Lease& operator=(const Lease&) = default;
        Lease(Lease&&) noexcept = default;
        Lease& operator=(Lease&&) noexcept = default;

        /// <summary>소유 실행자가 아직 있으면 작업을 넣는다.</summary>
        /// <returns>실행자가 이미 소멸했거나 멈췄으면 Closed다.</returns>
        Core::Status Post(std::function<void()> job) const;

        /// <summary>소유 실행자가 없거나 더는 새 작업을 받지 않으면 true다.</summary>
        [[nodiscard]] bool IsStopRequested() const noexcept;

    private:
        friend class JobRunner;
        friend class PeriodicRunner;
        explicit Lease(std::shared_ptr<SharedState> state) noexcept;

        void RecordSkippedPeriodicPeriods(std::uint64_t count) const noexcept;

        std::shared_ptr<SharedState> mState;
    };

    JobRunner();
    ~JobRunner();

    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    /// <summary>실행할 일을 넣는다.</summary>
    /// <returns>멈춘 뒤에는 Closed, 빈 작업은 InvalidArgument다.</returns>
    Core::Status Post(std::function<void()> job);

    /// <summary>멈추라는 요청이 올 때까지 이 스레드에서 작업을 계속 실행한다.</summary>
    void RunUntilStopped();

    /// <summary>RunUntilStopped에서 빠져나오게 한다. 스레드 안전하다.</summary>
    void Stop();

    /// <summary>현재 호출자가 이 실행자의 작업을 실제로 실행 중인 스레드인지 답한다.</summary>
    /// <remarks>RunUntilStopped 전과 반환 뒤에는 언제나 false다.</remarks>
    [[nodiscard]] bool IsCurrentThread() const noexcept;

    /// <summary>더는 새 작업을 받지 않는 상태인지 답한다.</summary>
    [[nodiscard]] bool IsStopRequested() const noexcept;

    /// <summary>아직 실행을 시작하지 않은 작업 수다.</summary>
    [[nodiscard]] std::size_t PendingCount() const;

    /// <summary>이 실행자의 Lease로 만든 PeriodicRunner들이 건너뛴 주기 수의 합이다.</summary>
    /// <remarks>
    /// PeriodicRunner 자체의 SkippedCount와 달리, 같은 JobRunner를 쓰는 여러 주기 실행자의
    /// 합이다. ServerHost가 L5 스냅숏에 넣기 위해 읽으며, 독립 실행자에서도 같은 규칙으로
    /// 쓸 수 있다.
    /// </remarks>
    [[nodiscard]] std::uint64_t PeriodicSkippedCount() const noexcept;

    /// <summary>
    /// 실행자 수명에 묶인 작업 투입 손잡이를 만든다.
    /// </summary>
    /// <remarks>
    /// 생성 시점에는 이 JobRunner가 살아 있어야 한다. 그 뒤에는 Lease가 큐 상태를 붙들고,
    /// JobRunner 소멸자가 정지를 요청한 뒤에는 Closed만 돌려준다. Lease에는 Stop이나
    /// RunUntilStopped가 없으므로, ServerHost가 소유한 실행자의 종료 순서를 호출자가 바꿀 수
    /// 없다.
    /// </remarks>
    [[nodiscard]] Lease AcquireLease() const noexcept;

private:
    std::shared_ptr<SharedState> mState;
};
}
