#include "ServerCore/Session/SessionRegistry.h"

#include "ServerCore/Core/Assert.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ServerCore::Session
{
namespace
{
/// <summary>프로세스 안의 모든 SessionRegistry가 함께 쓰는 다음 번호다.</summary>
/// <remarks>
/// 레지스트리 하나가 없어졌다 다시 만들어져도 번호가 되돌아가면 "프로세스 수명 동안 재사용
/// 없음"이라는 SessionId 계약이 깨진다. 그래서 멤버가 아니라 파일 지역 전역 원자다.
/// 0은 이미 모두 발급했다는 종료 표식이며, 유효한 번호로 다시 쓰지 않는다.
/// </remarks>
std::atomic<std::uint64_t> gNextSessionId{ 1 };

[[nodiscard]] std::uint64_t ToValue(SessionId id) noexcept
{
    return static_cast<std::uint64_t>(id);
}

[[nodiscard]] Core::Status PlatformFailureFrom(const std::exception& failure) noexcept
{
    try
    {
        return Core::Status::Fail(Core::ErrorCode::PlatformError, failure.what());
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
}
}

/// <summary>SessionRegistry가 감추는 목록과 그 소유 문맥이다.</summary>
class SessionRegistry::State
{
public:
    mutable std::mutex sessionsMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;
    std::mutex issuedIdsMutex;
    std::unordered_set<std::uint64_t> issuedIds;
    std::thread::id mutationThread;
    bool isBound = false;
};

SessionRegistry::SessionRegistry()
    : mState(std::make_unique<State>())
{
}

SessionRegistry::~SessionRegistry() = default;

Core::Status SessionRegistry::BindToCurrentThread()
{
    const std::thread::id currentThread = std::this_thread::get_id();
    if (!mState->isBound)
    {
        mState->mutationThread = currentThread;
        mState->isBound = true;
        return Core::Status::Ok();
    }

    if (mState->mutationThread == currentThread)
    {
        return Core::Status::Ok();
    }

    return Core::Status::Fail(
        Core::ErrorCode::AlreadyExists, "SessionRegistry is already bound to a different thread");
}

Core::Result<SessionId> SessionRegistry::IssueId()
{
    std::uint64_t current = gNextSessionId.load(std::memory_order_relaxed);
    for (;;)
    {
        if (current == 0)
        {
            return Core::Result<SessionId>::FromStatus(Core::Status::Fail(
                Core::ErrorCode::TooLarge, "the process has exhausted the SessionId space"));
        }

        const std::uint64_t next =
            current == std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1;
        if (gNextSessionId.compare_exchange_weak(
                current, next, std::memory_order_relaxed, std::memory_order_relaxed))
        {
            try
            {
                // 이 아래의 예약 저장이 실패해도 전역 번호는 되돌리지 않는다. 발급 실패로
                // 생기는 빈 번호는 허용하지만, 이전 세션 번호가 다시 발급되는 일은 금지한다.
                const std::scoped_lock issuedIdsLock(mState->issuedIdsMutex);
                const auto [iterator, inserted] = mState->issuedIds.emplace(current);
                (void)iterator;
                SERVERCORE_ASSERT(
                    inserted, "a process-global SessionId was already reserved by this registry");
                return Core::Result<SessionId>::FromValue(static_cast<SessionId>(current));
            }
            catch (const std::bad_alloc&)
            {
                return Core::Result<SessionId>::FromStatus(Core::Status::AllocationFailure());
            }
            catch (const std::exception& failure)
            {
                return Core::Result<SessionId>::FromStatus(PlatformFailureFrom(failure));
            }
        }
    }
}

Core::Status SessionRegistry::DiscardIssuedId(SessionId id)
{
    if (!IsValid(id))
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "SessionRegistry::DiscardIssuedId() was given SessionId::Invalid");
    }

    const std::scoped_lock issuedIdsLock(mState->issuedIdsMutex);
    const auto issued = mState->issuedIds.find(ToValue(id));
    if (issued == mState->issuedIds.end())
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "SessionRegistry::DiscardIssuedId() was given a "
            "SessionId without a pending reservation");
    }

    mState->issuedIds.erase(issued);
    return Core::Status::Ok();
}

