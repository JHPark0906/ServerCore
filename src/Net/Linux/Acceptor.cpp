#include "ServerCore/Net/Acceptor.h"

#include "Net/AcceptorInternal.h"
#include "Net/EndpointInternal.h"
#include "Net/Linux/ConnectionInternal.h"
#include "Net/Linux/EpollInternal.h"
#include "Net/Linux/PosixInternal.h"
#include "Net/SendBudgetInternal.h"
#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/Logging.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace ServerCore::Net
{
namespace
{
// How long a listener rests after accept4 runs out of descriptors or memory.
// No measurement backs the value (undecided); it only bounds retry frequency.
constexpr long AcceptRetryDelayNanoseconds = 100L * 1000L * 1000L;

[[nodiscard]] bool IsAcceptResourceExhaustion(const int error) noexcept
{
    return error == EMFILE || error == ENFILE || error == ENOMEM || error == ENOBUFS;
}
}

class Acceptor::State
{
public:
    struct Shared
    {
        void OnReady(std::uint64_t expectedGeneration);
        void OnRetry(std::uint64_t expectedGeneration);
        void ParkListenerLocked(int error) noexcept;
        [[nodiscard]] bool ScheduleRetryLocked() noexcept;
        void CloseListenerLocked() noexcept;

        std::mutex mutex;
        std::condition_variable changed;
        int descriptor = -1;
        std::uint16_t port = 0;
        Core::IpEndpoint localEndpoint;
        IoContext* context = nullptr;
        // Shared epoll state; Stop after the IoContext is destroyed stays defined.
        IoContextAccess::EventSet events;
        std::uint64_t registration = 0;
        // A timerfd created at Start, before any exhaustion can prevent it.
        int retryTimer = -1;
        std::uint64_t retryRegistration = 0;
        // The listener is parked until the retry timer fires. Logged once.
        bool acceptBackoff = false;
        std::uint64_t generation = 0;
        bool stopping = false;
        std::size_t activeHandoffs = 0;
        SharedConnectionHandler handler;
        std::shared_ptr<Core::ILogger> logger;
        std::shared_ptr<SendBudget> budget =
            std::make_shared<SendBudget>(SendQueueLimits{}.totalBytes);
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        // While set, OnReady reports ENOBUFS instead of calling accept4.
        bool injectAcceptFailures = false;
        std::size_t injectedAcceptFailures = 0;
#endif
    };
    // An epoll batch may retain a callback after Stop returns. It owns this
    // detached state and checks generation/stopping before touching the listener.
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
};

void Acceptor::State::Shared::CloseListenerLocked() noexcept
{
    stopping = true;
    if (descriptor >= 0)
    {
        if (registration != 0)
        {
            IoContextAccess::Remove(events, descriptor, registration);
            registration = 0;
        }
        ::close(descriptor);
        descriptor = -1;
    }
    if (retryTimer >= 0)
    {
        if (retryRegistration != 0)
        {
            IoContextAccess::Remove(events, retryTimer, retryRegistration);
            retryRegistration = 0;
        }
        ::close(retryTimer);
        retryTimer = -1;
    }
    acceptBackoff = false;
    port = 0;
    localEndpoint = {};
}

void Acceptor::State::Shared::ParkListenerLocked(const int error) noexcept
{
    // A ready listener stays ready while accept cannot allocate a socket,
    // so rearming now would spin. Closing it instead let one burst of
    // connections disable accepting for good (NET-1). Park the listener
    // and let the retry timer rearm it; pending peers wait in the backlog.
    // Pinned by Transport.AcceptorRetriesAfterAcceptResourceFailure and, on
    // Linux, Transport.AcceptorSurvivesDescriptorExhaustion.
    if (!acceptBackoff && logger)
    {
        const auto failure = MakePosixFailure("accept4 resource exhaustion", error);
        logger->Write(Core::LogLevel::Warn, failure.Message());
    }
    acceptBackoff = true;
    if (!ScheduleRetryLocked())
    {
        if (logger)
            logger->Write(Core::LogLevel::Warn,
                "the accept retry timer could not be armed; the listener was closed");
        CloseListenerLocked();
    }
}

bool Acceptor::State::Shared::ScheduleRetryLocked() noexcept
{
    if (retryTimer < 0 || retryRegistration == 0)
        return false;
    itimerspec delay{};
    delay.it_value.tv_nsec = AcceptRetryDelayNanoseconds;
    if (::timerfd_settime(retryTimer, 0, &delay, nullptr) < 0)
        return false;
    return IoContextAccess::Rearm(events, retryTimer, retryRegistration, EPOLLIN).IsOk();
}

void Acceptor::State::Shared::OnRetry(const std::uint64_t expectedGeneration)
{
    const std::lock_guard guard(mutex);
    if (stopping || descriptor < 0 || generation != expectedGeneration)
        return;
    // Consume the expiration so the level-triggered timer reads idle again.
    // glibc marks read() warn_unused_result, which a (void) cast does not silence.
    std::uint64_t expirations = 0;
    const ssize_t consumed = ::read(retryTimer, &expirations, sizeof(expirations));
    (void)consumed;
    auto rearmed = IoContextAccess::Rearm(events, descriptor, registration, EPOLLIN);
    if (!rearmed.IsOk())
    {
        if (logger)
            logger->Write(Core::LogLevel::Warn, rearmed.Message());
        CloseListenerLocked();
    }
}

void Acceptor::State::Shared::OnReady(const std::uint64_t expectedGeneration)
{
    int accepted = -1;
    sockaddr_storage acceptedAddress{};
    SocketAddressLength acceptedLength = sizeof acceptedAddress;
    IoContext* owner = nullptr;
    {
        const std::lock_guard guard(mutex);
        if (stopping || descriptor < 0 || generation != expectedGeneration)
            return;
        // Rearm before the application handoff, allowing independent connections
        // to be accepted on other workers while one application's handler blocks.
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        if (injectAcceptFailures)
        {
            // The peer stays in the backlog, exactly as after a real ENOBUFS.
            ++injectedAcceptFailures;
            ParkListenerLocked(ENOBUFS);
            return;
        }
#endif
        do
        {
            acceptedLength = sizeof acceptedAddress;
            accepted = ::accept4(descriptor, reinterpret_cast<sockaddr*>(&acceptedAddress),
                &acceptedLength, SOCK_NONBLOCK | SOCK_CLOEXEC);
        } while (accepted < 0 && errno == EINTR);
        const int acceptError = accepted < 0 ? errno : 0;
        if (accepted < 0 && IsAcceptResourceExhaustion(acceptError))
        {
            ParkListenerLocked(acceptError);
            return;
        }
        if (accepted >= 0)
            acceptBackoff = false;
        auto rearmed = IoContextAccess::Rearm(events, descriptor, registration, EPOLLIN);
        if (!rearmed.IsOk())
        {
            if (logger)
                logger->Write(Core::LogLevel::Warn, rearmed.Message());
            CloseListenerLocked();
        }
        if (accepted < 0)
        {
            if (!IsWouldBlock(acceptError))
            {
                const auto failure = MakePosixFailure("accept4", acceptError);
                if (logger)
                    logger->Write(Core::LogLevel::Warn, failure.Message());
            }
            return;
        }
        owner = context;
        ++activeHandoffs;
    }

    std::shared_ptr<TcpConnection> connection;
    try
    {
        connection =
            TcpConnection::Create(accepted, *owner, budget, ReadSocketEndpoint(accepted, false),
                FromNativeEndpoint(acceptedAddress, acceptedLength));
        (*handler)(connection);
        const auto started = connection->Start();
        if (!started.IsOk())
            connection->Close();
    }
    catch (...)
    {
        if (logger)
            logger->Write(
                Core::LogLevel::Warn, "an accept handoff threw and the connection was closed");
        if (connection)
            connection->Close();
        else
            ::close(accepted);
    }
    {
        const std::lock_guard guard(mutex);
        SERVERCORE_ASSERT(activeHandoffs != 0, "accept handoff counter underflow");
        --activeHandoffs;
        changed.notify_all();
    }
}

Acceptor::Acceptor()
    : mState(std::make_unique<State>())
{
}
Acceptor::~Acceptor()
{
    Stop();
}

Core::Status Acceptor::Listen(
    const std::string_view address, const std::uint16_t port, const int backlog)
{
    const auto parsed = Core::IpEndpoint::Parse(address, port);
    return Listen(parsed.IsOk() ? parsed.Value() : Core::IpEndpoint{}, backlog);
}
Core::Status Acceptor::Listen(
    const Core::IpEndpoint& endpoint, const int backlog, const bool ipv6Only)
{
    SERVERCORE_ASSERT(backlog > 0, "listen backlog must be positive");
    auto& state = *mState->shared;
    const std::lock_guard guard(state.mutex);
    if (state.descriptor >= 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::AlreadyExists);
    if (endpoint.port == 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    NativeEndpoint address;
    if (!ToNativeEndpoint(endpoint, address))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    const int descriptor =
        ::socket(address.Family(), SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (descriptor < 0)
        return MakePosixFailure("socket(listen)", errno);
    // Linux SO_REUSEADDR permits restarting after TIME_WAIT, but without
    // SO_REUSEPORT it cannot share an actively listening endpoint.
    const int reuse = 1;
    if (!SetIpv6Only(descriptor, address.Family(), ipv6Only) ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        ::bind(descriptor, address.Address(), address.length) < 0 ||
        ::listen(descriptor, backlog) < 0)
    {
        auto failure = MakePosixFailure("listen endpoint", errno);
        ::close(descriptor);
        return failure;
    }
    state.descriptor = descriptor;
    state.port = endpoint.port;
    state.localEndpoint = ReadSocketEndpoint(descriptor, false);
    state.stopping = false;
    ++state.generation;
    return Core::Status::Ok();
}
Core::IpEndpoint Acceptor::LocalEndpoint() const noexcept
{
    const std::lock_guard guard(mState->shared->mutex);
    return mState->shared->localEndpoint;
}

void Acceptor::SetLogger(std::shared_ptr<Core::ILogger> logger)
{
    auto& state = *mState->shared;
    std::shared_ptr<Core::ILogger> previous;
    {
        const std::lock_guard guard(state.mutex);
        SERVERCORE_ASSERT(state.registration == 0 && state.activeHandoffs == 0,
            "Acceptor::SetLogger requires an inactive listener");
        previous = std::exchange(state.logger, std::move(logger));
    }
}
void Acceptor::SetConnectionHandler(std::function<void(std::shared_ptr<Connection>)> handler)
{
    SharedConnectionHandler registered =
        handler ? std::make_shared<const std::function<void(std::shared_ptr<Connection>)>>(
                      std::move(handler))
                : nullptr;
    auto& state = *mState->shared;
    const std::lock_guard guard(state.mutex);
    SERVERCORE_ASSERT(state.registration == 0 && state.activeHandoffs == 0,
        "the connection handler must be configured before accepting");
    state.handler.swap(registered);
}

Core::Status Acceptor::Start(IoContext& io)
{
    auto shared = mState->shared;
    const std::lock_guard guard(shared->mutex);
    SERVERCORE_ASSERT(shared->descriptor >= 0, "Listen must precede Start");
    SERVERCORE_ASSERT(shared->handler != nullptr, "a handler must precede Start");
    SERVERCORE_ASSERT(io.IsRunning(), "the I/O context must be running");
    if (shared->registration != 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::AlreadyExists);
    shared->context = &io;
    shared->events = IoContextAccess::Share(io);
    const auto generation = shared->generation;
    // The retry timer must exist before descriptors run out; it cannot be
    // created at the moment accept4 reports EMFILE.
    const int retryTimer = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (retryTimer < 0)
    {
        shared->context = nullptr;
        shared->events.reset();
        return MakePosixFailure("timerfd_create(accept retry)", errno);
    }
    const auto release = [&shared, retryTimer]() noexcept
    {
        if (shared->registration != 0)
        {
            IoContextAccess::Remove(shared->events, shared->descriptor, shared->registration);
            shared->registration = 0;
        }
        ::close(retryTimer);
        shared->context = nullptr;
        shared->events.reset();
    };
    try
    {
        auto registration = IoContextAccess::Register(shared->events, shared->descriptor, EPOLLIN,
            [shared, generation](std::uint32_t) { shared->OnReady(generation); });
        if (!registration.IsOk())
        {
            release();
            return std::move(registration).TakeStatus();
        }
        shared->registration = registration.Value();
        // Registered without interest: Register adds EPOLLONESHOT, and the timer
        // is armed only by ScheduleRetryLocked.
        auto retryRegistration = IoContextAccess::Register(shared->events, retryTimer, 0,
            [shared, generation](std::uint32_t) { shared->OnRetry(generation); });
        if (!retryRegistration.IsOk())
        {
            release();
            return std::move(retryRegistration).TakeStatus();
        }
        shared->retryTimer = retryTimer;
        shared->retryRegistration = retryRegistration.Value();
    }
    catch (...)
    {
        release();
        return Core::Status::AllocationFailure();
    }
    return Core::Status::Ok();
}

void Acceptor::Stop()
{
    auto shared = mState->shared;
    SharedConnectionHandler retiredHandler;
    std::unique_lock guard(shared->mutex);
    SERVERCORE_ASSERT(shared->context == nullptr || !shared->context->IsCurrentThreadIoThread(),
        "Acceptor::Stop cannot wait on its own I/O worker");
    shared->CloseListenerLocked();
    shared->changed.wait(guard, [&shared] { return shared->activeHandoffs == 0; });
    shared->context = nullptr;
    shared->events.reset();
    retiredHandler = std::move(shared->handler);
}

std::uint16_t Acceptor::Port() const noexcept
{
    const std::lock_guard guard(mState->shared->mutex);
    return mState->shared->port;
}

void AcceptorAccess::SetSendBudget(Acceptor& acceptor, std::shared_ptr<SendBudget> budget)
{
    auto& state = *acceptor.mState->shared;
    const std::lock_guard guard(state.mutex);
    SERVERCORE_ASSERT(state.registration == 0 && state.activeHandoffs == 0,
        "send budget must be configured before accepting");
    state.budget = std::move(budget);
}

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
void AcceptorAccess::SetAcceptFailureInjection(Acceptor& acceptor, const bool enabled)
{
    auto& state = *acceptor.mState->shared;
    const std::lock_guard guard(state.mutex);
    state.injectAcceptFailures = enabled;
}

std::size_t AcceptorAccess::InjectedAcceptFailureCount(const Acceptor& acceptor)
{
    auto& state = *acceptor.mState->shared;
    const std::lock_guard guard(state.mutex);
    return state.injectedAcceptFailures;
}
#endif

Core::Status Acceptor::SetSendQueueLimits(const SendQueueLimits& limits)
{
    if (!ValidSendQueueLimits(limits))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    auto& state = *mState->shared;
    const std::lock_guard guard(state.mutex);
    if (state.registration != 0 || state.activeHandoffs != 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    try
    {
        state.budget = std::make_shared<SendBudget>(limits.totalBytes, limits.connectionBytes);
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
    return Core::Status::Ok();
}
}
