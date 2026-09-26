// ServerHost의 시험 훅이다. 모든 정의는 SERVERCORE_ENABLE_TEST_HOOKS 아래에서만 컴파일된다.
#include "Runtime/ServerHostInternal.h"

namespace ServerCore::Runtime
{
namespace
{
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
struct BeforeParseGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeParseGate> gate;
};

struct BeforeSessionReceiveGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeSessionReceiveGate> gate;
};

struct BeforeConnectionStartGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeConnectionStartGate> gate;
};

struct BeforeHostRunningGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IBeforeHostRunningGate> gate;
};

struct FailedStartOwnerGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IFailedStartOwnerGate> gate;
};

[[nodiscard]] BeforeParseGateSlot& GetBeforeParseGateSlot()
{
    static BeforeParseGateSlot slot;
    return slot;
}

[[nodiscard]] BeforeSessionReceiveGateSlot& GetBeforeSessionReceiveGateSlot()
{
    static BeforeSessionReceiveGateSlot slot;
    return slot;
}

[[nodiscard]] BeforeConnectionStartGateSlot& GetBeforeConnectionStartGateSlot()
{
    static BeforeConnectionStartGateSlot slot;
    return slot;
}

[[nodiscard]] BeforeHostRunningGateSlot& GetBeforeHostRunningGateSlot()
{
    static BeforeHostRunningGateSlot slot;
    return slot;
}

[[nodiscard]] FailedStartOwnerGateSlot& GetFailedStartOwnerGateSlot()
{
    static FailedStartOwnerGateSlot slot;
    return slot;
}

[[nodiscard]] std::atomic<bool>& GetFinalizePostFailureFlag()
{
    static std::atomic<bool> failNext{ false };
    return failNext;
}
#endif
}

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
[[nodiscard]] std::shared_ptr<TestAccess::IFailedStartOwnerGate> GetFailedStartOwnerGateForTest()
{
    const std::lock_guard<std::mutex> guard(GetFailedStartOwnerGateSlot().mutex);
    return GetFailedStartOwnerGateSlot().gate.lock();
}

void WaitBeforeParseForTest()
{
    std::shared_ptr<TestAccess::IBeforeParseGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeParseGateSlot().mutex);
        gate = GetBeforeParseGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeParse();
    }
}

void WaitBeforeSessionReceiveForTest()
{
    std::shared_ptr<TestAccess::IBeforeSessionReceiveGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeSessionReceiveGateSlot().mutex);
        gate = GetBeforeSessionReceiveGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeSessionReceive();
    }
}

void WaitBeforeConnectionStartForTest()
{
    std::shared_ptr<TestAccess::IBeforeConnectionStartGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeConnectionStartGateSlot().mutex);
        gate = GetBeforeConnectionStartGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeConnectionStart();
    }
}

void WaitBeforeHostRunningForTest()
{
    std::shared_ptr<TestAccess::IBeforeHostRunningGate> gate;
    {
        const std::lock_guard<std::mutex> guard(GetBeforeHostRunningGateSlot().mutex);
        gate = GetBeforeHostRunningGateSlot().gate.lock();
    }

    if (gate != nullptr)
    {
        gate->WaitBeforeHostRunning();
    }
}

[[nodiscard]] bool ConsumeFinalizePostFailureForTest() noexcept
{
    return GetFinalizePostFailureFlag().exchange(false, std::memory_order_acq_rel);
}
#endif

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
void TestAccess::InstallBeforeParseGate(std::shared_ptr<IBeforeParseGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeParseGateSlot().mutex);
    GetBeforeParseGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeParseGate(const std::shared_ptr<IBeforeParseGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeParseGateSlot().mutex);
    if (GetBeforeParseGateSlot().gate.lock() == expected)
    {
        GetBeforeParseGateSlot().gate.reset();
    }
}

void TestAccess::InstallBeforeSessionReceiveGate(std::shared_ptr<IBeforeSessionReceiveGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeSessionReceiveGateSlot().mutex);
    GetBeforeSessionReceiveGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeSessionReceiveGate(
    const std::shared_ptr<IBeforeSessionReceiveGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeSessionReceiveGateSlot().mutex);
    if (GetBeforeSessionReceiveGateSlot().gate.lock() == expected)
    {
        GetBeforeSessionReceiveGateSlot().gate.reset();
    }
}

void TestAccess::InstallBeforeConnectionStartGate(std::shared_ptr<IBeforeConnectionStartGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeConnectionStartGateSlot().mutex);
    GetBeforeConnectionStartGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeConnectionStartGate(
    const std::shared_ptr<IBeforeConnectionStartGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeConnectionStartGateSlot().mutex);
    if (GetBeforeConnectionStartGateSlot().gate.lock() == expected)
    {
        GetBeforeConnectionStartGateSlot().gate.reset();
    }
}

void TestAccess::InstallBeforeHostRunningGate(std::shared_ptr<IBeforeHostRunningGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetBeforeHostRunningGateSlot().mutex);
    GetBeforeHostRunningGateSlot().gate = std::move(gate);
}

void TestAccess::ClearBeforeHostRunningGate(const std::shared_ptr<IBeforeHostRunningGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetBeforeHostRunningGateSlot().mutex);
    if (GetBeforeHostRunningGateSlot().gate.lock() == expected)
    {
        GetBeforeHostRunningGateSlot().gate.reset();
    }
}

void TestAccess::FailNextFinalizePost() noexcept
{
    GetFinalizePostFailureFlag().store(true, std::memory_order_release);
}

void TestAccess::InstallFailedStartOwnerGate(std::shared_ptr<IFailedStartOwnerGate> gate)
{
    const std::lock_guard<std::mutex> guard(GetFailedStartOwnerGateSlot().mutex);
    GetFailedStartOwnerGateSlot().gate = std::move(gate);
}

void TestAccess::ClearFailedStartOwnerGate(const std::shared_ptr<IFailedStartOwnerGate>& expected)
{
    const std::lock_guard<std::mutex> guard(GetFailedStartOwnerGateSlot().mutex);
    if (GetFailedStartOwnerGateSlot().gate.lock() == expected)
    {
        GetFailedStartOwnerGateSlot().gate.reset();
    }
}

void TestAccess::ClearFinalizePostFailure() noexcept
{
    GetFinalizePostFailureFlag().store(false, std::memory_order_release);
}
#endif
}
