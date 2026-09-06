#pragma once

#include "ServerCore/Core/Config.h"
#include "ServerCore/Core/Error.h"
#include "ServerCore/Core/Logging.h"
#include "ServerCore/Dispatch/Dispatcher.h"
#include "ServerCore/Protocol/Framing.h"
#include "ServerCore/Runtime/JobRunner.h"
#include "ServerCore/Runtime/Metrics.h"
#include "ServerCore/Session/Session.h"
#include "ServerCore/Session/SessionRegistry.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

/// <summary>
/// 실행 환경 층. 서버 프로세스의 뼈대이고, 각 게임의 백엔드가 올라서는 자리다.
/// </summary>
namespace ServerCore::Runtime
{
/// <summary>ServerHost 하나가 설정으로 만들 수 있는 같은 종류의 worker 스레드 상한이다.</summary>
/// <remarks>
/// 설정 오타 하나가 운영체제 자원을 고갈시키지 않게 하는 안전 경계다. 실제로 알맞은 값은
/// 하드웨어와 부하 측정으로 이 상한 아래에서 정한다.
/// </remarks>
inline constexpr int MaximumServerHostWorkerThreadCount = 64;

/// <summary>서버 Host를 파일 형식과 무관하게 구성하는 값이다.</summary>
/// <remarks>
/// Config의 파일 형식과, ServerHost가 그 파일에서 읽을 키의 이름은 별개다. 게임 서버가 각자
/// 임시 설정 파서를 쓰지 않도록 이 값 객체를 먼저 둔다. Config 기반 Configure는 Host 전용 키를
/// 이 값으로 옮기는 어댑터가 된다.
/// </remarks>
struct ServerHostOptions
{
    /// <summary>들을 TCP 포트. 0은 미구성 값이며 Configure에서 거절한다.</summary>
    std::uint16_t port = 0;

    /// <summary>IOCP 완료를 처리할 I/O 스레드 수.</summary>
    /// <remarks>MaximumServerHostWorkerThreadCount를 넘을 수 없다.</remarks>
    int ioWorkerThreadCount = 1;

    /// <summary>완결 JSON 프레임의 파싱만 맡을 worker 수. 0이면 기존 JobRunner 동기 파싱이다.</summary>
    /// <remarks>
    /// FrameReader와 Dispatcher·게임 처리기는 언제나 JobRunner에 남는다. 양수면 서로 다른 세션의
    /// ParseMessage 호출만 이 worker들에서 병렬 실행하고, 결과 적용은 다시 JobRunner에 돌아온다.
    /// 같은 세션에서는 동시에 하나만 실행하므로 TCP 메시지 순서는 유지된다.
    /// MaximumServerHostWorkerThreadCount를 넘을 수 없다.
    /// </remarks>
    int parseWorkerThreadCount = 0;

    /// <summary>운영체제가 대기시킬 아직 수락하지 않은 연결 수.</summary>
    int acceptBacklog = 16;

    /// <summary>마지막 수신 뒤 세션을 유지할 최대 시간이다. 0이면 유휴 만료 검사를 끈다.</summary>
    /// <remarks>
    /// ServerCore는 비어 있지 않은 TCP 바이트가 도착한 시각만 기록한다. 하트비트의 메시지
    /// 타입·송신 주기·각 게임에 맞는 시간 값은 게임 서버가 정한다. 그러므로 부분 프레임도
    /// 수신 활동으로 보며, 프레임 완결 기한 같은 별도 보안 정책은 이 값에 포함하지 않는다.
    /// 실제 종료는 내부 주기 검사와 JobRunner 대기 뒤에 일어나므로 실시간 마감 시각을 약속하지
    /// 않는다.
    /// </remarks>
    std::chrono::milliseconds idleSessionTimeout{ 0 };

