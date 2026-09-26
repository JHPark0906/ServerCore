#pragma once

// ServerHost 구현 파일들이 함께 쓰는 내부 도구다. 공개 API가 아니며 src/Runtime 밖에서 include하지 않는다.
#include "ServerCore/Runtime/ServerHost.h"

#include "Core/AtomicBudgetInternal.h"

#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/Clock.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Net/Acceptor.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/IoContext.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/PeriodicRunner.h"

#include "Runtime/ParseWorkerPoolInternal.h"
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
#include "Runtime/ServerHostTestAccess.h"
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ServerCore::Runtime
{
/// <summary>원격 메시지마다 생길 수 있는 Warn 기록의 Host 전체 초당 상한을 적용한다.</summary>
/// <remarks>
/// 1초 고정 창마다 limit개까지 쓰고 나머지는 버리며 센다. 센 수는 다음 창의 첫 기록 앞, 세션 만료
/// 주기 검사, Stop에서 suppressed 필드를 단 한 줄로 알린다. 그래서 쓴 줄과 요약한 수의 합은 시도한
/// 기록 수와 같다. 이것은 Runtime.HostLimitsMessageFailureLogs가 고정한다.
/// </remarks>
class MessageLogLimiter final
{
public:
    explicit MessageLogLimiter(const std::uint32_t limit) noexcept
        : mLimit(limit)
    {
    }

    /// <summary>이번 기록을 써도 되면 true다. 새 창이 열리며 앞 창의 요약이 필요하면 먼저 쓴다.</summary>
    [[nodiscard]] bool Admit(Core::ILogger& logger) noexcept
    {
        if (mLimit == 0)
        {
            return true;
        }
        const std::uint64_t now = Core::MillisecondsSinceProcessStart();
        std::uint64_t report = 0;
        bool admitted = false;
        {
            const std::lock_guard guard(mMutex);
            if (!mStarted || now - mWindowStart >= 1000)
            {
                report = std::exchange(mSuppressed, 0);
                mWindowStart = now;
                mWritten = 0;
                mStarted = true;
            }
            if (mWritten < mLimit)
            {
                ++mWritten;
                admitted = true;
            }
            else
            {
                ++mSuppressed;
            }
        }
        WriteSummary(logger, report);
        return admitted;
    }

    /// <summary>창이 끝났거나 force면 아직 알리지 않은 버린 수를 한 줄로 알린다.</summary>
    void Flush(Core::ILogger& logger, const bool force) noexcept
    {
        std::uint64_t report = 0;
        {
            const std::lock_guard guard(mMutex);
            const std::uint64_t now = Core::MillisecondsSinceProcessStart();
            if (mSuppressed != 0 && (force || now - mWindowStart >= 1000))
            {
                report = std::exchange(mSuppressed, 0);
                mWindowStart = now;
                mWritten = 0;
            }
        }
        WriteSummary(logger, report);
    }

private:
    static void WriteSummary(Core::ILogger& logger, const std::uint64_t count) noexcept
    {
        if (count == 0)
        {
            return;
        }
        char text[24]{};
        const auto converted = std::to_chars(text, text + sizeof(text), count);
        const Core::LogField fields[]{ { "suppressed",
            std::string_view(text, static_cast<std::size_t>(converted.ptr - text)) } };
        (void)Core::WriteLog(
            logger, { Core::LogLevel::Warn, "ServerHost suppressed repeated per-message warnings",
                        fields, {} });
    }

    const std::uint32_t mLimit;
    std::mutex mMutex;
    std::uint64_t mWindowStart = 0;
    std::uint32_t mWritten = 0;
    std::uint64_t mSuppressed = 0;
    bool mStarted = false;
};

/// <summary>Dispatcher가 메시지마다 남기는 기록을 Host의 속도 제한에 태운다.</summary>
class RateLimitedLogger final : public Core::ILogger
{
public:
    RateLimitedLogger(
        std::shared_ptr<Core::ILogger> inner, std::shared_ptr<MessageLogLimiter> limiter) noexcept
        : mInner(std::move(inner))
        , mLimiter(std::move(limiter))
    {
    }

    void Write(const Core::LogLevel level, const std::string_view message) noexcept override
    {
        if (mLimiter->Admit(*mInner))
        {
            mInner->Write(level, message);
        }
    }

private:
    std::shared_ptr<Core::ILogger> mInner;
    std::shared_ptr<MessageLogLimiter> mLimiter;
};

class SendCapacityRelay;

/// <summary>현재 스레드가 쥔 NetworkSession 잠금 수와, 그 사이 미룬 송신 용량 완료의 대기열이다.</summary>
struct SessionLockThreadContext
{
    std::size_t depth = 0;
    std::shared_ptr<SendCapacityRelay> head;
    SendCapacityRelay* tail = nullptr;
};

extern thread_local SessionLockThreadContext gSessionLocks;

/// <summary>전송 계층의 송신 용량 완료를 Session 구독자에게 세션 잠금 밖에서 넘긴다.</summary>
/// <remarks>
/// Connection::Close·Send는 끝날 때 Host 공유 송신 예산의 모든 대기를 같은 스레드에서 완료시킬
/// 수 있다. ServerHost는 그 호출을 세션 잠금 아래에서 하므로, 완료를 그대로 넘기면 게임 callback이
/// 같은 세션의 잠금을 다시 잡다가 교착한다. 이 중계는 스레드가 세션 잠금을 쥐고 있으면 완료를
/// 스레드 지역 대기열에 넣고, 마지막 세션 잠금이 풀린 뒤 전달한다. 전달 대상은 Host가 만든 바깥
/// 구독이므로 구독자가 Reset한 뒤에는 늦은 전달이 callback을 부르지 않는다.
/// 이 경계는 Runtime.HostCapacityCallbackOutsideSessionLocks가 고정한다.
/// </remarks>
class SendCapacityRelay final
{
public:
    explicit SendCapacityRelay(Core::CompletionSource target) noexcept
        : mTarget(std::move(target))
    {
    }

    /// <summary>전송 계층 callback이다. Arm 전 완료는 보관했다가 Arm이 넘긴다.</summary>
    static void Deliver(
        const std::shared_ptr<SendCapacityRelay>& relay, const Core::ErrorCode code) noexcept
    {
        {
            const std::lock_guard guard(relay->mMutex);
            if (!relay->mArmed)
            {
                relay->mEarlyCode = code;
                return;
            }
        }
        Forward(relay, code);
    }

    /// <summary>등록과 취소 연결이 끝난 뒤 한 번 부른다. 등록 중 즉시 완료된 결과를 이때 넘긴다.</summary>
    static void Arm(const std::shared_ptr<SendCapacityRelay>& relay) noexcept
    {
        std::optional<Core::ErrorCode> early;
        {
            const std::lock_guard guard(relay->mMutex);
            relay->mArmed = true;
            early = relay->mEarlyCode;
        }
        if (early.has_value())
        {
            Forward(relay, *early);
        }
    }

    /// <summary>현재 스레드의 마지막 세션 잠금이 풀린 뒤 미룬 완료를 차례로 넘긴다.</summary>
    static void FlushDeferred() noexcept
    {
        SessionLockThreadContext& context = gSessionLocks;
        while (context.depth == 0 && context.head != nullptr)
        {
            const std::shared_ptr<SendCapacityRelay> relay = std::move(context.head);
            context.head = std::move(relay->mNext);
            if (context.head == nullptr)
            {
                context.tail = nullptr;
            }
            (void)relay->mTarget.Complete(relay->mDeferredCode);
        }
    }

private:
    static void Forward(
        const std::shared_ptr<SendCapacityRelay>& relay, const Core::ErrorCode code) noexcept
    {
        SessionLockThreadContext& context = gSessionLocks;
        if (context.depth == 0)
        {
            (void)relay->mTarget.Complete(code);
            return;
        }

        // 전송 계층은 한 대기를 한 번만 완료하므로 이 노드는 한 스레드의 대기열에 한 번만 들어간다.
        relay->mDeferredCode = code;
        if (context.tail == nullptr)
        {
            context.head = relay;
        }
        else
        {
            context.tail->mNext = relay;
        }
        context.tail = relay.get();
    }

    Core::CompletionSource mTarget;
    std::mutex mMutex;
    bool mArmed = false;
    std::optional<Core::ErrorCode> mEarlyCode;
    Core::ErrorCode mDeferredCode = Core::ErrorCode::Ok;
    std::shared_ptr<SendCapacityRelay> mNext;
};

/// <summary>전송 계층을 부를 수 있는 NetworkSession 잠금을 잡고, 스레드가 쥔 세션 잠금 수를 센다.</summary>
/// <remarks>마지막 세션 잠금을 놓은 직후 그 사이 미룬 송신 용량 완료를 전달한다.</remarks>
class SessionLockGuard final
{
public:
    explicit SessionLockGuard(std::mutex& mutex)
        : mMutex(mutex)
    {
        mMutex.lock();
        ++gSessionLocks.depth;
    }

    ~SessionLockGuard()
    {
        mMutex.unlock();
        if (--gSessionLocks.depth == 0)
        {
            SendCapacityRelay::FlushDeferred();
        }
    }

    SessionLockGuard(const SessionLockGuard&) = delete;
    SessionLockGuard& operator=(const SessionLockGuard&) = delete;

private:
    std::mutex& mMutex;
};

/// <summary>표준 예외를 PlatformError Status로 바꾼다. 설명을 담지 못하면 할당 실패다.</summary>
[[nodiscard]] Core::Status PlatformFailureFrom(const std::exception& failure) noexcept;

/// <summary>세션 만료 주기 검사의 간격이다.</summary>
[[nodiscard]] std::chrono::milliseconds SessionTimeoutScanPeriod(
    const ServerHostOptions& options) noexcept;

/// <summary>ServerHost가 JSON 본문 하나의 파싱에 적용하는 값 수 상한이다.</summary>
[[nodiscard]] Protocol::JsonParseLimits HostJsonParseLimits(
    const ServerHostOptions& options) noexcept;

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
// 시험 훅의 Host 쪽 진입점이다. 시험이 설치한 gate를 부르거나 표식을 소비한다.
[[nodiscard]] std::shared_ptr<TestAccess::IFailedStartOwnerGate> GetFailedStartOwnerGateForTest();
void WaitBeforeParseForTest();
void WaitBeforeSessionReceiveForTest();
void WaitBeforeConnectionStartForTest();
void WaitBeforeHostRunningForTest();
[[nodiscard]] bool ConsumeFinalizePostFailureForTest() noexcept;
#endif
}
