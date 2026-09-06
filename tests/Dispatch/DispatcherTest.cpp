#include "TestHarness.h"

#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Session/Session.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using ServerCore::Core::ErrorCode;
using ServerCore::Dispatch::Dispatcher;
using ServerCore::Dispatch::MessageHandler;
using ServerCore::Dispatch::UnknownTypePolicy;
using ServerCore::Protocol::Message;
using ServerCore::Session::Session;
using ServerCore::Session::SessionId;
using ServerCore::Session::SessionState;

[[nodiscard]] std::vector<std::byte> ToBytes(const std::string_view text)
{
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char character : text)
    {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

[[nodiscard]] ServerCore::Core::Result<Message> Parse(const std::string_view text)
{
    return ServerCore::Protocol::ParseMessage(ToBytes(text));
}

class TestSession final : public Session
{
public:
    [[nodiscard]] SessionId Id() const noexcept override { return SessionId::Invalid; }

    [[nodiscard]] SessionState State() const noexcept override { return SessionState::Connected; }

    ServerCore::Core::Status MarkAuthenticated() override { return ServerCore::Core::Status::Ok(); }

    ServerCore::Core::Status Send(const ServerCore::Protocol::MessageFields&) override
    {
        return ServerCore::Core::Status::Ok();
    }

    ServerCore::Core::Status SendAndDisconnect(
        const ServerCore::Protocol::MessageFields& fields, ServerCore::Core::Status reason) override
    {
        const ServerCore::Core::Status sent = Send(fields);
        if (sent.IsOk())
        {
            Disconnect(std::move(reason));
        }
        return sent;
    }

    void Disconnect(ServerCore::Core::Status reason) override
    {
        ++mDisconnectCount;
        mLastDisconnectReason = std::move(reason);
    }

    [[nodiscard]] int DisconnectCount() const noexcept { return mDisconnectCount; }

    [[nodiscard]] const ServerCore::Core::Status& LastDisconnectReason() const noexcept
    {
        return mLastDisconnectReason;
    }

private:
    int mDisconnectCount = 0;
    ServerCore::Core::Status mLastDisconnectReason = ServerCore::Core::Status::Ok();
};

class ThrowingDisconnectSession final : public Session
{
public:
    enum class Failure
    {
        Allocation,
        Standard
    };

    explicit ThrowingDisconnectSession(const Failure failure)
        : mFailure(failure)
    {
    }

    [[nodiscard]] SessionId Id() const noexcept override { return SessionId::Invalid; }

    [[nodiscard]] SessionState State() const noexcept override { return SessionState::Connected; }

    ServerCore::Core::Status MarkAuthenticated() override { return ServerCore::Core::Status::Ok(); }

    ServerCore::Core::Status Send(const ServerCore::Protocol::MessageFields&) override
    {
        return ServerCore::Core::Status::Ok();
    }

    ServerCore::Core::Status SendAndDisconnect(
        const ServerCore::Protocol::MessageFields& fields, ServerCore::Core::Status reason) override
    {
        const ServerCore::Core::Status sent = Send(fields);
        if (sent.IsOk())
        {
            Disconnect(std::move(reason));
        }
        return sent;
    }

    void Disconnect(ServerCore::Core::Status) override
    {
        if (mFailure == Failure::Allocation)
        {
            throw std::bad_alloc();
        }

        throw std::runtime_error("test disconnect failure");
    }

private:
    Failure mFailure;
};

class CountingLogger final : public ServerCore::Core::ILogger
{
public:
    void Write(const ServerCore::Core::LogLevel level, std::string_view) noexcept override
    {
        ++mWriteCount;
        mLastLevel = level;
    }

    [[nodiscard]] int WriteCount() const noexcept { return mWriteCount; }

    [[nodiscard]] ServerCore::Core::LogLevel LastLevel() const noexcept { return mLastLevel; }

private:
    int mWriteCount = 0;
    ServerCore::Core::LogLevel mLastLevel = ServerCore::Core::LogLevel::Trace;
};

void RegisterRoutesAndEnforcesRawBodyLimit()
{
    Dispatcher dispatcher;
    int handlerCalls = 0;
    const ServerCore::Core::Status registered = dispatcher.Register(
        "Limited",
        [&handlerCalls](const std::shared_ptr<Session>&, const Message&)
        {
            ++handlerCalls;
            return ServerCore::Core::Status::Ok();
        },
        7);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "limited handler registers");

    ServerCore::Core::Result<Message> accepted = Parse(R"({"type":"Limited","body":{"x":1}})");
    ServerCoreTest::ExpectTrue(accepted.IsOk(), "exact-limit message parses");
    if (!accepted.IsOk())
    {
        return;
    }

    const std::shared_ptr<TestSession> session = std::make_shared<TestSession>();
    ServerCoreTest::ExpectTrue(
        dispatcher.Dispatch(session, accepted.Value()).IsOk(), "exact raw-body limit is accepted");
    ServerCoreTest::ExpectEqual(1, handlerCalls, "accepted message reaches its handler");

    ServerCore::Core::Result<Message> oversized = Parse(R"({"type":"Limited","body":{"x":12}})");
    ServerCoreTest::ExpectTrue(oversized.IsOk(), "oversized message parses before dispatch limit");
    if (!oversized.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status rejected = dispatcher.Dispatch(session, oversized.Value());
    ServerCoreTest::ExpectTrue(!rejected.IsOk() && rejected.Code() == ErrorCode::TooLarge,
        "raw-body limit rejects an extra byte");
    ServerCoreTest::ExpectEqual(
        1, handlerCalls, "limited handler is not called for an oversized body");
    ServerCoreTest::ExpectEqual(
        0, session->DisconnectCount(), "limit rejection does not force disconnect");
}

void PassesEnvelopeContextToHandler()
{
    Dispatcher dispatcher;
    bool sawExpectedType = false;
    bool sawExpectedBody = false;
    bool sawExpectedSequence = false;
    const ServerCore::Core::Status registered = dispatcher.Register("Sequenced",
        [&sawExpectedType, &sawExpectedBody, &sawExpectedSequence](
            const std::shared_ptr<Session>&, const Message& message)
        {
            sawExpectedType = message.Type() == "Sequenced";

            const ServerCore::Protocol::JsonValue* const body = message.Body();
            const ServerCore::Protocol::JsonValue* const value =
                body == nullptr ? nullptr : body->Find("value");
            const std::string* const bodyText = value == nullptr ? nullptr : value->TryString();
            sawExpectedBody = bodyText != nullptr && *bodyText == "body";

            const ServerCore::Protocol::JsonValue* const sequence = message.Sequence();
            const ServerCore::Protocol::JsonValue* const token =
                sequence == nullptr ? nullptr : sequence->Find("token");
            const ServerCore::Protocol::JsonValue* const largest =
                sequence == nullptr ? nullptr : sequence->Find("largest");
            const std::string* const tokenText = token == nullptr ? nullptr : token->TryString();
            const std::uint64_t* const largestValue =
                largest == nullptr ? nullptr : largest->TryUInt64();
            sawExpectedSequence = tokenText != nullptr && *tokenText == "opaque" &&
                                  largestValue != nullptr &&
                                  *largestValue == (std::numeric_limits<std::uint64_t>::max)();
            return ServerCore::Core::Status::Ok();
        });
    ServerCoreTest::ExpectTrue(registered.IsOk(), "sequenced handler registers");
    if (!registered.IsOk())
    {
        return;
    }

    ServerCore::Core::Result<Message> message = Parse(
        R"({"type":"Sequenced","body":{"value":"body"},"seq":{"token":"opaque","largest":18446744073709551615}})");
    ServerCoreTest::ExpectTrue(message.IsOk(), "sequenced message parses");
    if (!message.IsOk())
    {
        return;
    }

    const std::shared_ptr<TestSession> session = std::make_shared<TestSession>();
    const ServerCore::Core::Status dispatched = dispatcher.Dispatch(session, message.Value());
    ServerCoreTest::ExpectTrue(
        dispatched.IsOk(), "Dispatcher passed the full envelope to its handler");
    ServerCoreTest::ExpectTrue(sawExpectedType, "the handler received the envelope type");
    ServerCoreTest::ExpectTrue(sawExpectedBody, "the handler received the envelope body");
    ServerCoreTest::ExpectTrue(
        sawExpectedSequence, "the handler received an exact object and 64-bit sequence value");
}

void FreezeRejectsLateRegistration()
{
    Dispatcher dispatcher;
    const ServerCore::Core::Status registered =
        dispatcher.Register("First", [](const std::shared_ptr<Session>&, const Message&)
            { return ServerCore::Core::Status::Ok(); });
    ServerCoreTest::ExpectTrue(registered.IsOk(), "first handler registers before freeze");

    dispatcher.Freeze();
    dispatcher.Freeze();
    ServerCoreTest::ExpectTrue(
        dispatcher.IsFrozen(), "freeze remains observable after repeated calls");

    const ServerCore::Core::Status late =
        dispatcher.Register("Second", [](const std::shared_ptr<Session>&, const Message&)
            { return ServerCore::Core::Status::Ok(); });
    ServerCoreTest::ExpectTrue(
        !late.IsOk() && late.Code() == ErrorCode::Closed, "registration after freeze is rejected");
}

void FreezeSerializesConcurrentRegistration()
{
    // All workers enter Register and Freeze from the same start gate.  A successful Register must
    // therefore finish before Freeze returns; otherwise it must observe the closed table.  This is
    // intentionally repeated because the relative winner is a scheduler decision, while either
    // completed outcome is deterministic.
    constexpr std::size_t RegistrationCount = 8;
    constexpr std::size_t RoundCount = 16;

    for (std::size_t round = 0; round < RoundCount; ++round)
    {
        Dispatcher dispatcher;
        int publishedHandlerCalls = 0;
        const ServerCore::Core::Status published = dispatcher.Register("PublishedBeforeFreeze",
            [&publishedHandlerCalls](const std::shared_ptr<Session>&, const Message&)
            {
                ++publishedHandlerCalls;
                return ServerCore::Core::Status::Ok();
            });
        ServerCoreTest::ExpectTrue(published.IsOk(), "the pre-freeze handler registers");

        std::array<ServerCore::Core::Status, RegistrationCount> statuses{
            ServerCore::Core::Status::Ok(), ServerCore::Core::Status::Ok(),
            ServerCore::Core::Status::Ok(), ServerCore::Core::Status::Ok(),
            ServerCore::Core::Status::Ok(), ServerCore::Core::Status::Ok(),
            ServerCore::Core::Status::Ok(), ServerCore::Core::Status::Ok()
        };
        std::latch workersReady{ static_cast<std::ptrdiff_t>(RegistrationCount + 1) };
        std::latch startWorkers{ 1 };

        std::vector<std::thread> registrations;
        registrations.reserve(RegistrationCount);
        for (std::size_t index = 0; index < RegistrationCount; ++index)
        {
            registrations.emplace_back(
                [&dispatcher, &startWorkers, &statuses, &workersReady, index]()
                {
                    workersReady.count_down();
                    startWorkers.wait();

                    const std::string type = "Concurrent" + std::to_string(index);
                    const ServerCore::Core::Status result = dispatcher.Register(type,
                        [](const std::shared_ptr<Session>&, const Message&)
                        { return ServerCore::Core::Status::Ok(); });
                    statuses[index] = result;
                });
        }

        std::thread freezer(
            [&dispatcher, &startWorkers, &workersReady]()
            {
                workersReady.count_down();
                startWorkers.wait();
                dispatcher.Freeze();
            });

        workersReady.wait();
        startWorkers.count_down();

        for (std::thread& registration : registrations)
        {
            registration.join();
        }
        freezer.join();

        ServerCoreTest::ExpectTrue(dispatcher.IsFrozen(), "concurrent freeze finishes as frozen");
        for (std::size_t index = 0; index < RegistrationCount; ++index)
        {
            ServerCoreTest::ExpectTrue(
                statuses[index].IsOk() || statuses[index].Code() == ErrorCode::Closed,
                "concurrent Register either commits before Freeze or observes a closed table");
        }

        const ServerCore::Core::Status late =
            dispatcher.Register("Late", [](const std::shared_ptr<Session>&, const Message&)
                { return ServerCore::Core::Status::Ok(); });
        ServerCoreTest::ExpectTrue(!late.IsOk() && late.Code() == ErrorCode::Closed,
            "a Register after concurrent Freeze is rejected");

        ServerCore::Core::Result<Message> message =
            Parse(R"({"type":"PublishedBeforeFreeze","body":{}})");
        ServerCoreTest::ExpectTrue(message.IsOk(), "the frozen-table message parses");
        if (!message.IsOk())
        {
            continue;
        }

        const std::shared_ptr<TestSession> session = std::make_shared<TestSession>();
        const ServerCore::Core::Status dispatched = dispatcher.Dispatch(session, message.Value());
        ServerCoreTest::ExpectTrue(dispatched.IsOk(),
            "a completed Freeze publishes the handler table to lock-free Dispatch");
        ServerCoreTest::ExpectEqual(
            1, publishedHandlerCalls, "the published handler runs after the frozen handoff");
    }
}

void DispatchAndRegisterAreSafeBeforeFreeze()
{
    constexpr std::size_t RegistrationCount = 192;
    constexpr std::size_t DispatchThreadCount = 4;
    constexpr std::size_t DispatchesPerThread = 256;

    Dispatcher dispatcher;
    std::atomic<int> stableHandlerCalls = 0;
    const ServerCore::Core::Status registered = dispatcher.Register("StableWhileRegistering",
        [&stableHandlerCalls](const std::shared_ptr<Session>&, const Message&)
        {
            stableHandlerCalls.fetch_add(1, std::memory_order_relaxed);
            return ServerCore::Core::Status::Ok();
        });
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the stable pre-freeze handler registers");
    if (!registered.IsOk())
    {
        return;
    }

    ServerCore::Core::Result<Message> message =
        Parse(R"({"type":"StableWhileRegistering","body":{}})");
    ServerCoreTest::ExpectTrue(message.IsOk(), "the concurrent-dispatch message parses");
    if (!message.IsOk())
    {
        return;
    }

    const Message& dispatchMessage = message.Value();
    const std::shared_ptr<TestSession> session = std::make_shared<TestSession>();
    std::atomic<bool> everyRegistrationSucceeded = true;
    std::atomic<bool> everyDispatchSucceeded = true;
    std::latch workersReady{ static_cast<std::ptrdiff_t>(DispatchThreadCount + 1) };
    std::latch startWorkers{ 1 };

    std::thread registering(
        [&dispatcher, &everyRegistrationSucceeded, &startWorkers, &workersReady]()
        {
            workersReady.count_down();
            startWorkers.wait();

            for (std::size_t index = 0; index < RegistrationCount; ++index)
            {
                const std::string type = "ConcurrentRegistration" + std::to_string(index);
                const ServerCore::Core::Status result =
                    dispatcher.Register(type, [](const std::shared_ptr<Session>&, const Message&)
                        { return ServerCore::Core::Status::Ok(); });
                if (!result.IsOk())
                {
                    everyRegistrationSucceeded.store(false, std::memory_order_relaxed);
                }
            }
        });

    std::vector<std::thread> dispatchers;
    dispatchers.reserve(DispatchThreadCount);
    for (std::size_t index = 0; index < DispatchThreadCount; ++index)
    {
        dispatchers.emplace_back(
            [&dispatcher, &dispatchMessage, &everyDispatchSucceeded, &session, &startWorkers,
                &workersReady]()
            {
                workersReady.count_down();
                startWorkers.wait();

                for (std::size_t dispatch = 0; dispatch < DispatchesPerThread; ++dispatch)
                {
                    const ServerCore::Core::Status result =
                        dispatcher.Dispatch(session, dispatchMessage);
                    if (!result.IsOk())
                    {
                        everyDispatchSucceeded.store(false, std::memory_order_relaxed);
                    }
                }
            });
    }

    workersReady.wait();
    startWorkers.count_down();

    registering.join();
    for (std::thread& dispatching : dispatchers)
    {
        dispatching.join();
    }

    ServerCoreTest::ExpectTrue(everyRegistrationSucceeded.load(std::memory_order_relaxed),
        "all pre-freeze registrations complete while Dispatch reads the table");
    ServerCoreTest::ExpectTrue(everyDispatchSucceeded.load(std::memory_order_relaxed),
        "all concurrent pre-freeze dispatches reach the stable handler");
    ServerCoreTest::ExpectEqual(static_cast<int>(DispatchThreadCount * DispatchesPerThread),
        stableHandlerCalls.load(std::memory_order_relaxed),
        "every pre-freeze dispatch invokes its handler exactly once");

    dispatcher.Freeze();
    const ServerCore::Core::Status frozenDispatch = dispatcher.Dispatch(session, dispatchMessage);
    ServerCoreTest::ExpectTrue(frozenDispatch.IsOk(),
        "the same stable handler remains available on the lock-free frozen path");
    ServerCoreTest::ExpectEqual(static_cast<int>(DispatchThreadCount * DispatchesPerThread + 1),
        stableHandlerCalls.load(std::memory_order_relaxed),
        "the frozen dispatch also invokes the stable handler");
}

void UnknownTypePolicyControlsDisconnect()
{
    const std::shared_ptr<CountingLogger> logger = std::make_shared<CountingLogger>();
    ServerCore::Core::SetGlobalLogger(logger);

    Dispatcher dispatcher;
    const std::shared_ptr<TestSession> session = std::make_shared<TestSession>();
    ServerCore::Core::Result<Message> message = Parse(R"({"type":"FutureType","body":{}})");
    ServerCoreTest::ExpectTrue(message.IsOk(), "unknown-type message parses");
    if (!message.IsOk())
    {
        return;
    }

    const ServerCore::Core::Status ignored = dispatcher.Dispatch(session, message.Value());
    ServerCoreTest::ExpectTrue(ignored.IsOk(), "unknown type is ignored by the default policy");
    ServerCoreTest::ExpectEqual(
        0, session->DisconnectCount(), "default unknown policy keeps session open");
    ServerCoreTest::ExpectEqual(
        1, logger->WriteCount(), "default unknown policy writes one log entry");
    ServerCoreTest::ExpectTrue(logger->LastLevel() == ServerCore::Core::LogLevel::Warn,
        "unknown type is logged as a warning");

    dispatcher.SetUnknownTypePolicy(UnknownTypePolicy::Disconnect);
    const ServerCore::Core::Status disconnected = dispatcher.Dispatch(session, message.Value());
    ServerCoreTest::ExpectTrue(
        !disconnected.IsOk() && disconnected.Code() == ErrorCode::UnknownType,
        "disconnect policy reports an unknown type");
    ServerCoreTest::ExpectEqual(
        1, session->DisconnectCount(), "disconnect policy closes the session once");
    ServerCoreTest::ExpectTrue(session->LastDisconnectReason().Code() == ErrorCode::UnknownType,
        "disconnect policy forwards UnknownType to the session");
    ServerCoreTest::ExpectTrue(session->LastDisconnectReason().Message().empty(),
        "disconnect policy does not allocate a second diagnostic for the session reason");
    ServerCoreTest::ExpectEqual(
        2, logger->WriteCount(), "disconnect policy also records the unknown type");

    ServerCore::Core::SetGlobalLogger(std::shared_ptr<ServerCore::Core::ILogger>());
}

void HandlerStatusIsPropagatedWithoutDisconnect()
{
    Dispatcher dispatcher;
    const ServerCore::Core::Status registered = dispatcher.Register("RejectedByHandler",
        [](const std::shared_ptr<Session>&, const Message&)
        {
            return ServerCore::Core::Status::Fail(
                ErrorCode::InvalidArgument, "handler rejected input");
        });
    ServerCoreTest::ExpectTrue(registered.IsOk(), "failing handler registers");

    ServerCore::Core::Result<Message> message = Parse(R"({"type":"RejectedByHandler","body":{}})");
    ServerCoreTest::ExpectTrue(message.IsOk(), "handler-failure message parses");
    if (!message.IsOk())
    {
        return;
    }

    const std::shared_ptr<TestSession> session = std::make_shared<TestSession>();
    const ServerCore::Core::Status result = dispatcher.Dispatch(session, message.Value());
    ServerCoreTest::ExpectTrue(!result.IsOk() && result.Code() == ErrorCode::InvalidArgument,
        "handler status is returned unchanged");
    ServerCoreTest::ExpectEqual(
        0, session->DisconnectCount(), "handler status alone does not disconnect");
}

void ErrorEnvelopeWithoutBodyIsNotRouted()
{
    int handlerCalls = 0;
    Dispatcher dispatcher;
    const ServerCore::Core::Status registered = dispatcher.Register("Reply",
        [&handlerCalls](const std::shared_ptr<Session>&, const Message&)
        {
            ++handlerCalls;
            return ServerCore::Core::Status::Ok();
        });
    ServerCoreTest::ExpectTrue(registered.IsOk(), "error-envelope type registers");

    ServerCore::Core::Result<Message> message =
        Parse(R"({"type":"Reply","error":{"code":"Rejected"}})");
    ServerCoreTest::ExpectTrue(message.IsOk(), "bodyless error envelope is valid protocol");
    if (!message.IsOk())
    {
        return;
    }

    const std::shared_ptr<TestSession> session = std::make_shared<TestSession>();
    const ServerCore::Core::Status result = dispatcher.Dispatch(session, message.Value());
    ServerCoreTest::ExpectTrue(!result.IsOk() && result.Code() == ErrorCode::InvalidFormat,
        "bodyless error envelope is not passed to a body handler");
    ServerCoreTest::ExpectEqual(0, handlerCalls, "bodyless error envelope does not invoke handler");
    ServerCoreTest::ExpectEqual(
        0, session->DisconnectCount(), "routing failure does not force disconnect");
}

void DispatchDoesNotHideSessionContractViolations()
{
    Dispatcher dispatcher;
    dispatcher.SetUnknownTypePolicy(UnknownTypePolicy::Disconnect);

    ServerCore::Core::Result<Message> message = Parse(R"({"type":"Unknown","body":{}})");
    ServerCoreTest::ExpectTrue(
        message.IsOk(), "unknown-type message parses for exception conversion");
    if (!message.IsOk())
    {
        return;
    }

    const std::shared_ptr<ThrowingDisconnectSession> allocationFailure =
        std::make_shared<ThrowingDisconnectSession>(ThrowingDisconnectSession::Failure::Allocation);
    bool allocationWasVisible = false;
    try
    {
        (void)dispatcher.Dispatch(allocationFailure, message.Value());
    }
    catch (const std::bad_alloc&)
    {
        allocationWasVisible = true;
    }
    ServerCoreTest::ExpectTrue(allocationWasVisible,
        "an allocation exception from Session::Disconnect remains a visible contract violation");

    const std::shared_ptr<ThrowingDisconnectSession> standardFailure =
        std::make_shared<ThrowingDisconnectSession>(ThrowingDisconnectSession::Failure::Standard);
    bool standardWasVisible = false;
    try
    {
        (void)dispatcher.Dispatch(standardFailure, message.Value());
    }
    catch (const std::runtime_error&)
    {
        standardWasVisible = true;
    }
    ServerCoreTest::ExpectTrue(standardWasVisible,
        "a standard exception from Session::Disconnect remains a visible contract violation");
}

ServerCoreTest::CheckRegistration gRegisterRoutesAndEnforcesRawBodyLimit(
    "Dispatch.RegisterRoutesAndEnforcesRawBodyLimit", &RegisterRoutesAndEnforcesRawBodyLimit);
ServerCoreTest::CheckRegistration gPassesEnvelopeContextToHandler(
    "Dispatch.PassesEnvelopeContextToHandler", &PassesEnvelopeContextToHandler);
ServerCoreTest::CheckRegistration gFreezeRejectsLateRegistration(
    "Dispatch.FreezeRejectsLateRegistration", &FreezeRejectsLateRegistration);
ServerCoreTest::CheckRegistration gFreezeSerializesConcurrentRegistration(
    "Dispatch.FreezeSerializesConcurrentRegistration", &FreezeSerializesConcurrentRegistration);
ServerCoreTest::CheckRegistration gDispatchAndRegisterAreSafeBeforeFreeze(
    "Dispatch.DispatchAndRegisterAreSafeBeforeFreeze", &DispatchAndRegisterAreSafeBeforeFreeze);
ServerCoreTest::CheckRegistration gUnknownTypePolicyControlsDisconnect(
    "Dispatch.UnknownTypePolicyControlsDisconnect", &UnknownTypePolicyControlsDisconnect);
ServerCoreTest::CheckRegistration gHandlerStatusIsPropagatedWithoutDisconnect(
    "Dispatch.HandlerStatusIsPropagatedWithoutDisconnect",
    &HandlerStatusIsPropagatedWithoutDisconnect);
ServerCoreTest::CheckRegistration gErrorEnvelopeWithoutBodyIsNotRouted(
    "Dispatch.ErrorEnvelopeWithoutBodyIsNotRouted", &ErrorEnvelopeWithoutBodyIsNotRouted);
ServerCoreTest::CheckRegistration gDispatchDoesNotHideSessionContractViolations(
    "Dispatch.DispatchDoesNotHideSessionContractViolations",
    &DispatchDoesNotHideSessionContractViolations);
}
