#include "TestHarness.h"

#include "ServerCore/Core/Error.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

/// <summary>
/// Status와 Result가 담은 것을 그대로 돌려주는지 고정하는 검사다.
/// </summary>
/// <remarks>
/// 계약 위반 경로는 여기 없다. IsOk()가 거짓인데 Value()를 부르는 것이나 Fail()에
/// ErrorCode::Ok를 넘기는 것은 단언으로 프로세스를 끊으므로 시험 대상이 아니다.
/// 그것들을 검사로 만들면 프로세스가 죽는 것을 시험이 기대하게 되는데, 그러면 시험 틀이
/// 죽음을 정상 결과로 다루는 장치를 갖게 된다. 그 장치는 진짜 죽음도 삼킨다.
///
/// 실패 메시지에는 무엇이 기대와 달랐는지만 적는다. 원인 짐작은 적지 않는다.
/// </remarks>
namespace
{
void OkStatusIsOk()
{
    const ServerCore::Core::Status status = ServerCore::Core::Status::Ok();

    ServerCoreTest::ExpectTrue(status.IsOk(), "Status::Ok() reports IsOk");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Ok),
        static_cast<int>(status.Code()), "Status::Ok() carries ErrorCode::Ok");
    ServerCoreTest::ExpectEqual(std::string(), status.Message(), "Status::Ok() carries no message");
}

void FailStatusCarriesCodeAndMessage()
{
    const ServerCore::Core::Status status = ServerCore::Core::Status::Fail(
        ServerCore::Core::ErrorCode::TooLarge, "body is 70000 bytes");

    ServerCoreTest::ExpectTrue(!status.IsOk(), "a failed Status does not report IsOk");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::TooLarge),
        static_cast<int>(status.Code()), "Status::Fail() carries the code");
    ServerCoreTest::ExpectEqual(
        std::string("body is 70000 bytes"), status.Message(), "Status::Fail() carries the message");

    static_assert(noexcept(
        ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed)));
    const ServerCore::Core::Status messageFreeFailure =
        ServerCore::Core::Status::FailWithoutMessage(ServerCore::Core::ErrorCode::Closed);
    ServerCoreTest::ExpectTrue(
        !messageFreeFailure.IsOk(), "a message-free Status does not report IsOk");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Closed),
        static_cast<int>(messageFreeFailure.Code()),
        "Status::FailWithoutMessage() carries the requested code");
    ServerCoreTest::ExpectEqual(std::string(), messageFreeFailure.Message(),
        "Status::FailWithoutMessage() carries no diagnostic");

    static_assert(noexcept(ServerCore::Core::Status::AllocationFailure()));
    const ServerCore::Core::Status allocationFailure =
        ServerCore::Core::Status::AllocationFailure();
    ServerCoreTest::ExpectTrue(
        !allocationFailure.IsOk(), "an allocation failure Status does not report IsOk");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::PlatformError),
        static_cast<int>(allocationFailure.Code()),
        "Status::AllocationFailure() carries ErrorCode::PlatformError");
    ServerCoreTest::ExpectEqual(std::string(), allocationFailure.Message(),
        "Status::AllocationFailure() carries no allocating diagnostic");
}

void UnimplementedStatusNamesWhat()
{
    const ServerCore::Core::Status status = ServerCore::Core::Status::Unimplemented("Config::Load");

    ServerCoreTest::ExpectTrue(!status.IsOk(), "an Unimplemented Status does not report IsOk");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::Unimplemented),
        static_cast<int>(status.Code()),
        "Status::Unimplemented() carries ErrorCode::Unimplemented");

    // 문구 전체를 고정하지 않고 넘긴 이름이 들어 있는지만 본다. 문구는 사람이 읽는 것이고
    // 예고 없이 바뀌어도 되지만, 넘긴 이름이 사라지면 그 실패가 어디 것인지 알 수 없게 된다.
    ServerCoreTest::ExpectTrue(status.Message().find("Config::Load") != std::string::npos,
        "Status::Unimplemented() message names what was not implemented");
}

void ResultFromValueHoldsValue()
{
    ServerCore::Core::Result<int> result = ServerCore::Core::Result<int>::FromValue(7);

    ServerCoreTest::ExpectTrue(result.IsOk(), "Result::FromValue() reports IsOk");
    ServerCoreTest::ExpectEqual(7, result.Value(), "Result::FromValue() holds the value");
    ServerCoreTest::ExpectTrue(result.GetStatus().IsOk() && result.GetStatus().Message().empty(),
        "a successful Result exposes an Ok Status with no message");

    const auto independent = ServerCore::Core::Result<int>::FromValue(8);
    auto copied = result;
    const auto moved = std::move(copied);
    ServerCoreTest::ExpectTrue(&result.GetStatus() != &independent.GetStatus() &&
                                   &result.GetStatus() != &copied.GetStatus() &&
                                   &result.GetStatus() != &moved.GetStatus() &&
                                   &copied.GetStatus() != &moved.GetStatus(),
        "each successful Result owns its Status independently, including copies and moves");
    ServerCoreTest::ExpectTrue(independent.GetStatus().IsOk() && copied.GetStatus().IsOk() &&
                                   moved.GetStatus().IsOk() && moved.GetStatus().Message().empty(),
        "copying and moving successful Results preserves their success Status");
    ServerCoreTest::ExpectEqual(7, moved.Value(), "moving a copied Result preserves its value");
}

