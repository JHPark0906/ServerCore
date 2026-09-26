#include "ServerCore/Runtime/DatagramTransport.h"

#include "Net/DatagramSocket.h"
#include "ServerCore/Runtime/TimerScheduler.h"
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
#include "Runtime/TransportTestAccess.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ServerCore::Runtime
{
namespace
{
namespace Codec = Protocol::DatagramCodec;
using Core::ErrorCode;
using Core::Result;
using Core::Status;
using Session::SessionId;

struct TokenHash
{
    std::size_t operator()(const Codec::Token& token) const noexcept
    {
        std::size_t value = static_cast<std::size_t>(1469598103934665603ull);
        for (const auto byte : token)
            value = (value ^ std::to_integer<unsigned>(byte)) *
                    static_cast<std::size_t>(1099511628211ull);
        return value;
    }
};

void ConsumeBytes(std::size_t& consumed, const std::size_t count) noexcept
{
    const auto remaining = std::numeric_limits<std::size_t>::max() - consumed;
    consumed += count < remaining ? count : remaining;
}

using Clock = std::chrono::steady_clock;
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
struct ReceivePumpGateSlot
{
    std::mutex mutex;
    std::weak_ptr<TestAccess::IReceivePumpGate> gate;
};
[[nodiscard]] ReceivePumpGateSlot& GetReceivePumpGateSlot()
{
    static ReceivePumpGateSlot slot;
    return slot;
}
[[nodiscard]] std::shared_ptr<TestAccess::IReceivePumpGate> ReceivePumpGateForTest()
{
    const std::lock_guard guard(GetReceivePumpGateSlot().mutex);
    return GetReceivePumpGateSlot().gate.lock();
}
#endif
bool ValidRate(const DatagramTransportOptions::SendRate& rate) noexcept
{
    if (!rate.bytesPerInterval && !rate.burstBytes)
        return true;
    return rate.bytesPerInterval && rate.bytesPerInterval <= (1ull << 40) &&
           rate.burstBytes >= Codec::MaximumDatagramBytes && rate.burstBytes <= (1ull << 40) &&
           rate.interval.count() > 0 && rate.interval <= std::chrono::hours(24);
}
struct SendBucket
{
    long double bytes = 0;
    Clock::time_point updated{};
    void Reset(const DatagramTransportOptions::SendRate& rate) noexcept
    {
        bytes = static_cast<long double>(rate.burstBytes);
        updated = Clock::now();
    }
    bool Available(const DatagramTransportOptions::SendRate& rate, std::size_t count,
        Clock::time_point now) noexcept
    {
        if (!rate.bytesPerInterval)
            return true;
        const auto elapsed = std::chrono::duration<long double, std::milli>(now - updated).count();
        bytes = (std::min)(static_cast<long double>(rate.burstBytes),
            bytes + elapsed * static_cast<long double>(rate.bytesPerInterval) /
                        static_cast<long double>(rate.interval.count()));
        updated = now;
        return bytes >= static_cast<long double>(count);
    }
    void Consume(const DatagramTransportOptions::SendRate& rate, std::size_t count) noexcept
    {
        if (rate.bytesPerInterval)
            bytes -= static_cast<long double>(count);
    }
};

class ReadableMonitor : public std::enable_shared_from_this<ReadableMonitor>
{
public:
    explicit ReadableMonitor(std::shared_ptr<Net::DatagramReadiness> value)
        : native(std::move(value))
    {
    }
    ~ReadableMonitor() { Stop(); }
    Result<Core::CompletionSubscription> Subscribe(
        std::function<void(Status)> callback, std::stop_token token)
    {
        auto value = Core::CompletionSubscription::Create(std::move(callback));
        if (!value.IsOk())
            return value;
        bool closed = false;
        try
        {
            const std::lock_guard guard(mutex);
            closed = stopping;
            if (!closed)
            {
                auto found = std::find_if(
                    sources.begin(), sources.end(), [](const auto& s) { return !s.IsPending(); });
                if (found == sources.end())
                    return Result<Core::CompletionSubscription>::FromStatus(
                        Status::FailWithoutMessage(ErrorCode::WouldBlock));
                *found = value.Value().GetSource();
                if (!started)
                {
                    const std::lock_guard joinGuard(joinMutex);
                    worker = std::thread([self = shared_from_this()] { self->Run(); });
                    started = true;
                }
            }
        }
        catch (...)
        {
            return Result<Core::CompletionSubscription>::FromStatus(Status::AllocationFailure());
        }
        if (closed)
            (void)value.Value().GetSource().Complete(ErrorCode::Closed);
        value.Value().BindCancellation(token);
        wake.notify_all();
        return value;
    }
    void Stop(const bool wait = true) noexcept
    {
        std::array<Core::CompletionSource, 16> retired;
        {
            const std::lock_guard guard(mutex);
            stopping = true;
            retired.swap(sources);
        }
        native->Close();
        wake.notify_all();
        for (const auto& source : retired)
            (void)source.Complete(ErrorCode::Closed);
        // A receive callback must not join a monitor whose user observer may
        // already be waiting for that receive callback to stop. Run retains
        // this monitor until its own thread can retire it safely.
        if (!wait)
            return;
        std::thread joined;
        {
            const std::lock_guard guard(joinMutex);
            if (worker.joinable())
            {
                if (worker.get_id() == std::this_thread::get_id())
                    worker.detach();
                else
                    joined = std::move(worker);
            }
        }
        if (joined.joinable())
            joined.join();
    }

private:
    void Run() noexcept
    {
        for (;;)
        {
            {
                std::unique_lock guard(mutex);
                wake.wait(guard,
                    [&]
                    {
                        return stopping || std::any_of(sources.begin(), sources.end(),
                                               [](const auto& s) { return s.IsPending(); });
                    });
                if (stopping)
                    return;
            }
            const auto status = native->Wait();
            std::array<Core::CompletionSource, 16> ready;
            ErrorCode result = status.Code();
            {
                const std::lock_guard guard(mutex);
                if (stopping)
                    result = ErrorCode::Closed;
                if (result != ErrorCode::Ok)
                    stopping = true;
                ready.swap(sources);
            }
            for (const auto& source : ready)
                (void)source.Complete(result);
            if (result != ErrorCode::Ok)
                return;
        }
    }
    std::shared_ptr<Net::DatagramReadiness> native;
    std::mutex mutex, joinMutex;
    std::condition_variable wake;
    std::thread worker;
    std::array<Core::CompletionSource, 16> sources;
    bool started = false, stopping = false;
};

class ReceivePump : public std::enable_shared_from_this<ReceivePump>
{
public:
    DatagramTransport* transport = nullptr;
    std::shared_ptr<TaskExecutor> executor;
    DatagramTransport::DatagramAdmission admission;
    DatagramTransport::DatagramReceiver receiver;
    DatagramReceiveOptions options;
    std::function<void(bool)> count;
    std::atomic<bool> stopping{ false };
    std::atomic<std::size_t> pending{ 0 };
    Status Start()
    {
        auto status = scheduler.Start(
            *executor, { 2, (std::max)(std::size_t{ 1 }, options.retainedCallbackBytes) });
        if (!status.IsOk())
            return status;
        parent = std::make_unique<std::stop_callback<std::function<void()>>>(options.cancellation,
            [weak = weak_from_this()]
            {
                if (auto self = weak.lock())
                    self->RequestStop();
            });
        Arm();
        return Status::Ok();
    }
    void RequestStop() noexcept
    {
        stopping.store(true);
        scheduler.RequestStop();
        Core::CompletionSubscription retired;
        {
            const std::lock_guard guard(mutex);
            ++generation;
            retired = std::move(readable);
        }
        retired.Reset();
    }
    Status Stop()
    {
        RequestStop();
        if (executor->IsCurrentThreadWorker())
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
        auto status = scheduler.Stop();
        if (status.IsOk())
        {
            terminal.Reset();
            parent.reset();
            pending.store(0);
        }
        return status;
    }

private:
    void Arm() noexcept
    {
        if (stopping.load())
            return;
        std::uint64_t current;
        {
            const std::lock_guard guard(mutex);
            current = ++generation;
        }
        try
        {
            auto result = transport->WaitForReadable(
                [weak = weak_from_this(), current](Status status)
                {
                    if (auto self = weak.lock())
                        self->Ready(current, status.Code());
                });
            if (!result.IsOk())
            {
                stopping.store(true);
                count(true);
                return;
            }
            // 이전 readable 구독은 그 콜백(Ready)이 시작하자마자 스스로 꺼내 간다. 그래서 완료 콜백에서
            // 불린 이 재무장이 교체하는 구독은 비어 있고, 모니터 스레드의 콜백을 기다리지 않는다(EXEC-2).
            Core::CompletionSubscription retired;
            {
                const std::lock_guard guard(mutex);
                if (current == generation && !stopping.load())
                {
                    retired = std::move(readable);
                    readable = std::move(result.Value());
                    readableGeneration = current;
                }
            }
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
            if (const auto gate = ReceivePumpGateForTest())
                gate->AfterRearmPublished();
#endif
        }
        catch (...)
        {
            stopping.store(true);
            count(true);
        }
    }
    void Ready(std::uint64_t current, ErrorCode code) noexcept
    {
        // 이 콜백을 부른 구독을 여기서 꺼내 이 스레드에서 정리한다. 자기 콜백 안의 Reset은 기다리지
        // 않으므로, 다른 스레드의 재무장이 이 콜백의 끝을 기다리며 서로 막히는 일이 없다. 재무장이
        // 인라인으로 Closed를 전달한 경우에는 readable이 아직 이전 구독이라 세대가 달라 꺼내지 않는다.
        Core::CompletionSubscription consumed;
        {
            const std::lock_guard guard(mutex);
            if (stopping.load() || current != generation)
                return;
            if (readableGeneration == current)
                consumed = std::move(readable);
        }
        if (code != ErrorCode::Ok)
        {
            stopping.store(true);
            return;
        }
        try
        {
            pending.store(1);
            TimerOptions timerOptions;
            timerOptions.retainedBytes = options.retainedCallbackBytes;
            auto scheduled = scheduler.Schedule(
                [self = shared_from_this()](std::stop_token token)
                {
                    if (!token.stop_requested() && !self->stopping.load())
                    {
                        self->transport->PollDatagrams(
                            self->admission, self->receiver, self->options.budget);
                        self->count(false);
                    }
                    return Status::Ok();
                },
                timerOptions);
            if (!scheduled.IsOk())
            {
                pending.store(0);
                stopping.store(true);
                count(true);
                return;
            }
            auto finished = scheduled.Value().WaitForCompletion(
                [weak = weak_from_this()](Status status)
                {
                    if (auto self = weak.lock())
                    {
                        self->pending.store(0);
                        if (!status.IsOk())
                        {
                            // 정지 요청이 먼저 있었다면 그 취소의 결과다. 아니면 배치가 실패해 수신이
                            // 멈추는 것이므로 pumpFailures로 드러낸다(EXEC-7).
                            if (!self->stopping.exchange(true))
                                self->count(true);
                            return;
                        }
                        self->Arm();
                    }
                });
            if (!finished.IsOk())
            {
                (void)scheduled.Value().RequestCancel();
                pending.store(0);
                stopping.store(true);
                count(true);
                return;
            }
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
            if (const auto gate = ReceivePumpGateForTest())
                gate->AfterCompletionSubscribed();
#endif
            // Completion can already have rearmed/readied the next generation.
            Core::CompletionSubscription retired;
            {
                const std::lock_guard guard(mutex);
                if (current == generation && !stopping.load())
                {
                    retired = std::move(terminal);
                    terminal = std::move(finished.Value());
                }
            }
        }
        catch (...)
        {
            pending.store(0);
            stopping.store(true);
            count(true);
        }
    }
    TimerScheduler scheduler;
    std::mutex mutex;
    std::uint64_t generation = 0, readableGeneration = 0;
    Core::CompletionSubscription readable, terminal;
    std::unique_ptr<std::stop_callback<std::function<void()>>> parent;
};
}

struct DatagramTransport::Impl
{
    struct Peer
    {
        Codec::Token token{};
        Core::IpEndpoint endpoint{};
        std::uint64_t registrationGeneration = 0;
        std::uint64_t receivedSequence = 0;
        std::uint64_t sentSequence = 0;
        bool ready = false;
        SendBucket sendBucket;
    };

    mutable std::mutex mutex;
    std::mutex pumpLifecycle;
    std::atomic_flag polling = ATOMIC_FLAG_INIT;
    Net::DatagramSocket socket;
    std::uint64_t bindingGeneration = 0;
    std::uint64_t registrationGeneration = 0;
    std::unordered_map<SessionId, Peer> peers;
    std::unordered_map<Codec::Token, SessionId, TokenHash> tokens;
    Metrics metrics;
    DatagramTransportOptions options;
    SendBucket sendBucket;
    std::shared_ptr<ReadableMonitor> readable;
    std::shared_ptr<ReceivePump> pump;
};

DatagramTransport::DatagramTransport()
    : mImpl(std::make_unique<Impl>())
{
}
DatagramTransport::~DatagramTransport()
{
    Close();
}

Status DatagramTransport::Configure(const DatagramTransportOptions& options)
{
    if (options.maxRegisteredSessions == 0 || options.maxSequenceJump == 0 ||
        (options.payloadMode != Protocol::PayloadMode::Json &&
            options.payloadMode != Protocol::PayloadMode::Binary) ||
        !ValidRate(options.totalSendRate) || !ValidRate(options.peerSendRate))
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    if (options.maxRegisteredSessions > 65536)
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    const std::lock_guard guard(mImpl->mutex);
    if (mImpl->bindingGeneration != 0)
        return Status::FailWithoutMessage(ErrorCode::Closed);
    mImpl->options = options;
    return Status::Ok();
}

Status DatagramTransport::Bind(const std::string_view address, const std::uint16_t port)
{
    const auto parsed = Core::IpEndpoint::Parse(address, port);
    return Bind(parsed.IsOk() ? parsed.Value() : Core::IpEndpoint{});
}
Status DatagramTransport::Bind(const Core::IpEndpoint& endpoint, const bool ipv6Only)
{
    const std::lock_guard guard(mImpl->mutex);
    if (mImpl->socket.IsOpen())
        return Status::FailWithoutMessage(ErrorCode::AlreadyExists);
    if (mImpl->bindingGeneration == std::numeric_limits<std::uint64_t>::max())
        return Status::FailWithoutMessage(ErrorCode::Closed);
    auto status = mImpl->socket.Bind(endpoint, ipv6Only);
    if (status.IsOk())
    {
        try
        {
            mImpl->readable = std::make_shared<ReadableMonitor>(mImpl->socket.Readiness());
        }
        catch (...)
        {
            mImpl->socket.Close();
            return Status::AllocationFailure();
        }
        mImpl->sendBucket.Reset(mImpl->options.totalSendRate);
        ++mImpl->bindingGeneration;
    }
    return status;
}
Core::IpEndpoint DatagramTransport::LocalEndpoint() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    return mImpl->socket.LocalEndpoint();
}
Result<Core::IpEndpoint> DatagramTransport::RemoteEndpoint(SessionId id) const
{
    const std::lock_guard guard(mImpl->mutex);
    const auto found = mImpl->peers.find(id);
    if (found == mImpl->peers.end())
        return Result<Core::IpEndpoint>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::NotFound));
    if (!found->second.ready)
        return Result<Core::IpEndpoint>::FromStatus(
            Status::FailWithoutMessage(ErrorCode::WouldBlock));
    return Result<Core::IpEndpoint>::FromValue(found->second.endpoint);
}

