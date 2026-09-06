#pragma once

#include "ServerCore/Core/Error.h"
#include "ServerCore/Net/Connection.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace ServerCore::Net
{
class AcceptorAccess;
class IoContext;

/// <summary>
/// 포트를 열고 들어오는 연결을 받는다.
/// </summary>
/// <remarks>
/// 소유한다: 리슨 소켓 하나와, 아직 완료되지 않은 수락 요청이 쓰는 소켓들.
/// 소유하지 않는다: IoContext. 비소유 관찰자로 들고 있을 뿐이다. 만들어진 연결도 소유하지
/// 않는다. 그 소유권은 수락 처리기에게 넘어간다.
///
/// 소유권과 수명:
/// IoContext보다 오래 살면 안 된다. Stop()이 진행 중인 수락 요청과 이미 완료된 수락의
/// 처리기 인계가 전부 정리될 때까지 기다리므로, 그 뒤에는 이 객체를 가리키는 완료나 인계
/// 코드가 남지 않는다. 소멸자가 Stop()을 부르므로 호출자가 잊어도 요청이 진행 중인 채로
/// 이 객체가 사라지지 않는다. 그 둘이 위 "오래 살면 안 된다"를 지키는 수단이다.
///
/// 복사와 이동을 지운 이유는 IoContext와 같다. 리슨 소켓을 소유하는 것이 복사되면 같은
/// 소켓을 두 번 닫는다. /W4 /WX는 그것을 잡지 않으므로 아래의 = delete가 유일한 수단이다.
///
/// 스레드 안전성: Listen()·Start()·SetConnectionHandler()는 부팅 스레드에서 부른다.
/// Stop()은 스레드 안전하되 I/O 스레드에서 부르면 계약 위반이다. 수락 처리기는 I/O
/// 스레드에서 불린다.
///
/// endpoint 정책:
/// IPv4 주소와 0이 아닌 TCP 포트만 이 층이 판정한다. 프로젝트별 포트 대역, 외부 공개 여부,
/// 방화벽 규칙은 이 라이브러리를 쓰는 서버 프로그램이 정한다. 그래야 서로 다른 서버가 같은
/// 전송 라이브러리를 써도 각자의 배포 정책을 가질 수 있다.
///
/// 약속하지 않는 것:
/// - 수락 처리기가 불린 시점에 그 연결이 아직 살아 있다고 약속하지 않는다. 붙자마자 끊는
///   상대가 있다.
/// - 접속을 제한하지 않는다. 동시 접속 수 제한이나 IP 차단은 이 층의 일이 아니다(미정).
/// - IPv6를 다루지 않는다. 지금은 IPv4로만 듣는다(미정).
/// - 상대의 주소를 알려 주지 않는다. 그것을 담을 자리가 공개 계약에 없다(미정).
/// - 수락 처리기가 어느 I/O 스레드에서 불릴지 약속하지 않는다. 여러 연결의 처리기가 서로
///   다른 스레드에서 동시에 돌 수 있다.
/// </remarks>
class Acceptor
{
public:
    Acceptor();

    /// <summary>Stop()을 부르고, 진행 중인 수락이 정리된 뒤에 끝난다.</summary>
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    Acceptor(Acceptor&&) = delete;
    Acceptor& operator=(Acceptor&&) = delete;

    /// <summary>IPv4 endpoint를 열고 수락을 준비한다. 아직 수락하지는 않는다.</summary>
    /// <param name="listenAddress">
    /// 들 IPv4 주소. 예를 들어 loopback만 받을 때는 "127.0.0.1", 모든 IPv4 인터페이스에서
    /// 받을 때는 "0.0.0.0"을 넘긴다.
    /// </param>
    /// <param name="port">들을 TCP 포트. 0은 미구성 값이므로 InvalidArgument로 거절한다.</param>
    /// <param name="backlog">
    /// 운영체제가 쌓아 둘 대기 연결 수. 0 이하를 넘기는 것은 계약 위반이며 단언으로 끊는다.
    /// </param>
    /// <returns>
    /// 이미 듣고 있는데 다시 부르면 AlreadyExists. 빈 주소, IPv4가 아닌 주소, 포트 0이면
    /// InvalidArgument. 소켓 호출이 실패하면 PlatformError이며 설명 문자열에
    /// WSAGetLastError()의 값이 숫자 그대로 들어간다. 포트가 이미 쓰이고 있으면 bind가
    /// 10048로 실패해 그 경로로 온다.
    /// </returns>
    Core::Status Listen(std::string_view listenAddress, std::uint16_t port, int backlog);

    /// <summary>
    /// 새 연결이 들어왔을 때 부를 처리기를 건다.
    /// </summary>
    /// <remarks>
    /// 누가 이것을 부르는가: 실행 환경 층의 부팅 절차가 Start() 전에 건다. 이것이 걸리기 전에
    /// 수락이 시작되면 받은 연결을 아무도 가져가지 않는다. 그 순서를 지키는 것은 Start()의
    /// 단언이다. 처리기가 없으면 Start()가 그 자리에서 끊는다.
    ///
    /// 처리기 안에서 해야 하는 것: 받은 연결에 관찰자를 걸어야 한다. 처리기가 돌아온 뒤에야
    /// 수신이 시작되므로, 처리기 안에서 관찰자를 걸면 첫 바이트를 놓치지 않는다. 그 순서를
    /// 지키는 것은 수락 경로가 처리기를 부른 다음에 수신을 거는 것이다. 관찰자를 걸지 않으면
    /// 받은 바이트는 버려진다.
    /// </remarks>
    void SetConnectionHandler(std::function<void(std::shared_ptr<Connection>)> handler);

    /// <summary>수락을 시작한다. 처리기가 걸려 있지 않은 상태로 부르는 것은 계약 위반이다.</summary>
    /// <remarks>
    /// Listen()이 성공하지 않은 상태로 부르는 것도 계약 위반이다. 둘 다 단언으로 끊는다.
    /// </remarks>
    /// <param name="io">
    /// 완료를 처리할 실행 환경. 돌고 있지 않은 것을 넘기는 것은 계약 위반이다. 이 객체보다
    /// 오래 살아야 한다.
    /// </param>
    Core::Status Start(IoContext& io);

    /// <summary>수락을 멈추고 포트를 닫는다. 이미 맺어진 연결은 그대로 둔다.</summary>
    /// <remarks>
    /// 진행 중인 수락 요청과 완료 뒤의 처리기 인계가 전부 정리될 때까지 기다린다. 그래서 이
    /// 함수가 돌아온 뒤에는 이 객체를 가리키는 완료나 인계 코드가 남지 않는다. 여러 번 불러도
    /// 된다.
    ///
    /// 이미 실행 중인 수락 처리기 인계에는 시간 제한이 없다. 처리기는 서버 프로그램의 일이라
    /// 실행 시간을 이 층이 정할 수 없고, 이 객체의 상태를 쓰는 동안 돌아오면 수명이 깨진다.
    /// 리슨 소켓을 닫은 뒤 남은 AcceptEx 취소 완료에만 시간 제한이 있다. 그 제한을 넘기는
    /// 유일하게 알려진 이유는 IoContext가 이미 멈춰서 완료를 처리할 스레드가 없는 것이고, 그것은
    /// 종료 순서를 어긴 것이므로 계약 위반이다. 그래서 제한을 넘기면 단언으로 끊는다. 여기서
    /// 그냥 돌아오면 이 객체가 사라진 뒤에 완료가 그 자리를 가리키게 된다.
    ///
    /// I/O 스레드에서 부르는 것은 계약 위반이다. 자기가 처리해야 할 완료를 자기가 기다리게
    /// 된다. 그것을 잡는 것은 이 함수 안의 단언이며 판정 근거는 IoContext::IsCurrentThreadIoThread()다.
    /// </remarks>
    void Stop();

    /// <summary>지금 듣고 있는 포트다. 듣고 있지 않으면 0이다.</summary>
    /// <remarks>스레드 안전성: 스레드 안전.</remarks>
    [[nodiscard]] std::uint16_t Port() const noexcept;

private:
    friend class AcceptorAccess;

    /// <summary>구현 상태. 정의는 src/Net/Acceptor.cpp에 있다.</summary>
    /// <remarks>
    /// 왜 감추는가: 리슨 소켓과 진행 중인 수락 요청은 Windows 타입이라 공개 헤더에 나올 수
    /// 없다. 코드 규약이 그것을 막는다.
    /// </remarks>
    class State;

    std::unique_ptr<State> mState;
};
}