void ResultFromStatusHoldsStatus()
{
    ServerCore::Core::Result<int> result = ServerCore::Core::Result<int>::FromStatus(
        ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::NotFound, "no such key"));

    ServerCoreTest::ExpectTrue(!result.IsOk(), "a failed Result does not report IsOk");
    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::NotFound),
        static_cast<int>(result.GetStatus().Code()), "a failed Result carries the code");
    ServerCoreTest::ExpectEqual(std::string("no such key"), result.GetStatus().Message(),
        "a failed Result carries the message");
}

void ResultTakeStatusMovesFailure()
{
    ServerCore::Core::Result<int> result = ServerCore::Core::Result<int>::FromStatus(
        ServerCore::Core::Status::Fail(ServerCore::Core::ErrorCode::NotFound, "movable failure"));

    static_assert(noexcept(std::move(result).TakeStatus()));
    const ServerCore::Core::Status status = std::move(result).TakeStatus();

    ServerCoreTest::ExpectEqual(static_cast<int>(ServerCore::Core::ErrorCode::NotFound),
        static_cast<int>(status.Code()), "Result::TakeStatus() preserves the failure code");
    ServerCoreTest::ExpectEqual(std::string("movable failure"), status.Message(),
        "Result::TakeStatus() transfers the failure message");
    ServerCoreTest::ExpectTrue(
        !result.IsOk(), "Result::TakeStatus() leaves the source Result in a failed state");
    ServerCoreTest::ExpectTrue(result.GetStatus().Code() == ServerCore::Core::ErrorCode::NotFound,
        "taking the message leaves the source failure code intact");
}

void ResultHoldsNonTrivialValue()
{
    ServerCore::Core::Result<std::string> result =
        ServerCore::Core::Result<std::string>::FromValue(std::string("PlayerMove"));

    ServerCoreTest::ExpectTrue(result.IsOk(), "Result<std::string>::FromValue() reports IsOk");
    ServerCoreTest::ExpectEqual(
        std::string("PlayerMove"), result.Value(), "Result<std::string> holds the value");

    std::string source(256, 'x');
    const auto copied = ServerCore::Core::Result<std::string>::FromValue(source);
    source[0] = 'y';
    ServerCoreTest::ExpectEqual(std::string(256, 'x'), copied.Value(),
        "FromValue copies an lvalue into independent owned storage");

    const auto moved = ServerCore::Core::Result<std::string>::FromValue(std::move(source));
    ServerCoreTest::ExpectEqual(std::string("y") + std::string(255, 'x'), moved.Value(),
        "FromValue preserves heap-backed strings passed as rvalues");

    auto pointer = std::make_unique<int>(42);
    const int* const original = pointer.get();
    const auto owned =
        ServerCore::Core::Result<std::unique_ptr<int>>::FromValue(std::move(pointer));
    ServerCoreTest::ExpectTrue(
        pointer == nullptr && owned.Value().get() == original && *owned.Value() == 42,
        "FromValue transfers a move-only value into the result exactly once");
}

void ResultValueIsMutable()
{
    ServerCore::Core::Result<int> result = ServerCore::Core::Result<int>::FromValue(1);
    result.Value() = 9;

    ServerCoreTest::ExpectEqual(9, result.Value(), "Result::Value() returns a mutable reference");
}

struct DefaultConstructionProbe
{
    inline static int constructions = 0;
    inline static int destructions = 0;
    DefaultConstructionProbe() { ++constructions; }
    DefaultConstructionProbe(const DefaultConstructionProbe&) { ++constructions; }
    DefaultConstructionProbe(DefaultConstructionProbe&&) noexcept { ++constructions; }
    DefaultConstructionProbe& operator=(const DefaultConstructionProbe&) = default;
    DefaultConstructionProbe& operator=(DefaultConstructionProbe&&) noexcept = default;
    ~DefaultConstructionProbe() { ++destructions; }
};

void ResultFailureDoesNotConstructValue()
{
    using namespace ServerCore::Core;
    const int constructionsBefore = DefaultConstructionProbe::constructions;
    const int destructionsBefore = DefaultConstructionProbe::destructions;
    {
        const auto failed = Result<DefaultConstructionProbe>::FromStatus(
            Status::Fail(ErrorCode::NotFound, "missing value"));
        auto copied = failed;
        const auto moved = std::move(copied);
        ServerCoreTest::ExpectTrue(!failed.IsOk() && !moved.IsOk() &&
                                       moved.GetStatus().Code() == ErrorCode::NotFound &&
                                       moved.GetStatus().Message() == "missing value",
            "copying and moving failure results preserves their error state");
        ServerCoreTest::ExpectEqual(constructionsBefore, DefaultConstructionProbe::constructions,
            "creating, copying and moving failed Results never constructs T");
    }
    ServerCoreTest::ExpectEqual(destructionsBefore, DefaultConstructionProbe::destructions,
        "destroying failed Results never destroys an uncreated T");
}