void DatagramTransport::Close() noexcept
{
    bool receiveContext = false;
    {
        const std::lock_guard guard(mImpl->mutex);
        receiveContext = mImpl->pump && mImpl->pump->executor->IsCurrentThreadWorker();
    }
    try
    {
        (void)StopReceiving();
    }
    catch (...)
    {
        RequestStopReceiving();
    }
    std::shared_ptr<ReadableMonitor> readable;
    {
        const std::lock_guard guard(mImpl->mutex);
        mImpl->socket.Close();
        readable = std::move(mImpl->readable);
        mImpl->tokens.clear();
        mImpl->peers.clear();
    }
    if (readable)
        readable->Stop(!receiveContext);
}

Result<Core::CompletionSubscription> DatagramTransport::WaitForReadable(
    std::function<void(Status)> callback, std::stop_token cancellation)
{
    std::shared_ptr<ReadableMonitor> readable;
    {
        const std::lock_guard guard(mImpl->mutex);
        readable = mImpl->readable;
    }
    if (readable)
        return readable->Subscribe(std::move(callback), cancellation);
    auto result = Core::CompletionSubscription::Create(std::move(callback));
    if (result.IsOk())
    {
        (void)result.Value().GetSource().Complete(ErrorCode::Closed);
        result.Value().BindCancellation(cancellation);
    }
    return result;
}

