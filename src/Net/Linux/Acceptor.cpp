#include "ServerCore/Net/Acceptor.h"

#include "Net/AcceptorInternal.h"
#include "Net/Ipv4EndpointInternal.h"
#include "Net/Linux/ConnectionInternal.h"
#include "Net/Linux/EpollInternal.h"
#include "Net/Linux/PosixInternal.h"
#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/Logging.h"

#include <condition_variable>
#include <mutex>
#include <utility>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ServerCore::Net
{
class Acceptor::State
{
public:
    struct Shared
    {
        void OnReady(std::uint64_t expectedGeneration);
        void CloseListenerLocked() noexcept;

        std::mutex mutex;
        std::condition_variable changed;
        int descriptor = -1;
        std::uint16_t port = 0;
        IoContext* context = nullptr;
        std::uint64_t registration = 0;
        std::uint64_t generation = 0;
        bool stopping = false;
        std::size_t activeHandoffs = 0;
        SharedConnectionHandler handler;
        std::shared_ptr<SendBudget> budget;
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
            IoContextAccess::Remove(*context, descriptor, registration);
            registration = 0;
        }
        ::close(descriptor);
        descriptor = -1;
    }
    port = 0;
}

void Acceptor::State::Shared::OnReady(const std::uint64_t expectedGeneration)
{
    int accepted = -1;
    IoContext* owner = nullptr;
    {
        const std::lock_guard guard(mutex);
        if (stopping || descriptor < 0 || generation != expectedGeneration) return;
        // Rearm before the application handoff, allowing independent connections
        // to be accepted on other workers while one application's handler blocks.
        do { accepted = ::accept4(descriptor, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC); }
        while (accepted < 0 && errno == EINTR);
        const int acceptError = accepted < 0 ? errno : 0;
        if (accepted < 0 && (acceptError == EMFILE || acceptError == ENFILE ||
            acceptError == ENOMEM || acceptError == ENOBUFS))
        {
            // A ready listener stays ready while accept cannot allocate a socket.
            // Re-arming would spin and flood logs. Match an unrecoverable accept
            // failure by closing this listener; established connections survive.
            const auto failure = MakePosixFailure("accept4 resource exhaustion", acceptError);
            Core::GetGlobalLogger().Write(Core::LogLevel::Warn, failure.Message());
            CloseListenerLocked();
            return;
        }
        auto rearmed = IoContextAccess::Rearm(*context, descriptor, registration, EPOLLIN);
        if (!rearmed.IsOk())
        {
            Core::GetGlobalLogger().Write(Core::LogLevel::Warn, rearmed.Message());
            CloseListenerLocked();
        }
        if (accepted < 0)
        {
            if (!IsWouldBlock(acceptError))
            {
                const auto failure = MakePosixFailure("accept4", acceptError);
                Core::GetGlobalLogger().Write(Core::LogLevel::Warn, failure.Message());
            }
            return;
        }
        owner = context;
        ++activeHandoffs;
    }

    std::shared_ptr<TcpConnection> connection;
    try
    {
        connection = TcpConnection::Create(accepted, *owner, budget);
        (*handler)(connection);
        const auto started = connection->Start();
        if (!started.IsOk()) connection->Close();
    }
    catch (...)
    {
        Core::GetGlobalLogger().Write(Core::LogLevel::Warn,
            "an accept handoff threw and the connection was closed");
        if (connection) connection->Close();
        else ::close(accepted);
    }
    {
        const std::lock_guard guard(mutex);
        SERVERCORE_ASSERT(activeHandoffs != 0, "accept handoff counter underflow");
        --activeHandoffs;
        changed.notify_all();
    }
}

Acceptor::Acceptor() : mState(std::make_unique<State>()) {}
Acceptor::~Acceptor() { Stop(); }

Core::Status Acceptor::Listen(const std::string_view address, const std::uint16_t port, const int backlog)
{
    SERVERCORE_ASSERT(backlog > 0, "listen backlog must be positive");
    auto& state = *mState->shared;
    const std::lock_guard guard(state.mutex);
    if (state.descriptor >= 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::AlreadyExists);
    if (port == 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    sockaddr_in endpoint{};
    if (!TryParseIpv4Endpoint(address, port, endpoint))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (descriptor < 0) return MakePosixFailure("socket(listen)", errno);
    // Linux SO_REUSEADDR permits restarting after TIME_WAIT, but without
    // SO_REUSEPORT it cannot share an actively listening endpoint.
    const int reuse = 1;
    if (::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        ::bind(descriptor, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0 ||
        ::listen(descriptor, backlog) < 0)
    {
        auto failure = MakePosixFailure("listen endpoint", errno);
        ::close(descriptor);
        return failure;
    }
    state.descriptor = descriptor;
    state.port = port;
    state.stopping = false;
    ++state.generation;
    return Core::Status::Ok();
}

void Acceptor::SetConnectionHandler(std::function<void(std::shared_ptr<Connection>)> handler)
{
    SharedConnectionHandler registered = handler ?
        std::make_shared<const std::function<void(std::shared_ptr<Connection>)>>(std::move(handler)) : nullptr;
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
    const auto generation = shared->generation;
    try
    {
        auto registration = IoContextAccess::Register(io, shared->descriptor, EPOLLIN,
            [shared, generation](std::uint32_t) { shared->OnReady(generation); });
        if (!registration.IsOk())
        {
            shared->context = nullptr;
            return std::move(registration).TakeStatus();
        }
        shared->registration = registration.Value();
    }
    catch (...)
    {
        shared->context = nullptr;
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
}
