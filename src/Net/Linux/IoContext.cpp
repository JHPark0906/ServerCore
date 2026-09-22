#include "ServerCore/Net/IoContext.h"

#include "Net/Linux/EpollInternal.h"
#include "Net/Linux/PosixInternal.h"
#include "ServerCore/Core/Assert.h"

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace ServerCore::Net
{
namespace
{
thread_local const IoContext* gCurrentIoContext = nullptr;
}

class IoContext::State
{
public:
    void Run(const IoContext* owner) noexcept;
    void ShutDown();

    std::mutex lifecycleMutex;
    std::mutex registrationsMutex;
    int pollDescriptor = -1;
    int wakeDescriptor = -1;
    std::atomic<bool> running{ false };
    std::atomic<bool> stopping{ false };
    std::uint64_t nextRegistration = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<IoContextAccess::Callback>> registrations;
    std::vector<std::thread> workers;
};

void IoContext::State::Run(const IoContext* const owner) noexcept
{
    gCurrentIoContext = owner;
    // Fetch one target per worker. Taking a batch of one-shot events would hold
    // the rest of that batch hostage if the first application's callback blocks.
    std::array<epoll_event, 1> events{};
    while (!stopping.load(std::memory_order_acquire))
    {
        const int count = ::epoll_wait(pollDescriptor, events.data(),
            static_cast<int>(events.size()), -1);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            Core::ReportAssertFailure("epoll_wait succeeded", __FILE__, __LINE__,
                "the Linux I/O event loop failed");
        }
        for (int index = 0; index < count; ++index)
        {
            if (stopping.load(std::memory_order_acquire)) break;
            const epoll_event& event = events[static_cast<std::size_t>(index)];
            // epoll_event may be packed. Copy its key before find() binds a
            // naturally aligned uint64_t reference to it.
            const std::uint64_t registration = event.data.u64;
            if (registration == 0) continue;
            std::shared_ptr<IoContextAccess::Callback> callback;
            {
                const std::lock_guard guard(registrationsMutex);
                const auto found = registrations.find(registration);
                if (found != registrations.end()) callback = found->second;
            }
            if (!callback) continue;
            try
            {
                (*callback)(event.events);
            }
            catch (...)
            {
                Core::ReportAssertFailure("I/O target did not throw", __FILE__, __LINE__,
                    "an I/O callback threw across the worker boundary");
            }
        }
    }
    gCurrentIoContext = nullptr;
}

void IoContext::State::ShutDown()
{
    if (pollDescriptor < 0) return;
    stopping.store(true, std::memory_order_release);
    // Leave the eventfd readable. Level-triggered readiness wakes every epoll waiter.
    const std::uint64_t value = 1;
    ssize_t written;
    do { written = ::write(wakeDescriptor, &value, sizeof(value)); } while (written < 0 && errno == EINTR);
    SERVERCORE_ASSERT(written == static_cast<ssize_t>(sizeof(value)) || errno == EAGAIN,
        "failed to wake Linux I/O workers");
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    workers.clear();
    std::unordered_map<std::uint64_t, std::shared_ptr<IoContextAccess::Callback>> detached;
    {
        const std::lock_guard guard(registrationsMutex);
        running.store(false, std::memory_order_release);
        ::close(pollDescriptor);
        ::close(wakeDescriptor);
        pollDescriptor = -1;
        wakeDescriptor = -1;
        detached.swap(registrations);
    }
    // Callback captures may own connections. Release outside the registry lock.
}

IoContext::IoContext() : mState(std::make_unique<State>()) {}
IoContext::~IoContext() { Stop(); }