    /// <summary>프레임 하나의 JSON 본문 상한. 기본은 64 KiB다.</summary>
    /// <remarks>
    /// 이 Host가 보내는 프레임도 L1의 단일 송신 큐에 통째로 들어가야 한다. 따라서 Configure는
    /// 머리까지 포함한 크기가 그 큐 상한을 넘는 값을 거절한다. 더 큰 메시지의 분할·스트리밍은
    /// 와이어 의미가 정해진 게임 층의 일이다.
    /// </remarks>
    std::uint32_t maxBodySize = Protocol::DefaultMaxBodySize;

    /// <summary>동시에 살아 있을 수 있는 네트워크 세션 수의 상한이다.</summary>
    /// <remarks>
    /// 세션마다 전송 버퍼와 FrameReader 저장소를 가지므로 이 값은 연결 수뿐 아니라 Host 전체
    /// 메모리의 첫 번째 경계다. Configure는 이 값과 maxBodySize의 곱으로 생기는 FrameReader
    /// 저장소의 합을 확인하고, 실제 송신 payload 합계는 maxTotalSendQueueCapacityBytes의 공유
    /// 런타임 예산으로 제한한다. 넘는 연결은 세션을 만들기 전에 닫는다.
    /// 연결 수의 안전 상한은 65,536이며, (maxBodySize + 4) * maxConcurrentSessions는
    /// 1 GiB 이하여야 한다. FrameReader는 각 세션의 첫 수신 때 저장소를 확보하므로 이 곱은
    /// 시작 시 일괄 할당량이 아닌 최악의 저장소 합계다. 연결별 전송 버퍼·OS 소켓 메모리와
    /// 실제 수신·파싱·송신 대기열은 별도이며, 이 상한이 총 프로세스 메모리를 뜻하지는 않는다.
    /// </remarks>
    std::uint32_t maxConcurrentSessions = 256;

    /// <summary>
    /// 한 세션이 JobRunner에서 아직 처리 중이거나 대기 중인 수신 바이트에 쓸 수 있는 상한이다.
    /// </summary>
    /// <remarks>
    /// 느린 게임 처리기가 있는 동안 TCP 수신 완료마다 복사본을 무한히 쌓지 않기 위한 역압이다.
    /// 넘으면 그 연결만 TooLarge로 닫고 다른 세션의 작업 큐는 계속 돈다.
    /// </remarks>
    std::uint32_t maxPendingReceiveBytes = 4u * Protocol::DefaultMaxBodySize;

    /// <summary>모든 세션이 합쳐 아직 처리 중이거나 대기 중인 수신 바이트에 쓸 수 있는 상한이다.</summary>
    /// <remarks>
    /// 세션별 상한만 있으면 많은 연결이 각각 한도까지 쌓을 수 있다. 이 예산은 그런 합계를
    /// 막고, 넘는 새 수신은 해당 연결만 TooLarge로 닫는다.
    /// </remarks>
    std::uint32_t maxTotalPendingReceiveBytes = 128u * Protocol::DefaultMaxBodySize;

    /// <summary>파싱 worker가 켜졌을 때 세션 하나가 보관할 완결 JSON 본문 바이트 상한이다.</summary>
    /// <remarks>
    /// FrameReader 안에서 다음 순서를 기다리는 본문과 worker가 실행 중인 본문을 모두 센다.
    /// 수신 예산과 다른 경계인 이유는 수신 callback 복사본을 JobRunner가 비운 뒤에도 JSON 본문은
    /// worker 완료까지 남기 때문이다.
    /// </remarks>
    std::uint32_t maxPendingParseBytes = 4u * Protocol::DefaultMaxBodySize;

    /// <summary>파싱 worker가 켜졌을 때 Host 전체가 보관할 완결 JSON 본문 바이트 상한이다.</summary>
    std::uint32_t maxTotalPendingParseBytes = 128u * Protocol::DefaultMaxBodySize;

