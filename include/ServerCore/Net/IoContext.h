#pragma once

#include "ServerCore/Core/Error.h"

#include <memory>

namespace ServerCore::Net
{
class IoContextAccess;

/// <summary>
/// 완료 포트와 그 위에서 도는 I/O 스레드 풀을 소유한다. 전송 층의 심장이다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 이 층이 플랫폼을 아는 유일한 층이다. Winsock과 IOCP를 아는 코드가 여기 밖으로 새지 않아야
/// 나중에 다른 플랫폼 백엔드를 이 층 옆에 세울 수 있고, 그때 위층은 바뀌지 않는다.
///
/// 소유한다: 완료 포트 핸들 하나와 I/O 스레드 전부.
/// 소유하지 않는다: 소켓. 소켓은 수락기와 연결이 소유한다. 이 객체는 소켓을 완료 포트에
/// 이어 붙이기만 하고 닫지 않는다.
///
/// 소유권과 수명:
/// 이 객체가 I/O 스레드를 소유한다. 소멸 전에 반드시 Stop()이 불려야 하고, Stop()은 스레드가
/// 모두 빠져나올 때까지 기다린다. 그 "반드시"를 지키는 것은 소멸자다. 소멸자가 Stop()을
/// 부르므로 호출자가 잊어도 스레드가 도는 채로 이 객체가 사라지지 않는다.
///
/// 복사와 이동을 지운 것이 같은 약속의 나머지 절반이다. 완료 포트와 스레드를 소유하는 것이
/// 복사되면 같은 핸들을 두 번 닫고 같은 스레드를 두 번 join한다. 컴파일러가 만들어 주는
/// 복사는 그것을 막지 않고 /W4 /WX도 잡지 않으므로, 막는 수단은 아래의 = delete뿐이다.
///
/// 스레드 안전성: Stop()과 IsRunning()과 IsCurrentThreadIoThread()는 스레드 안전하다.
/// Start()는 부팅 스레드에서 한 번만 부른다.
///
/// 종료 순서 — 이 층을 쓰는 쪽이 지켜야 하는 것:
/// 수락기를 먼저 멈추고, 연결을 전부 닫고, 각 연결의 OnDisconnected를 확인한 뒤 마지막에
/// 이것을 멈춘다. Connection::Close()는 커널 요청의 취소 완료를 기다리지 않고 돌아오므로,
/// Close() 직후 곧바로 Stop()을 부르는 것으로는 충분하지 않다. OnDisconnected는 그 연결의
/// 진행 중인 겹침 요청이 전부 완료 포트를 지나 정리되었다는 경계다. 순서를 뒤집으면 수락기나
/// 연결이 기다리는 완료를 처리할 스레드가 이미 없다. 수락기의 어김은 Acceptor::Stop()이 두는
/// 시간 제한과 단언으로 잡지만, 연결에는 그 대기가 없으므로 호출자가 이 순서를 지켜야 한다.
///
/// 약속하지 않는 것:
/// - 스레드 수가 성능에 어떻게 걸리는지 약속하지 않는다. 적절한 값은 측정으로 정한다(미정).
/// - Stop() 이후에 완료 통지가 하나도 안 온다고 약속하지 않는다. 이미 큐에 들어간 완료는
///   빠져나오는 동안 전달될 수 있다.
/// - Stop()이 아직 커널에 걸려 있는 요청까지 기다린다고 약속하지 않는다. 닫지 않았거나
///   OnDisconnected까지 확인하지 않은 연결이 남아 있으면 완료가 처리되지 않고, 그 연결이 든
///   자기 몫의 참조도 남을 수 있다. Stop()은 I/O drain 장벽이 아니다.
/// - 완료 스레드가 무거운 일을 어디로 넘기는지 약속하지 않는다. 이 층에는 그런 곳이 없고
///   관찰자가 그 자리에서 처리한다. 작업 큐를 물릴지는 미정이며 이 층의 결정이 아니다.
/// </remarks>
class IoContext
{
public:
    IoContext();

    /// <summary>Stop()을 부르고, 스레드가 모두 빠져나온 뒤에 끝난다.</summary>
    /// <remarks>
    /// 위 "소멸 전에 반드시 Stop()"을 지키는 것이 이 소멸자다. 여기서 부르지 않으면 그 문장은
    /// 약속이 아니라 희망이 된다.
    /// </remarks>
    ~IoContext();

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    /// <summary>완료 포트를 만들고 I/O 스레드를 띄운다.</summary>
    /// <param name="workerThreadCount">
    /// 완료를 처리할 스레드 수. 0 이하를 넘기는 것은 계약 위반이며 단언으로 끊는다.
    /// </param>
    /// <returns>
    /// 이미 돌고 있는데 다시 부르면 AlreadyExists. 완료 포트를 만들지 못하면 PlatformError이며
    /// 설명 문자열에 GetLastError()의 값이 숫자 그대로 들어간다.
    /// </returns>
    Core::Status Start(int workerThreadCount);

    /// <summary>I/O 스레드를 멈추고 모두 빠져나올 때까지 기다린다. 여러 번 불러도 된다.</summary>
    /// <remarks>
    /// 이 함수는 아직 커널에 걸려 있는 연결 요청을 기다리지 않는다. 위 종료 순서대로 각 연결에
    /// Close()를 부르고 OnDisconnected를 확인한 뒤 호출해야 한다.
    ///
    /// I/O 스레드에서 부르는 것은 계약 위반이다. 자기가 빠져나오기를 자기가 기다리게 되어
    /// 돌아오지 않는다. 그것을 잡는 것은 이 함수 안의 단언이고 판정 근거는
    /// IsCurrentThreadIoThread()다.
    /// </remarks>
    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept;

    /// <summary>지금 이 스레드가 이 객체의 I/O 스레드인지 알린다.</summary>
    /// <remarks>
    /// 왜 공개인가: "완료 스레드에서 이것을 부르지 마라"라고 적은 약속들이 이 함수로 판정된다.
    /// 판정 수단이 밖에서 보이지 않으면 그 약속들은 이 층 밖에서 지킬 수도 확인할 수도 없다.
    ///
    /// 스레드 안전성: 스레드 안전.
    /// </remarks>
    [[nodiscard]] bool IsCurrentThreadIoThread() const noexcept;

private:
    /// <summary>전송 층 구현이 완료 포트에 닿는 통로다. src/Net 안에서만 정의된다.</summary>
    /// <remarks>
    /// 왜 friend인가: 완료 포트 핸들은 Windows 타입이라 공개 헤더에 나올 수 없고, 공개
    /// 메서드로 열면 소비자가 만질 수 있는 것이 된다. 이름 하나만 friend로 두고 정의를
    /// 구현 쪽에 두면 공개 계약은 그대로다.
    /// </remarks>
    friend class IoContextAccess;

    /// <summary>구현 상태. 정의는 src/Net/IoContext.cpp에 있다.</summary>
    /// <remarks>
    /// 왜 감추는가: 완료 포트 핸들과 스레드 목록을 여기 두면 이 헤더가 Windows 타입과 스레드
    /// 헤더를 소비자에게 딸려 보낸다. 코드 규약이 그것을 막는다.
    /// </remarks>
    class State;

    std::unique_ptr<State> mState;
};
}