Status DatagramTransport::StartReceiving(std::shared_ptr<TaskExecutor> executor,
    DatagramAdmission admission, DatagramReceiver receiver, const DatagramReceiveOptions& options)
{
    if (!executor || !admission || !receiver || !options.budget.maximumDatagrams ||
        options.budget.maximumDatagrams > 65536 || !options.budget.maximumBytes ||
        options.budget.maximumBytes > 64 * 1024 * 1024 ||
        options.retainedCallbackBytes > 1024 * 1024 * 1024)
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    // Prepare and retire arbitrary user captures outside both owner locks,
    // including allocation failures and concurrent duplicate Start calls.
    std::shared_ptr<ReceivePump> pump;
    std::unique_lock lifecycle(mImpl->pumpLifecycle, std::defer_lock);
    bool published = false;
    try
    {
        pump = std::make_shared<ReceivePump>();
        pump->transport = this;
        pump->executor = std::move(executor);
        pump->admission = std::move(admission);
        pump->receiver = std::move(receiver);
        pump->options = options;
        pump->count = [this](bool failure)
        {
            const std::lock_guard guard(mImpl->mutex);
            if (failure)
                ++mImpl->metrics.pumpFailures;
            else
                ++mImpl->metrics.pumpBatches;
        };
        lifecycle.lock();
        {
            const std::lock_guard guard(mImpl->mutex);
            if (!mImpl->socket.IsOpen())
                return Status::FailWithoutMessage(ErrorCode::Closed);
            if (mImpl->pump)
                return Status::FailWithoutMessage(ErrorCode::AlreadyExists);
            mImpl->pump = pump;
            published = true;
        }
        auto status = pump->Start();
        if (!status.IsOk())
        {
            (void)pump->Stop();
            {
                const std::lock_guard guard(mImpl->mutex);
                mImpl->pump.reset();
            }
            lifecycle.unlock();
        }
        return status;
    }
    catch (...)
    {
        if (published)
        {
            const std::lock_guard guard(mImpl->mutex);
            mImpl->pump.reset();
        }
        if (published)
            (void)pump->Stop();
        if (lifecycle.owns_lock())
            lifecycle.unlock();
        return Status::AllocationFailure();
    }
}
void DatagramTransport::RequestStopReceiving() noexcept
{
    std::shared_ptr<ReceivePump> pump;
    {
        const std::lock_guard guard(mImpl->mutex);
        pump = mImpl->pump;
    }
    if (pump)
        pump->RequestStop();
}
Status DatagramTransport::StopReceiving()
{
    std::shared_ptr<ReceivePump> pump;
    {
        const std::lock_guard guard(mImpl->mutex);
        pump = mImpl->pump;
    }
    // Never wait for lifecycle while a control thread holds it and waits for us.
    if (pump && pump->executor->IsCurrentThreadWorker())
    {
        pump->RequestStop();
        return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    }
    const std::lock_guard lifecycle(mImpl->pumpLifecycle);
    {
        const std::lock_guard guard(mImpl->mutex);
        pump = mImpl->pump;
    }
    if (!pump)
        return Status::Ok();
    auto status = pump->Stop();
    if (status.IsOk())
    {
        const std::lock_guard guard(mImpl->mutex);
        mImpl->pump.reset();
    }
    return status;
}

