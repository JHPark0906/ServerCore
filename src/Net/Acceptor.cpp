#include "ServerCore/Net/Acceptor.h"

#include "Net/AcceptorInternal.h"
#include "Net/ConnectionInternal.h"
#include "Net/EndpointInternal.h"
#include "Net/IoContextInternal.h"
#include "Net/IoOperationInternal.h"
#include "Net/WinsockInternal.h"
#include "ServerCore/Core/Assert.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Net/IoContext.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace ServerCore::Net
{
namespace
{
/// <summary>동시에 걸어 두는 수락 요청의 수다.</summary>
/// <remarks>
/// 하나만 걸면 수락 하나를 처리해 다시 걸 때까지 그 사이의 접속이 전부 backlog에서 기다린다.
/// 여럿 걸면 그 틈이 줄어든다. 값의 근거는 없다(미정). 이것은 backlog를 대신하지 않는다.
/// </remarks>
constexpr std::size_t PendingAcceptCount = 4;

/// <summary>AcceptEx가 주소 하나를 적는 데 필요한 자리다.</summary>
/// <remarks>
/// AcceptEx 규격이 실제 주소 크기보다 16바이트를 더 요구한다. 이 여유를 빼면 호출이
/// WSAEINVAL로 실패한다. IPv4와 IPv6를 모두 담도록 sockaddr_storage를 기준으로 잡는다.
/// </remarks>
constexpr std::size_t AcceptAddressSize = sizeof(sockaddr_storage) + 16;

/// <summary>IoContext가 멈춘 것을 본 뒤 남은 수락 요청의 취소 완료를 더 기다리는 시간 제한이다.</summary>
/// <remarks>
/// IoContext가 돌고 있는 동안에는 이 제한을 재지 않는다. I/O 스레드가 다른 연결의 긴 콜백에
/// 묶여 있어도 결국 완료를 처리하기 때문이다(NET-7). IoContext가 멈춘 뒤에는 멈추기 전에 쌓인
/// 완료만 처리되므로, 그 뒤에도 제한을 넘기면 완료를 처리할 스레드가 없다는 뜻이고 그것은 종료
/// 순서를 어긴 것이다. 이미 시작된 처리기·연결 인계가 오래 걸리는 것은 이 제한의 대상이 아니다.
/// </remarks>
constexpr std::chrono::seconds PendingAcceptDrainTimeout{ 10 };

/// <summary>Stop()이 취소 완료를 기다리는 동안 IoContext의 멈춤을 확인하는 간격이다.</summary>
/// <remarks>IoContext::Stop()은 수락기를 깨우지 않으므로 주기적으로 본다. 위반 판정이 늦어지는 폭일 뿐이다.</remarks>
constexpr std::chrono::milliseconds IoRunningPollInterval{ 100 };

/// <summary>게시에 실패한 수락 자리를 다시 걸기까지 쉬는 시간이다.</summary>
/// <remarks>
/// 소켓이나 메모리가 모자라 실패한 것을 곧바로 다시 걸면 같은 실패를 되풀이하며 스레드를 태운다.
/// 값의 근거는 없다(미정). 재시도 빈도의 상한일 뿐이다.
/// </remarks>
constexpr LONGLONG AcceptRetryDelay100Nanoseconds = 100LL * 10'000LL;

/// <summary>수락 경로에서 조용히 버려질 뻔한 실패를 기록에 남긴다.</summary>
/// <remarks>
/// 수락 완료는 아무도 반환값을 받지 않는 자리다. 여기서 실패를 그냥 버리면 접속이 사라지는데
/// 아무 흔적이 없다. 기록이 그 흔적이다.
/// </remarks>
void ReportAcceptFailure(const std::shared_ptr<Core::ILogger>& logger, const Core::Status& status)
{
    if (logger)
        logger->Write(Core::LogLevel::Warn, status.Message());
}
}

/// <summary>진행 중인 수락 요청 하나다.</summary>
/// <remarks>
/// acceptSocket이 미리 만들어져 있는 것이 AcceptEx의 성질이다. 수락이 완료된 뒤에 소켓을
/// 만드는 것이 아니라, 만들어 둔 소켓에 접속이 담겨 온다.
/// </remarks>
struct AcceptOperation : IoOperation
{
    SOCKET acceptSocket = INVALID_SOCKET;
    std::array<std::byte, AcceptAddressSize * 2> addressBuffer{};

    /// <summary>게시에 실패해 재시도 타이머가 다시 걸어야 하는 자리다. mutex가 지킨다.</summary>
    /// <remarks>
    /// 완료 인계 중인 자리도 acceptSocket이 비어 있으므로, 소켓 유무로는 쉬는 자리를 가를 수 없다.
    /// 이 표시가 없으면 타이머가 인계 중인 OVERLAPPED에 요청을 한 번 더 건다.
    /// </remarks>
    bool needsRepost = false;
};

/// <summary>Acceptor의 구현 상태다.</summary>
/// <remarks>
/// 데이터 멤버에 m 접두를 붙이지 않은 이유는 IoContext::State와 같다. 캡슐화의 경계는
/// Acceptor 쪽에 있고 이 타입은 그 안쪽의 통이다.
/// </remarks>
class Acceptor::State final : public IIoCompletionTarget
{
public:
    void OnIoCompleted(
        IoOperation& operation, DWORD bytesTransferred, unsigned long errorCode) override;

    /// <summary>수락 요청 하나를 건다. mutex를 쥔 채 부른다.</summary>
    Core::Status PostAcceptLocked(AcceptOperation& operation);

    /// <summary>리슨 소켓을 닫는다. mutex를 쥔 채 부른다.</summary>
    void CloseListenSocketLocked();

    /// <summary>수락 요청이나 완료 뒤 인계의 수가 바뀌었음을 Stop() 대기자에게 알린다.</summary>
    /// <remarks>
    /// Stop()은 이미 시작된 처리기 인계와 남은 AcceptEx 취소를 서로 다른 조건으로 기다린다.
    /// 어느 한쪽만 0이 되어도 다음 단계로 갈 수 있으므로 상태가 바뀔 때마다 깨운다.
    /// </remarks>
    void NotifyAcceptStateChangedLocked();

    /// <summary>수락된 소켓을 연결로 만들어 처리기에 넘긴다. 잠금 밖에서 부른다.</summary>
    void HandleAcceptedSocket(SOCKET accepted, SOCKET listenSocketAtAccept, IoContext& context,
        const std::function<void(std::shared_ptr<Connection>)>& handler,
        const std::shared_ptr<WinsockScope>& winsockScope,
        const std::shared_ptr<SendBudget>& sendBudget, Core::IpEndpoint local,
        Core::IpEndpoint remote) noexcept;

    /// <summary>완료 뒤 인계 몫을 내리고 다음 수락을 준비한다. 절대 I/O 루프 밖으로 던지지 않는다.</summary>
    void CompleteHandoff(AcceptOperation& operation) noexcept;

    /// <summary>게시에 실패한 자리를 표시하고 재시도를 예약한다. mutex를 쥔 채 부른다.</summary>
    /// <remarks>
    /// 자리를 버리거나 리슨 소켓을 닫지 않는다. 예전에는 실패마다 자리가 영구히 줄고, 0이 되면
    /// 포트를 닫아 일시적인 자원 부족 몇 번이 리스너를 없앴다(NET-3). 이것을 고정하는 것은
    /// Transport.AcceptorRetriesAfterAcceptResourceFailure다.
    /// </remarks>
    void DeferRepostLocked(AcceptOperation& operation, const Core::Status& failure) noexcept;

    /// <summary>재시도 타이머를 한 번 예약한다. 이미 예약되어 있으면 아무것도 하지 않는다.</summary>
    void ScheduleRetryLocked() noexcept;

    /// <summary>표시된 자리를 다시 건다. 하나라도 실패하면 다시 예약한다. mutex를 쥔 채 부른다.</summary>
    void RetryDeferredAcceptsLocked();

    /// <summary>스레드 풀 타이머 콜백이다. 문맥은 이 State다.</summary>
    static void CALLBACK OnRetryTimer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) noexcept;

    std::mutex mutex;
    std::condition_variable acceptStateChanged;

    std::shared_ptr<WinsockScope> winsock;
    SOCKET listenSocket = INVALID_SOCKET;
    int family = AF_INET;
    Core::IpEndpoint localEndpoint;

    /// <summary>원자인 이유는 Port()가 noexcept라 잠금을 잡을 수 없기 때문이다.</summary>
    std::atomic<std::uint16_t> port{ 0 };

    IoContext* io = nullptr;

    /// <summary>io가 돌고 있는지를 담은 공유 표시다. Start()가 받고 Stop()이 놓는다.</summary>
    /// <remarks>Stop()이 IoContext 객체에 닿지 않고 완료를 처리할 스레드가 있는지 보는 데 쓴다.</remarks>
    std::shared_ptr<const std::atomic<bool>> ioRunning;

    LPFN_ACCEPTEX acceptEx = nullptr;
    LPFN_GETACCEPTEXSOCKADDRS getAcceptAddresses = nullptr;
    SharedConnectionHandler connectionHandler;
    std::shared_ptr<Core::ILogger> logger;
    std::shared_ptr<SendBudget> sendBudget =
        std::make_shared<SendBudget>(SendQueueLimits{}.totalBytes);

    std::array<AcceptOperation, PendingAcceptCount> acceptOperations;
    std::size_t pendingAccepts = 0;

    /// <summary>
    /// AcceptEx 완료를 받았지만 아직 처리기 인계와 다음 수락 준비를 끝내지 못한 수다.
    /// </summary>
    /// <remarks>
    /// 이 값은 pendingAccepts가 내려간 직후부터 완료 경로가 다시 mutex를 잡을 때까지 든다.
    /// Stop()은 둘 다 0일 때만 돌아가므로, 호출자는 Stop() 직후 IoContext를 멈춰도 된다.
    /// </remarks>
    std::size_t activeCompletionHandoffs = 0;

    bool stopping = false;
    bool started = false;

    /// <summary>게시에 실패한 자리를 다시 거는 타이머다. Start()가 만들고 Stop()이 닫는다.</summary>
    /// <remarks>
    /// 걸린 수락이 하나도 없으면 다음 완료가 오지 않으므로, 재시도를 깨울 것은 이 타이머뿐이다.
    /// Stop()은 잠금을 놓고 이 타이머의 콜백이 끝나기를 기다린 뒤 닫는다. 그래서 콜백이 가리키는
    /// 이 State는 콜백보다 오래 산다.
    /// </remarks>
    PTP_TIMER retryTimer = nullptr;
    bool retryScheduled = false;

    /// <summary>게시 실패를 기록했고 아직 모든 자리를 되찾지 못했다. 실패 줄은 이 동안 한 번만 남긴다.</summary>
    bool acceptDegraded = false;

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
    /// <summary>켜져 있으면 PostAcceptLocked가 WSAENOBUFS로 실패한다. mutex가 지킨다.</summary>
    bool injectAcceptFailures = false;
    std::size_t injectedAcceptFailures = 0;

    /// <summary>수락된 소켓을 IoContext에 붙이기 직전에 부른다. mutex가 지킨다.</summary>
    std::function<void()> beforeAssociateHook;
#endif

    /// <summary>Stop()이 취소된 AcceptEx 완료를 기다리는 제한이다. 시험만 바꾼다. mutex가 지킨다.</summary>
    std::chrono::milliseconds pendingAcceptDrainTimeout = PendingAcceptDrainTimeout;
};

