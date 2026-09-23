#pragma once
#include "ServerCore/Core/Error.h"
#include <cstdint>
#include <memory>

namespace ServerCore::Net
{
// Private binding lifetime. Close interrupts blocking waits and waits until no
// wait references the socket descriptor before DatagramSocket closes/reuses it.
class DatagramReadiness final
{
public:
    static Core::Result<std::shared_ptr<DatagramReadiness>> Create(std::uintptr_t socket);
    ~DatagramReadiness();
    Core::Status Wait() noexcept;
    void Close() noexcept;

private:
    struct State;
    explicit DatagramReadiness(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> mState;
};
}