    /// <summary>파싱 worker가 켜졌을 때 세션 하나가 보관할 완결 프레임 수 상한이다.</summary>
    /// <remarks>
    /// 바이트만 제한하면 아주 작은 프레임이 task 객체와 deque 노드를 먼저 늘릴 수 있다. 이 값은
    /// worker에 아직 안 들어간 순서 대기 프레임도 포함한다.
    /// </remarks>
    std::uint32_t maxPendingParseTasks = 64;

    /// <summary>파싱 worker가 켜졌을 때 Host 전체가 보관할 완결 프레임 수 상한이다.</summary>
    std::uint32_t maxTotalPendingParseTasks = 4096;

    /// <summary>들을 IPv4 주소다. 기본은 같은 기계의 loopback뿐이다.</summary>
    /// <remarks>
    /// 외부 연결을 받을 서버는 명시적으로 "0.0.0.0" 또는 배포 환경의 특정 IPv4 주소를 둔다.
    /// 주소 형식은 Acceptor가 포트를 열 때 검사한다.
    /// </remarks>
    std::string listenAddress = "127.0.0.1";

    /// <summary>모든 세션의 Connection 송신 큐 용량을 합친 상한이다.</summary>
    /// <remarks>
    /// ServerHost가 만든 모든 Connection은 전송 완료 전까지 보관하는 payload를 이 공유 예산에서
    /// 먼저 예약한다. 남은 예산이 없으면 해당 Send는 아무것도 넣지 않고 WouldBlock을 돌려준다.
    /// 세션별 SendQueueLimitBytes도 함께 유지된다. 기본은 256 MiB이며 안전 상한은 512 MiB다.
    /// aggregate 초기화 호환성을 위해 이 필드까지의 선언 순서를 유지한다.
    /// </remarks>
    std::uint32_t maxTotalSendQueueCapacityBytes = 256u * 1024u * 1024u;

    /// <summary>SendAndDisconnect가 Closing으로 전이한 뒤 송신 큐를 비울 최대 시간이다.</summary>
    /// <remarks>
    /// 양수만 허용하며 0으로 끌 수 없다. 상대가 읽지 않아 송신이 멈춰도 이 시간이 지나면
    /// 남은 큐를 버리고 연결을 닫아 세션 슬롯과 공유 송신 예산을 반환한다. 수신 바이트는 이
    /// 절대 기한을 연장하지 않는다. idleSessionTimeout이 0이어도 검사는 계속 실행된다.
    /// 공통 검사 주기는 활성화된 두 제한 중 짧은 값으로 정하며, 실제 종료는 그 주기와
    /// JobRunner 대기 뒤에 일어난다. 따라서 실시간 마감 시각을 약속하지는 않는다.
    /// aggregate 초기화 호환성을 위해 이 필드까지의 선언 순서를 유지한다.
    /// </remarks>
    std::chrono::milliseconds gracefulCloseTimeout{ 5000 };
};

/// <summary>
/// 전용 서버 하나를 세우고 돌리고 내린다.
/// </summary>
/// <remarks>
/// 왜 이 자리에 있는가:
/// 각 게임이 자기 백엔드를 올린다고 할 때, 그 "올리는 자리"가 실체로 있어야 한다. 부팅 순서와
/// 종료가 여기 없으면 게임마다 그것을 따로 쓰게 되고, 그러면 두 서버가 서로 다른 순서로
/// 시작한다.
///
/// 부팅 순서 - 이 순서를 이 클래스가 소유한다:
///   1. 로거 설치
///   2. 설정 적재
///   3. 게임 백엔드가 처리기와 관찰자를 등록한다
///   4. I/O 문맥과 작업 실행자를 시작한다
///   5. 등록표를 닫고 포트를 연다
///   6. 수락을 시작한다
///
/// 3번이 5번보다 먼저인 것은 계약이다. 순서가 뒤집히면 처리기가 등록되기 전에 첫 클라이언트가
/// 붙을 수 있고, 그때 들어온 메시지는 "모르는 타입"이 된다. 기록만 남고 원인은 안 보이는
/// 종류의 결함이다.
///
/// 소유한다: 스레드, 부팅과 종료의 순서, 조립 지점.
/// 소유하지 않는다: 게임 내용. 방, 플레이어, 좌표라는 낱말이 이 층에 없다.
///
/// 약속하지 않는 것:
/// - 종료 대기 경계는 Stop()의 호출 계약을 따른다.
/// - 게임의 주기 실행(틱) 정책은 호출자가 정한다.
/// - 여러 서버 인스턴스를 한 프로세스에서 돌리는 것을 약속하지 않는다.
/// </remarks>
class ServerHost
{
public:
    ServerHost();
    ~ServerHost();

    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;