Core::Status SessionRegistry::Register(const std::shared_ptr<Session>& session)
{
    RequireMutationThread();

    if (session == nullptr)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "SessionRegistry::Register() was given a null session");
    }

    const SessionId id = session->Id();
    if (!IsValid(id))
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "SessionRegistry::Register() was given a session with SessionId::Invalid");
    }

    const std::scoped_lock sessionsLock(mState->sessionsMutex);
    const std::uint64_t value = ToValue(id);
    if (mState->sessions.find(value) != mState->sessions.end())
    {
        return Core::Status::Fail(Core::ErrorCode::AlreadyExists,
            "SessionRegistry::Register() was given a SessionId that is already registered");
    }

    const std::scoped_lock issuedIdsLock(mState->issuedIdsMutex);
    const auto issued = mState->issuedIds.find(value);
    if (issued == mState->issuedIds.end())
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "SessionRegistry::Register() was given a SessionId not issued by this registry");
    }

    try
    {
        const auto [iterator, inserted] = mState->sessions.emplace(value, session);
        if (!inserted)
        {
            return Core::Status::Fail(Core::ErrorCode::AlreadyExists,
                "SessionRegistry::Register() was given a SessionId that is already registered");
        }

        (void)iterator;
        mState->issuedIds.erase(issued);
        return Core::Status::Ok();
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::exception& failure)
    {
        return PlatformFailureFrom(failure);
    }
}

Core::Status SessionRegistry::Unregister(SessionId id)
{
    if (!IsValid(id))
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument,
            "SessionRegistry::Unregister() was given SessionId::Invalid");
    }

    std::shared_ptr<Session> removed;
    {
        const std::scoped_lock sessionsLock(mState->sessionsMutex);
        const auto iterator = mState->sessions.find(ToValue(id));
        if (iterator == mState->sessions.end())
        {
            return Core::Status::Fail(Core::ErrorCode::NotFound,
                "SessionRegistry::Unregister() could not find the SessionId");
        }

        // 마지막 shared_ptr은 목록 잠금을 놓은 뒤 사라져야 한다. 세션 소멸자가 다시 Host나
        // Registry 경계를 건드려도 잠금 재진입으로 바뀌지 않는다.
        removed = std::move(iterator->second);
        mState->sessions.erase(iterator);
    }

    (void)removed;
    return Core::Status::Ok();
}

std::shared_ptr<Session> SessionRegistry::Find(SessionId id) const
{
    RequireMutationThread();

    if (!IsValid(id))
    {
        return nullptr;
    }

    const std::scoped_lock sessionsLock(mState->sessionsMutex);
    const auto iterator = mState->sessions.find(ToValue(id));
    if (iterator == mState->sessions.end())
    {
        return nullptr;
    }

    return iterator->second;
}

void SessionRegistry::ForEach(
    const std::function<void(const std::shared_ptr<Session>&)>& visitor) const
{
    RequireMutationThread();
    SERVERCORE_ASSERT(
        static_cast<bool>(visitor), "SessionRegistry::ForEach() was given an empty visitor");

    // 방문자가 연결을 닫아 Registry에서 제거하더라도 이번 순회의 객체는 살아 있어야 한다.
    // 목록 잠금은 snapshot 복사에만 쓰고 사용자 코드는 모두 그 밖에서 실행한다.
    std::vector<std::shared_ptr<Session>> snapshot;
    {
        const std::scoped_lock sessionsLock(mState->sessionsMutex);
        snapshot.reserve(mState->sessions.size());
        for (const auto& [id, session] : mState->sessions)
        {
            (void)id;
            snapshot.push_back(session);
        }
    }

    for (const std::shared_ptr<Session>& session : snapshot)
    {
        visitor(session);
    }
}

std::size_t SessionRegistry::Count() const noexcept
{
    RequireMutationThread();
    const std::scoped_lock sessionsLock(mState->sessionsMutex);
    return mState->sessions.size();
}

void SessionRegistry::RequireMutationThread() const
{
    SERVERCORE_ASSERT(mState->isBound,
        "SessionRegistry must be bound before reading or changing its session list");
    SERVERCORE_ASSERT(mState->mutationThread == std::this_thread::get_id(),
        "SessionRegistry must be used from its bound serial execution thread");
}
}
