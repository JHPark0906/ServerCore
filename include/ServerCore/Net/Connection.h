#pragma once

#include "ServerCore/Core/Error.h"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace ServerCore::Net
{
/// <summary>
/// 연결 하나에서 일어난 일을 받는 자리다. 세션 층이 구현한다.
/// </summary>
/// <remarks>
/// 호출 계약:
/// - OnBytesReceived는 I/O 스레드에서 불린다. OnDisconnected는 마지막 겹침 완료를 처리한 I/O
///   스레드일 수도 있고, 진행 중인 I/O가 없을 때 Close·CloseAfterSend·SetObserver를 부른
///   스레드일 수도 있다. 여기서 오래 걸리는 일을 하면 그 스레드가 맡은 일을 막으므로 무거운
///   작업은 별도 실행 문맥으로 넘긴다.
/// - 연결은 관찰자를 약한 참조로 잡는다. 즉 연결이 관찰자를 살려 두지 않는다.
/// - 이 두 함수에서 예외를 던지지 않는다. 던지면 I/O 스레드에서 받을 호출자가 없다.
///   그것을 지키는 수단은 없다. 즉 이것은 강제되는 계약이 아니라 구현자에게 맡긴 약속이다.
///
/// 약속하지 않는 것:
/// - OnBytesReceived가 메시지 하나씩 온다고 약속하지 않는다. 아래 Connection의 설명을 보라.
/// - 두 함수가 같은 스레드에서 불린다고 약속하지 않는다. 다만 한 연결에 대해 동시에 불리지는
///   않는다. 그것을 지키는 것은 연결이 수신 요청을 한 번에 하나만 걸고, 끊김 통지를 진행
///   중인 요청이 전부 끝난 뒤에 보내는 것이다.
/// - OnDisconnected의 시점을 약속하지 않는다. 횟수만 약속한다. 아래 Connection의 "정확히
///   한 번"을 보라.
/// </remarks>
class IConnectionObserver
{
public:
    virtual ~IConnectionObserver() = default;

    IConnectionObserver(const IConnectionObserver&) = delete;
    IConnectionObserver& operator=(const IConnectionObserver&) = delete;
    IConnectionObserver(IConnectionObserver&&) = delete;
    IConnectionObserver& operator=(IConnectionObserver&&) = delete;

    /// <param name="bytes">
    /// 방금 도착한 바이트. 이 호출이 끝나면 유효하지 않다. 남겨야 하면 받는 쪽이 복사한다.
    /// 비어 있는 채로 불리지 않는다.
    /// </param>
    virtual void OnBytesReceived(std::span<const std::byte> bytes) = 0;

    /// <param name="reason">왜 끊겼는지. 상대가 정상적으로 닫은 경우도 포함한다.</param>
    virtual void OnDisconnected(Core::Status reason) = 0;

protected:
    IConnectionObserver() = default;
};

/// <summary>보낼 큐 하나가 담아 둘 수 있는 최대 바이트 수다.</summary>
/// <remarks>
/// 왜 상한이 있는가:
/// 상대가 느리거나 일부러 안 읽으면 보낼 것이 쌓인다. 상한이 없으면 그 연결 하나가 서버의
/// 메모리를 다 쓴다. 즉 상한은 편의가 아니라 악의적 상대에 대한 방어다. 상한을 넘는 Send는
/// 큐에 담지 않고 WouldBlock으로 거절하며, 그 판정이 "무한히 자라지 않는다"를 지키는 수단이다.
/// 부분 전송된 큐 맨 앞의 보낸 prefix도 WSASend가 그 vector를 완전히 놓을 때까지 이 상한에
/// 포함한다. 아직 안 보낸 바이트만 세면 같은 메모리 자리에 새 payload를 계속 더할 수 있기 때문이다.
///
/// 값의 근거: 지금은 근거가 없다. 1 MiB는 기본 프레임 상한(64 KiB)의 열여섯 배라는 것 외에
/// 아무 측정도 없다(미정). 측정이 생기면 여기 한 줄을 고친다.
/// </remarks>
inline constexpr std::size_t SendQueueLimitBytes = 1024 * 1024;

/// <summary>한 Send 호출의 상태와 그 실패가 연결을 직접 닫았는지를 함께 돌려준다.</summary>
/// <remarks>
/// connectionClosedByFailure는 호출 뒤의 순간 IsOpen 값이 아니다. 이 Send가 소켓 오류를 만나
/// 직접 연결을 닫았을 때만 참이다. 실행 환경은 반환 실패와 OnDisconnected의 같은 오류를 두 번
/// 집계하지 않으면서, 동시에 일어난 별도 종료 오류도 잃지 않기 위해 이 인과 정보를 쓴다.
/// </remarks>
struct ConnectionSendOutcome
{
    Core::Status status = Core::Status::Ok();
    bool connectionClosedByFailure = false;
};

/// <summary>
/// 살아 있는 TCP 연결 하나다. 바이트만 알고 그 뜻은 모른다.
/// </summary>
/// <remarks>
/// 소유한다: 소켓 하나와, 아직 보내지 못한 바이트의 사본.
/// 소유하지 않는다: 관찰자. 약한 참조로만 잡는다. 완료 포트도, 수락기도 소유하지 않는다.
///
/// 소유권과 수명:
/// 항상 std::shared_ptr로만 다룬다. 진행 중인 겹침 I/O가 자기 몫의 참조를 들고 있으므로,
/// 관찰자가 놓아도 완료가 돌아올 때까지는 살아 있다. 이것이 원시 포인터를 쓰지 않는 이유다.
/// 복사와 이동을 지운 것이 그 규칙의 나머지 절반이다. 소켓을 소유하는 것이 복사되면 같은
/// 소켓을 두 번 닫는다.
///
/// 스레드 안전성: Send와 Close와 CloseAfterSend와 IsOpen과 QueuedSendBytes는 스레드 안전하다.
/// SetObserver는 수락 처리기 안에서 한 번만 부른다.
///
/// OnDisconnected가 정확히 한 번 오는 것 — 무엇이 그것을 지키는가:
/// 통지 자리에 원자적 플래그가 하나 있고, 통지하려는 쪽이 그것을 먼저 뒤집는다. 뒤집기에
/// 성공한 하나만 관찰자를 부른다. 여러 I/O 스레드가 동시에 끊김을 발견해도 통과하는 것은
/// 하나다. 그리고 관찰자가 걸린 연결이 통지 없이 사라지지 않도록, 소멸자도 같은 자리를
/// 지난다. 즉 "정확히"는 그 플래그가, "한 번은"은 그 소멸자가 지킨다.
///
/// 통지는 진행 중인 겹침 요청이 전부 끝난 뒤에 간다. 그래서 OnDisconnected를 받은 뒤에는
/// 이 연결이 관찰자를 다시 부르지 않는다.
///
/// 약속하지 않는 것:
/// - 메시지 경계를 약속하지 않는다. 한 번의 Send가 상대편의 한 번의 수신으로 오지 않는다.
///   이 사실 하나가 프로토콜 층이 따로 있는 이유 전부다.
/// - Send가 성공을 돌려준 것은 보낼 큐에 들어갔다는 뜻이다. 보내졌다는 뜻도, 상대가 받았다는
///   뜻도 아니다.
/// - 재연결하지 않는다. 끊기면 끊긴 것이다.
/// - JSON도, 메시지 타입도, 세션도 모른다.
/// - 관찰자를 걸기 전에 이미 끊긴 연결에 관찰자를 걸면 통지가 그때 간다. 관찰자를 끝내
///   걸지 않으면 통지는 갈 곳이 없어 사라진다. "정확히 한 번"은 관찰자가 걸린 연결에 대한
///   약속이며, 관찰자가 이미 소멸한 경우는 포함하지 않는다.
/// - 바이트가 실제로 나갔는지 알려 주지 않는다. 보낼 큐에서 빠졌다는 것을 알리는 창구가 없다.
/// </remarks>
class Connection
{
public:
    virtual ~Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    /// <summary>바이트를 보낼 큐에 넣는다. 즉시 돌아온다.</summary>
    /// <param name="bytes">
    /// 보낼 바이트. 이 호출 안에서 복사하므로 돌아온 뒤에 호출자가 놓아도 된다. 열린 연결에
    /// 비어 있는 것을 보내면 아무 일도 하지 않고 성공을 돌려준다.
    /// </param>
    /// <returns>
    /// 이미 닫혔거나 CloseAfterSend()가 요청되었으면 Closed. 닫힘을 빈 요청보다 먼저 보므로,
    /// 그 두 상태에서는 비어 있는 것을 보내도 Closed다. 이 연결의 보낼 큐가
    /// SendQueueLimitBytes를 넘거나 실행 환경이 주입한 공유 송신 예산이 차면 WouldBlock이며,
    /// 이때 한 바이트도 담기지 않는다. 절반만 담으면 상대가 받는 바이트 흐름이 깨지기 때문이다.
    /// 소켓 호출이나 큐 할당이 실패하면 PlatformError이며, 소켓 실패의 설명 문자열에는
    /// WSAGetLastError()의 값이 숫자 그대로 들어간다. 다만 그 설명 문자열을 만들 메모리조차
    /// 부족한 경우에는 예외를 내보내지 않고 설명 없는 PlatformError를 돌려준다.
    /// </returns>
    virtual Core::Status Send(std::span<const std::byte> bytes) = 0;

    /// <summary>Send 결과와 이 호출이 연결을 닫았는지를 원자적으로 함께 얻는다.</summary>
    /// <remarks>
    /// 바이트 수락과 오류 계약은 Send()와 같다. 기존 사용자 구현을 깨지 않도록 기본 구현은
    /// Send()에 위임하고 인과 표식을 false로 둔다. 한 호출의 소켓 실패를 구분할 수 있는 구현은
    /// 이 함수를 재정의한다.
    /// </remarks>
    [[nodiscard]] virtual ConnectionSendOutcome SendWithOutcome(std::span<const std::byte> bytes)
    {
        Core::Status status = Send(bytes);
        return ConnectionSendOutcome{ std::move(status), false };
    }

    /// <summary>연결을 닫는다. 여러 번 불러도 되고, 이미 닫혔으면 아무 일도 하지 않는다.</summary>
    /// <remarks>
    /// 아직 보내지 못한 바이트는 버려진다. CloseAfterSend()가 진행 중이어도 이 호출은 즉시
    /// 닫기를 택해 남은 바이트를 버린다. 다만 진행 중인 WSASend가 큐의 버퍼를 빌리고 있으면 그
    /// 완료가 돌아오기 전에는 버퍼를 해제할 수 없다. 그 동안 QueuedSendBytes()가 잠시 남을 수
    /// 있지만, OnDisconnected 통지보다 먼저 남은 큐를 모두 버린다.
    ///
    /// 이 호출은 진행 중인 겹침 요청의 취소 완료를 기다리지 않는다. 이 연결이 쓰는 IoContext를
    /// 멈추려면 Close()가 돌아온 것만 보지 말고 OnDisconnected를 받은 뒤에 멈춰야 한다.
    /// 구현은 예외를 던지지 않아야 한다. 종료 실행자는 모든 연결에 이 함수를 호출한 뒤
    /// OnDisconnected를 기다리므로, 한 연결의 예외는 전체 종료 수명을 깨뜨린다.
    /// </remarks>
    virtual void Close() = 0;

    /// <summary>이미 받아 둔 송신 큐를 비운 뒤 연결을 닫는다.</summary>
    /// <remarks>
    /// 이 호출은 기다리지 않는다. 이 호출보다 먼저 Send()가 성공하여 큐에 들어간 바이트는
    /// 모두 WSASend 완료까지 진행한 뒤 보내기 쪽을 정상 종료한다. 그 뒤 Send()는 비어 있는
    /// 요청도 Closed로 거절한다. 동시에 부른 Send()와의 포함 여부는 연결의 잠금을 먼저 잡은
    /// 호출 순서로 정한다.
    ///
    /// 이는 이 프로세스의 송신 큐를 끝까지 처리한다는 뜻이지, 원격 애플리케이션이 바이트를
    /// 소비했다는 확인은 아니다. 전송 중 즉시 Close()를 부르거나 peer가 먼저 끊기면 남은
    /// 바이트는 버려질 수 있다.
    /// 구현은 예외를 던지지 않아야 한다.
    /// </remarks>
    virtual void CloseAfterSend() = 0;

    /// <summary>이 연결에서 일어나는 일을 받을 관찰자를 건다. 약한 참조로 잡는다.</summary>
    /// <remarks>
    /// 두 번 부르는 것은 계약 위반이며 단언으로 끊는다. 두 번째 관찰자는 첫 번째가 이미
    /// 받은 바이트를 못 받으므로, 바꿔 끼우는 것에 뜻이 없다.
    /// </remarks>
    virtual void SetObserver(std::weak_ptr<IConnectionObserver> observer) = 0;

    /// <summary>로컬 소켓이 아직 닫히지 않았는지 알려 준다.</summary>
    /// <remarks>
    /// CloseAfterSend() 뒤에는 큐를 비우는 동안 참일 수 있지만, 그 동안에도 Send()는 새
    /// 바이트를 받지 않는다. 따라서 이 값은 Send()의 수락 가능 여부를 뜻하지 않는다.
    /// </remarks>
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;

    /// <summary>아직 보내지 못한 논리 바이트 수의 순간값을 준다.</summary>
    /// <remarks>
    /// 부분 전송이 완료될 때마다 그만큼 줄어든다. 다만 송신 중인 vector의 이미 보낸 prefix는
    /// 마지막 완료까지 실제 메모리에 남고 SendQueueLimitBytes 및 공유 송신 예산에도 계속
    /// 포함된다. 따라서 이 값은 실제 보관 메모리, peer 수신량, OS 소켓 버퍼 점유량이 아니다.
    /// </remarks>
    [[nodiscard]] virtual std::size_t QueuedSendBytes() const noexcept = 0;

protected:
    Connection() = default;
};
}
