#include "TestHarness.h"

#include "ServerCore/Session/SessionRegistry.h"

#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class FakeSession final : public ServerCore::Session::Session
{
public:
    explicit FakeSession(ServerCore::Session::SessionId id)
        : mId(id)
    {
    }

    [[nodiscard]] ServerCore::Session::SessionId Id() const noexcept override { return mId; }

    [[nodiscard]] ServerCore::Session::SessionState State() const noexcept override
    {
        return mState;
    }

    [[nodiscard]] ServerCore::Core::Status MarkAuthenticated() override
    {
        if (mState == ServerCore::Session::SessionState::Authenticated)
        {
            return ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::AlreadyExists,
                "the fake session is already authenticated");
        }
        if (mState == ServerCore::Session::SessionState::Closing ||
            mState == ServerCore::Session::SessionState::Closed)
        {
            return ServerCore::Core::Status::Fail(
                ServerCore::Core::ErrorCode::Closed, "the fake session is closed");
        }

        mState = ServerCore::Session::SessionState::Authenticated;
        return ServerCore::Core::Status::Ok();
    }

    [[nodiscard]] ServerCore::Core::Status Send(const ServerCore::Protocol::MessageFields&) override
    {
        return ServerCore::Core::Status::Ok();
    }

    [[nodiscard]] ServerCore::Core::Status SendAndDisconnect(
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
        mState = ServerCore::Session::SessionState::Closed;
    }

private:
    ServerCore::Session::SessionId mId;
    ServerCore::Session::SessionState mState = ServerCore::Session::SessionState::Connected;
};

ServerCore::Session::SessionId IssueIdOrFail(ServerCore::Session::SessionRegistry& registry)
{
    const ServerCore::Core::Result<ServerCore::Session::SessionId> result = registry.IssueId();
    ServerCoreTest::ExpectTrue(result.IsOk(), "IssueId() succeeds while the process has ID space");
    if (!result.IsOk())
    {
        return ServerCore::Session::SessionId::Invalid;
    }

    return result.Value();
}

void RegistryIssuesValidDistinctIds()
{
    ServerCore::Session::SessionRegistry registry;

    const ServerCore::Session::SessionId first = IssueIdOrFail(registry);
    const ServerCore::Session::SessionId second = IssueIdOrFail(registry);

    ServerCoreTest::ExpectTrue(ServerCore::Session::IsValid(first), "the first issued ID is valid");
    ServerCoreTest::ExpectTrue(
        ServerCore::Session::IsValid(second), "the second issued ID is valid");
    ServerCoreTest::ExpectTrue(first != second, "two issued IDs are distinct");
    ServerCoreTest::ExpectTrue(
        static_cast<std::uint64_t>(first) < static_cast<std::uint64_t>(second),
        "issued IDs increase monotonically");
}

void RegistryRegistersFindsAndUnregistersSessions()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Core::Status bind = registry.BindToCurrentThread();
    ServerCoreTest::ExpectTrue(bind.IsOk(), "the current test thread binds the registry");

    const ServerCore::Session::SessionId id = IssueIdOrFail(registry);
    const std::shared_ptr<FakeSession> session = std::make_shared<FakeSession>(id);

    const ServerCore::Core::Status registerStatus = registry.Register(session);
    ServerCoreTest::ExpectTrue(registerStatus.IsOk(), "Register() accepts a valid new session");
    ServerCoreTest::ExpectEqual(
        std::size_t{ 1 }, registry.Count(), "Register() increases the count");
    ServerCoreTest::ExpectTrue(
        registry.Find(id) == session, "Find() returns the registered session");

    const ServerCore::Core::Status unregisterStatus = registry.Unregister(id);
    ServerCoreTest::ExpectTrue(
        unregisterStatus.IsOk(), "Unregister() removes a registered session");
    ServerCoreTest::ExpectEqual(
        std::size_t{ 0 }, registry.Count(), "Unregister() decreases the count");
    ServerCoreTest::ExpectTrue(registry.Find(id) == nullptr, "Find() is empty after Unregister()");
}

void RegistryAllowsFailureCleanupFromAnyThread()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Core::Status bind = registry.BindToCurrentThread();
    ServerCoreTest::ExpectTrue(bind.IsOk(), "the current test thread binds the registry");

    const ServerCore::Session::SessionId id = IssueIdOrFail(registry);
    const ServerCore::Core::Status registered =
        registry.Register(std::make_shared<FakeSession>(id));
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the cleanup fixture session registers");

    ServerCore::Core::Status unregistered = ServerCore::Core::Status::Ok();
    std::thread cleanupThread(
        [&registry, id, &unregistered]() { unregistered = registry.Unregister(id); });
    cleanupThread.join();

    ServerCoreTest::ExpectTrue(
        unregistered.IsOk(), "Unregister() removes a session from a failure-cleanup thread");
    ServerCoreTest::ExpectEqual(std::size_t{ 0 }, registry.Count(),
        "cross-thread failure cleanup leaves no registered session");
    ServerCoreTest::ExpectTrue(
        registry.Find(id) == nullptr, "the bound thread observes the cross-thread removal");
}

