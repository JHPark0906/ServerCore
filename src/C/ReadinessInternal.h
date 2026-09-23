#pragma once
#include "ServerCore/C/Types.h"
#include "ServerCore/Net/ConnectionFlowControl.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace ServerCore::CDetail {
struct NotifierState;
struct ReadinessSlot {
    std::weak_ptr<NotifierState> owner;
    uint64_t key = 0;
    // Both flags are protected by owner->mutex.
    bool registered = true;
    bool ready = false;
    void Signal() noexcept;
    void Reset() noexcept;
};
struct NotifierState : std::enable_shared_from_this<NotifierState> {
    explicit NotifierState(size_t capacity) : slots(capacity) {}
    sc_status Add(uint64_t key, std::shared_ptr<ReadinessSlot>& result) {
        if (!key) return SC_INVALID_ARGUMENT;
        auto slot = std::make_shared<ReadinessSlot>();
        slot->owner = weak_from_this(); slot->key = key;
        std::lock_guard guard(mutex);
        if (closed) return SC_CLOSED;
        for (const auto& current : slots)
            if (current && current->key == key) return SC_ALREADY_EXISTS;
        for (auto& current : slots) if (!current) {
            current = slot; result = std::move(slot); return SC_OK;
        }
        return SC_WOULD_BLOCK;
    }
    sc_status Next(uint32_t timeout, uint64_t* key) {
        if (!key) return SC_INVALID_ARGUMENT;
        *key = 0;
        std::unique_lock guard(mutex);
        const auto ready = [&] {
            return closed || interrupted || std::any_of(slots.begin(), slots.end(),
                [](const auto& slot) { return slot && slot->ready; });
        };
        if (!ready()) {
            if (!timeout) return SC_WOULD_BLOCK;
            if (timeout == UINT32_MAX) changed.wait(guard, ready);
            else if (!changed.wait_for(guard, std::chrono::milliseconds(timeout), ready)) return SC_TIMEOUT;
        }
        if (closed) return SC_CLOSED;
        // Round-robin delivery prevents a hot socket starving other keys.
        for (size_t n = 0; n < slots.size(); ++n) {
            const size_t index = (next + n) % slots.size();
            if (slots[index] && slots[index]->ready) {
                slots[index]->ready = false; *key = slots[index]->key;
                next = (index + 1) % slots.size(); return SC_OK;
            }
        }
        interrupted = false;
        return SC_WOULD_BLOCK;
    }
    void Interrupt(bool close = false) noexcept {
        { std::lock_guard guard(mutex); interrupted = true; closed = closed || close; }
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<ReadinessSlot>> slots;
    size_t next = 0;
    bool interrupted = false, closed = false;
};
inline void ReadinessSlot::Signal() noexcept {
    if (auto state = owner.lock()) {
        { std::lock_guard guard(state->mutex); if (!registered || state->closed) return; ready = true; }
        state->changed.notify_one();
    }
}
inline void ReadinessSlot::Reset() noexcept {
    if (auto state = owner.lock()) {
        std::lock_guard guard(state->mutex);
        registered = false;
        for (auto& slot : state->slots) if (slot.get() == this) { slot.reset(); break; }
    }
}
// Sources hold only weak slots. Signalling allocates nothing and only marks the
// native mailbox; no caller callback, destructor, or Rust Waker executes here.
class ReadinessSource {
public:
    bool Attach(const std::shared_ptr<ReadinessSlot>& slot) {
        std::lock_guard guard(mMutex);
        std::erase_if(mSlots, [](const auto& value) { return value.expired(); });
        if (mSlots.size() >= 4096) return false;
        mSlots.push_back(slot);
        return true;
    }
    void Signal() noexcept {
        std::lock_guard guard(mMutex);
        for (const auto& value : mSlots) if (auto slot = value.lock()) slot->Signal();
    }
private:
    std::mutex mMutex;
    std::vector<std::weak_ptr<ReadinessSlot>> mSlots;
};
struct ReadinessStop {
    std::shared_ptr<ReadinessSlot> slot;
    void operator()() const noexcept { slot->Signal(); }
};
}
struct sc_notifier { std::shared_ptr<ServerCore::CDetail::NotifierState> state; };
struct sc_subscription {
    std::shared_ptr<ServerCore::CDetail::ReadinessSlot> slot;
    ServerCore::Net::SendCapacitySubscription completion;
    std::unique_ptr<std::stop_callback<ServerCore::CDetail::ReadinessStop>> cancellation;
    ~sc_subscription() {
        completion.Reset(); cancellation.reset();
        if (slot) slot->Reset();
    }
};
namespace ServerCore::CDetail {
inline sc_status MakeSubscription(sc_notifier* notifier, uint64_t key,
    std::unique_ptr<sc_subscription>& result) {
    if (!notifier) return SC_INVALID_ARGUMENT;
    auto value = std::make_unique<sc_subscription>();
    auto status = notifier->state->Add(key, value->slot);
    if (status != SC_OK) return status;
    result = std::move(value); return SC_OK;
}
template<class Ready> sc_status Subscribe(ReadinessSource& source, Ready&& ready,
    sc_notifier* notifier, uint64_t key, sc_subscription** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<sc_subscription> result;
    auto status = MakeSubscription(notifier, key, result);
    if (status != SC_OK) return status;
    if (!source.Attach(result->slot)) return SC_WOULD_BLOCK;
    if (ready()) result->slot->Signal();
    *out = result.release(); return SC_OK;
}
inline sc_status SubscribeCancellation(std::stop_token token, sc_notifier* notifier,
    uint64_t key, sc_subscription** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<sc_subscription> result;
    auto status = MakeSubscription(notifier, key, result);
    if (status != SC_OK) return status;
    result->cancellation = std::make_unique<std::stop_callback<ReadinessStop>>(
        token, ReadinessStop{result->slot});
    *out = result.release(); return SC_OK;
}
template<class Register> sc_status SubscribeCompletion(Register&& callback,
    sc_notifier* notifier, uint64_t key, sc_subscription** out) {
    if (!out) return SC_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<sc_subscription> result;
    auto status = MakeSubscription(notifier, key, result);
    if (status != SC_OK) return status;
    auto registered = callback([slot = result->slot](Core::Status) { slot->Signal(); });
    if (!registered.IsOk()) return static_cast<sc_status>(registered.GetStatus().Code());
    result->completion = std::move(registered.Value());
    *out = result.release(); return SC_OK;
}
}
