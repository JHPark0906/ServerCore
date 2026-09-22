#pragma once

#include <atomic>
#include <cstddef>

namespace ServerCore::Net
{
// Counts retained payload storage, including a partially sent vector's prefix.
// The runtime injects one budget into all connections belonging to a host.
class SendBudget
{
public:
    explicit SendBudget(std::size_t limitBytes) noexcept;
    [[nodiscard]] bool TryReserve(std::size_t byteCount) noexcept;
    void Release(std::size_t byteCount) noexcept;
    [[nodiscard]] std::size_t LimitBytes() const noexcept;
    [[nodiscard]] std::size_t UsedBytes() const noexcept;

private:
    std::size_t mLimitBytes;
    std::atomic<std::size_t> mUsedBytes{ 0 };
};
}
