#pragma once

#include "ServerCore/Session/Session.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace ServerCore::Session
{
/// <summary>
/// 지금 붙어 있는 세션들의 목록이다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 상태 동기화는 본질적으로 여럿에게 보내는 일이고, 그 여럿의 목록을 누군가는 들고 있어야
/// 한다. 게임마다 따로 만들면 두 게임이 서로 다른 방식으로 같은 목록을 관리하게 된다.
///
/// 상태 소유와 실행 문맥:
/// - 목록의 등록과 읽기는 하나의 직렬 JobRunner 문맥에서만 한다. ServerHost가 그 JobRunner
///   안에서 BindToCurrentThread()를 한 번 부른 뒤, 같은 문맥에서 해당 API를 쓴다.
/// - Unregister()는 실행자에 정리 작업을 넣을 수 없는 자원 고갈 경계에서도 세션을 남기지 않도록
///   어느 스레드에서나 부를 수 있다. 내부 잠금은 그 제거와 직렬 문맥의 짧은 목록 접근만
///   직렬화하고 visitor 호출 전에는 놓으므로 재진입 교착을 만들지 않는다.
/// - IssueId()와 DiscardIssuedId()만 예외다. 연결을 만든 I/O 스레드에서도 부를 수 있도록
///   발급과 예약 폐기를 잠금으로 직렬화한다. 발급한 번호는 이 레지스트리 안에 등록 대기
///   번호로 기록한다. 발급만 되고 등록되지 않은 번호는 폐기해도 된다.
///
/// 약속하지 않는 것:
/// - ForEach는 호출 시점의 얕은 스냅숏을 순회한다. visitor가 등록/해제해도 이번 순회 대상은
///   바뀌지 않는다.
/// - 브로드캐스트의 원자성이 없다. 절반은 받고 절반은 못 받은 상태가 정상적으로 존재한다.
///   보내는 도중에 끊긴 상대가 있으면 그쪽만 실패한다.
/// - 순서를 약속하지 않는다. ForEach가 도는 차례는 정해져 있지 않다.
/// - 방이나 구역 같은 게임별 묶음을 관리하지 않는다.
/// </remarks>
class SessionRegistry
{
public:
    SessionRegistry();
    ~SessionRegistry();

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;
    SessionRegistry(SessionRegistry&&) = delete;
    SessionRegistry& operator=(SessionRegistry&&) = delete;

    /// <summary>이 레지스트리를 현재 직렬 실행 문맥에 묶는다.</summary>
    /// <remarks>
    /// ServerHost는 JobRunner::RunUntilStopped 안에서 이 함수를 한 번 불러야 한다. 같은
    /// 스레드에서의 반복 호출은 성공으로 본다. 다른 스레드로 다시 묶는 것은 지원하지 않는다.
    /// 이 함수와 목록 API를 동시에 부르는 것은 계약 위반이다.
    /// </remarks>
    [[nodiscard]] Core::Status BindToCurrentThread();

    /// <summary>새 세션 번호를 발급한다.</summary>
    /// <remarks>
    /// 프로세스 전체에서 원자적으로 단조 증가하며 재사용하지 않는다. 번호 공간이 모두
    /// 소진되면 TooLarge를 돌려준다. 발급 결과는 이 레지스트리에서 한 번 등록할 수 있도록
    /// 예약된다. 이 함수는 목록을 바꾸지 않으므로 어떤 스레드에서도 부를 수 있다.
    /// </remarks>
    [[nodiscard]] Core::Result<SessionId> IssueId();

    /// <summary>아직 등록하지 않을 발급 번호의 예약을 버린다.</summary>
    /// <remarks>
    /// 연결을 받아 번호를 발급한 뒤 세션을 만들거나 등록하기 전에 실패한 경로가 이 함수를
    /// 쓴다. 성공하면 해당 번호는 더는 Register()할 수 없지만, 프로세스 전체 번호를
    /// 되돌리거나 다른 세션에 재사용하지는 않는다.
    ///
    /// 이 함수는 IssueId()와 마찬가지로 목록을 바꾸지 않으므로 어떤 스레드에서도 부를 수
    /// 있다. Register()와 동시에 같은 번호를 처리하면 둘 중 먼저 예약 잠금을 얻은 쪽만
    /// 성공한다. Invalid 번호, 다른 레지스트리가 발급한 번호, 이미 등록·폐기된 번호는
    /// InvalidArgument를 돌려준다.
    /// </remarks>
    [[nodiscard]] Core::Status DiscardIssuedId(SessionId id);

    /// <summary>발급된 번호를 가진 세션을 목록에 넣는다.</summary>
    /// <remarks>
    /// BindToCurrentThread()로 묶은 직렬 문맥에서만 부를 수 있다. session이 널이거나
    /// Invalid 번호를 가지면 InvalidArgument, 이 레지스트리의 IssueId()가 아직 등록하지 않은
    /// 번호가 아니어도 InvalidArgument, 같은 번호가 이미 있으면 AlreadyExists다. 성공한
    /// 등록은 번호의 예약을 소비하므로 해제한 번호를 다시 등록할 수 없다. Session의 Id는
    /// 등록 뒤에도 바뀌지 않아야 한다.
    /// </remarks>
    [[nodiscard]] Core::Status Register(const std::shared_ptr<Session>& session);

    /// <summary>번호에 해당하는 세션을 목록에서 뺀다.</summary>
    /// <remarks>
    /// 어느 스레드에서나 부를 수 있다. 이 함수는 Disconnect나 관찰자 통지를 하지 않는다.
    /// 전송 수명과 게임 통지는 ServerHost의 책임이다. 같은 번호의 등록과 경합하면 목록 잠금으로
    /// 순서가 정해진다.
    /// </remarks>
    [[nodiscard]] Core::Status Unregister(SessionId id);

    /// <summary>번호로 세션을 찾는다. 없으면 비어 있는 포인터를 준다.</summary>
    /// <remarks>BindToCurrentThread()로 묶은 직렬 문맥에서만 부를 수 있다.</remarks>
    [[nodiscard]] std::shared_ptr<Session> Find(SessionId id) const;

    /// <summary>모든 세션을 한 번씩 준다.</summary>
    /// <param name="visitor">
    /// 각 세션에 대해 부른다. 이 안에서 Register나 Unregister를 불러도 된다. visitor는
    /// 직렬 실행 문맥을 오래 점유해서는 안 된다.
    /// </param>
    /// <remarks>BindToCurrentThread()로 묶은 직렬 문맥에서만 부를 수 있다.</remarks>
    void ForEach(const std::function<void(const std::shared_ptr<Session>&)>& visitor) const;

    /// <summary>현재 등록된 세션 수를 준다.</summary>
    /// <remarks>BindToCurrentThread()로 묶은 직렬 문맥에서만 부를 수 있다.</remarks>
    [[nodiscard]] std::size_t Count() const noexcept;

private:
    void RequireMutationThread() const;

    class State;
    std::unique_ptr<State> mState;
};
}
