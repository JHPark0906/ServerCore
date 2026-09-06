#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Protocol/Message.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

/// <summary>
/// 세션 층. 연결 하나를 게임에 참여한 상대 하나로 승격시킨다.
/// </summary>
namespace ServerCore::Session
{
/// <summary>
/// 세션을 가리키는 번호다. 정수와 섞이지 않도록 강타입으로 둔다.
/// </summary>
/// <remarks>
/// 번호는 재사용되지 않는다. 끊긴 세션의 번호가 새 세션에 다시 붙으면, 뒤늦게 도착한 작업이
/// 엉뚱한 상대를 건드리게 된다.
/// </remarks>
enum class SessionId : std::uint64_t
{
    Invalid = 0
};

/// <summary>세션 번호가 실제 세션에 붙일 수 있는 값인지 본다.</summary>
/// <remarks>
/// Invalid는 아직 레지스트리에 등록할 수 없는 연결을 나타내는 예약값이다. 유효한 번호는
/// SessionRegistry::IssueId()만 발급한다.
/// </remarks>
[[nodiscard]] constexpr bool IsValid(SessionId id) noexcept
{
    return id != SessionId::Invalid;
}

/// <summary>세션의 수명 단계다.</summary>
enum class SessionState
{
    /// <summary>연결은 되었으나 아직 신원이 없다.</summary>
    Connected,
    /// <summary>신원이 확인되었다.</summary>
    Authenticated,
    /// <summary>끊는 중이다. 새 메시지를 받지 않는다.</summary>
    Closing,
    /// <summary>끊겼다.</summary>
    Closed
};

/// <summary>
/// 붙어 있는 상대 하나다. 게임 백엔드가 다루는 가장 낮은 단위다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 전송 층은 연결을 알고 게임은 플레이어를 안다. 그 사이를 잇는 것이 없으면 게임 코드가
/// 소켓 핸들을 들고 다니게 된다. 그리고 어떤 플레이어가 어디에 있는지를 보내려면 누구에게
/// 보낼지 아는 층이 반드시 있어야 한다.
///
/// 소유한다: 연결 하나, 프레임 리더 하나, 자신의 수명 상태.
/// 소유하지 않는다: 이 상대가 조종하는 게임 개체. 그것은 게임 백엔드의 것이다.
///
/// 소유권과 수명: 항상 std::shared_ptr로 다룬다. 보관해야 하면 std::weak_ptr로 잡는다.
/// 스레드 안전성: Send와 SendAndDisconnect와 Disconnect는 스레드 안전하다. MarkAuthenticated는
/// 게임 백엔드의 직렬 실행 문맥에서 부른다.
///
/// 약속하지 않는 것:
/// - 재연결과 세션 복원을 약속하지 않는다. 같은 사람이 다시 붙으면 그것은 다른 세션이다.
/// - 인증 정책이 없다. Authenticated로 옮길 조건과 신원의 내용은 게임 또는 인증 제공자가
///   정한다.
/// - Send는 도착을 약속하지 않는다. 보낼 큐에 들어갔다는 뜻이다.
/// - 게임 개체를 모른다. 코어 타입에 플레이어나 좌표라는 낱말은 없다.
/// - Id를 스스로 발급하지 않는다. ServerHost가 SessionRegistry::IssueId()로 발급한 값을
///   구현체를 만들 때 넘긴다. 구현체는 그 값을 수명 내내 바꾸지 않는다.
/// </remarks>
class Session
{
public:
    virtual ~Session() = default;

    [[nodiscard]] virtual SessionId Id() const noexcept = 0;

    [[nodiscard]] virtual SessionState State() const noexcept = 0;

    /// <summary>검증을 마친 신원이 있음을 세션 수명 상태에 반영한다.</summary>
    /// <remarks>
    /// 자격 증명을 검사하는 일은 하지 않는다. 그 정책은 게임 또는 인증 제공자의 몫이고, 이
    /// 함수는 검증이 성공한 뒤 Connected에서 Authenticated로 한 번 옮기는 자리일 뿐이다.
    /// 이미 인증됐으면 AlreadyExists, 닫는 중이거나 닫혔으면 Closed를 돌려준다.
    /// </remarks>
    [[nodiscard]] virtual Core::Status MarkAuthenticated() = 0;

    /// <summary>이 상대에게 봉투 하나를 보낸다.</summary>
    /// <param name="fields">type, body, seq, error를 담은 직렬화 전 봉투 필드.</param>
    /// <returns>이미 끊긴 세션이면 ErrorCode::Closed.</returns>
    [[nodiscard]] virtual Core::Status Send(const Protocol::MessageFields& fields) = 0;

    /// <summary>이미 검증·직렬화한 봉투를 보낸다. 성공·종료·큐 압력 계약은 Send와 같다.</summary>
    /// <remarks>NetworkSession은 원본 바이트에 프레이밍만 한다. 사용자 정의 세션의 기본
    /// 구현은 기존 Send를 재사용해 소스 호환성을 보존한다. 호출 후 송신 큐가 바이트를 소유한다.</remarks>
    [[nodiscard]] virtual Core::Status SendPrepared(const Protocol::PreparedMessage& prepared)
    {
        if (prepared.Size() == 0) return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
        auto message = Protocol::ParseMessage(prepared.Bytes());
        if (!message.IsOk()) return std::move(message).TakeStatus();
        const auto& value = message.Value();
        return Send({value.Type(), value.Body(), value.Sequence(), value.Error()});
    }