Core::Status Acceptor::State::PostAcceptLocked(AcceptOperation& operation)
{
    SERVERCORE_ASSERT(operation.acceptSocket == INVALID_SOCKET,
        "PostAcceptLocked() was called on a slot that still holds a socket");

    if (listenSocket == INVALID_SOCKET || acceptEx == nullptr)
    {
        return Core::Status::Fail(
            Core::ErrorCode::Closed, "the accept was not posted because the acceptor is not open");
    }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
    if (injectAcceptFailures)
    {
        ++injectedAcceptFailures;
        return MakeSocketFailure("AcceptEx", WSAENOBUFS);
    }
#endif

    const SOCKET accepted =
        ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (accepted == INVALID_SOCKET)
    {
        return MakeSocketFailure("WSASocketW(accept)", ::WSAGetLastError());
    }

    operation.acceptSocket = accepted;
    operation.overlapped = OVERLAPPED{};
    operation.kind = IoOperationKind::Accept;
    operation.target = this;

    DWORD receivedBytes = 0;

    // 넷째 인자(받아 둘 데이터 길이)를 0으로 둔다. 0이 아니면 첫 바이트가 올 때까지 수락이
    // 완료되지 않으므로, 붙어 놓고 아무것도 안 보내는 상대 하나가 수락 자리 하나를 잡아 둔다.
    const BOOL posted = acceptEx(listenSocket, accepted, operation.addressBuffer.data(), 0,
        static_cast<DWORD>(AcceptAddressSize), static_cast<DWORD>(AcceptAddressSize),
        &receivedBytes, &operation.overlapped);

    if (posted == FALSE)
    {
        const int socketError = ::WSAGetLastError();
        if (socketError != WSA_IO_PENDING)
        {
            ::closesocket(accepted);
            operation.acceptSocket = INVALID_SOCKET;
            return MakeSocketFailure("AcceptEx", socketError);
        }
    }

    ++pendingAccepts;
    return Core::Status::Ok();
}

