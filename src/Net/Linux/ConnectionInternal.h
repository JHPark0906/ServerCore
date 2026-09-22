#pragma once

#include "Net/SendBudgetInternal.h"
#include "Net/SendQueueInternal.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Net/IoContext.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ServerCore::Net
{
inline constexpr std::size_t ReceiveBufferSize = 16 * 1024;

class TcpConnection final : public Connection, public std::enable_shared_from_this<TcpConnection>
{
public:
    struct CreationKey {};
    // Ownership of descriptor transfers only after Create succeeds.
    [[nodiscard]] static std::shared_ptr<TcpConnection> Create(int descriptor,
        IoContext& context, std::shared_ptr<SendBudget> budget);
    TcpConnection(CreationKey, int descriptor, IoContext& context, std::shared_ptr<SendBudget> budget);
    ~TcpConnection() override;

    [[nodiscard]] Core::Status Start();
    Core::Status Send(std::span<const std::byte> bytes) override;
    [[nodiscard]] ConnectionSendOutcome SendWithOutcome(std::span<const std::byte> bytes) override;
    void Close() override;
    void CloseAfterSend() override;
    void SetObserver(std::weak_ptr<IConnectionObserver> observer) override;
    [[nodiscard]] bool IsOpen() const noexcept override;
    [[nodiscard]] std::size_t QueuedSendBytes() const noexcept override;

private:
    void OnReady(std::uint32_t events);
    [[nodiscard]] Core::Status RearmLocked() noexcept;
    void CloseWithFailureLocked(Core::Status& failure) noexcept;
    void CloseLocked(Core::Status reason, bool graceful = false) noexcept;
    void DiscardSendsLocked() noexcept;
    void NotifyDisconnected();

    mutable std::mutex mMutex;
    IoContext& mContext;
    int mDescriptor;
    std::uint64_t mRegistration = 0;
    std::weak_ptr<IConnectionObserver> mObserver;
    bool mObserverAssigned = false;
    bool mStarted = false;
    bool mProcessing = false;
    bool mCloseAfterSend = false;
    bool mDisconnectNotified = false;
    std::atomic<bool> mClosed{ false };
    Core::Status mCloseReason = Core::Status::Ok();
    SendQueue mSends;
};
}
