#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/Message.h"
#include "ServerCore/Session/Session.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

/// <summary>
/// 디스패치 층. 메시지를 그것을 처리할 함수로 보낸다. 게임 백엔드가 붙는 경계다.
/// </summary>
namespace ServerCore::Dispatch
{
/// <summary>
/// 메시지 하나를 처리하는 함수다. 게임 백엔드가 제공한다.
/// </summary>
/// <remarks>
/// 호출 계약:
/// - 예외를 던지지 않는다. 실패는 Status로 돌려준다.
/// - Dispatcher가 body 존재와 등록된 raw body 상한을 먼저 검사한 전체 봉투를 받는다. body는
///   널이 아니며, Sequence()와 Error()는 널일 수 있다.
/// - Message와 거기서 얻은 포인터·참조는 이 호출 안에서만 쓴다. 나중에도 필요하면 게임이
///   자기 값으로 복사한다.
/// - 어느 스레드에서 불리는지는 실행 환경 층이 정한다.
/// - 오래 걸리는 일을 하면 그 스레드가 다른 메시지를 못 본다. 제한 시간은 걸려 있지 않다.
/// </remarks>
using MessageHandler = std::function<Core::Status(
    const std::shared_ptr<Session::Session>& session, const Protocol::Message& message)>;

/// <summary>등록되지 않은 타입이 왔을 때 무엇을 할지다.</summary>
enum class UnknownTypePolicy
{
    /// <summary>
    /// 기록하고 넘어간다. 기본값이다.
    /// 두 게임이 각자 다른 속도로 메시지를 늘리므로, 클라이언트가 서버보다 먼저 새 타입을
    /// 보내는 상황이 정상적으로 생긴다. 그때마다 끊으면 버전이 반 발짝만 어긋나도 접속이
    /// 안 된다. 다만 조용히 넘어가지는 않는다. 반드시 기록한다.
    /// </summary>
    LogAndIgnore,
    /// <summary>연결을 끊는다. 엄격하게 다뤄야 하는 게임이 고른다.</summary>
    Disconnect
};

/// <summary>등록된 타입의 raw body 크기에 적용할 기본 상한이다.</summary>
/// <remarks>
/// 프레임 전체의 절대 상한은 L2가 이미 적용한다. 이 값은 특정 타입에 더 작은 상한이
/// 필요하지 않을 때 쓰는 "추가 제한 없음" 표시다.
/// </remarks>
inline constexpr std::size_t UnlimitedRawBodySize = (std::numeric_limits<std::size_t>::max)();

/// <summary>
/// 메시지 타입에서 처리기로 가는 표다.
/// </summary>
/// <remarks>
/// 소유한다: 등록표와 라우팅 규칙.
/// 소유하지 않는다: 처리기의 내용. 그것이 게임 백엔드다. 그리고 소켓을 모른다.
///
/// 호출 계약:
/// - 등록은 부팅 때만 하고, 수락을 시작하기 전에 Freeze()로 닫는다. Register와 Freeze는
///   서로 다른 스레드에서 불러도 되며, 등록 게이트를 먼저 얻은 쪽이 먼저 끝난다. Register
///   들끼리도 게이트로 직렬화한다. Freeze가 돌아오면 그보다 먼저 게이트를 얻은 Register는
///   모두 끝났고, 그 뒤 Register는 Closed다.
/// - Freeze 전 Dispatch는 등록표를 읽는 동안만 공유 게이트를 잡고, 처리기는 게이트 밖에서
///   부른다. 따라서 Register 및 Freeze와 동시에 불러도 된다.
/// - Freeze가 돌아온 뒤에는 표를 더 바꾸지 않으므로 Dispatch는 등록표를 잠그지 않고 읽는다.
/// - Dispatch는 부른 스레드에서 그대로 실행된다. 이 층은 스레드를 만들지 않는다.
///
/// 약속하지 않는 것:
/// - 처리기의 실행 순서를 약속하지 않는다. 같은 세션의 두 메시지가 순서대로 처리되는지는
///   실행 환경 층이 어떤 실행자를 물리느냐에 달렸다(미정).
/// - 여기서 연결을 직접 끊을 수 없다. 끊는 것은 Session을 통해서만 한다.
/// </remarks>
class Dispatcher
{
public:
    /// <summary>타입 하나에 처리기를 건다.</summary>
    /// <param name="type">봉투의 type과 정확히 같아야 한다. 대소문자를 구분한다.</param>
    /// <param name="maximumRawBodySize">
    /// body 값이 wire에서 차지한 UTF-8 바이트 상한이다. 글자 수가 아니라 바이트로 재며,
    /// UnlimitedRawBodySize면 L2 프레임 상한 외에는 더 제한하지 않는다.
    /// </param>
    /// <returns>
    /// 같은 타입이 이미 있으면 ErrorCode::AlreadyExists, Freeze 뒤면 ErrorCode::Closed,
    /// 빈 타입이나 비어 있는 처리기면 ErrorCode::InvalidArgument. 등록표를 준비하다 표준
    /// 라이브러리 예외가 나면 ErrorCode::PlatformError로 바꿔 돌려준다.
    /// Freeze와 동시에 부르면 등록 게이트를 먼저 얻은 쪽이 먼저 끝난다. Freeze가 먼저 닫으면
    /// 이 호출은 ErrorCode::Closed다.
    /// </returns>
    Core::Status Register(std::string_view type, MessageHandler handler,
        std::size_t maximumRawBodySize = UnlimitedRawBodySize);

