#pragma once

#include "ServerCore/Core/Assert.h"

#include <string>
#include <string_view>
#include <utility>

/// <summary>
/// 기반 층. 서버라는 것을 모르는 도구들이 모인다.
/// </summary>
namespace ServerCore::Core
{
/// <summary>
/// 코어 전체가 공유하는 실패 종류다.
/// 여기 있는 것은 전부 "예상되는 실패"이며, 프로그래밍 오류(계약 위반)는 이 목록에 없다.
/// 계약 위반은 값으로 알리지 않고 단언으로 즉시 드러낸다.
/// </summary>
enum class ErrorCode
{
    /// <summary>실패가 아니다.</summary>
    Ok = 0,
    /// <summary>호출자가 넘긴 값이 이 함수가 받을 수 있는 범위 밖이다.</summary>
    InvalidArgument = 1,
    /// <summary>와이어에서 온 바이트가 규격을 지키지 않았다. 깨진 JSON, 없는 필드 따위.</summary>
    InvalidFormat = 2,
    /// <summary>규격이 정한 상한을 넘었다. 프레임 본문 크기 상한이 대표적이다.</summary>
    TooLarge = 3,
    /// <summary>찾는 것이 없다.</summary>
    NotFound = 4,
    /// <summary>이미 있는 것을 또 만들려 했다. 같은 메시지 타입을 두 번 등록하는 경우 따위.</summary>
    AlreadyExists = 5,
    /// <summary>대상이 이미 닫혔다. 끊긴 연결로 보내려는 경우 따위.</summary>
    Closed = 6,
    /// <summary>지금은 줄 것이 없다. 프레임이 아직 다 안 왔을 때 쓴다. 실패가 아니라 "아직"이다.</summary>
    WouldBlock = 7,
    /// <summary>
    /// 운영체제 호출이나 자원 확보가 실패했다. 자세한 것은 Status의 설명 문자열에 담되, 설명
    /// 자체를 안전하게 소유할 수 없는 할당 실패에서는 빈 문자열일 수 있다.
    /// </summary>
    PlatformError = 8,
    /// <summary>
    /// 구현되지 않은 연산을 나타낸다. 미구현 경로가 성공으로 처리되지 않게 한다.
    /// </summary>
    Unimplemented = 9,
    /// <summary>등록되지 않은 메시지 타입을 받았다.</summary>
    /// <remarks>
    /// 새 값은 기존 오류 코드의 수치를 바꾸지 않도록 끝에만 더한다. 이 열거형의 수치를
    /// 네트워크·파일 형식으로 직접 저장하는 계약은 제공하지 않지만, 진단 데이터의 호환성을
    /// 불필요하게 깨지 않기 위한 경계다.
    /// </remarks>
    UnknownType = 10,
    /// <summary>필요한 활동이 정해진 시간 안에 오지 않았다.</summary>
    /// <remarks>
    /// 네트워크 연결의 유휴 만료처럼, 호출자가 설정한 시간 경계가 지나 발생한 예상 가능한
    /// 종료를 나타낸다. 새 값은 기존 오류 코드의 수치를 바꾸지 않도록 끝에만 더한다.
    /// </remarks>
    Timeout = 11
};

/// <summary>
/// 값을 돌려주지 않는 연산의 결과다.
/// </summary>
/// <remarks>
/// 왜 예외가 아닌가:
/// 이 코어의 입력은 와이어 건너편에서 온다. 깨진 JSON은 드문 사건이 아니라 정상적으로 예상되는
/// 입력이고, 예외는 드문 사건을 위한 도구다. 그리고 서버는 잘못된 패킷 하나에 대해 그 연결만
/// 끊고 계속 돌아야 한다. 마지막으로 IOCP 완료는 I/O 스레드에서 실행되므로 거기서 던진 예외를
/// 받을 호출자가 없다.
///
/// 스레드 안전성: 값 타입이며 공유 상태가 없다. 복사해서 스레드 사이로 넘겨도 된다.
///
/// 약속하지 않는 것:
/// - Message()의 문자열은 사람이 읽기 위한 것이다. 기계가 그 문자열로 분기하면 안 된다.
///   문구는 예고 없이 바뀐다. 분기는 Code()로만 한다.
/// - 원인 짐작을 담지 않는다. 무엇이 기대와 달랐는지만 담는다. 짐작은 코드가 변한 뒤에도
///   그 자리에 남아 다음 사람의 첫 가설을 낡은 것으로 고정한다.
///
/// 왜 [[nodiscard]]가 클래스에 붙어 있는가:
/// 예상되는 실패를 호출자가 처리한다는 계약을 [[nodiscard]]가 컴파일 시간에 확인한다.
/// 이 프로젝트는 /WX로 빌드하므로, 반환된 Status를 명시적으로 처리하지 않으면 빌드가 실패한다.
///
/// 일부러 무시해야 하는 자리:
/// 종료 중의 최선 노력 전송처럼 실패를 정말로 무시하는 것이 옳은 자리가 있다. 그때는 (void)로
/// 명시적으로 버린다. 그 (void)가 "여기서는 무시하기로 했다"는 표시가 되고, 표시가 없으면
/// 무시한 것인지 잊은 것인지 나중에 구분할 수 없다.
/// </remarks>
class [[nodiscard]] Status
{
public:
    /// <summary>성공을 뜻하는 Status를 만든다.</summary>
    static Status Ok() noexcept;

