#include "Net/DatagramReadinessInternal.h"
#ifdef _WIN32
#include "Net/WinsockInternal.h"
#else
#include "Net/Linux/PosixInternal.h"
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif
#include <condition_variable>
#include <mutex>
#include <utility>

namespace ServerCore::Net
{
using Core::ErrorCode;
using Core::Status;
struct DatagramReadiness::State
{
    std::mutex mutex;
    std::condition_variable idle;
    bool closed = false;
    std::size_t active = 0;
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
    WSAEVENT readable = WSA_INVALID_EVENT, interrupt = WSA_INVALID_EVENT;
    std::shared_ptr<WinsockScope> winsock;
    ~State()
    {
        if (readable != WSA_INVALID_EVENT)
            ::WSACloseEvent(readable);
        if (interrupt != WSA_INVALID_EVENT)
            ::WSACloseEvent(interrupt);
    }
#else
    int socket = -1, interrupt = -1;
    ~State()
    {
        if (interrupt >= 0)
            ::close(interrupt);
    }
#endif
    void Wake() noexcept
    {
#ifdef _WIN32
        if (interrupt != WSA_INVALID_EVENT)
            (void)::WSASetEvent(interrupt);
#else
        const std::uint64_t one = 1;
        if (interrupt >= 0)
        {
            ssize_t result;
            do
            {
                result = ::write(interrupt, &one, sizeof(one));
            } while (result < 0 && errno == EINTR);
        }
#endif
    }
};
DatagramReadiness::DatagramReadiness(std::unique_ptr<State> state) noexcept
    : mState(std::move(state))
{
}
DatagramReadiness::~DatagramReadiness()
{
    Close();
}
Core::Result<std::shared_ptr<DatagramReadiness>> DatagramReadiness::Create(std::uintptr_t socket)
{
    using Result = Core::Result<std::shared_ptr<DatagramReadiness>>;
    try
    {
        auto state = std::make_unique<State>();
#ifdef _WIN32
        auto acquired = AcquireWinsock();
        if (!acquired.IsOk())
            return Result::FromStatus(std::move(acquired).TakeStatus());
        state->winsock = std::move(acquired.Value());
        state->socket = static_cast<SOCKET>(socket);
        state->readable = ::WSACreateEvent();
        state->interrupt = ::WSACreateEvent();
        if (state->readable == WSA_INVALID_EVENT || state->interrupt == WSA_INVALID_EVENT ||
            ::WSAEventSelect(state->socket, state->readable, FD_READ | FD_CLOSE) == SOCKET_ERROR)
            return Result::FromStatus(MakeSocketFailure("UDP readiness", ::WSAGetLastError()));
#else
        state->socket = static_cast<int>(socket);
        state->interrupt = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (state->interrupt < 0)
            return Result::FromStatus(MakePosixFailure("UDP eventfd", errno));
#endif
        return Result::FromValue(
            std::shared_ptr<DatagramReadiness>(new DatagramReadiness(std::move(state))));
    }
    catch (...)
    {
        return Result::FromStatus(Status::AllocationFailure());
    }
}
Status DatagramReadiness::Wait() noexcept
{
    auto& state = *mState;
    {
        const std::lock_guard guard(state.mutex);
        if (state.closed)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        ++state.active;
    }
    struct Retire
    {
        State& state;
        ~Retire()
        {
            const std::lock_guard guard(state.mutex);
            --state.active;
            state.idle.notify_all();
        }
    } retire{ state };
    for (;;)
    {
#ifdef _WIN32
        // WSA events are edge notifications. Check the level first so a new
        // one-shot subscriber also observes a packet left unread by its caller.
        WSAPOLLFD probe{ state.socket, POLLRDNORM, 0 };
        const int available = ::WSAPoll(&probe, 1, 0);
        if (available == SOCKET_ERROR)
            return MakeSocketFailure("UDP WSAPoll", ::WSAGetLastError());
        if (available > 0)
            return Status::Ok();
        const WSAEVENT events[]{ state.interrupt, state.readable };
        const auto result = ::WSAWaitForMultipleEvents(2, events, FALSE, WSA_INFINITE, FALSE);
        if (result == WSA_WAIT_EVENT_0)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        if (result == WSA_WAIT_FAILED)
            return MakeSocketFailure("UDP WSAWait", ::WSAGetLastError());
        WSANETWORKEVENTS network{};
        if (::WSAEnumNetworkEvents(state.socket, state.readable, &network) == SOCKET_ERROR)
            return MakeSocketFailure("UDP network events", ::WSAGetLastError());
        if ((network.lNetworkEvents & (FD_READ | FD_CLOSE)) != 0)
            return Status::Ok();
#else
        pollfd descriptors[]{ { state.interrupt, POLLIN, 0 }, { state.socket, POLLIN, 0 } };
        const int result = ::poll(descriptors, 2, -1);
        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            return MakePosixFailure("UDP poll", errno);
        }
        if (descriptors[0].revents != 0)
            return Status::FailWithoutMessage(ErrorCode::Closed);
        if ((descriptors[1].revents & POLLNVAL) != 0)
            return Status::FailWithoutMessage(ErrorCode::PlatformError);
        if ((descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0)
            return Status::Ok();
#endif
    }
}
void DatagramReadiness::Close() noexcept
{
    std::unique_lock guard(mState->mutex);
    mState->closed = true;
    mState->Wake();
    mState->idle.wait(guard, [&] { return mState->active == 0; });
}
}