std::uint16_t DatagramTransport::Port() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    return mImpl->socket.Port();
}

Result<Codec::Token> DatagramTransport::RegisterSession(const SessionId id)
{
    try
    {
        const std::lock_guard guard(mImpl->mutex);
        if (!mImpl->socket.IsOpen())
            return Result<Codec::Token>::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
        if (!Session::IsValid(id) || mImpl->peers.contains(id))
            return Result<Codec::Token>::FromStatus(
                Status::FailWithoutMessage(ErrorCode::InvalidArgument));
        if (mImpl->peers.size() >= mImpl->options.maxRegisteredSessions)
            return Result<Codec::Token>::FromStatus(
                Status::FailWithoutMessage(ErrorCode::WouldBlock));
        if (mImpl->registrationGeneration == std::numeric_limits<std::uint64_t>::max())
            return Result<Codec::Token>::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));

        Impl::Peer peer;
        peer.sendBucket.Reset(mImpl->options.peerSendRate);
        do
        {
            auto status = Net::GenerateDatagramSecret(peer.token);
            if (!status.IsOk())
                return Result<Codec::Token>::FromStatus(std::move(status));
        } while (mImpl->tokens.contains(peer.token));
        peer.registrationGeneration = mImpl->registrationGeneration + 1;

        mImpl->peers.emplace(id, peer);
        try
        {
            mImpl->tokens.emplace(peer.token, id);
        }
        catch (...)
        {
            mImpl->peers.erase(id);
            throw;
        }
        mImpl->registrationGeneration = peer.registrationGeneration;
        return Result<Codec::Token>::FromValue(peer.token);
    }
    catch (const std::bad_alloc&)
    {
        return Result<Codec::Token>::FromStatus(Status::AllocationFailure());
    }
}