    /// <summary>실패를 뜻하는 Status를 만든다.</summary>
    /// <param name="code">실패 종류. ErrorCode::Ok를 넘기는 것은 계약 위반이다.</param>
    /// <param name="message">무엇이 기대와 달랐는지에 대한 설명.</param>
    static Status Fail(ErrorCode code, std::string message);

    /// <summary>설명 문자열을 할당하지 않는 실패를 만든다.</summary>
    /// <param name="code">실패 종류. ErrorCode::Ok를 넘기는 것은 계약 위반이다.</param>
    /// <remarks>
    /// 종료나 자원 고갈처럼 새 문자열을 확보하지 않아야 하는 경로에서 쓴다. Message()는 빈
    /// 문자열을 돌려준다.
    /// </remarks>
    static Status FailWithoutMessage(ErrorCode code) noexcept;

    /// <summary>할당 실패를 두 번째 할당 없이 나타내는 표준 실패다.</summary>
    /// <remarks>
    /// std::bad_alloc을 잡은 뒤 Fail()에 설명 문자열을 넘기면 그 문자열을 소유하는 과정에서
    /// 다시 할당에 실패할 수 있다. 이 함수는 PlatformError와 빈 설명을 가진 Status를 만들며
    /// 메모리 할당을 시도하지 않는다.
    /// </remarks>
    static Status AllocationFailure() noexcept;

    /// <summary>구현되지 않은 연산의 표준 실패다.</summary>
    /// <param name="what">아직 구현되지 않은 대상의 이름.</param>
    static Status Unimplemented(std::string_view what);