void Acceptor::State::CloseListenSocketLocked()
{
    if (listenSocket == INVALID_SOCKET)
    {
        return;
    }

    // 리슨 소켓을 닫으면 이 소켓에 걸려 있던 AcceptEx가 전부 실패로 돌아온다. 그 완료들이
    // pendingAccepts를 내린다. 이미 시작된 처리기 인계와 취소 완료는 Stop()이 단계별로
    // 기다린다.
    ::closesocket(listenSocket);
    listenSocket = INVALID_SOCKET;
    port.store(0, std::memory_order_release);
    localEndpoint = {};
}

void Acceptor::State::NotifyAcceptStateChangedLocked()
{
    acceptStateChanged.notify_all();
}

void Acceptor::State::HandleAcceptedSocket(SOCKET accepted, SOCKET listenSocketAtAccept,
    IoContext& context, const std::function<void(std::shared_ptr<Connection>)>& handler,
    const std::shared_ptr<WinsockScope>& winsockScope,
    const std::shared_ptr<SendBudget>& sendBudgetValue, Core::IpEndpoint local,
    Core::IpEndpoint remote) noexcept
{
    std::shared_ptr<TcpConnection> connection;
    try
    {
        // 수락된 소켓은 이 한 줄을 거쳐야 리슨 소켓의 성질을 물려받는다. 빠뜨리면 getpeername
        // 같은 것이 WSAENOTCONN으로 실패한다.
        if (::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                reinterpret_cast<const char*>(&listenSocketAtAccept),
                sizeof(listenSocketAtAccept)) == SOCKET_ERROR)
        {
            ReportAcceptFailure(logger,
                MakeSocketFailure("setsockopt(SO_UPDATE_ACCEPT_CONTEXT)", ::WSAGetLastError()));
            ::closesocket(accepted);
            return;
        }

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
        std::function<void()> beforeAssociate;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            beforeAssociate = beforeAssociateHook;
        }
        if (beforeAssociate)
        {
            beforeAssociate();
        }