void DatagramTransport::UnregisterSession(const SessionId id) noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto peer = mImpl->peers.find(id);
    if (peer == mImpl->peers.end())
        return;
    mImpl->tokens.erase(peer->second.token);
    mImpl->peers.erase(peer);
}

bool DatagramTransport::IsReady(const SessionId id) const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto peer = mImpl->peers.find(id);
    return peer != mImpl->peers.end() && peer->second.ready;
}

Status DatagramTransport::Send(
    const SessionId id, const std::span<const std::byte> payload) noexcept
{
    return SendSerialized(id, payload);
}

Status DatagramTransport::SendSerialized(
    const SessionId id, const std::span<const std::byte> payload) noexcept
{
    {
        const std::lock_guard guard(mImpl->mutex);
        if (mImpl->options.payloadMode != Protocol::PayloadMode::Json)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    }
    return SendPayload(id, payload);
}

Status DatagramTransport::SendPayload(
    const SessionId id, const std::span<const std::byte> payload) noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    const auto found = mImpl->peers.find(id);
    if (found == mImpl->peers.end())
        return Status::FailWithoutMessage(ErrorCode::Closed);
    auto& peer = found->second;
    if (!peer.ready || !mImpl->socket.IsOpen())
    {
        ++mImpl->metrics.sendNotReady;
        return Status::FailWithoutMessage(ErrorCode::WouldBlock);
    }
    if (peer.sentSequence == std::numeric_limits<std::uint64_t>::max())
        return Status::FailWithoutMessage(ErrorCode::Closed);

    std::array<std::byte, Codec::MaximumDatagramBytes> bytes{};
    const auto count = Codec::Encode(bytes, peer.token, peer.sentSequence + 1, payload);
    if (count == 0)
        return Status::FailWithoutMessage(ErrorCode::TooLarge);
    const auto now = Clock::now();
    const bool global = mImpl->sendBucket.Available(mImpl->options.totalSendRate, count, now);
    const bool perPeer = peer.sendBucket.Available(mImpl->options.peerSendRate, count, now);
    if (!global || !perPeer)
    {
        ++mImpl->metrics.sendRateLimited;
        if (!global)
            ++mImpl->metrics.sendLimitedGlobal;
        if (!perPeer)
            ++mImpl->metrics.sendLimitedPeer;
        return Status::FailWithoutMessage(ErrorCode::WouldBlock);
    }
    auto status = mImpl->socket.Send(peer.endpoint, std::span(bytes).first(count));
    if (!status.IsOk())
    {
        if (status.Code() == ErrorCode::WouldBlock)
            ++mImpl->metrics.sendWouldBlock;
        else
            ++mImpl->metrics.socketErrors;
        return status;
    }

    mImpl->sendBucket.Consume(mImpl->options.totalSendRate, count);
    peer.sendBucket.Consume(mImpl->options.peerSendRate, count);
    ++peer.sentSequence;
    ++mImpl->metrics.sentDatagrams;
    mImpl->metrics.sentBytes += static_cast<std::uint64_t>(count);
    return Status::Ok();
}