    /// <summary>전송 큐의 미완료 바이트 관측값이다. 예약이나 이후 송신 성공을 보장하지 않는다.</summary>
    /// <remarks>사용자 정의 세션의 기본 구현은 계측을 제공하지 않아 0을 반환한다.</remarks>
    [[nodiscard]] virtual std::size_t QueuedSendBytes() const noexcept { return 0; }

    /// <summary>마지막 봉투를 보낼 큐에 넣고, 그 큐를 비운 뒤 이 세션을 닫는다.</summary>
    /// <param name="fields">type, body, seq, error를 담은 마지막 직렬화 전 봉투 필드.</param>
    /// <param name="reason">로컬 세션 종료 사유. wire error 봉투와는 별개로 기록에 남는다.</param>
    /// <returns>
    /// 마지막 봉투가 Connection 송신 큐에 들어가고 drain 요청까지 끝나면 성공. 이미 닫는 중이거나
    /// 닫혔으면 ErrorCode::Closed.
    /// </returns>
    /// <remarks>
    /// 이 함수와 Send(), Disconnect()의 동시 호출은 한 세션 안에서 차례로 정해진다. 이 함수보다
    /// 먼저 수락된 Send()는 마지막 봉투보다 앞에 남고, 뒤에 오는 Send()는 Closed로 거절된다.
    ///
    /// 성공은 이 프로세스의 송신 큐에 마지막 봉투가 들어가고 그 큐를 비우도록 요청했다는 뜻이다.
    /// 원격 애플리케이션의 수신을 확인하지 않으며, 반환 뒤 Disconnect(), ServerHost::Stop(), 또는
    /// peer 종료가 오면 즉시 종료가 drain을 중단해 마지막 바이트가 버려질 수 있다.
    /// ServerHost는 ServerHostOptions::gracefulCloseTimeout도 적용한다. Closing 시작 뒤 그
    /// 시간이 지나면 수신 활동과 관계없이 남은 송신을 버리고 닫으므로, 성공은 도착 보장이 아니다.
    /// </remarks>
    [[nodiscard]] virtual Core::Status SendAndDisconnect(
        const Protocol::MessageFields& fields, Core::Status reason) = 0;

    /// <summary>body만 가진 보통 봉투를 보내는 편의 함수다.</summary>
    /// <remarks>
    /// 응답 상관을 위한 seq나 error를 실어야 하면 MessageFields 버전을 쓴다. 이 함수는
    /// 프레이밍이나 직렬화를 직접 하지 않고 구현체의 Send(MessageFields)에 맡긴다.
    /// </remarks>
    [[nodiscard]] Core::Status Send(std::string_view type, const Protocol::JsonValue& body)
    {
        return Send(Protocol::MessageFields{ type, &body, nullptr, nullptr });
    }

    /// <summary>이 세션을 즉시 끊는다. 여러 번 불러도 된다.</summary>
    /// <param name="reason">왜 끊는지. 기록에 남는다.</param>
    /// <remarks>
    /// 아직 보내지 못한 바이트는 버린다. SendAndDisconnect()가 큐 drain을 요청한 뒤에도 이 함수는
    /// 즉시 종료를 택한다. Host 종료가 느린 peer의 송신을 기다리지 않게 하려는 경계다.
    /// 구현은 예외를 던지지 않아야 한다. 실행 환경은 이 함수를 모든 세션의 필수 종료 경계로 쓰며,
    /// 한 구현의 예외 뒤에는 그 세션의 종료 완료를 안전하게 기다릴 방법이 없다.
    /// </remarks>
    virtual void Disconnect(Core::Status reason) = 0;
};

/// <summary>
/// 세션이 열리고 닫히는 것을 알고 싶은 쪽이 구현한다. 게임 백엔드의 자리다.
/// </summary>
/// <remarks>
/// 누가 이것을 붙이는가:
/// 실행 환경 층의 ServerHost::SetSessionObserver로 게임 백엔드가 부팅 때 건다.
/// 약한 참조로 잡으므로 코어가 관찰자를 살려 두지 않는다.
///
/// 약속하지 않는 것:
/// - 어느 스레드에서 불리는지는 실행 환경 층이 정한다. 여기서 고정하지 않는다.
/// - OnSessionAuthenticated는 ServerHost가 MarkAuthenticated()의 성공 뒤에 부른다. Session
///   자체는 관찰자를 소유하지 않는다.
/// - OnSessionClosed가 불린 뒤에 그 세션으로 보내는 것은 실패한다.
///
/// 호출 계약:
/// - 세 함수에서 예외를 던지지 않는다. 실행 환경이 소유한 콜백 경계 밖으로 예외가 나가면 이후
///   세션 순서와 종료 절차를 계속할 안전한 지점이 없다. 구현이 예외를 던지면 계약 위반으로
///   프로세스를 끝낸다.
/// </remarks>
class ISessionObserver
{
public:
    virtual ~ISessionObserver() = default;

    virtual void OnSessionOpened(const std::shared_ptr<Session>& session) = 0;

    /// <summary>세션이 Connected에서 Authenticated로 옮겨진 직후 알린다.</summary>
    /// <remarks>
    /// 신원의 내용은 게임별 정책이므로 이 훅에 싣지 않는다. 기존 관찰자는 이 이벤트가 필요
    /// 없으면 아무것도 구현하지 않아도 된다.
    /// </remarks>
    virtual void OnSessionAuthenticated(const std::shared_ptr<Session>& session) { (void)session; }

    virtual void OnSessionClosed(SessionId id, Core::Status reason) = 0;
};
}