struct OwnedResultResource
{
    int value;
    int* destructions;
    ~OwnedResultResource() { ++*destructions; }
};

struct NonDefaultMoveOnlyValue
{
    NonDefaultMoveOnlyValue() = delete;
    explicit NonDefaultMoveOnlyValue(std::unique_ptr<OwnedResultResource> resource)
        : owned(std::move(resource))
    {
    }
    NonDefaultMoveOnlyValue(NonDefaultMoveOnlyValue&&) noexcept = default;
    NonDefaultMoveOnlyValue& operator=(NonDefaultMoveOnlyValue&&) noexcept = default;
    NonDefaultMoveOnlyValue(const NonDefaultMoveOnlyValue&) = delete;
    NonDefaultMoveOnlyValue& operator=(const NonDefaultMoveOnlyValue&) = delete;
    std::unique_ptr<OwnedResultResource> owned;
};

void ResultSupportsNonDefaultMoveOnlyValues()
{
    using namespace ServerCore::Core;
    using OwnedResult = Result<NonDefaultMoveOnlyValue>;
    static_assert(!std::is_default_constructible_v<OwnedResult>);
    static_assert(!std::is_copy_constructible_v<OwnedResult>);
    static_assert(std::is_move_constructible_v<OwnedResult>);
    static_assert(std::is_same_v<decltype(std::declval<const OwnedResult&>().Value()),
        const NonDefaultMoveOnlyValue&>);
    int destructions = 0;
    {
        auto failed = OwnedResult::FromStatus(Status::FailWithoutMessage(ErrorCode::Closed));
        ServerCoreTest::ExpectTrue(!failed.IsOk() && failed.GetStatus().Code() == ErrorCode::Closed,
            "failure results support non-default-constructible, move-only T");
        auto resource = std::make_unique<OwnedResultResource>(42, &destructions);
        const auto* identity = resource.get();
        auto original = OwnedResult::FromValue(NonDefaultMoveOnlyValue(std::move(resource)));
        auto moved = std::move(original);
        ServerCoreTest::ExpectTrue(!resource && moved.Value().owned.get() == identity &&
                                       moved.Value().owned->value == 42 && !original.Value().owned,
            "moving a Result transfers exclusive resource ownership without copying");
        moved = std::move(failed);
        ServerCoreTest::ExpectTrue(!moved.IsOk() && moved.GetStatus().Code() == ErrorCode::Closed,
            "move assignment can replace a successful result with its failure alternative");
        ServerCoreTest::ExpectEqual(1, destructions,
            "replacing a successful result releases its former resource exactly once");
        moved = OwnedResult::FromValue(
            NonDefaultMoveOnlyValue(std::make_unique<OwnedResultResource>(9, &destructions)));
        ServerCoreTest::ExpectTrue(moved.GetStatus().IsOk() &&
                                       moved.GetStatus().Message().empty() &&
                                       moved.Value().owned->value == 9,
            "replacing a failed result with a value restores the success Status contract");
    }
    ServerCoreTest::ExpectEqual(2, destructions,
        "each resource owned by a non-default move-only Result is destroyed exactly once");
}

const ServerCoreTest::CheckRegistration gOkStatusIsOk{ "Error.OkStatusIsOk", OkStatusIsOk };
const ServerCoreTest::CheckRegistration gFailStatusCarriesCodeAndMessage{
    "Error.FailStatusCarriesCodeAndMessage", FailStatusCarriesCodeAndMessage
};
const ServerCoreTest::CheckRegistration gUnimplementedStatusNamesWhat{
    "Error.UnimplementedStatusNamesWhat", UnimplementedStatusNamesWhat
};
const ServerCoreTest::CheckRegistration gResultFromValueHoldsValue{
    "Error.ResultFromValueHoldsValue", ResultFromValueHoldsValue
};
const ServerCoreTest::CheckRegistration gResultFromStatusHoldsStatus{
    "Error.ResultFromStatusHoldsStatus", ResultFromStatusHoldsStatus
};
const ServerCoreTest::CheckRegistration gResultTakeStatusMovesFailure{
    "Error.ResultTakeStatusMovesFailure", ResultTakeStatusMovesFailure
};
const ServerCoreTest::CheckRegistration gResultHoldsNonTrivialValue{
    "Error.ResultHoldsNonTrivialValue", ResultHoldsNonTrivialValue
};
const ServerCoreTest::CheckRegistration gResultValueIsMutable{ "Error.ResultValueIsMutable",
    ResultValueIsMutable };
const ServerCoreTest::CheckRegistration gResultFailureDoesNotConstructValue{
    "Error.ResultFailureDoesNotConstructValue", ResultFailureDoesNotConstructValue
};
const ServerCoreTest::CheckRegistration gResultSupportsNonDefaultMoveOnlyValues{
    "Error.ResultSupportsNonDefaultMoveOnlyValues", ResultSupportsNonDefaultMoveOnlyValues
};
}