void RegistryRejectsInvalidAndDuplicateRegistrations()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Core::Status bind = registry.BindToCurrentThread();
    ServerCoreTest::ExpectTrue(bind.IsOk(), "the current test thread binds the registry");

    const ServerCore::Core::Status nullStatus = registry.Register(nullptr);
    ServerCoreTest::ExpectTrue(nullStatus.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "Register() rejects a null session");

    const std::shared_ptr<FakeSession> invalid =
        std::make_shared<FakeSession>(ServerCore::Session::SessionId::Invalid);
    const ServerCore::Core::Status invalidStatus = registry.Register(invalid);
    ServerCoreTest::ExpectTrue(invalidStatus.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "Register() rejects SessionId::Invalid");

    const ServerCore::Session::SessionId id = IssueIdOrFail(registry);
    const std::shared_ptr<FakeSession> first = std::make_shared<FakeSession>(id);
    const std::shared_ptr<FakeSession> second = std::make_shared<FakeSession>(id);
    const ServerCore::Core::Status firstStatus = registry.Register(first);
    const ServerCore::Core::Status duplicateStatus = registry.Register(second);

    ServerCoreTest::ExpectTrue(firstStatus.IsOk(), "the first registration succeeds");
    ServerCoreTest::ExpectTrue(duplicateStatus.Code() == ServerCore::Core::ErrorCode::AlreadyExists,
        "Register() rejects an ID that is already registered");
}

void RegistryRejectsUnissuedAndReusedRegistrations()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Core::Status bind = registry.BindToCurrentThread();
    ServerCoreTest::ExpectTrue(bind.IsOk(), "the current test thread binds the registry");

    ServerCore::Session::SessionRegistry foreignRegistry;
    const ServerCore::Session::SessionId foreignId = IssueIdOrFail(foreignRegistry);
    const ServerCore::Core::Status foreignStatus =
        registry.Register(std::make_shared<FakeSession>(foreignId));
    ServerCoreTest::ExpectTrue(foreignStatus.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "Register() rejects an ID issued by a different registry");

    const ServerCore::Session::SessionId arbitraryId =
        static_cast<ServerCore::Session::SessionId>((std::numeric_limits<std::uint64_t>::max)());
    const ServerCore::Core::Status arbitraryStatus =
        registry.Register(std::make_shared<FakeSession>(arbitraryId));
    ServerCoreTest::ExpectTrue(
        arbitraryStatus.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "Register() rejects an arbitrary nonzero SessionId that this registry never issued");

    const ServerCore::Session::SessionId id = IssueIdOrFail(registry);
    const ServerCore::Core::Status firstRegistration =
        registry.Register(std::make_shared<FakeSession>(id));
    const ServerCore::Core::Status unregisterStatus = registry.Unregister(id);
    const ServerCore::Core::Status reusedRegistration =
        registry.Register(std::make_shared<FakeSession>(id));

    ServerCoreTest::ExpectTrue(
        firstRegistration.IsOk(), "Register() accepts an ID issued by the same registry");
    ServerCoreTest::ExpectTrue(
        unregisterStatus.IsOk(), "the issued session can be unregistered once");
    ServerCoreTest::ExpectTrue(
        reusedRegistration.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "Register() consumes the issue reservation and rejects reuse after Unregister()");

    ServerCore::Session::SessionId historicalId = ServerCore::Session::SessionId::Invalid;
    {
        ServerCore::Session::SessionRegistry previousRegistry;
        const ServerCore::Core::Status previousBind = previousRegistry.BindToCurrentThread();
        ServerCoreTest::ExpectTrue(previousBind.IsOk(), "the prior registry binds its test thread");
        historicalId = IssueIdOrFail(previousRegistry);
        const ServerCore::Core::Status previousRegistration =
            previousRegistry.Register(std::make_shared<FakeSession>(historicalId));
        const ServerCore::Core::Status previousUnregister =
            previousRegistry.Unregister(historicalId);
        ServerCoreTest::ExpectTrue(
            previousRegistration.IsOk(), "the prior registry can register its issued ID");
        ServerCoreTest::ExpectTrue(
            previousUnregister.IsOk(), "the prior registry can end its issued session");
    }

    ServerCore::Session::SessionRegistry replacementRegistry;
    const ServerCore::Core::Status replacementBind = replacementRegistry.BindToCurrentThread();
    const ServerCore::Core::Status historicalStatus =
        replacementRegistry.Register(std::make_shared<FakeSession>(historicalId));
    ServerCoreTest::ExpectTrue(
        replacementBind.IsOk(), "the replacement registry binds its test thread");
    ServerCoreTest::ExpectTrue(
        historicalStatus.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "Register() rejects a historical ID after its original registry ended");
}

