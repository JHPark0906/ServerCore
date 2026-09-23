#pragma once
#include "ServerCore/Export.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#include "ServerCore/Session/Session.h"
#include <functional>
#include <optional>
#include <vector>

namespace ServerCore::Runtime
{
class OutboundPayload
{
public:
    enum class Kind
    {
        Raw,
        Prepared,
        Binary
    };
    SERVERCORE_API static Core::Result<std::shared_ptr<const OutboundPayload>> CopyBytes(
        std::span<const std::byte>, std::size_t maxBytes);
    SERVERCORE_API static Core::Result<std::shared_ptr<const OutboundPayload>> CopyBinary(
        std::uint32_t type, std::span<const std::byte>, std::size_t maxBytes);
    SERVERCORE_API static Core::Result<std::shared_ptr<const OutboundPayload>> CopyPrepared(
        const Protocol::PreparedMessage&, std::size_t maxBytes);
    Kind Type() const noexcept { return mKind; }
    SERVERCORE_API std::span<const std::byte> Bytes() const noexcept;
    std::uint32_t BinaryType() const noexcept { return mBinaryType; }
    const Protocol::PreparedMessage* Prepared() const noexcept { return mPrepared.get(); }
    SERVERCORE_API std::size_t WireBytes() const noexcept;

private:
    Kind mKind = Kind::Raw;
    std::uint32_t mBinaryType = 0;
    std::vector<std::byte> mBytes;
    std::unique_ptr<Protocol::PreparedMessage> mPrepared;
};
struct OutboundQueueOptions
{
    std::size_t maxMessages = 128, maxRetainedBytes = 4 * 1024 * 1024;
    std::size_t maxPumpMessages = 64;
};
struct OutboundEnqueueOptions
{
    std::optional<std::uint64_t> latestKey{};
    std::optional<std::chrono::steady_clock::time_point> expires{};
};
struct OutboundMetrics
{
    std::size_t pending = 0, retainedBytes = 0;
    std::uint64_t enqueued = 0, sent = 0, replaced = 0, expired = 0, discarded = 0;
};
struct OutboundSink
{
    std::function<Core::Status(const OutboundPayload&)> send;
    std::function<Core::Result<Core::CompletionSubscription>(
        std::size_t, std::function<void(Core::Status)>)>
        waitCapacity;
};
// Thread-safe staging BEFORE transport admission; one pump/one capacity wait.
// The sink must never accept any bytes when returning WouldBlock. Accepted
// transport bytes are removed here and can NEVER be replaced. A latest-key
// update preserves its queued position; replacing an in-flight attempt returns
// WouldBlock. Count and logical wire-byte budgets include in-flight admission.
// No delivery acknowledgement: success only accepts the staging item. Metrics
// report expiry/discards. Shared payloads are charged per recipient; application
// copies/owners and container overhead are outside logical-byte accounting.
// TimerScheduler drives explicit expiry deadlines and bounded pump continuation,
// not polling. It must remain usable until queue Close/drain and callbacks retire.
// Queue destruction cancels local work; it never closes the underlying transport.
class OutboundQueue
{
public:
    OutboundQueue(const OutboundQueue&) = delete;
    OutboundQueue& operator=(const OutboundQueue&) = delete;
    SERVERCORE_API static Core::Result<std::shared_ptr<OutboundQueue>> Create(
        TimerScheduler&, OutboundSink, const OutboundQueueOptions& = {});
    SERVERCORE_API static Core::Result<std::shared_ptr<OutboundQueue>> ForConnection(
        TimerScheduler&, std::shared_ptr<Net::Connection>, const OutboundQueueOptions& = {});
    SERVERCORE_API static Core::Result<std::shared_ptr<OutboundQueue>> ForSession(
        TimerScheduler&, std::shared_ptr<Session::Session>, const OutboundQueueOptions& = {});
    SERVERCORE_API ~OutboundQueue();
    SERVERCORE_API Core::Status Enqueue(
        std::shared_ptr<const OutboundPayload>, const OutboundEnqueueOptions& = {});
    SERVERCORE_API void BeginDrain() noexcept;
    SERVERCORE_API void Close() noexcept;
    SERVERCORE_API bool IsFinished() const noexcept;
    // WouldBlock while unfinished, or as a terminal resource-admission error.
    // IsFinished disambiguates; accepted completion observers fire in both cases.
    SERVERCORE_API Core::Status GetStatus() const noexcept;
    SERVERCORE_API OutboundMetrics Metrics() const noexcept;
    SERVERCORE_API Core::Result<Core::CompletionSubscription> WaitForCompletion(
        std::function<void(Core::Status)>, std::stop_token = {}) const;

private:
    class State;
    explicit OutboundQueue(std::shared_ptr<State> state)
        : mState(std::move(state))
    {
    }
    std::shared_ptr<State> mState;
};
// At most 4096 recipients per call, in supplied order; duplicates are deliberate.
// Each result describes staging admission. Earlier successes survive later errors.
SERVERCORE_API Core::Result<std::vector<Core::ErrorCode>> BatchEnqueue(
    std::span<const std::shared_ptr<OutboundQueue>> recipients,
    std::shared_ptr<const OutboundPayload>, const OutboundEnqueueOptions& = {});
}