    [[nodiscard]] ErrorCode Code() const noexcept;
    [[nodiscard]] bool IsOk() const noexcept;
    [[nodiscard]] const std::string& Message() const noexcept;

private:
    ErrorCode mCode = ErrorCode::Ok;
    std::string mMessage;
};

/// <summary>
/// 값을 돌려주는 연산의 결과다. 성공이면 값을, 실패면 Status를 담는다.
/// </summary>
/// <remarks>
/// 호출 계약:
/// IsOk()가 거짓인데 Value()를 부르는 것은 계약 위반이다. 이때 오류 값을 돌려주지 않고
/// 단언으로 끊는다. 그 자리에 무언가를 돌려주면 호출자가 자기 버그를 조용히 넘기게 된다.
///
/// 스레드 안전성: 값 타입이며 공유 상태가 없다.
///
/// 약속하지 않는 것:
/// - 값의 유효성을 검사하지 않는다. 성공이라는 것은 연산이 성공했다는 뜻이지
///   담긴 값이 쓰기 좋다는 뜻이 아니다.
///
/// 왜 기본 생성자가 없는가:
/// 기본 생성이 되면 Result&lt;int&gt; r; 이 성공을 담은 것처럼 보인다. mStatus의 기본값이 Ok이기
/// 때문이다. 값을 넣은 적이 없는데 IsOk()가 참인 것은 그 자체로 결함이므로 막는다.
/// 만드는 길은 FromValue와 FromStatus 둘뿐이다.
///
/// 알려진 제약:
/// - T는 기본 생성 가능해야 하고, 실패일 때도 T가 하나 만들어진다. 그 값은 쓰이지 않지만
///   자리는 차지한다.
/// - T가 Status인 경우는 지원하지 않는다. 두 비공개 생성자가 구분되지 않아 컴파일이 실패한다.
///   조용히 잘못 도는 것이 아니라 그 자리에서 멈춘다.
///
/// [[nodiscard]]를 붙인 이유와 일부러 버릴 때 쓰는 (void)에 대해서는 Status의 설명을 보라.
/// </remarks>
template <typename T> class [[nodiscard]] Result
{
public:
    /// <summary>기본 생성을 막는다. 위 "왜 기본 생성자가 없는가"를 보라.</summary>
    Result() = delete;

    /// <summary>성공 결과를 만든다.</summary>
    static Result FromValue(T value);

    /// <summary>실패 결과를 만든다. status가 성공을 담고 있으면 계약 위반이다.</summary>
    static Result FromStatus(Status status);

    [[nodiscard]] bool IsOk() const noexcept;
    [[nodiscard]] const Status& GetStatus() const noexcept;

    /// <summary>실패 Status의 소유권을 꺼낸다.</summary>
    /// <remarks>
    /// IsOk()가 거짓일 때만 부를 수 있다. 오류 설명 문자열을 복사하지 않고 다음 경계로
    /// 넘겨야 하는 자리에 쓴다. 이 호출 뒤에도 IsOk()는 거짓이지만, 꺼낸 설명 문자열은 더
    /// 이상 이 Result에 남아 있다고 약속하지 않는다.
    /// </remarks>
    [[nodiscard]] Status TakeStatus() && noexcept;

    /// <summary>담긴 값을 준다. IsOk()가 참일 때만 부를 수 있다.</summary>
    [[nodiscard]] T& Value();

    /// <summary>담긴 값을 준다. IsOk()가 참일 때만 부를 수 있다.</summary>
    [[nodiscard]] const T& Value() const;

private:
    /// <summary>성공 결과를 만든다. mStatus는 기본값 그대로 Ok다.</summary>
    explicit Result(T value);

    /// <summary>실패 결과를 만든다. mValue는 기본 생성된 채로 남고 쓰이지 않는다.</summary>
    explicit Result(Status status);

    Status mStatus;
    T mValue{};
};

// Result의 멤버 정의가 소스 파일이 아니라 여기 있는 이유:
// 템플릿의 멤버 정의는 인스턴스화되는 자리에서 보여야 한다. 소스 파일에 두면 그 자리에서
// 보이지 않아 링크되지 않고, 명시적 인스턴스화로 막을 수도 없다. 어떤 T로 쓰이는지는
// 위층과 소비자가 정하기 때문이다.

template <typename T>
Result<T>::Result(T value)
    : mStatus(Status::Ok())
    , mValue(std::move(value))
{
}

template <typename T>
Result<T>::Result(Status status)
    : mStatus(std::move(status))
{
}

template <typename T> Result<T> Result<T>::FromValue(T value)
{
    return Result(std::move(value));
}

template <typename T> Result<T> Result<T>::FromStatus(Status status)
{
    SERVERCORE_ASSERT(!status.IsOk(), "FromStatus() was given a status that is Ok");
    return Result(std::move(status));
}

template <typename T> bool Result<T>::IsOk() const noexcept
{
    return mStatus.IsOk();
}

template <typename T> const Status& Result<T>::GetStatus() const noexcept
{
    return mStatus;
}

template <typename T> Status Result<T>::TakeStatus() && noexcept
{
    SERVERCORE_ASSERT(!IsOk(), "TakeStatus() was called while IsOk() is true");
    return std::move(mStatus);
}

template <typename T> T& Result<T>::Value()
{
    SERVERCORE_ASSERT(IsOk(), "Value() was called while IsOk() is false");
    return mValue;
}

template <typename T> const T& Result<T>::Value() const
{
    SERVERCORE_ASSERT(IsOk(), "Value() was called while IsOk() is false");
    return mValue;
}
}
