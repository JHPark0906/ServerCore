#pragma once

#include "ServerCore/Export.h"

#include "ServerCore/Core/Error.h"

#include <concepts>
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <type_traits>
#include <utility>

namespace ServerCore::Core
{
/// <summary>
/// 여러 스레드가 넣고 한 스레드가 꺼내 실행하는 작업 큐다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 하나의 상태를 여러 스레드가 동시에 건드리지 않게 만드는 가장 값싼 방법이, 그 상태를 만지는
/// 일을 전부 한 줄로 세우는 것이다. 상태마다 잠금을 거는 대신 이 큐 하나를 쓴다.
///
/// 스레드 안전성:
/// - Post는 스레드 안전하다.
/// - DrainOnce는 호출자 직렬화다. 두 스레드가 동시에 부르면 안 된다.
///
/// 약속하지 않는 것:
/// - 작업이 언제 실행되는지 약속하지 않는다. DrainOnce를 부르는 쪽이 정한다.
/// - 작업이 던지면 계약 위반으로 프로세스를 끝낸다. 코어의 작업은 예외를 던지지 않기로 되어 있다.
/// - 큐가 비었다는 것과 실행이 끝났다는 것은 다른 말이다. 꺼낸 작업이 아직 돌고 있을 수 있다.
/// </remarks>
class JobQueue
{
public:
    /// <summary>작업의 소유권을 넘긴다. 이름 있는 Job은 std::move로 전달한다.</summary>
    using Job = std::move_only_function<void()>;

    /// <summary>작업을 큐에 넣는다.</summary>
    /// <returns>빈 작업은 InvalidArgument, 메모리를 확보하지 못하면 PlatformError다.</returns>
    SERVERCORE_API Status Post(Job job);
    // Preserve empty std::function rejection: wrapping an empty function inside
    // move_only_function directly would instead create a nonempty wrapper.
    template <class Callback>
        requires std::same_as<std::remove_cvref_t<Callback>, std::function<void()>>
    Status Post(Callback&& job)
    {
        return Post(FromLegacyJob(std::forward<Callback>(job)));
    }

    /// <summary>지금 큐에 있는 작업들을 부른 스레드에서 실행한다.</summary>
    /// <returns>실행한 작업 수.</returns>
    SERVERCORE_API std::size_t DrainOnce();

    [[nodiscard]] SERVERCORE_API std::size_t PendingCount() const;

private:
    static Job FromLegacyJob(std::function<void()> job)
    {
        return job ? Job(std::move(job)) : Job{};
    }

    /// <summary>넣는 쪽들만 잠근다. 실행 중에는 잠금을 잡지 않는다.</summary>
    mutable std::mutex mMutex;
    std::list<Job> mJobs;
};
}