#endif

        const Core::Status attached = IoContextAccess::AssociateSocket(context, accepted);
        if (!attached.IsOk())
        {
            ReportAcceptFailure(logger, attached);
            ::closesocket(accepted);
            return;
        }

        connection = TcpConnection::Create(accepted, winsockScope, sendBudgetValue, local, remote);

        // 처리기가 먼저다. 처리기 안에서 관찰자를 걸어야 첫 바이트를 놓치지 않는다.
        if (handler)
        {
            handler(connection);
        }

        const Core::Status receiveStarted = connection->Start();
        if (!receiveStarted.IsOk())
        {
            ReportAcceptFailure(logger, receiveStarted);

            // 수신을 못 걸었으면 이 연결은 아무것도 받지 못한다. 닫아서 그것을 관찰자에게 알린다.
            // Start()가 이미 닫았을 수도 있으나 Close()는 여러 번 불러도 된다.
            connection->Close();
        }
    }
    catch (...)
    {
        // 사용자 처리기와 동적 할당은 I/O 완료 루프 밖으로 예외를 내보내면 안 된다. 연결을
        // 닫아 이 세션만 정리하고, 아래 CompleteHandoff가 항상 다음 수락 자리와 Stop 대기를
        // 회복한다.
        if (logger)
            logger->Write(Core::LogLevel::Warn,
                "an accept completion handoff threw and the connection was closed");

        if (connection != nullptr)
        {
            connection->Close();
        }
        else
        {
            ::closesocket(accepted);
        }
    }
}

void Acceptor::State::CompleteHandoff(AcceptOperation& operation) noexcept
{
    try
    {
        const std::lock_guard<std::mutex> guard(mutex);

        SERVERCORE_ASSERT(activeCompletionHandoffs != 0,
            "an accept completion finished without an active completion handoff");
        --activeCompletionHandoffs;

        if (!stopping)
        {
            try
            {
                const Core::Status posted = PostAcceptLocked(operation);
                if (!posted.IsOk())
                {
                    DeferRepostLocked(operation, posted);
                }
            }
            catch (...)
            {
                // PostAcceptLocked의 오류 Status를 만드는 데조차 메모리를 못 얻는 경우다. 이것도
                // 일시적인 자원 부족이므로 자리를 버리지 않고 재시도에 맡긴다.
                DeferRepostLocked(operation, Core::Status::AllocationFailure());
            }
        }

        NotifyAcceptStateChangedLocked();
    }
    catch (...)
    {
        Core::ReportAssertFailure("Acceptor handoff accounting did not throw", __FILE__, __LINE__,
            "an accept handoff could not restore its completion accounting");
    }
}

void Acceptor::State::DeferRepostLocked(
    AcceptOperation& operation, const Core::Status& failure) noexcept
{
    operation.needsRepost = true;
    if (!acceptDegraded)
    {
        acceptDegraded = true;
        ReportAcceptFailure(logger, failure);
    }
    ScheduleRetryLocked();
}

void Acceptor::State::ScheduleRetryLocked() noexcept
{
    if (retryTimer == nullptr || retryScheduled)
    {
        return;
    }

    // 음수는 지금부터의 상대 시간이다. 단위는 100ns다.
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-AcceptRetryDelay100Nanoseconds);
    FILETIME dueTime{};
    dueTime.dwLowDateTime = due.LowPart;
    dueTime.dwHighDateTime = due.HighPart;
    ::SetThreadpoolTimer(retryTimer, &dueTime, 0, 0);
    retryScheduled = true;
}

void Acceptor::State::RetryDeferredAcceptsLocked()
{
    bool anyFailed = false;
    for (AcceptOperation& operation : acceptOperations)
    {
        if (!operation.needsRepost)
        {
            continue;
        }

        const Core::Status posted = PostAcceptLocked(operation);
        if (posted.IsOk())
        {
            operation.needsRepost = false;
        }
        else
        {
            anyFailed = true;
        }
    }

    if (anyFailed)
    {
        ScheduleRetryLocked();
    }
    else
    {
        acceptDegraded = false;
    }
}

