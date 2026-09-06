#include "ServerCore/Dispatch/Dispatcher.h"

#include "ServerCore/Core/Logging.h"

#include <exception>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>
#include <utility>

namespace ServerCore::Dispatch
{
namespace
{
[[nodiscard]] Core::Status UnknownTypeStatus(const std::string_view type)
{
    return Core::Status::Fail(Core::ErrorCode::UnknownType,
        "no handler is registered for message type: " + std::string(type));
}

void LogUnknownType(const std::string_view type)
{
    Core::GetGlobalLogger().Write(
        Core::LogLevel::Warn, "unregistered message type: " + std::string(type));
}

}

Core::Status Dispatcher::Register(
    const std::string_view type, MessageHandler handler, const std::size_t maximumRawBodySize)
{
    try
    {
        const std::unique_lock<std::shared_mutex> registrationWriteLock(mRegistrationMutex);
        if (mFrozen.load(std::memory_order_acquire))
        {
            return Core::Status::Fail(
                Core::ErrorCode::Closed, "the dispatcher registration table is frozen");
        }
        if (type.empty())
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "a message type must not be empty");
        }
        if (!handler)
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "a message handler must not be empty");
        }

        const std::string key(type);
        if (mHandlers.contains(key))
        {
            return Core::Status::Fail(Core::ErrorCode::AlreadyExists,
                "a handler is already registered for message type: " + std::string(type));
        }

        // The shared owner lets Dispatch drop its read lock before calling the handler without
        // leaving a map entry dangling after a concurrent registration rehash.
        const std::shared_ptr<const RegisteredHandler> registeredHandler =
            std::make_shared<RegisteredHandler>(
                RegisteredHandler{ std::move(handler), maximumRawBodySize });

        const auto [iterator, inserted] = mHandlers.emplace(key, registeredHandler);
        if (!inserted)
        {
            return Core::Status::Fail(Core::ErrorCode::AlreadyExists,
                "a handler is already registered for message type: " + std::string(type));
        }

        (void)iterator;
        return Core::Status::Ok();
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::exception&)
    {
        return Core::Status::AllocationFailure();
    }
}

void Dispatcher::Freeze() noexcept
{
    const std::unique_lock<std::shared_mutex> registrationWriteLock(mRegistrationMutex);
    mFrozen.store(true, std::memory_order_release);
}

bool Dispatcher::IsFrozen() const noexcept
{
    return mFrozen.load(std::memory_order_acquire);
}

Core::Status Dispatcher::Dispatch(
    const std::shared_ptr<Session::Session>& session, const Protocol::Message& message)
{
    std::shared_ptr<const RegisteredHandler> handler;

    try
    {
        if (!session)
        {
            return Core::Status::Fail(
                Core::ErrorCode::InvalidArgument, "a dispatch requires a session");
        }

        if (mFrozen.load(std::memory_order_acquire))
        {
            const auto found = std::as_const(mHandlers).find(message.Type());
            if (found != mHandlers.end())
            {
                handler = found->second;
            }
        }
        else
        {
            // While registration is open, hold the shared side only long enough to copy an owning
            // handler pointer.  Register and Freeze need the exclusive side, and the handler itself
            // runs after this lock has gone away.
            const std::shared_lock<std::shared_mutex> registrationReadLock(mRegistrationMutex);
            const auto found = std::as_const(mHandlers).find(message.Type());
            if (found != mHandlers.end())
            {
                handler = found->second;
            }
        }

        if (handler && message.Body() == nullptr)
        {
            return Core::Status::Fail(Core::ErrorCode::InvalidFormat,
                "a message without body cannot be routed to a body handler");
        }
        if (handler && message.RawBodySize() > handler->maximumRawBodySize)
        {
            return Core::Status::Fail(Core::ErrorCode::TooLarge,
                "message body exceeds the registered byte limit for type: " +
                    std::string(message.Type()));
        }
    }
    catch (const std::bad_alloc&)
    {
        return Core::Status::AllocationFailure();
    }
    catch (const std::exception&)
    {
        return Core::Status::AllocationFailure();
    }

    if (!handler)
    {
        const UnknownTypePolicy policy = GetUnknownTypePolicy();
        const auto failUnknownPreparation = [&session, policy]() -> Core::Status
        {
            // 엄격 정책은 로그나 상세 진단 문자열을 만들 메모리가 없어도 유지한다. 종료 사유는
            // 설명 문자열을 소유하지 않으므로 이 두 번째 경계에서 다시 할당하지 않는다.
            if (policy == UnknownTypePolicy::Disconnect)
            {
                session->Disconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::UnknownType));
            }
            return Core::Status::AllocationFailure();
        };

        Core::Status unknown = Core::Status::FailWithoutMessage(Core::ErrorCode::UnknownType);
        try
        {
            LogUnknownType(message.Type());
            if (policy == UnknownTypePolicy::LogAndIgnore)
            {
                return Core::Status::Ok();
            }
            unknown = UnknownTypeStatus(message.Type());
        }
        catch (const std::bad_alloc&)
        {
            return failUnknownPreparation();
        }
        catch (const std::exception&)
        {
            return failUnknownPreparation();
        }

        // 반환용 상세 Status와 연결에 넘길 사유를 분리한다. Session::Disconnect의 by-value
        // 경계에서 긴 설명을 복사하다 실패해 엄격 정책이 무시되는 일을 막는다. Disconnect의
        // no-throw 계약 위반은 아래 준비 예외 변환 경계 밖에서 실행 계층에 그대로 드러난다.
        session->Disconnect(Core::Status::FailWithoutMessage(Core::ErrorCode::UnknownType));
        return unknown;
    }

    // MessageHandler에는 예외 금지 계약이 있다. 처리기 호출을 위 준비 예외 변환 경계 밖에 두어,
    // 계약 위반을 라우팅 실패로 잘못 보고하지 않고 실행 계층에 그대로 드러낸다.
    return handler->handler(session, message);
}

void Dispatcher::SetUnknownTypePolicy(const UnknownTypePolicy policy) noexcept
{
    mUnknownTypePolicy.store(policy, std::memory_order_release);
}

UnknownTypePolicy Dispatcher::GetUnknownTypePolicy() const noexcept
{
    return mUnknownTypePolicy.load(std::memory_order_acquire);
}
}