void RegistryDiscardsUnregisteredIssueReservations()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Session::SessionId id = IssueIdOrFail(registry);

    // I/O 완료 경로처럼 레지스트리의 직렬 문맥 밖에서도 예약 정리가 가능해야 한다.
    ServerCore::Core::Status discardStatus = ServerCore::Core::Status::Ok();
    std::thread discardThread(
        [&registry, id, &discardStatus]() { discardStatus = registry.DiscardIssuedId(id); });
    discardThread.join();

    ServerCoreTest::ExpectTrue(discardStatus.IsOk(),
        "DiscardIssuedId() removes a pending reservation from another thread");

    const ServerCore::Core::Status bind = registry.BindToCurrentThread();
    const ServerCore::Core::Status registration =
        registry.Register(std::make_shared<FakeSession>(id));
    const ServerCore::Core::Status repeatedDiscard = registry.DiscardIssuedId(id);
    const ServerCore::Core::Status invalidDiscard =
        registry.DiscardIssuedId(ServerCore::Session::SessionId::Invalid);

    ServerCoreTest::ExpectTrue(bind.IsOk(), "the current test thread binds the registry");
    ServerCoreTest::ExpectTrue(registration.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "a discarded reservation cannot be registered");
    ServerCoreTest::ExpectTrue(
        repeatedDiscard.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "DiscardIssuedId() rejects a reservation that was already discarded");
    ServerCoreTest::ExpectTrue(
        invalidDiscard.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "DiscardIssuedId() rejects SessionId::Invalid");

    ServerCore::Session::SessionRegistry foreignRegistry;
    const ServerCore::Session::SessionId foreignId = IssueIdOrFail(foreignRegistry);
    const ServerCore::Core::Status foreignDiscard = registry.DiscardIssuedId(foreignId);
    ServerCoreTest::ExpectTrue(
        foreignDiscard.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "DiscardIssuedId() rejects an ID issued by a different registry");

    const ServerCore::Session::SessionId registeredId = IssueIdOrFail(registry);
    const ServerCore::Core::Status registered =
        registry.Register(std::make_shared<FakeSession>(registeredId));
    const ServerCore::Core::Status registeredDiscard = registry.DiscardIssuedId(registeredId);
    ServerCoreTest::ExpectTrue(registered.IsOk(), "the setup session registers");
    ServerCoreTest::ExpectTrue(
        registeredDiscard.Code() == ServerCore::Core::ErrorCode::InvalidArgument,
        "DiscardIssuedId() rejects a reservation consumed by Register()");
}

void RegistrySerializesConcurrentReservationDiscards()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Session::SessionId id = IssueIdOrFail(registry);

    std::latch callersReady{ 2 };
    std::latch startDiscard{ 1 };
    ServerCore::Core::Status firstStatus = ServerCore::Core::Status::Ok();
    ServerCore::Core::Status secondStatus = ServerCore::Core::Status::Ok();

    std::thread first(
        [&registry, id, &callersReady, &startDiscard, &firstStatus]()
        {
            callersReady.count_down();
            startDiscard.wait();
            firstStatus = registry.DiscardIssuedId(id);
        });
    std::thread second(
        [&registry, id, &callersReady, &startDiscard, &secondStatus]()
        {
            callersReady.count_down();
            startDiscard.wait();
            secondStatus = registry.DiscardIssuedId(id);
        });

    callersReady.wait();
    startDiscard.count_down();
    first.join();
    second.join();

    const bool firstSucceeded = firstStatus.IsOk();
    const bool secondSucceeded = secondStatus.IsOk();
    ServerCoreTest::ExpectTrue(firstSucceeded != secondSucceeded,
        "exactly one concurrent DiscardIssuedId() call consumes a reservation");
    ServerCoreTest::ExpectTrue(
        (firstSucceeded || firstStatus.Code() == ServerCore::Core::ErrorCode::InvalidArgument) &&
            (secondSucceeded ||
                secondStatus.Code() == ServerCore::Core::ErrorCode::InvalidArgument),
        "the losing concurrent DiscardIssuedId() call observes no pending reservation");
}