void CALLBACK Acceptor::State::OnRetryTimer(
    PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) noexcept
{
    State& state = *static_cast<State*>(context);
    try
    {
        const std::lock_guard<std::mutex> guard(state.mutex);
        state.retryScheduled = false;
        if (state.stopping || state.listenSocket == INVALID_SOCKET)
        {
            return;
        }

        try
        {
            state.RetryDeferredAcceptsLocked();
        }
        catch (...)
        {
            // 오류 Status조차 만들지 못했다. 표시된 자리는 남아 있으므로 다음 예약이 다시 시도한다.
            state.ScheduleRetryLocked();
        }
    }
    catch (...)
    {
        Core::ReportAssertFailure("Acceptor retry timer did not throw", __FILE__, __LINE__,
            "an accept retry could not take the acceptor lock");
    }
}

void Acceptor::State::OnIoCompleted(IoOperation& operation, DWORD, unsigned long errorCode)
{
    SERVERCORE_ASSERT(operation.kind == IoOperationKind::Accept,
        "an acceptor received a completion whose kind is not Accept");

    AcceptOperation& acceptOperation = static_cast<AcceptOperation&>(operation);

    SOCKET accepted = INVALID_SOCKET;
    SOCKET listenSocketAtAccept = INVALID_SOCKET;
    IoContext* context = nullptr;
    SharedConnectionHandler handler;
    std::shared_ptr<WinsockScope> winsockScope;
    std::shared_ptr<SendBudget> sendBudgetValue;
    Core::IpEndpoint local, remote;
    bool isStopping = false;
    bool handoffActive = false;

    try
    {
        {
            const std::lock_guard<std::mutex> guard(mutex);

            SERVERCORE_ASSERT(pendingAccepts != 0,
                "an accept completion arrived without a matching pending accept request");

            // pendingAccepts는 완료를 받는 즉시 내려야 다음 요청을 걸 수 있다. 그러나 여기서
            // 기다림을 끝내면 잠금 밖의 AssociateSocket()과 처리기 호출이 IoContext::Stop()과
            // 경합한다. 완료 경로가 끝날 때까지 별도 몫을 들고 있는 이유다.
            ++activeCompletionHandoffs;
            handoffActive = true;
            --pendingAccepts;

            accepted = acceptOperation.acceptSocket;
            acceptOperation.acceptSocket = INVALID_SOCKET;

            listenSocketAtAccept = listenSocket;
            context = io;
            isStopping = stopping;

            if (!isStopping && errorCode == 0)
            {
                handler = connectionHandler;
                winsockScope = winsock;
                sendBudgetValue = sendBudget;
                sockaddr *localAddress = nullptr, *remoteAddress = nullptr;
                int localLength = 0, remoteLength = 0;
                getAcceptAddresses(acceptOperation.addressBuffer.data(), 0,
                    static_cast<DWORD>(AcceptAddressSize), static_cast<DWORD>(AcceptAddressSize),
                    &localAddress, &localLength, &remoteAddress, &remoteLength);
                local = FromNativeEndpoint(localAddress, localLength);
                remote = FromNativeEndpoint(remoteAddress, remoteLength);
            }
        }

        const bool canHandOver =
            accepted != INVALID_SOCKET && errorCode == 0 && !isStopping && context != nullptr;

        if (canHandOver)
        {
            HandleAcceptedSocket(accepted, listenSocketAtAccept, *context, *handler, winsockScope,
                sendBudgetValue, local, remote);
        }
        else if (accepted != INVALID_SOCKET)
        {
            // 멈추는 중이거나 수락 자체가 실패했다. 미리 만들어 둔 소켓을 여기서 거둔다.
            if (errorCode != 0 && !isStopping)
            {
                ReportAcceptFailure(logger, MakeWin32Failure("AcceptEx completion", errorCode));
            }
            ::closesocket(accepted);
        }
    }
    catch (...)
    {
        if (!handoffActive)
        {
            // handoffActive가 켜지기 전에는 mutex 획득 말고 예외를 내는 연산이 없다. 여기서
            // 계속 돌면 pendingAccepts를 내릴 수 없어 Stop이 영구히 기다리므로 계약 위반이다.
            Core::ReportAssertFailure("Acceptor completion accounting did not throw", __FILE__,
                __LINE__, "an accept completion could not claim its accounting state");
        }

        // handoff 준비나 오류 보고의 예외도 완료 루프를 죽여서는 안 된다. 아직 Connection이
        // 소유하지 않은 소켓만 이 자리에서 닫는다.
        if (accepted != INVALID_SOCKET)
        {
            ::closesocket(accepted);
        }
        if (logger)
            logger->Write(Core::LogLevel::Warn,
                "an accept completion setup threw and the connection was closed");
    }

    if (handoffActive)
    {
        // Stop may release the registered handler as soon as the handoff count
        // reaches zero. Release this invocation's snapshot before that boundary.
        handler.reset();
        CompleteHandoff(acceptOperation);
    }
}

Acceptor::Acceptor()
    : mState(std::make_unique<State>())
{
}

