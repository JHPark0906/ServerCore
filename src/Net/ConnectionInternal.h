#pragma once

#include "ServerCore/Net/Connection.h"

#include "Net/IoOperationInternal.h"
#include "Net/WinsockInternal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace ServerCore::Net
{
class TcpConnection;

/// <summary>여러 Connection이 함께 쓰는 보관 송신 payload 바이트 예산이다.</summary>
/// <remarks>
/// ServerHost가 만든 연결들에만 같은 인스턴스를 주입한다. Acceptor를 직접 쓰는 소비자는 기존의
/// Connection별 SendQueueLimitBytes만 적용받는다. 예약과 반납은 여러 I/O·게임 스레드에서 동시에
/// 일어나므로 원자적으로 직렬화한다.
/// </remarks>
class SendBudget
{
public:
    explicit SendBudget(std::size_t limitBytes) noexcept;

    [[nodiscard]] bool TryReserve(std::size_t byteCount) noexcept;
    void Release(std::size_t byteCount) noexcept;

    [[nodiscard]] std::size_t LimitBytes() const noexcept;
    [[nodiscard]] std::size_t UsedBytes() const noexcept;

private:
    std::size_t mLimitBytes;
    std::atomic<std::size_t> mUsedBytes{ 0 };
};

/// <summary>수신 요청 하나가 한 번에 받아 둘 수 있는 바이트 수다.</summary>
/// <remarks>
/// 이 통은 쌓아 두는 곳이 아니라 한 번의 수신이 쓰고 비우는 자리다. 이 층은 바이트를 모으지
/// 않고 받은 즉시 관찰자에게 올리기 때문이다. 모으는 일은 프로토콜 층의 ByteBuffer가 한다.
///
/// 값의 근거: 없다. 흔한 MTU보다 크다는 것 외에 측정이 없다(미정). 작으면 완료가 잦아지고
/// 크면 연결마다 그만큼을 든다. 연결 하나가 이 크기를 상시 차지한다는 것이 대가다.
/// </remarks>
inline constexpr std::size_t ReceiveBufferSize = 16 * 1024;

/// <summary>연결이 거는 겹침 요청의 공통 부분이다.</summary>
/// <remarks>
/// owner가 이 설계의 핵심이다. 요청을 걸 때 자기 자신의 shared_ptr를 여기 담아 두므로,
/// 관찰자가 연결을 놓아도 완료가 돌아올 때까지는 연결이 살아 있다. 코드 규약이 "진행 중인
/// 요청은 대상 객체의 shared_ptr를 자기 몫으로 들고 있어야 한다"고 적은 것이 이 자리다.
/// 완료를 받은 쪽이 이것을 비우고, 그 비움이 마지막 참조를 놓는 자리가 될 수 있다.
/// </remarks>
struct ConnectionOperation : IoOperation
{
    /// <summary>완료가 돌아올 때까지 대상을 살려 두는 자기 몫의 참조.</summary>
    std::shared_ptr<TcpConnection> owner;
};

/// <summary>진행 중인 수신 요청 하나. 연결마다 하나만 존재한다.</summary>
struct ReceiveOperation : ConnectionOperation
{
    std::array<std::byte, ReceiveBufferSize> buffer{};
};

/// <summary>진행 중인 송신 요청 하나. 연결마다 하나만 존재한다.</summary>
/// <remarks>
/// 보낼 바이트를 여기 담지 않는 이유: 담을 곳은 연결의 보낼 큐이고, 요청은 그 큐의 맨 앞을
/// 가리키기만 한다. 큐가 std::deque인 것이 그것을 가능하게 한다. deque는 뒤에 넣어도 이미
/// 든 원소의 주소가 바뀌지 않으므로, 보내는 중인 바이트를 다른 스레드의 Send가 옮기지 못한다.
/// </remarks>
struct SendOperation : ConnectionOperation
{
};

/// <summary>
/// Connection의 Winsock 구현이다. 전송 층 안에서만 만들어진다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 공개 계약인 Connection은 순수 가상이라 소켓도 겹침 요청도 모른다. 그 둘을 아는 것이 이
/// 타입이고, 공개 헤더에 나올 수 없으므로 여기 있다.
///
/// 소유한다: 소켓 하나, 아직 보내지 못한 바이트의 사본, 진행 중인 요청 두 자리(수신 하나와
/// 송신 하나), 그리고 Winsock이 살아 있게 하는 참조 하나.
/// 소유하지 않는다: 관찰자(약한 참조), 완료 포트, 수락기.
///
/// 왜 Winsock 참조를 연결이 드는가:
/// 수락기가 먼저 사라져도 연결의 소켓은 살아 있어야 한다. 수락기만 Winsock을 들고 있으면
/// 그때 Winsock이 내려가고 closesocket이 실패한다. 참조를 나눠 들면 마지막 소유자가 사라질
/// 때까지 내려가지 않는다.
///
/// 스레드 안전성: 공개 함수는 전부 스레드 안전하다. 상태는 mMutex가 지킨다.
/// 다만 관찰자 호출은 잠금 밖에서 한다. 관찰자가 그 안에서 Send나 Close를 부를 수 있고,
/// 잠금을 쥔 채 부르면 그 자리에서 같은 잠금을 다시 잡는다.
///
/// 왜 수신 요청이 한 번에 하나인가:
/// 둘 이상을 동시에 걸면 완료가 걸린 순서대로 돌아온다는 보장이 없어 바이트 순서가 뒤집힌다.
/// TCP가 지키는 순서를 우리가 깨는 것이 되므로 하나만 건다.
///
/// 약속하지 않는 것:
/// - 이 헤더는 공개 계약이 아니다. 여기 있는 것은 예고 없이 바뀐다.
/// </remarks>
class TcpConnection final : public Connection,
                            public IIoCompletionTarget,
                            public std::enable_shared_from_this<TcpConnection>
{
public:
    /// <summary>Create()만 이 생성자를 부를 수 있게 하는 열쇠다.</summary>
    /// <remarks>
    /// 생성자가 공개인 이유는 std::make_shared가 부를 수 있어야 하기 때문이고, 그러면서도
    /// 아무나 못 만들게 하려고 이 열쇠를 받는다. 코드 규약이 new를 금지하므로 make_shared를
    /// 써야 하고, 그래서 생성자를 감출 수 없다.
    /// </remarks>
    struct CreationKey
    {
        explicit CreationKey() = default;
    };

    /// <summary>수락된 소켓을 감싸는 연결을 만든다. 아직 수신을 걸지 않는다.</summary>
    /// <param name="socket">
    /// 이미 완료 포트에 붙은 겹침 소켓. INVALID_SOCKET을 넘기는 것은 계약 위반이다.
    /// </param>
    /// <param name="winsock">Winsock이 살아 있게 하는 참조. 널을 넘기는 것은 계약 위반이다.</param>
    [[nodiscard]] static std::shared_ptr<TcpConnection> Create(SOCKET socket,
        std::shared_ptr<WinsockScope> winsock, std::shared_ptr<SendBudget> sendBudget = nullptr);

    TcpConnection(CreationKey, SOCKET socket, std::shared_ptr<WinsockScope> winsock,
        std::shared_ptr<SendBudget> sendBudget);

    /// <summary>소켓을 닫고, 아직 통지하지 않았다면 끊김을 통지한다.</summary>
    /// <remarks>
    /// 여기서 통지하는 이유: 관찰자를 걸어 둔 연결이 Close() 없이 사라지면 관찰자는 아무
    /// 소식도 못 받는다. 그러면 "정확히 한 번"의 "한 번"이 지켜지지 않는다. 이 소멸자가 그
    /// 자리를 메우고, 원자적 플래그가 두 번 가지 않게 막는다.
    ///
    /// 여기서 진행 중인 요청이 없다는 것은 요청이 자기 몫의 참조를 들기 때문이다. 요청이
    /// 남아 있으면 참조도 남아 있어 여기까지 오지 못한다. 단언이 그것을 고정한다.
    /// </remarks>
    ~TcpConnection() override;

    Core::Status Send(std::span<const std::byte> bytes) override;
    [[nodiscard]] ConnectionSendOutcome SendWithOutcome(std::span<const std::byte> bytes) override;
    void Close() override;
    void CloseAfterSend() override;
    void SetObserver(std::weak_ptr<IConnectionObserver> observer) override;
    [[nodiscard]] bool IsOpen() const noexcept override;
    [[nodiscard]] std::size_t QueuedSendBytes() const noexcept override;

    void OnIoCompleted(
        IoOperation& operation, DWORD bytesTransferred, unsigned long errorCode) override;

    /// <summary>수신을 걸기 시작한다. 수락 처리기가 돌아온 뒤에 부른다.</summary>
    /// <remarks>
    /// 왜 생성과 나뉘어 있는가:
    /// 만들자마자 수신을 걸면 수락 처리기가 관찰자를 걸기 전에 첫 바이트가 도착할 수 있고,
    /// 그 바이트는 갈 곳이 없어 사라진다. 처리기가 돌아온 뒤에 이것을 부르는 것이 그 순서를
    /// 지키는 수단이다.
    ///
    /// 두 번 부르는 것은 계약 위반이며 단언으로 끊는다.
    /// </remarks>
    /// <returns>이미 닫혔으면 Closed. 소켓 호출이 실패하면 PlatformError.</returns>
    Core::Status Start();

private:
    Core::Status SendInternal(std::span<const std::byte> bytes, bool& connectionClosedByFailure);

    void OnReceiveCompleted(
        ReceiveOperation& operation, DWORD bytesTransferred, unsigned long errorCode);
    void OnSendCompleted(SendOperation& operation, DWORD bytesTransferred, unsigned long errorCode);

    /// <summary>수신 요청 하나를 건다. mMutex를 쥔 채 부른다.</summary>
    Core::Status StartReceiveLocked();

    /// <summary>보낼 큐의 맨 앞에서 송신 요청 하나를 건다. mMutex를 쥔 채 부른다.</summary>
    Core::Status StartSendLocked();

    /// <summary>더 보내지 않을 큐 바이트를 버린다. mMutex를 쥔 채 부른다.</summary>
    /// <remarks>
    /// 진행 중인 WSASend는 mSendQueue의 맨 앞 vector를 직접 가리킨다. 그래서 그 요청이 끝나기
    /// 전에는 clear()하면 안 된다. mSendInFlight가 거짓이 된 뒤에만 이 함수를 불러야 버퍼 수명과
    /// Close()의 "남은 바이트를 버린다"는 공개 계약을 함께 지킨다.
    /// </remarks>
    void DiscardQueuedSendsLocked() noexcept;

    /// <summary>닫힘과 그 사유를 기록한다. 첫 사유가 이긴다. mMutex를 쥔 채 부른다.</summary>
    void MarkClosedLocked(Core::Status reason);

    /// <summary>소켓을 실제로 내리고 닫는다. mMutex를 쥔 채 부른다.</summary>
    void CloseSocketLocked();

    /// <summary>보낼 쪽을 정상 종료한 뒤 소켓을 닫는다. mMutex를 쥔 채 부른다.</summary>
    void CloseSocketAfterSendLocked();

    /// <summary>CloseAfterSend 요청이 끝날 조건이면 정상 종료한다. mMutex를 쥔 채 부른다.</summary>
    void FinishCloseAfterSendLocked();

    /// <summary>닫혔고 진행 중인 요청이 없으면 끊김을 통지한다.</summary>
    void MaybeNotifyDisconnected();

    /// <summary>끊김을 통지한다. 여러 번 불려도 관찰자는 한 번만 불린다.</summary>
    void NotifyDisconnectedOnce(Core::Status reason);

    mutable std::mutex mMutex;
    std::shared_ptr<WinsockScope> mWinsock;
    std::shared_ptr<SendBudget> mSendBudget;
    SOCKET mSocket = INVALID_SOCKET;

    std::weak_ptr<IConnectionObserver> mObserver;
    bool mObserverAssigned = false;
    bool mStarted = false;

    ReceiveOperation mReceiveOperation;
    SendOperation mSendOperation;

    /// <summary>아직 보내지 못한 바이트. 맨 앞이 지금 보내는 중인 것이다.</summary>
    std::deque<std::vector<std::byte>> mSendQueue;

    /// <summary>맨 앞 원소에서 이미 보낸 바이트 수. 부분 전송이 있어 필요하다.</summary>
    std::size_t mSendOffset = 0;

    /// <summary>아직 보내지 못한 전체 바이트 수. QueuedSendBytes()가 공개하는 값이다.</summary>
    std::size_t mQueuedSendBytes = 0;

    /// <summary>큐 vector가 아직 보관하는 payload 바이트 수. SendQueueLimitBytes와 견준다.</summary>
    /// <remarks>
    /// 부분 전송된 맨 앞 vector는 WSASend가 끝날 때까지 보낸 prefix도 메모리에 남긴다. 그 prefix를
    /// 빼고 입장을 받으면 실제 보관 payload가 상한의 두 배 가까이 늘 수 있어 별도로 센다.
    /// </remarks>
    std::size_t mRetainedSendBytes = 0;

    bool mSendInFlight = false;

    /// <summary>새 Send를 막고 현재 송신 큐가 빈 뒤 닫으라는 요청이다. mMutex가 지킨다.</summary>
    bool mCloseAfterSendRequested = false;

    /// <summary>커널에 걸려 있는 요청 수. 0이 되어야 끊김을 통지할 수 있다.</summary>
    std::size_t mPendingOperations = 0;

    /// <summary>잠금 밖에서 실행 중인 OnBytesReceived 호출이 있는지 나타낸다.</summary>
    /// <remarks>
    /// 수신 요청은 한 번에 하나뿐이므로 bool이면 충분하다. 커널 완료는 이미 끝났어도 관찰자가
    /// 그 완료 버퍼를 읽는 동안 Close()가 OnDisconnected를 먼저 부르면 공개 관찰자 계약의
    /// "동시 호출 없음"이 깨진다. 그래서 이 구간도 종료 통지의 수명 경계에 넣는다.
    /// mMutex가 지킨다.
    /// </remarks>
    bool mReceiveCallbackActive = false;

    /// <summary>원자인 이유는 IsOpen()이 noexcept라 잠금을 잡을 수 없기 때문이다.</summary>
    std::atomic<bool> mClosed{ false };

    Core::Status mCloseReason = Core::Status::Ok();

    /// <summary>이 플래그의 뒤집기가 "정확히 한 번"을 지킨다.</summary>
    std::atomic<bool> mDisconnectNotified{ false };
};
}