void SessionMarksAuthenticationWithoutOwningIdentityPolicy()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Session::SessionId id = IssueIdOrFail(registry);
    FakeSession session(id);

    const ServerCore::Core::Status first = session.MarkAuthenticated();
    const ServerCore::Core::Status second = session.MarkAuthenticated();

    ServerCoreTest::ExpectTrue(first.IsOk(), "MarkAuthenticated() moves a connected session once");
    ServerCoreTest::ExpectTrue(session.State() == ServerCore::Session::SessionState::Authenticated,
        "MarkAuthenticated() leaves the fake session authenticated");
    ServerCoreTest::ExpectTrue(second.Code() == ServerCore::Core::ErrorCode::AlreadyExists,
        "MarkAuthenticated() rejects a duplicate state transition");
}

void ForEachUsesSnapshotSoVisitorMayRemoveSessions()
{
    ServerCore::Session::SessionRegistry registry;
    const ServerCore::Core::Status bind = registry.BindToCurrentThread();
    ServerCoreTest::ExpectTrue(bind.IsOk(), "the current test thread binds the registry");

    for (int index = 0; index < 3; ++index)
    {
        const ServerCore::Session::SessionId id = IssueIdOrFail(registry);
        const ServerCore::Core::Status registerStatus =
            registry.Register(std::make_shared<FakeSession>(id));
        ServerCoreTest::ExpectTrue(registerStatus.IsOk(), "the setup session registers");
    }

    std::size_t visited = 0;
    registry.ForEach(
        [&registry, &visited](const std::shared_ptr<ServerCore::Session::Session>& session)
        {
            ++visited;
            const ServerCore::Core::Status unregisterStatus = registry.Unregister(session->Id());
            ServerCoreTest::ExpectTrue(unregisterStatus.IsOk(),
                "a visitor can unregister its snapshot session without invalidating the iteration");
        });

    ServerCoreTest::ExpectEqual(std::size_t{ 3 }, visited,
        "the snapshot visits every session present when ForEach() began");
    ServerCoreTest::ExpectEqual(
        std::size_t{ 0 }, registry.Count(), "visitor removals leave the registry empty");
}

const ServerCoreTest::CheckRegistration gRegistryIssuesValidDistinctIds{
    "SessionRegistry.RegistryIssuesValidDistinctIds", RegistryIssuesValidDistinctIds
};
const ServerCoreTest::CheckRegistration gRegistryRegistersFindsAndUnregistersSessions{
    "SessionRegistry.RegistryRegistersFindsAndUnregistersSessions",
    RegistryRegistersFindsAndUnregistersSessions
};
const ServerCoreTest::CheckRegistration gRegistryAllowsFailureCleanupFromAnyThread{
    "SessionRegistry.RegistryAllowsFailureCleanupFromAnyThread",
    RegistryAllowsFailureCleanupFromAnyThread
};
const ServerCoreTest::CheckRegistration gRegistryRejectsInvalidAndDuplicateRegistrations{
    "SessionRegistry.RegistryRejectsInvalidAndDuplicateRegistrations",
    RegistryRejectsInvalidAndDuplicateRegistrations
};
const ServerCoreTest::CheckRegistration gRegistryRejectsUnissuedAndReusedRegistrations{
    "SessionRegistry.RegistryRejectsUnissuedAndReusedRegistrations",
    RegistryRejectsUnissuedAndReusedRegistrations
};
const ServerCoreTest::CheckRegistration gRegistryDiscardsUnregisteredIssueReservations{
    "SessionRegistry.RegistryDiscardsUnregisteredIssueReservations",
    RegistryDiscardsUnregisteredIssueReservations
};
const ServerCoreTest::CheckRegistration gRegistrySerializesConcurrentReservationDiscards{
    "SessionRegistry.RegistrySerializesConcurrentReservationDiscards",
    RegistrySerializesConcurrentReservationDiscards
};
const ServerCoreTest::CheckRegistration gSessionMarksAuthenticationWithoutOwningIdentityPolicy{
    "SessionRegistry.SessionMarksAuthenticationWithoutOwningIdentityPolicy",
    SessionMarksAuthenticationWithoutOwningIdentityPolicy
};
const ServerCoreTest::CheckRegistration gForEachUsesSnapshotSoVisitorMayRemoveSessions{
    "SessionRegistry.ForEachUsesSnapshotSoVisitorMayRemoveSessions",
    ForEachUsesSnapshotSoVisitorMayRemoveSessions
};
}