Core::Status IoContext::Start(const int workerThreadCount)
{
    SERVERCORE_ASSERT(workerThreadCount > 0, "I/O worker count must be positive");
    const std::lock_guard guard(mState->lifecycleMutex);
    if (mState->pollDescriptor >= 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::AlreadyExists);
    mState->pollDescriptor = ::epoll_create1(EPOLL_CLOEXEC);
    if (mState->pollDescriptor < 0) return MakePosixFailure("epoll_create1", errno);
    mState->wakeDescriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (mState->wakeDescriptor < 0)
    {
        auto failure = MakePosixFailure("eventfd", errno);
        ::close(mState->pollDescriptor);
        mState->pollDescriptor = -1;
        return failure;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (::epoll_ctl(mState->pollDescriptor, EPOLL_CTL_ADD, mState->wakeDescriptor, &event) < 0)
    {
        auto failure = MakePosixFailure("epoll_ctl(wakeup)", errno);
        ::close(mState->wakeDescriptor);
        ::close(mState->pollDescriptor);
        mState->wakeDescriptor = -1;
        mState->pollDescriptor = -1;
        return failure;
    }
    mState->stopping.store(false, std::memory_order_release);
    mState->running.store(true, std::memory_order_release);
    try
    {
        mState->workers.reserve(static_cast<std::size_t>(workerThreadCount));
        for (int index = 0; index < workerThreadCount; ++index)
            mState->workers.emplace_back([this] { mState->Run(this); });
    }
    catch (...)
    {
        mState->ShutDown();
        return Core::Status::AllocationFailure();
    }
    return Core::Status::Ok();
}

void IoContext::Stop()
{
    SERVERCORE_ASSERT(!IsCurrentThreadIoThread(), "IoContext::Stop cannot join its own worker");
    const std::lock_guard guard(mState->lifecycleMutex);
    mState->ShutDown();
}

bool IoContext::IsRunning() const noexcept
{
    return mState->running.load(std::memory_order_acquire);
}

bool IoContext::IsCurrentThreadIoThread() const noexcept { return gCurrentIoContext == this; }

Core::Result<std::uint64_t> IoContextAccess::Register(IoContext& context, const int descriptor,
    const std::uint32_t events, Callback callback)
{
    using Result = Core::Result<std::uint64_t>;
    auto& state = *context.mState;
    try
    {
        auto owned = std::make_shared<Callback>(std::move(callback));
        const std::lock_guard guard(state.registrationsMutex);
        if (!state.running.load(std::memory_order_acquire) || state.stopping.load(std::memory_order_acquire))
            return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::Closed));
        // Never reuse an identity, including after Stop/Start. Stale epoll batches
        // cannot accidentally address a newly opened descriptor with the same integer.
        if (state.nextRegistration == (std::numeric_limits<std::uint64_t>::max)())
            return Result::FromStatus(Core::Status::FailWithoutMessage(Core::ErrorCode::TooLarge));
        const std::uint64_t id = state.nextRegistration++;
        state.registrations.emplace(id, std::move(owned));
        epoll_event event{};
        event.events = events | EPOLLONESHOT;
        event.data.u64 = id;
        if (::epoll_ctl(state.pollDescriptor, EPOLL_CTL_ADD, descriptor, &event) < 0)
        {
            const int error = errno;
            state.registrations.erase(id);
            return Result::FromStatus(MakePosixFailure("epoll_ctl(ADD)", error));
        }
        return Result::FromValue(id);
    }
    catch (...)
    {
        return Result::FromStatus(Core::Status::AllocationFailure());
    }
}

Core::Status IoContextAccess::Rearm(IoContext& context, const int descriptor,
    const std::uint64_t registration, const std::uint32_t events) noexcept
{
    auto& state = *context.mState;
    const std::lock_guard guard(state.registrationsMutex);
    if (state.pollDescriptor < 0 || state.stopping.load(std::memory_order_acquire) ||
        !state.registrations.contains(registration))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    epoll_event event{};
    event.events = events | EPOLLONESHOT;
    event.data.u64 = registration;
    if (::epoll_ctl(state.pollDescriptor, EPOLL_CTL_MOD, descriptor, &event) < 0)
        return MakePosixFailure("epoll_ctl(MOD)", errno);
    return Core::Status::Ok();
}

void IoContextAccess::Remove(IoContext& context, const int descriptor,
    const std::uint64_t registration) noexcept
{
    auto& state = *context.mState;
    std::shared_ptr<Callback> removed;
    {
        const std::lock_guard guard(state.registrationsMutex);
        const auto found = state.registrations.find(registration);
        if (found == state.registrations.end()) return;
        if (state.pollDescriptor >= 0)
            (void)::epoll_ctl(state.pollDescriptor, EPOLL_CTL_DEL, descriptor, nullptr);
        removed = std::move(found->second);
        state.registrations.erase(found);
    }
}
}
