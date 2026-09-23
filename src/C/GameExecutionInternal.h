#pragma once
#include "ServerCore/C/Net.h"
#include "ServerCore/C/Runtime.h"
#include "ServerCore/Net/Connection.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#include <memory>
namespace ServerCore::CDetail
{
struct TimerSchedulerLease
{
    std::shared_ptr<void> owner;
    Runtime::TimerScheduler* scheduler = nullptr;
};
TimerSchedulerLease RetainTimerScheduler(const sc_timer_scheduler*) noexcept;
void EnterRuntimeCallback() noexcept;
void LeaveRuntimeCallback() noexcept;
struct TcpConnectionLease
{
    std::shared_ptr<void> owner;
    std::shared_ptr<Net::Connection> connection;
};
TcpConnectionLease RetainTcpConnection(const sc_tcp_connection*) noexcept;
}