    /// <summary>Config의 ServerCore Host 예약 키를 적용한다.</summary>
    /// <remarks>
    /// 필수 키는 다음 하나다.
    /// - servercore.host.port: 1부터 65535까지의 TCP 포트
    ///
    /// 아래 선택 키가 없으면 ServerHostOptions의 현재 기본값을 쓴다.
    /// - servercore.host.listen-address
    /// - servercore.host.io-worker-thread-count
    /// - servercore.host.parse-worker-thread-count
    /// - servercore.host.accept-backlog
    /// - servercore.host.idle-session-timeout-ms (밀리초)
    /// - servercore.host.graceful-close-timeout-ms (양수 밀리초)
    /// - servercore.host.max-body-size
    /// - servercore.host.max-concurrent-sessions
    /// - servercore.host.max-total-send-queue-capacity-bytes
    /// - servercore.host.max-pending-receive-bytes
    /// - servercore.host.max-total-pending-receive-bytes
    /// - servercore.host.max-pending-parse-bytes
    /// - servercore.host.max-total-pending-parse-bytes
    /// - servercore.host.max-pending-parse-tasks
    /// - servercore.host.max-total-pending-parse-tasks
    ///
    /// 이 어댑터는 위 목록 외의 키를 읽지 않는다. 나머지 키의 소유자와 해석은 호출자가 정한다.
    /// 모든 값을 지역 옵션에 옮긴 뒤 ServerHostOptions overload를 한 번 호출하므로, 기존 옵션
    /// 검증과 lifecycle 경계가 똑같이 적용되고 중간 실패가 이미 적용한 설정을 남기지 않는다.
    ///
    /// Config 조회 실패는 그 ErrorCode를 유지한다. 부호 없는 옵션에 음수를 주거나 port가
    /// uint16_t 범위를 벗어나면 InvalidArgument이며, 다른 옵션 범위·상호 관계 검증은
    /// ServerHostOptions overload가 맡는다.
    /// </remarks>
    Core::Status Configure(const Core::Config& config);

    /// <summary>명시적 옵션을 적용한다. Start 전에 부른다.</summary>
    Core::Status Configure(const ServerHostOptions& options);

    /// <summary>기록을 남길 곳을 정한다. 부팅의 가장 첫 단계다.</summary>
    void SetLogger(std::shared_ptr<Core::ILogger> logger);

    /// <summary>
    /// 게임 백엔드가 메시지 처리기를 등록하는 자리다.
    /// </summary>
    /// <remarks>Start 전에만 쓴다. 도는 중에 등록하는 것은 지원하지 않는다.</remarks>
    [[nodiscard]] Dispatch::Dispatcher& GetDispatcher() noexcept;

    /// <summary>붙어 있는 세션 목록의 읽기 전용 참조를 준다. 브로드캐스트가 필요한 쪽이 쓴다.</summary>
    /// <remarks>
    /// SessionRegistry의 계약대로 JobRunner 문맥 안에서만 사용한다. 등록과 해제는 연결 수명과
    /// ServerHost 종료 계수를 함께 바꾸므로 Host만 수행한다.
    /// </remarks>
    [[nodiscard]] const Session::SessionRegistry& GetSessions() const noexcept;

