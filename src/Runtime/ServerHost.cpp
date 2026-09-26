#include "Runtime/ServerHostState.h"

namespace ServerCore::Runtime
{
Core::Status ServerHost::State::SetLogger(std::shared_ptr<Core::ILogger> logger)
{
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    // Start 뒤에는 I/O·runner 스레드가 로거를 읽으므로 바꾸지 않는다. 다른 부팅 설정과 같이 거절한다.
    if (mLifecycle != Lifecycle::Ready)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    mLogger = std::move(logger);
    return Core::Status::Ok();
}

Core::Status ServerHost::State::SetBinaryHandler(ServerHost::BinaryHandler handler)
{
    const std::lock_guard guard(mLifecycleMutex);
    if (mLifecycle != Lifecycle::Ready)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    if (!handler)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    mBinaryHandler = std::move(handler);
    return Core::Status::Ok();
}

Core::Status ServerHost::State::AttachDatagramTransport(
    std::shared_ptr<DatagramTransport> transport)
{
    const std::lock_guard guard(mLifecycleMutex);
    if (mLifecycle != Lifecycle::Ready)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    if (!transport || transport->Port() == 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    mDatagrams = std::move(transport);
    return Core::Status::Ok();
}

Core::Result<Protocol::DatagramCodec::Token> ServerHost::State::GetDatagramToken(
    Session::SessionId id) const
{
    if (!mJobRunner.IsCurrentThread())
        return Core::Result<Protocol::DatagramCodec::Token>::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument));
    if (!mDatagrams)
        return Core::Result<Protocol::DatagramCodec::Token>::FromStatus(
            Core::Status::FailWithoutMessage(Core::ErrorCode::NotFound));
    return mDatagrams->GetToken(id);
}

Dispatch::Dispatcher& ServerHost::State::GetDispatcher() noexcept
{
    return mDispatcher;
}

const Session::SessionRegistry& ServerHost::State::GetSessions() const noexcept
{
    return mRegistry;
}

JobRunner::Lease ServerHost::State::GetJobRunner() const noexcept
{
    return mJobRunner.AcquireLease();
}

Core::Status ServerHost::State::SetSessionObserver(
    std::weak_ptr<Session::ISessionObserver> observer)
{
    const std::lock_guard<std::mutex> guard(mLifecycleMutex);
    // Start 뒤에는 세션 통지가 이 값을 읽으므로 바꾸지 않는다. 다른 부팅 설정과 같이 거절한다.
    if (mLifecycle != Lifecycle::Ready)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    mSessionObserver = std::move(observer);
    return Core::Status::Ok();
}

ServerHost::ServerHost()
    : mState(std::make_shared<State>())
{
}

ServerHost::~ServerHost()
{
    Stop();
}

Core::Status ServerHost::Configure(const Core::Config& config)
{
    return mState->Configure(config);
}

Core::Status ServerHost::Configure(const ServerHostOptions& options)
{
    return mState->Configure(options);
}

Core::Status ServerHost::SetLogger(std::shared_ptr<Core::ILogger> logger)
{
    return mState->SetLogger(std::move(logger));
}

Core::Status ServerHost::SetBinaryHandler(BinaryHandler handler)
{
    return mState->SetBinaryHandler(std::move(handler));
}

Core::Status ServerHost::AttachDatagramTransport(std::shared_ptr<DatagramTransport> transport)
{
    return mState->AttachDatagramTransport(std::move(transport));
}

Core::Result<Protocol::DatagramCodec::Token> ServerHost::GetDatagramToken(
    Session::SessionId id) const
{
    return mState->GetDatagramToken(id);
}

Core::Status ServerHost::BeginDrain()
{
    return mState->BeginDrain();
}
Core::Status ServerHost::DrainStatus() const
{
    return mState->DrainStatus();
}
Core::Status ServerHost::StopGracefully(const std::chrono::steady_clock::time_point deadline)
{
    return mState->StopGracefully(deadline);
}

Dispatch::Dispatcher& ServerHost::GetDispatcher() noexcept
{
    return mState->GetDispatcher();
}

const Session::SessionRegistry& ServerHost::GetSessions() const noexcept
{
    return mState->GetSessions();
}

JobRunner::Lease ServerHost::GetJobRunner() const noexcept
{
    return mState->GetJobRunner();
}

Core::Status ServerHost::SetSessionObserver(std::weak_ptr<Session::ISessionObserver> observer)
{
    return mState->SetSessionObserver(std::move(observer));
}

Core::Status ServerHost::Start()
{
    return mState->Start();
}

void ServerHost::Stop()
{
    mState->Stop();
}

int ServerHost::Run()
{
    return mState->Run();
}

bool ServerHost::IsRunning() const noexcept
{
    return mState->IsRunning();
}

std::uint16_t ServerHost::Port() const noexcept
{
    return mState->Port();
}

Core::Result<ServerMetricsSnapshot> ServerHost::SnapshotMetrics() const
{
    return mState->SnapshotMetrics();
}
}
