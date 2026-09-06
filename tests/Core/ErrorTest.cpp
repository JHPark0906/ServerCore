#include "TestHarness.h"

#include "ServerCore/Core/Error.h"

#include <string>

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
}

void ResultHoldsNonTrivialValue()
{
    ServerCore::Core::Result<std::string> result =
        ServerCore::Core::Result<std::string>::FromValue(std::string("PlayerMove"));

    ServerCoreTest::ExpectTrue(result.IsOk(), "Result<std::string>::FromValue() reports IsOk");
    ServerCoreTest::ExpectEqual(
        std::string("PlayerMove"), result.Value(), "Result<std::string> holds the value");
}

void ResultValueIsMutable()
{
    ServerCore::Core::Result<int> result = ServerCore::Core::Result<int>::FromValue(1);
    result.Value() = 9;

    ServerCoreTest::ExpectEqual(9, result.Value(), "Result::Value() returns a mutable reference");
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
}