void AcceptorAccess::SetSendBudget(Acceptor& acceptor, std::shared_ptr<SendBudget> sendBudget)
{
    const std::lock_guard<std::mutex> guard(acceptor.mState->mutex);
    SERVERCORE_ASSERT(
        !acceptor.mState->started, "Acceptor send budget must be configured before Start()");
    acceptor.mState->sendBudget = std::move(sendBudget);
}

#if defined(SERVERCORE_ENABLE_TEST_HOOKS)
void AcceptorAccess::SetAcceptFailureInjection(Acceptor& acceptor, const bool enabled)
{
    const std::lock_guard<std::mutex> guard(acceptor.mState->mutex);
    acceptor.mState->injectAcceptFailures = enabled;
}

std::size_t AcceptorAccess::InjectedAcceptFailureCount(const Acceptor& acceptor)
{
    const std::lock_guard<std::mutex> guard(acceptor.mState->mutex);
    return acceptor.mState->injectedAcceptFailures;
}

void AcceptorAccess::SetBeforeAssociateHook(Acceptor& acceptor, std::function<void()> hook)
{
    const std::lock_guard<std::mutex> guard(acceptor.mState->mutex);
    acceptor.mState->beforeAssociateHook = std::move(hook);
}

void AcceptorAccess::SetPendingAcceptDrainTimeout(
    Acceptor& acceptor, const std::chrono::milliseconds timeout)
{
    const std::lock_guard<std::mutex> guard(acceptor.mState->mutex);
    acceptor.mState->pendingAcceptDrainTimeout = timeout;
}

std::size_t AcceptorAccess::PendingAcceptCount(const Acceptor& acceptor)
{
    const std::lock_guard<std::mutex> guard(acceptor.mState->mutex);
    return acceptor.mState->pendingAccepts;
}
#endif

Acceptor::~Acceptor()
{
    Stop();
}

