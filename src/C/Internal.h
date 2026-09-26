#pragma once
#include "C/ReadinessInternal.h"
#include "ServerCore/C/Observability.h"
#include "ServerCore/C/Types.h"
#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Net/ConnectionFlowControl.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace ServerCore::CDetail
{
inline sc_status Code(const Core::Status& status) noexcept
{
    return static_cast<sc_status>(status.Code());
}
inline bool Valid(sc_bytes value) noexcept
{
    return value.len == 0 || value.data != nullptr;
}
inline std::string_view Text(sc_bytes value) noexcept
{
    return value.len == 0 ? std::string_view{}
                          : std::string_view(reinterpret_cast<const char*>(value.data), value.len);
}
inline std::span<const std::byte> Bytes(sc_bytes value) noexcept
{
    return { reinterpret_cast<const std::byte*>(value.data), value.len };
}
inline sc_bytes View(std::string_view value) noexcept
{
    return { reinterpret_cast<const uint8_t*>(value.data()), value.size() };
}
/// <summary>입력 서술자로 받아들이는 struct_size의 상한. 뒤쪽을 읽는 범위를 이것으로 막는다.</summary>
inline constexpr uint32_t MaxDescriptorBytes = 4096;
/// <summary>
/// 입력 서술자를 검사한다. struct_size가 이 라이브러리의 sizeof(T)보다 크면 모르는 뒤쪽 바이트가
/// 모두 0이어야 한다. 새 헤더가 더한 옵션을 옛 라이브러리가 조용히 무시하는 대신 거절하기 위해서다.
/// CAbi.TcpOwnershipAndTerminal이 0인 뒤쪽·0이 아닌 뒤쪽·상한 초과를 확인한다.
/// </summary>
template <class T> bool Version(const T* value) noexcept
{
    if (!value || value->abi_version != SC_ABI_VERSION || value->struct_size < sizeof(T) ||
        value->struct_size > MaxDescriptorBytes)
        return false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(value);
    for (size_t index = sizeof(T); index < value->struct_size; ++index)
        if (bytes[index] != 0)
            return false;
    return true;
}
/// <summary>출력 구조체를 검사한다. 라이브러리는 sizeof(T)까지만 쓰고 뒤쪽은 읽지도 쓰지도 않는다.</summary>
template <class T> bool OutputVersion(const T* value) noexcept
{
    return value && value->abi_version == SC_ABI_VERSION && value->struct_size >= sizeof(T);
}
// 네이티브 ErrorCode를 sc_status로 그대로 옮기므로(Code) 두 번호가 같아야 한다.
static_assert(SC_OK == static_cast<int>(Core::ErrorCode::Ok) &&
              SC_INVALID_ARGUMENT == static_cast<int>(Core::ErrorCode::InvalidArgument) &&
              SC_INVALID_FORMAT == static_cast<int>(Core::ErrorCode::InvalidFormat) &&
              SC_TOO_LARGE == static_cast<int>(Core::ErrorCode::TooLarge) &&
              SC_NOT_FOUND == static_cast<int>(Core::ErrorCode::NotFound) &&
              SC_ALREADY_EXISTS == static_cast<int>(Core::ErrorCode::AlreadyExists) &&
              SC_CLOSED == static_cast<int>(Core::ErrorCode::Closed) &&
              SC_WOULD_BLOCK == static_cast<int>(Core::ErrorCode::WouldBlock) &&
              SC_PLATFORM_ERROR == static_cast<int>(Core::ErrorCode::PlatformError) &&
              SC_UNIMPLEMENTED == static_cast<int>(Core::ErrorCode::Unimplemented) &&
              SC_UNKNOWN_TYPE == static_cast<int>(Core::ErrorCode::UnknownType) &&
              SC_TIMEOUT == static_cast<int>(Core::ErrorCode::Timeout) &&
              SC_CANCELLED == static_cast<int>(Core::ErrorCode::Cancelled));
template <class Function> sc_status Protect(Function&& function) noexcept
{
    try
    {
        return function();
    }
    catch (...)
    {
        return SC_PLATFORM_ERROR;
    }
}
inline bool Add(size_t& total, size_t bytes) noexcept
{
    if (bytes > (std::numeric_limits<size_t>::max)() - total)
        return false;
    total += bytes;
    return true;
}

// The lease follows the actual event/handle lifetime, including popped events.
struct Budget : std::enable_shared_from_this<Budget>
{
    struct Lease
    {
        std::shared_ptr<Budget> owner;
        size_t bytes = 0;
        bool charged = false;
        // 몫을 돌려준 뒤 잠금 밖에서 released를 부른다. 그 콜백이 다시 이 몫을 읽을 수 있다.
        ~Lease()
        {
            if (!charged)
                return;
            {
                std::lock_guard lock(owner->mutex);
                --owner->count;
                owner->bytes -= bytes;
            }
            if (owner->released)
                owner->released();
        }
    };
    Budget(size_t countLimit, size_t byteLimit)
        : maxCount(countLimit)
        , maxBytes(byteLimit)
    {
    }
    std::shared_ptr<Lease> Acquire(size_t size)
    {
        std::lock_guard lock(mutex);
        if (count >= maxCount || size > maxBytes - bytes)
            return {};
        auto lease = std::make_shared<Lease>();
        lease->owner = shared_from_this();
        lease->bytes = size;
        ++count;
        bytes += size;
        lease->charged = true;
        return lease;
    }
    /// <summary>
    /// 한도와 무관하게 과금한다. 이미 받은 바이트처럼 거절할 수 없는 것을 세고, 한도는 Full로 보고
    /// 수신을 멈추는 데 쓴다(backpressure). 그래서 한도를 넘는 폭은 멈추기 전에 이미 진행 중이던 수신
    /// 하나로 묶인다.
    /// </summary>
    std::shared_ptr<Lease> Charge(size_t size)
    {
        auto lease = std::make_shared<Lease>();
        lease->owner = shared_from_this();
        lease->bytes = size;
        std::lock_guard lock(mutex);
        ++count;
        bytes += size;
        lease->charged = true;
        return lease;
    }
    bool Full()
    {
        std::lock_guard lock(mutex);
        return count >= maxCount || bytes >= maxBytes;
    }
    std::mutex mutex;
    const size_t maxCount, maxBytes;
    size_t count = 0, bytes = 0;
    /// <summary>임차가 몫을 돌려줄 때마다 부른다. 첫 과금 전에 한 번만 정한다. noexcept인 임차 소멸자
    /// 안에서 불리므로 여기서 던지면 std::terminate다.</summary>
    std::function<void()> released;
};

template <class T> class PullQueue
{
public:
    ~PullQueue() { Close(); }
    bool Push(std::unique_ptr<T> item)
    {
        auto link = std::make_unique<Link>();
        link->item = std::move(item);
        {
            std::lock_guard lock(mMutex);
            if (mClosed)
                return false;
            auto* tail = link.get();
            if (mLast)
                mLast->next = std::move(link);
            else
                mFirst = std::move(link);
            mLast = tail;
        }
        mWake.notify_one();
        mReadiness.Signal();
        return true;
    }
    // Terminal storage is allocated at connection/request admission, never here.
    void Finish(std::unique_ptr<T> terminal) noexcept
    {
        {
            std::lock_guard lock(mMutex);
            if (mClosed)
                return;
            mClosed = true;
            mTerminal = std::move(terminal);
        }
        mWake.notify_all();
        mReadiness.Signal();
    }
    void Close() noexcept
    {
        std::unique_ptr<Link> discarded;
        std::unique_ptr<T> terminal;
        {
            std::lock_guard lock(mMutex);
            mClosed = true;
            discarded = std::move(mFirst);
            mLast = nullptr;
            terminal = std::move(mTerminal);
        }
        mWake.notify_all();
        mReadiness.Signal();
        // Item destruction may close a transport; never do it under mMutex.
        // Detach each next link before deletion to avoid recursive destruction.
        while (discarded)
        {
            auto next = std::move(discarded->next);
            discarded.reset();
            discarded = std::move(next);
        }
    }
    sc_status Subscribe(sc_notifier* notifier, uint64_t key, sc_subscription** out)
    {
        return CDetail::Subscribe(
            mReadiness,
            [&]
            {
                std::lock_guard guard(mMutex);
                return mFirst || mTerminal || mClosed;
            },
            notifier, key, out);
    }
    sc_status Next(uint32_t timeout, T** out)
    {
        if (!out)
            return SC_INVALID_ARGUMENT;
        *out = nullptr;
        std::unique_lock lock(mMutex);
        const auto ready = [&] { return mFirst || mTerminal || mClosed; };
        if (!ready())
        {
            if (timeout == 0)
                return SC_WOULD_BLOCK;
            if (timeout == UINT32_MAX)
                mWake.wait(lock, ready);
            else if (!mWake.wait_for(lock, std::chrono::milliseconds(timeout), ready))
                return SC_TIMEOUT;
        }
        if (mFirst)
        {
            auto first = std::move(mFirst);
            mFirst = std::move(first->next);
            if (!mFirst)
                mLast = nullptr;
            *out = first->item.release();
            return SC_OK;
        }
        if (mTerminal)
        {
            *out = mTerminal.release();
            return SC_OK;
        }
        return SC_CLOSED;
    }

private:
    struct Link
    {
        std::unique_ptr<T> item;
        std::unique_ptr<Link> next;
    };
    std::mutex mMutex;
    std::condition_variable mWake;
    ReadinessSource mReadiness;
    std::unique_ptr<Link> mFirst;
    Link* mLast = nullptr;
    std::unique_ptr<T> mTerminal;
    bool mClosed = false;
};

struct WaitState
{
    std::mutex mutex;
    std::condition_variable wake;
    sc_status result = SC_WOULD_BLOCK;
    ReadinessSource readiness;
    void Complete(Core::Status status) noexcept
    {
        {
            std::lock_guard lock(mutex);
            result = Code(status);
        }
        wake.notify_all();
        readiness.Signal();
    }
};
using RegisterWait =
    std::function<Core::Result<Net::SendCapacitySubscription>(std::function<void(Core::Status)>)>;
sc_status MakeWait(const RegisterWait& registerWait, sc_wait** out);
std::shared_ptr<Core::ILogger> LoggerInstance(const sc_logger* logger);
sc_status MakeOwnedText(std::string text, sc_owned_text** out);
}

struct sc_wait
{
    std::shared_ptr<ServerCore::CDetail::WaitState> state;
    // Destroy/reset before state; Reset is a callback quiescence boundary.
    ServerCore::Net::SendCapacitySubscription subscription;
};
