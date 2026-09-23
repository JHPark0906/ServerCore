#pragma once
#include "ServerCore/C/Types.h"
#include "ServerCore/C/Observability.h"
#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Net/ConnectionFlowControl.h"
#include "C/ReadinessInternal.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace ServerCore::CDetail {
inline sc_status Code(const Core::Status& status) noexcept { return static_cast<sc_status>(status.Code()); }
inline bool Valid(sc_bytes value) noexcept { return value.len == 0 || value.data != nullptr; }
inline std::string_view Text(sc_bytes value) noexcept {
    return value.len == 0 ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(value.data), value.len);
}
inline std::span<const std::byte> Bytes(sc_bytes value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data), value.len};
}
inline sc_bytes View(std::string_view value) noexcept {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
template<class T> bool Version(const T* value) noexcept {
    return value && value->abi_version == SC_ABI_VERSION && value->struct_size >= sizeof(T);
}
template<class Function> sc_status Protect(Function&& function) noexcept {
    try { return function(); } catch (...) { return SC_PLATFORM_ERROR; }
}
inline bool Add(size_t& total, size_t bytes) noexcept {
    if (bytes > (std::numeric_limits<size_t>::max)() - total) return false;
    total += bytes; return true;
}

// The lease follows the actual event/handle lifetime, including popped events.
struct Budget : std::enable_shared_from_this<Budget> {
    struct Lease {
        std::shared_ptr<Budget> owner;
        size_t bytes = 0;
        bool charged = false;
        ~Lease() { if (charged) { std::lock_guard lock(owner->mutex); --owner->count; owner->bytes -= bytes; } }
    };
    Budget(size_t countLimit, size_t byteLimit) : maxCount(countLimit), maxBytes(byteLimit) {}
    std::shared_ptr<Lease> Acquire(size_t size) {
        std::lock_guard lock(mutex);
        if (count >= maxCount || size > maxBytes - bytes) return {};
        auto lease = std::make_shared<Lease>();
        lease->owner = shared_from_this(); lease->bytes = size;
        ++count; bytes += size; lease->charged = true;
        return lease;
    }
    std::mutex mutex;
    const size_t maxCount, maxBytes;
    size_t count = 0, bytes = 0;
};

template<class T> class PullQueue {
public:
    ~PullQueue() { Close(); }
    bool Push(std::unique_ptr<T> item) {
        auto link = std::make_unique<Link>();
        link->item = std::move(item);
        {
            std::lock_guard lock(mMutex);
            if (mClosed) return false;
            auto* tail = link.get();
            if (mLast) mLast->next = std::move(link);
            else mFirst = std::move(link);
            mLast = tail;
        }
        mWake.notify_one(); mReadiness.Signal(); return true;
    }
    // Terminal storage is allocated at connection/request admission, never here.
    void Finish(std::unique_ptr<T> terminal) noexcept {
        { std::lock_guard lock(mMutex); if (mClosed) return; mClosed = true; mTerminal = std::move(terminal); }
        mWake.notify_all(); mReadiness.Signal();
    }
    void Close() noexcept {
        std::unique_ptr<Link> discarded;
        std::unique_ptr<T> terminal;
        { std::lock_guard lock(mMutex); mClosed = true; discarded = std::move(mFirst);
          mLast = nullptr; terminal = std::move(mTerminal); }
        mWake.notify_all(); mReadiness.Signal();
        // Item destruction may close a transport; never do it under mMutex.
        // Detach each next link before deletion to avoid recursive destruction.
        while (discarded) {
            auto next = std::move(discarded->next);
            discarded.reset();
            discarded = std::move(next);
        }
    }
    sc_status Subscribe(sc_notifier* notifier, uint64_t key, sc_subscription** out) {
        return CDetail::Subscribe(mReadiness, [&] {
            std::lock_guard guard(mMutex); return mFirst || mTerminal || mClosed;
        }, notifier, key, out);
    }
    sc_status Next(uint32_t timeout, T** out) {
        if (!out) return SC_INVALID_ARGUMENT;
        *out = nullptr;
        std::unique_lock lock(mMutex);
        const auto ready = [&] { return mFirst || mTerminal || mClosed; };
        if (!ready()) {
            if (timeout == 0) return SC_WOULD_BLOCK;
            if (timeout == UINT32_MAX) mWake.wait(lock, ready);
            else if (!mWake.wait_for(lock, std::chrono::milliseconds(timeout), ready)) return SC_TIMEOUT;
        }
        if (mFirst) {
            auto first = std::move(mFirst);
            mFirst = std::move(first->next);
            if (!mFirst) mLast = nullptr;
            *out = first->item.release();
            return SC_OK;
        }
        if (mTerminal) { *out = mTerminal.release(); return SC_OK; }
        return SC_CLOSED;
    }
private:
    struct Link { std::unique_ptr<T> item; std::unique_ptr<Link> next; };
    std::mutex mMutex;
    std::condition_variable mWake;
    ReadinessSource mReadiness;
    std::unique_ptr<Link> mFirst;
    Link* mLast = nullptr;
    std::unique_ptr<T> mTerminal;
    bool mClosed = false;
};

struct WaitState {
    std::mutex mutex;
    std::condition_variable wake;
    sc_status result = SC_WOULD_BLOCK;
    ReadinessSource readiness;
    void Complete(Core::Status status) noexcept {
        { std::lock_guard lock(mutex); result = Code(status); }
        wake.notify_all(); readiness.Signal();
    }
};
using RegisterWait = std::function<Core::Result<Net::SendCapacitySubscription>(std::function<void(Core::Status)>)>;
sc_status MakeWait(const RegisterWait& registerWait, sc_wait** out);
std::shared_ptr<Core::ILogger> LoggerInstance(const sc_logger* logger);
sc_status MakeOwnedText(std::string text, sc_owned_text** out);
}

struct sc_wait {
    std::shared_ptr<ServerCore::CDetail::WaitState> state;
    // Destroy/reset before state; Reset is a callback quiescence boundary.
    ServerCore::Net::SendCapacitySubscription subscription;
};