Core::Status Acceptor::Listen(
    const std::string_view listenAddress, const std::uint16_t port, const int backlog)
{
    const auto parsed = Core::IpEndpoint::Parse(listenAddress, port);
    return Listen(parsed.IsOk() ? parsed.Value() : Core::IpEndpoint{}, backlog);
}
Core::Status Acceptor::Listen(
    const Core::IpEndpoint& endpoint, const int backlog, const bool ipv6Only)
{
    SERVERCORE_ASSERT(backlog > 0, "Listen() was given a backlog that is not positive");

    const std::lock_guard<std::mutex> guard(mState->mutex);

    if (mState->listenSocket != INVALID_SOCKET)
    {
        return Core::Status::Fail(
            Core::ErrorCode::AlreadyExists, "Listen() was called while already listening");
    }

    if (endpoint.port == 0)
    {
        return Core::Status::Fail(Core::ErrorCode::InvalidArgument, "listen port must not be zero");
    }

    NativeEndpoint address;
    if (!ToNativeEndpoint(endpoint, address))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);

    Core::Result<std::shared_ptr<WinsockScope>> winsock = AcquireWinsock();
    if (!winsock.IsOk())
    {
        return std::move(winsock).TakeStatus();
    }

    const SOCKET listenSocket =
        ::WSASocketW(address.Family(), SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (listenSocket == INVALID_SOCKET)
    {
        return MakeSocketFailure("WSASocketW(listen)", ::WSAGetLastError());
    }
    if (!SetIpv6Only(listenSocket, address.Family(), ipv6Only))
    {
        auto failure = MakeSocketFailure("setsockopt(IPV6_V6ONLY)", ::WSAGetLastError());
        ::closesocket(listenSocket);
        return failure;
    }

    // SO_EXCLUSIVEADDRUSE를 켜고 SO_REUSEADDR는 켜지 않는다. Windows에서 SO_REUSEADDR는
    // 다른 프로세스가 같은 포트를 가로채는 것을 허용한다. 포트가 이미 쓰이면 bind가 10048로
    // 실패해 그 자리에서 드러나는 편이, 두 서버가 접속을 나눠 갖는 것보다 낫다.
    const BOOL exclusive = TRUE;
    if (::setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
    {
        Core::Status failure =
            MakeSocketFailure("setsockopt(SO_EXCLUSIVEADDRUSE)", ::WSAGetLastError());
        ::closesocket(listenSocket);
        return failure;
    }

    if (::bind(listenSocket, address.Address(), address.length) == SOCKET_ERROR)
    {
        Core::Status failure = MakeSocketFailure("bind", ::WSAGetLastError());
        ::closesocket(listenSocket);
        return failure;
    }

    if (::listen(listenSocket, backlog) == SOCKET_ERROR)
    {
        Core::Status failure = MakeSocketFailure("listen", ::WSAGetLastError());
        ::closesocket(listenSocket);
        return failure;
    }

    mState->winsock = std::move(winsock.Value());
    mState->listenSocket = listenSocket;
    mState->family = address.Family();
    mState->localEndpoint = ReadSocketEndpoint(listenSocket, false);
    mState->port.store(endpoint.port, std::memory_order_release);
    mState->stopping = false;

    return Core::Status::Ok();
}

void Acceptor::SetLogger(std::shared_ptr<Core::ILogger> logger)
{
    std::shared_ptr<Core::ILogger> previous;
    {
        const std::lock_guard guard(mState->mutex);
        SERVERCORE_ASSERT(!mState->started, "Acceptor::SetLogger requires an inactive listener");
        previous = std::exchange(mState->logger, std::move(logger));
    }
}
void Acceptor::SetConnectionHandler(std::function<void(std::shared_ptr<Connection>)> handler)
{
    SharedConnectionHandler registered =
        handler ? std::make_shared<const std::function<void(std::shared_ptr<Connection>)>>(
                      std::move(handler))
                : nullptr;
    const std::lock_guard<std::mutex> guard(mState->mutex);
    SERVERCORE_ASSERT(!mState->started && mState->activeCompletionHandoffs == 0,
        "the connection handler must be configured before accepting");
    mState->connectionHandler.swap(registered);
}

Core::Status Acceptor::SetSendQueueLimits(const SendQueueLimits& limits)
{
    if (!ValidSendQueueLimits(limits))
        return Core::Status::FailWithoutMessage(Core::ErrorCode::InvalidArgument);
    const std::lock_guard guard(mState->mutex);
    if (mState->started || mState->activeCompletionHandoffs != 0)
        return Core::Status::FailWithoutMessage(Core::ErrorCode::Closed);
    try
    {
        mState->sendBudget =
            std::make_shared<SendBudget>(limits.totalBytes, limits.connectionBytes);
    }
    catch (...)
    {
        return Core::Status::AllocationFailure();
    }
    return Core::Status::Ok();
}

Core::IpEndpoint Acceptor::LocalEndpoint() const noexcept
{
    const std::lock_guard guard(mState->mutex);
    return mState->localEndpoint;
}

Core::Status Acceptor::Start(IoContext& io)
{
    const std::lock_guard<std::mutex> guard(mState->mutex);

    SERVERCORE_ASSERT(
        mState->listenSocket != INVALID_SOCKET, "Start() was called before Listen() succeeded");
    SERVERCORE_ASSERT(static_cast<bool>(mState->connectionHandler),
        "Start() was called before a connection handler was set");
    SERVERCORE_ASSERT(io.IsRunning(), "Start() was given an IoContext that is not running");

    if (mState->started)
    {
        return Core::Status::Fail(
            Core::ErrorCode::AlreadyExists, "Start() was called while already accepting");
    }

    mState->io = &io;

    // AcceptEx의 주소는 실행 시간에 물어봐야 한다. 확장 함수라 가져다 쓸 심볼이 없다.
    GUID acceptExGuid = WSAID_ACCEPTEX;
    DWORD returnedBytes = 0;
    if (::WSAIoctl(mState->listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &acceptExGuid,
            sizeof(acceptExGuid), &mState->acceptEx, sizeof(mState->acceptEx), &returnedBytes,
            nullptr, nullptr) == SOCKET_ERROR)
    {
        mState->io = nullptr;
        return MakeSocketFailure(
            "WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER, AcceptEx)", ::WSAGetLastError());
    }

    GUID addressesGuid = WSAID_GETACCEPTEXSOCKADDRS;
    if (::WSAIoctl(mState->listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &addressesGuid,
            sizeof addressesGuid, &mState->getAcceptAddresses, sizeof mState->getAcceptAddresses,
            &returnedBytes, nullptr, nullptr) == SOCKET_ERROR)
    {
        mState->io = nullptr;
        return MakeSocketFailure("WSAIoctl(GetAcceptExSockaddrs)", ::WSAGetLastError());
    }
    Core::Status attached = IoContextAccess::AssociateSocket(io, mState->listenSocket);
    if (!attached.IsOk())
    {
        mState->io = nullptr;
        mState->acceptEx = nullptr;
        return attached;
    }

    // 자원이 모자랄 때 재시도를 깨울 타이머는 그 전에 만들어 둔다. 모자란 순간에는 만들 수 없을 수 있다.
    mState->retryTimer = ::CreateThreadpoolTimer(&State::OnRetryTimer, mState.get(), nullptr);
    if (mState->retryTimer == nullptr)
    {
        mState->io = nullptr;
        mState->acceptEx = nullptr;
        return MakeWin32Failure("CreateThreadpoolTimer(accept retry)", ::GetLastError());
    }

    Core::Status lastFailure = Core::Status::Ok();
    for (AcceptOperation& operation : mState->acceptOperations)
    {
        Core::Status posted = mState->PostAcceptLocked(operation);
        if (!posted.IsOk())
        {
            operation.needsRepost = true;
            lastFailure = std::move(posted);
        }
    }

    if (mState->pendingAccepts == 0)
    {
        // 하나도 걸지 못했으면 이 수락기는 아무것도 받지 못한다. 실패로 돌려준다. 예약한 것이
        // 없으므로 타이머 콜백을 기다리지 않고 닫는다.
        for (AcceptOperation& operation : mState->acceptOperations)
        {
            operation.needsRepost = false;
        }
        ::CloseThreadpoolTimer(std::exchange(mState->retryTimer, nullptr));
        mState->io = nullptr;
        mState->acceptEx = nullptr;
        return lastFailure;
    }

    mState->started = true;
    mState->ioRunning = IoContextAccess::RunningFlag(io);
    if (!lastFailure.IsOk())
    {
        // 일부만 걸렸다. 받기는 하지만 자리가 모자라므로 기록하고 나머지를 재시도에 맡긴다.
        mState->acceptDegraded = true;
        ReportAcceptFailure(mState->logger, lastFailure);
        mState->ScheduleRetryLocked();
    }

    return Core::Status::Ok();
}

void Acceptor::Stop()
{
    SharedConnectionHandler retiredHandler;
    std::unique_lock<std::mutex> guard(mState->mutex);

    if (mState->io != nullptr)
    {
        SERVERCORE_ASSERT(!mState->io->IsCurrentThreadIoThread(),
            "Stop() was called from an I/O thread of the IoContext this acceptor uses");
    }

    mState->stopping = true;
    mState->CloseListenSocketLocked();

    // 재시도 콜백은 이 잠금을 잡으므로, 잠금을 쥔 채 콜백을 기다리면 서로 기다린다. 놓고 기다린다.
    // 이미 stopping을 세웠으므로 이 사이에 도는 콜백은 아무것도 걸지 않는다.
    if (PTP_TIMER retiredTimer = std::exchange(mState->retryTimer, nullptr))
    {
        mState->retryScheduled = false;
        guard.unlock();
        ::SetThreadpoolTimer(retiredTimer, nullptr, 0, 0);
        ::WaitForThreadpoolTimerCallbacks(retiredTimer, TRUE);
        ::CloseThreadpoolTimer(retiredTimer);
        guard.lock();
    }

    // Stop()이 stopping을 세우기 전에 이미 시작된 완료 경로만 사용자 처리기를 부를 수 있다.
    // 처리기는 실행 시간이 정해진 일이 아니며, I/O 스레드가 하나라면 이 처리기가 돌아와야 그
    // 스레드가 나머지 AcceptEx 취소 완료도 처리할 수 있다. 따라서 이 단계에는 시간 제한을 두지
    // 않는다.
    mState->acceptStateChanged.wait(
        guard, [this] { return mState->activeCompletionHandoffs == 0; });

    // 이제 새 완료 경로는 stopping을 보고 사용자 처리기를 부르지 않는다. 남은 것은 리슨 소켓을
    // 닫으며 취소한 AcceptEx의 완료와 그 짧은 내부 정리뿐이다. pendingAccepts는 완료 시작 때
    // 먼저 내려가므로 activeCompletionHandoffs까지 함께 보아야 정리가 끝난 경계가 된다.
    const auto drained = [this]
    { return mState->pendingAccepts == 0 && mState->activeCompletionHandoffs == 0; };

    // IoContext가 돌고 있으면 I/O 스레드가 다른 연결의 긴 콜백에 묶여 있어도 결국 이 완료를
    // 처리하므로 제한 없이 기다린다(NET-7). 이것을 고정하는 것은
    // Transport.AcceptorStopWaitsForBusyIoWorkers다. 제한은 IoContext가 멈춘 것을 본 뒤에만 잰다.
    while (!drained() && mState->ioRunning != nullptr &&
           mState->ioRunning->load(std::memory_order_acquire))
    {
        (void)mState->acceptStateChanged.wait_for(guard, IoRunningPollInterval, drained);
    }

    // 멈춘 IoContext는 멈추기 전에 쌓인 완료만 처리한다. 그 뒤에도 제한 안에 오지 않으면 IoContext를
    // 먼저 멈춘 잘못된 종료 순서이므로 계약 위반이다.
    const bool pendingAcceptsDrained =
        mState->acceptStateChanged.wait_for(guard, mState->pendingAcceptDrainTimeout, drained);

    // 여기서 그냥 돌아오면 이 객체가 사라진 뒤에 완료가 그 자리를 가리킨다. 제한을 넘긴 것은
    // 종료 순서를 어겼다는 뜻이므로 계약 위반으로 다룬다.
    SERVERCORE_ASSERT(pendingAcceptsDrained,
        "Stop() timed out while waiting for cancelled accept requests to complete");

    mState->io = nullptr;
    mState->ioRunning.reset();
    mState->acceptEx = nullptr;
    mState->started = false;
    for (AcceptOperation& operation : mState->acceptOperations)
    {
        operation.needsRepost = false;
    }
    mState->acceptDegraded = false;
    retiredHandler = std::move(mState->connectionHandler);
    mState->winsock.reset();

    // 다시 Listen()할 수 있게 되돌린다. 이 객체는 한 번 쓰고 버리는 것이 아니다.
    mState->stopping = false;
}

std::uint16_t Acceptor::Port() const noexcept
{
    return mState->port.load(std::memory_order_acquire);
}
}