    /// <summary>등록표를 닫는다.</summary>
    /// <remarks>
    /// ServerHost는 포트를 열기 직전에 한 번 부른다. 여러 번 불러도 된다. 이 호출 뒤의
    /// Register는 ErrorCode::Closed를 돌려준다. 이미 등록 게이트 안에 들어간 Register가
    /// 있으면 그것이 끝날 때까지 기다린 뒤 표를 닫으므로, 이 함수가 돌아온 뒤에는 실행 중 표
    /// 변경이 읽기 경합으로 바뀌지 않는다.
    /// </remarks>
    void Freeze() noexcept;

    /// <summary>등록표가 닫혔는지 준다.</summary>
    [[nodiscard]] bool IsFrozen() const noexcept;

    /// <summary>메시지를 그 타입의 처리기로 보낸다.</summary>
    /// <returns>
    /// 등록된 처리기가 없으면 정책에 따른다. 처리기가 실패를 돌려주면 그것을 그대로 돌려준다.
    /// 라우팅 준비 중 표준 라이브러리 예외가 나면 ErrorCode::PlatformError로 바꿔 돌려준다.
    /// 처리기 자신은 MessageHandler의 예외 금지 계약을 지켜야 한다. Disconnect 정책에서 호출하는
    /// Session::Disconnect도 그 함수의 예외 금지 계약을 지켜야 하며, 그 계약 위반은 라우팅
    /// 실패로 숨기지 않고 호출자에게 그대로 드러낸다.
    /// </returns>
    Core::Status Dispatch(
        const std::shared_ptr<Session::Session>& session, const Protocol::Message& message);

    void SetUnknownTypePolicy(UnknownTypePolicy policy) noexcept;

    [[nodiscard]] UnknownTypePolicy GetUnknownTypePolicy() const noexcept;

private:
    struct TransparentStringHash
    {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct RegisteredHandler
    {
        MessageHandler handler;
        std::size_t maximumRawBodySize = UnlimitedRawBodySize;
    };

    // 표가 바뀔 때는 배타적으로, 아직 열려 있는 표를 Dispatch할 때는 핸들러 포인터를
    // 복사할 때까지만 공유로 잡는다. Freeze가 끝난 뒤의 Dispatch는 이 잠금을 잡지 않고
    // mFrozen의 acquire 관찰로 완성된 등록표를 읽는다.
    std::shared_mutex mRegistrationMutex;
    std::unordered_map<std::string, std::shared_ptr<const RegisteredHandler>, TransparentStringHash,
        std::equal_to<>>
        mHandlers;
    std::atomic<bool> mFrozen{ false };
    std::atomic<UnknownTypePolicy> mUnknownTypePolicy{ UnknownTypePolicy::LogAndIgnore };
};
}