void DatagramTransport::Poll(
    const Admission& admission, const Receiver& receiver, const DatagramPollBudget budget) noexcept
{
    if (!admission || !receiver)
        return;
    {
        const std::lock_guard guard(mImpl->mutex);
        if (mImpl->options.payloadMode != Protocol::PayloadMode::Json)
            return;
    }
    try
    {
        std::optional<Protocol::Message> message;
        PollPayload(
            [&](const DatagramMessageView& view)
            {
                auto parsed = Protocol::ParseMessage(view.payload);
                if (!parsed.IsOk())
                    return PayloadDecision::InvalidPayload;
                message.emplace(std::move(parsed.Value()));
                return admission(view, *message) ? PayloadDecision::Allow : PayloadDecision::Reject;
            },
            [&](const DatagramMessageView& view) { receiver(view.session, *message); }, budget);
    }
    catch (...)
    {
        const std::lock_guard guard(mImpl->mutex);
        ++mImpl->metrics.rejectedDatagrams;
    }
}

void DatagramTransport::PollPayload(const PayloadAdmission& admission,
    const PayloadReceiver& receiver, const DatagramPollBudget budget) noexcept
{
    if (!admission || !receiver || budget.maximumDatagrams == 0 || budget.maximumBytes == 0)
        return;
    if (mImpl->polling.test_and_set())
    {
        const std::lock_guard guard(mImpl->mutex);
        ++mImpl->metrics.pollContentions;
        return;
    }
    struct ReleasePoll
    {
        std::atomic_flag& active;
        ~ReleasePoll() { active.clear(); }
    } release{ mImpl->polling };

    std::uint64_t bindingGeneration = 0;
    {
        const std::lock_guard guard(mImpl->mutex);
        if (!mImpl->socket.IsOpen())
            return;
        bindingGeneration = mImpl->bindingGeneration;
    }

    std::size_t consumed = 0;
    for (std::size_t attempt = 0;
        attempt < budget.maximumDatagrams && consumed < budget.maximumBytes; ++attempt)
    {
        try
        {
            std::array<std::byte, Codec::MaximumDatagramBytes + 1> bytes{};
            Codec::PacketView packet;
            Core::IpEndpoint endpoint{};
            SessionId id = SessionId::Invalid;
            std::uint64_t registrationGeneration = 0;
            {
                const std::lock_guard guard(mImpl->mutex);
                if (!mImpl->socket.IsOpen() || mImpl->bindingGeneration != bindingGeneration)
                    return;
                auto received = mImpl->socket.Receive(bytes);
                if (!received.status.IsOk())
                {
                    if (received.status.Code() == ErrorCode::WouldBlock)
                        return;
                    if (received.status.Code() == ErrorCode::TooLarge)
                    {
                        ++mImpl->metrics.rejectedDatagrams;
                        ++mImpl->metrics.truncatedDatagrams;
                        ConsumeBytes(consumed, bytes.size());
                        continue;
                    }
                    ++mImpl->metrics.socketErrors;
                    if (received.retryable)
                        continue;
                    return;
                }
                if (received.bytes > bytes.size())
                {
                    ++mImpl->metrics.socketErrors;
                    return;
                }
                ++mImpl->metrics.receivedDatagrams;
                mImpl->metrics.receivedBytes += static_cast<std::uint64_t>(received.bytes);
                ConsumeBytes(consumed, received.bytes);

                const auto decoded = Codec::Decode(std::span(bytes).first(received.bytes));
                if (!decoded || !received.endpoint.IsValid())
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    ++mImpl->metrics.malformedDatagrams;
                    continue;
                }
                packet = *decoded;
                const auto token = mImpl->tokens.find(packet.token);
                if (token == mImpl->tokens.end())
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    ++mImpl->metrics.unknownTokenDatagrams;
                    continue;
                }
                const auto peer = mImpl->peers.find(token->second);
                if (peer == mImpl->peers.end() || packet.sequence <= peer->second.receivedSequence)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    ++mImpl->metrics.replayedDatagrams;
                    continue;
                }
                // 수신 시퀀스와 엔드포인트는 이 Poll의 커밋에서만 바뀌고 Poll은 직렬이므로, 여기서 한
                // 판정이 아래 커밋까지 유지된다.
                if (!mImpl->options.allowEndpointMigration && peer->second.ready &&
                    peer->second.endpoint != received.endpoint)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    ++mImpl->metrics.endpointMismatchDatagrams;
                    continue;
                }
                if (packet.sequence - peer->second.receivedSequence >
                    mImpl->options.maxSequenceJump)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    ++mImpl->metrics.sequenceJumpDatagrams;
                    continue;
                }
                id = token->second;
                registrationGeneration = peer->second.registrationGeneration;
                endpoint = received.endpoint;
            }

            const DatagramMessageView view{ id, endpoint, packet.sequence,
                mImpl->options.payloadMode, packet.payload };
            const auto decision = admission(view);
            if (decision != PayloadDecision::Allow)
            {
                const std::lock_guard guard(mImpl->mutex);
                ++mImpl->metrics.rejectedDatagrams;
                if (decision == PayloadDecision::InvalidPayload)
                    ++mImpl->metrics.invalidPayloadDatagrams;
                else
                    ++mImpl->metrics.admissionRejectedDatagrams;
                continue;
            }

            {
                const std::lock_guard guard(mImpl->mutex);
                if (!mImpl->socket.IsOpen() || mImpl->bindingGeneration != bindingGeneration)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    ++mImpl->metrics.staleDatagrams;
                    return;
                }
                const auto found = mImpl->peers.find(id);
                if (found == mImpl->peers.end() ||
                    found->second.registrationGeneration != registrationGeneration ||
                    found->second.token != packet.token ||
                    packet.sequence <= found->second.receivedSequence)
                {
                    ++mImpl->metrics.rejectedDatagrams;
                    ++mImpl->metrics.staleDatagrams;
                    continue;
                }
                auto& peer = found->second;
                peer.receivedSequence = packet.sequence;
                peer.endpoint = endpoint;
                peer.ready = true;
            }
            receiver(view);
        }
        catch (...)
        {
            const std::lock_guard guard(mImpl->mutex);
            ++mImpl->metrics.rejectedDatagrams;
            ++mImpl->metrics.callbackFailures;
            return;
        }
    }
}