    /// <summary>세션 목록과 같은 직렬 문맥에 일을 넣을 수 있는 수명 핸들이다.</summary>
    /// <remarks>
    /// 이 값은 Post와 정지 상태 조회만 제공한다. Host가 스레드와 종료 순서를 소유하므로 게임
    /// 백엔드는 이 실행자를 멈추거나 직접 돌릴 수 없다. PeriodicRunner에는 이 Lease를 그대로
    /// 넘긴다.
    /// </remarks>
    [[nodiscard]] JobRunner::Lease GetJobRunner() const noexcept;

    /// <summary>세션이 열리고 닫히는 것을 받을 관찰자를 건다. 약한 참조로 잡는다.</summary>
    void SetSessionObserver(std::weak_ptr<Session::ISessionObserver> observer);

    /// <summary>부팅 순서대로 서버를 세운다. 성공으로 돌아오면 포트가 열려 있다.</summary>
    /// <remarks>
    /// 로거·관찰자·처리기 등록과 Configure는 이 함수를 부르기 전에 끝나야 한다. Stop()이 시작됐거나
    /// 앞선 부팅 시도가 실패한 객체를 다시 시작하면 Closed를 돌린다. 새 서버가 필요하면 새
    /// ServerHost를 만든다. 수락기가 열린 뒤 최종 Running 전이 사이에 동기 종료 callback이
    /// Stop()을 요청한 경우에도 부팅을 끝까지 되돌린 뒤 Closed를 돌린다. 따라서 성공으로 돌아오면
    /// 포트가 열려 있다는 위 계약은 그대로 유지된다.
    /// </remarks>
    Core::Status Start();

    /// <summary>종료를 요청한다. 스레드 안전하다.</summary>
    /// <remarks>
    /// 호출은 완료 I/O 스레드나 JobRunner 스레드, 이 Host의 parse worker 밖에서 해야 한다. 이
    /// 함수는 그 스레드들이 끝날 때까지 기다리므로, 그 안에서 부르면 자기 자신을 기다리게 된다.
    ///
    /// OnSessionClosed가 자원 고갈 fallback으로 외부 호출 스레드에서 동기 실행된 경우에는 그
    /// callback 안에서도 부를 수 있다. 그 호출은 자기 callback만 제외하고 다른 세션과 종료
    /// 통지가 끝날 때까지 기다린다. 다른 스레드의 Stop과 Run은 현재 callback까지 반환한 뒤에야
    /// 끝난다. 단, callback이 Start의 최종 Running 전이보다 먼저 온 경우에는 자기 자신과 Start
    /// 실패 정리가 서로 기다리지 않도록 종료 요청만 기록하고 돌아온다. 그 Start 호출이 종료를
    /// 완료하고 Closed를 돌려준다.
    /// </remarks>
    void Stop();

    /// <summary>종료가 요청될 때까지 이 스레드를 붙잡아 둔다.</summary>
    /// <returns>프로세스 종료 코드로 쓸 값. 정상 종료면 0.</returns>
    int Run();

    /// <summary>지금 포트를 열고 수락 중인지 답한다.</summary>
    [[nodiscard]] bool IsRunning() const noexcept;

    /// <summary>현재 듣고 있는 포트다. 멈췄으면 0이다.</summary>
    [[nodiscard]] std::uint16_t Port() const noexcept;

    /// <summary>고정 운영 지표를 한 번 읽는다.</summary>
    /// <remarks>
    /// SessionRegistry와 같은 JobRunner 문맥에서만 읽는다. 따라서 처리기나 GetJobRunner()으로
    /// 넣은 작업 안에서 부른다. 관리 스레드가 필요하면 별도 지표 실행자를 만들지 말고 기존
    /// JobRunner에 한 번의 요청을 넣어 결과를 가져간다. 벡터 스냅숏 할당이 실패하면
    /// PlatformError를 돌려준다.
    /// </remarks>
    [[nodiscard]] Core::Result<ServerMetricsSnapshot> SnapshotMetrics() const;

private:
    class State;
    std::shared_ptr<State> mState;
};
}