DatagramTransport::Metrics DatagramTransport::SnapshotMetrics() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    auto metrics = mImpl->metrics;
    metrics.registeredSessions = mImpl->peers.size();
    metrics.bound = mImpl->socket.IsOpen();
    metrics.receiving = mImpl->pump && !mImpl->pump->stopping.load();
    metrics.pendingBatches = mImpl->pump ? mImpl->pump->pending.load() : 0;
    return metrics;
}

std::size_t DatagramTransport::RegisteredSessionCount() const noexcept
{
    const std::lock_guard guard(mImpl->mutex);
    return mImpl->peers.size();
}

Result<Codec::Token> DatagramTransport::GetToken(const SessionId id) const
{
    const std::lock_guard guard(mImpl->mutex);
    const auto peer = mImpl->peers.find(id);
    if (peer == mImpl->peers.end())
        return Result<Codec::Token>::FromStatus(Status::FailWithoutMessage(ErrorCode::NotFound));
    return Result<Codec::Token>::FromValue(peer->second.token);
}

Status DatagramTransport::SendBinary(
    const SessionId id, const std::uint32_t type, const std::span<const std::byte> payload) noexcept
{
    {
        const std::lock_guard guard(mImpl->mutex);
        if (mImpl->options.payloadMode != Protocol::PayloadMode::Binary)
            return Status::FailWithoutMessage(ErrorCode::InvalidArgument);
    }
    auto encoded = Protocol::EncodeBinaryMessage(type, payload, Codec::MaximumPayloadBytes);
    if (!encoded.IsOk())
        return std::move(encoded).TakeStatus();
    return SendPayload(id, encoded.Value());
}

void DatagramTransport::PollBinary(const BinaryAdmission& admission, const BinaryReceiver& receiver,
    const DatagramPollBudget budget) noexcept
{
    if (!admission || !receiver)
        return;
    {
        const std::lock_guard guard(mImpl->mutex);
        if (mImpl->options.payloadMode != Protocol::PayloadMode::Binary)
            return;
    }
    try
    {
        Protocol::BinaryMessageView message;
        PollPayload(
            [&](const DatagramMessageView& view)
            {
                auto parsed = Protocol::DecodeBinaryMessage(view.payload);
                if (!parsed.IsOk())
                    return PayloadDecision::InvalidPayload;
                message = parsed.Value();
                return admission(view, message) ? PayloadDecision::Allow : PayloadDecision::Reject;
            },
            [&](const DatagramMessageView& view) { receiver(view.session, message); }, budget);
    }
    catch (...)
    {
        const std::lock_guard guard(mImpl->mutex);
        ++mImpl->metrics.rejectedDatagrams;
    }
}
void DatagramTransport::PollDatagrams(const DatagramAdmission& admission,
    const DatagramReceiver& receiver, const DatagramPollBudget budget) noexcept
{
    if (!admission || !receiver)
        return;
    try
    {
        PollPayload(
            [&](const DatagramMessageView& view)
            {
                const bool valid = view.payloadMode == Protocol::PayloadMode::Json
                                       ? Protocol::ParseMessage(view.payload).IsOk()
                                       : Protocol::DecodeBinaryMessage(view.payload).IsOk();
                if (!valid)
                    return PayloadDecision::InvalidPayload;
                return admission(view) ? PayloadDecision::Allow : PayloadDecision::Reject;
            },
            receiver, budget);
    }
    catch (...)
    {
        const std::lock_guard guard(mImpl->mutex);
        ++mImpl->metrics.callbackFailures;
        ++mImpl->metrics.rejectedDatagrams;
    }
}
#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
void TestAccess::InstallReceivePumpGate(std::shared_ptr<IReceivePumpGate> gate)
{
    const std::lock_guard guard(GetReceivePumpGateSlot().mutex);
    GetReceivePumpGateSlot().gate = std::move(gate);
}
void TestAccess::ClearReceivePumpGate(const std::shared_ptr<IReceivePumpGate>& expected)
{
    const std::lock_guard guard(GetReceivePumpGateSlot().mutex);
    if (GetReceivePumpGateSlot().gate.lock() == expected)
        GetReceivePumpGateSlot().gate.reset();
}
#endif
}
