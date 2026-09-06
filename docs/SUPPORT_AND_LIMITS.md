# 지원 범위와 설정

현재 공개 API와 구현을 기준으로 설정 가능한 값과 지원 범위를 정리합니다. 소개는 [README](../README.md), 스레드와 종료 순서는 [아키텍처](ARCHITECTURE.md), 회귀 검사 방법은 [빌드·테스트·배포](BUILD_TEST_DEPLOY.md)를 참고합니다.

## 1. 플랫폼과 기능 범위

| 영역 | 제공하는 기능 | 제공하지 않는 기능 |
| --- | --- | --- |
| 플랫폼 | Windows, WinSock2, IOCP, MSVC 기반 빌드 | Linux/macOS 백엔드, 다른 플랫폼의 동등 동작 보장 |
| 네트워크 | IPv4 TCP 수락, 부분 수신·송신, 연결별 송신 큐, 비차단 UDP와 토큰·endpoint·순번 관리 | IPv6, TLS/DTLS, HTTP/WebSocket, DNS 해석·TCP outbound connect API |
| 프로토콜 | 길이 프레임, UTF-8 JSON, 준비된 봉투 재사용, 타입별 디스패치, 헤더 전용 DatagramCodec | 게임 스키마 자동 생성, 압축, 암호화, 큰 메시지 분할·스트리밍 |
| 세션 | ID 발급, 연결 상태, 인증 완료 상태 표시, 목록, UDP 토큰 등록·폐기 | 계정 인증과 자격 증명 검증, 자동 재접속, 세션 이관·복구 |
| 실행 | 직렬 JobRunner, 주기 작업 예약, 선택적 JSON 파싱 병렬화 | 게임 처리기의 병렬 실행 보장, 고정 시간 내 완료 보장 |
| 게임 | 게임이 처리기와 관찰자를 등록하는 확장 지점 | 방, AOI, 매치메이킹, DB, 월드 저장, 서버 권위 물리 |
| 관측 | 주입형 ILogger, 고정 지표 스냅숏 | 기본 파일 로거, 로그 회전, 시계열 저장, HTTP 관리 서버 |

구체 게임의 채팅·프로필 검증·접속 제한 정책은 소비 프로젝트의 기능입니다. ServerCore의 `MarkAuthenticated()`는 검증이 끝났음을 표시하는 상태 전이이며, 그 호출만으로 사용자가 인증되었다는 근거를 만들지 않습니다.

### UDP 전송의 별도 한도

`Runtime::DatagramTransport`는 Host의 TCP 설정과 독립적입니다. 숫자 IPv4 주소를 사용하며 포트 0의 임시 포트 할당을 지원합니다. datagram 전체는 최대 1,200바이트이고 JSON payload 상한은 1,172바이트입니다. 소켓 버퍼 요청값은 수신 4 MiB·송신 1 MiB이며 OS가 실제 저장소를 관리합니다.

`DatagramPollBudget`의 기본값은 호출당 4,096회 수신 시도와 1 MiB입니다. 오류와 거절 패킷도 시도 횟수를 소비하며, 바이트 한도는 수신 전에 검사하므로 마지막 패킷만큼 초과할 수 있습니다. 한도 중 하나가 0이면 수신하지 않습니다. 소비자가 Poll을 직렬 호출하며 실행 주기는 직접 정합니다. 등록 수의 별도 상한은 없으므로 소비자의 TCP 입장 상한과 등록 해제 수명을 연결해야 합니다. 자체 재전송 큐·암호화·혼잡 제어는 제공하지 않습니다.

## 2. Host 기본값과 유효 범위

기준 코드는 [ServerHostOptions](../include/ServerCore/Runtime/ServerHost.h)와 [ValidateOptions](../src/Runtime/ServerHost.cpp)입니다. KiB/MiB/GiB는 각각 1,024의 거듭제곱 단위입니다. 아래는 **ServerCore의 기본값**이며 소비 서버가 덮어쓴 값과 구분합니다.

| 옵션 | 기본값 | 현재 범위 또는 의미 |
| --- | --- | --- |
| `listenAddress` | `127.0.0.1` | 숫자 IPv4 문자열. 주소 형식은 Start의 수락기 구성에서 검사 |
| `port` | `0` | 실제 설정에는 1~65,535 필요. 0의 자동 포트 할당은 Host에서 미지원 |
| `ioWorkerThreadCount` | `1` | 1~64 |
| `parseWorkerThreadCount` | `0` | 0~64. 0은 JobRunner에서 동기 JSON 파싱 |
| `acceptBacklog` | `16` | 양의 int. OS의 아직 수락되지 않은 연결 대기열 요청값 |
| `idleSessionTimeout` | `0ms` | 0은 유휴 검사 해제, 음수 거절 |
| `gracefulCloseTimeout` | `5,000ms` | 양수 필수. 0으로 끌 수 없음 |
| `maxBodySize` | `65,536`바이트 | 양수. 현재 유효 최대는 1 MiB − 4바이트이며 아래 결합 조건도 적용 |
| `maxConcurrentSessions` | `256` | 1~65,536, 프레임 리더 합계 조건도 적용 |
| `maxPendingReceiveBytes` | `256 KiB` | 연결별 처리 중·대기 중 수신 바이트. 1바이트~16 MiB |
| `maxTotalPendingReceiveBytes` | `8 MiB` | Host 전체 수신 대기 예산. 1바이트~64 MiB |
| `maxPendingParseBytes` | `256 KiB` | 연결별 완결 본문 예약. 파싱 worker 사용 시 양수~16 MiB |
| `maxTotalPendingParseBytes` | `8 MiB` | Host 전체 완결 본문 예약. 파싱 worker 사용 시 양수~64 MiB |
| `maxPendingParseTasks` | `64` | 연결별 완결 프레임 예약 수. 파싱 worker 사용 시 1~4,096 |
| `maxTotalPendingParseTasks` | `4,096` | Host 전체 완결 프레임 예약 수. 파싱 worker 사용 시 1~65,536 |
| `maxTotalSendQueueCapacityBytes` | `256 MiB` | Host 전체 Connection 송신 payload 예산. 1~512 MiB |

타임아웃의 명시적 옵션 타입은 `std::chrono::milliseconds`입니다. 파일 기반 어댑터는 밀리초를 `Config::GetInt`로 읽으므로 int 범위도 적용합니다. 큰 숫자의 별도 파일 표기나 단위 접미사를 해석하지 않습니다.

### 결합 조건

1. **단일 송신 큐:** [Connection의 고정 송신 상한](../include/ServerCore/Net/Connection.h)은 1 MiB입니다. `maxBodySize + 4`가 이를 넘으면 Configure가 거절합니다. 코드의 별도 Host 본문 안전 경계 16 MiB보다 이 조건이 먼저 실질적인 한도를 결정합니다.
2. **프레임 리더 합계:** `(maxBodySize + 4) × maxConcurrentSessions ≤ 1 GiB`여야 합니다. 예를 들어 최대 65,536 연결에는 본문 상한이 16,380바이트 이하여야 합니다. 기본 64 KiB를 유지한 채 연결 수만 최대치로 올릴 수 없습니다.
3. **파싱 예산:** `parseWorkerThreadCount > 0`일 때 바이트 예산은 각각 최소 한 개의 최대 본문을 담아야 합니다. 전체 바이트·태스크 한도는 각각 연결별 한도 이상이어야 합니다. worker가 0이면 파싱 전용 값은 현재 ValidateOptions에서 검사하지 않고 사용하지 않습니다.
4. **수신 예산:** 수신 예산에는 파싱 예산과 같은 전체≥개별 검사가 없습니다. 전체 예산이 작으면 여러 연결이 이를 먼저 소진할 수 있습니다. Configure 성공을 충분한 처리량의 증거로 삼지 않습니다.

`acceptBacklog`, 현재 outstanding AcceptEx 요청 수, `maxConcurrentSessions`는 다른 값입니다. 구현은 수락 요청 4개를 미리 걸지만 이것이 동시 접속자를 4명으로 제한하지는 않습니다. TCP 연결 한도와 게임 가입자 한도를 같은 값으로 사용할지도 게임 서버가 정합니다.

### 큰 연결 상한 구성 예

```cpp
ServerCore::Runtime::ServerHostOptions options;
options.listenAddress = "127.0.0.1";
options.port = 17891;
options.maxConcurrentSessions = 65536;
options.maxBodySize = 8192;
```

이 옵션을 Start 전에 `host.Configure(options)`에 넘기고 Status를 검사합니다. 값 객체를 만드는 것만으로 소켓을 열거나 최대 수만큼 연결을 생성하지는 않습니다.

## 3. 설정 파일

[Core::Config](../include/ServerCore/Core/Config.h)는 UTF-8 `key = value` 파일을 읽어 불변 스냅숏을 만듭니다. Host가 파일을 자동 탐색하거나 환경 변수·명령행 인수를 읽는 구조는 아닙니다. 소비 프로그램이 파일 경로를 정하고 적재합니다.

```ini
# 검증용 로컬 서버 설정
servercore.host.port = 17891
servercore.host.listen-address = 127.0.0.1
servercore.host.io-worker-thread-count = 1
servercore.host.parse-worker-thread-count = 0
servercore.host.accept-backlog = 16
servercore.host.max-concurrent-sessions = 65536
servercore.host.max-body-size = 8192
servercore.host.idle-session-timeout-ms = 30000
servercore.host.graceful-close-timeout-ms = 5000
```

다음은 `ServerCore::Runtime::ServerHost host;`를 만든 뒤 사용하는 적재 부분입니다. 상대 파일 경로는 이 프로그램의 현재 작업 디렉터리를 기준으로 해석합니다.

```cpp
const auto loaded = ServerCore::Core::Config::LoadFromFile("server.conf");
if (!loaded.IsOk())
{
    std::cerr << loaded.GetStatus().Message() << '\n';
    return 1;
}
const auto configured = host.Configure(loaded.Value());
if (!configured.IsOk())
{
    std::cerr << configured.Message() << '\n';
    return 1;
}
```

필수 Host 키는 `servercore.host.port` 하나입니다. 생략한 선택 키에는 `ServerHostOptions`의 기본값을 사용합니다. 예약 키 목록은 공개 헤더에도 있으며 다음처럼 대응합니다.

| `servercore.host.` 뒤에 붙는 키 | Options 필드 |
| --- | --- |
| `listen-address`, `port`, `accept-backlog` | `listenAddress`, `port`, `acceptBacklog` |
| `io-worker-thread-count`, `parse-worker-thread-count` | `ioWorkerThreadCount`, `parseWorkerThreadCount` |
| `idle-session-timeout-ms`, `graceful-close-timeout-ms` | `idleSessionTimeout`, `gracefulCloseTimeout` |
| `max-body-size`, `max-concurrent-sessions` | `maxBodySize`, `maxConcurrentSessions` |
| `max-total-send-queue-capacity-bytes` | `maxTotalSendQueueCapacityBytes` |
| `max-pending-receive-bytes`, `max-total-pending-receive-bytes` | `maxPendingReceiveBytes`, `maxTotalPendingReceiveBytes` |
| `max-pending-parse-bytes`, `max-total-pending-parse-bytes` | `maxPendingParseBytes`, `maxTotalPendingParseBytes` |
| `max-pending-parse-tasks`, `max-total-pending-parse-tasks` | `maxPendingParseTasks`, `maxTotalPendingParseTasks` |

파일 형식에는 다음 규칙이 있습니다.

- UTF-8 BOM, LF/CRLF, 빈 줄을 허용합니다. 키와 값 양끝의 ASCII 공백·탭은 제거합니다.
- 공백·탭 뒤 첫 글자가 `#`인 줄만 주석입니다. `port = 17891 # comment`처럼 뒤에 붙이는 주석은 지원하지 않습니다.
- 첫 `=`에서 나눕니다. 값 안의 나머지 `=`는 그대로 남고, 빈 값도 허용합니다.
- 키는 대소문자를 구별하는 ASCII `[A-Za-z0-9._-]+`입니다. 중복 키, 잘못된 UTF-8과 NUL 바이트는 거절합니다.
- 따옴표, escape, 환경 변수 치환, include, 자동 재적재는 없습니다. 주소를 따옴표로 감싸면 따옴표 자체가 값에 포함됩니다.
- 정수는 int 범위의 10진수입니다. `+1`, `1.0`, `8KiB` 같은 값은 정수가 아닙니다.
- Host는 예약 키만 읽습니다. 다른 키를 자동으로 오타로 판정하지 않으므로 소비 프로젝트가 자신의 키 검증을 담당합니다.

## 4. 예산이 제한하는 메모리

프레임 리더는 각 세션의 첫 수신 때 `maxBodySize + 4` 저장소를 확보합니다. 1 GiB 조건은 그 저장소들의 최악 합계에 대한 조건이며 시작 시 전부를 일괄 할당한다는 뜻은 아닙니다. 연결별 16 KiB 수신 I/O 버퍼, OS 소켓, 세션 객체·등록표와 임시 JSON 저장소는 별도입니다.

수신 예약은 I/O 콜백에서 직렬 실행 문맥으로 넘길 바이트 사본을 제한합니다. 파싱 예약은 완결 본문이 worker와 순서 대기열에 남아 있는 기간을 제한하며, 작은 프레임이 태스크 수만 늘리는 경우도 막기 위해 바이트와 개수를 함께 셉니다. 송신 예산은 비동기 전송이 참조 중인 payload를 제한하며, 부분 전송이 끝난 앞부분도 해당 저장소가 해제될 때까지 포함합니다.

이 예산의 합은 프로세스 전체 메모리 상한이 아닙니다. JSON DOM, 게임 객체, 콜백 캡처, 로그 출력, 커널 메모리 등은 같은 바이트 카운터로 제한하지 않습니다. 사용자가 `JobRunner::Lease::Post`로 넣는 임의 작업에도 일반적인 태스크 개수 상한이 따로 제공되지 않습니다.

수신·파싱 예산을 넘는 연결은 종료됩니다. 송신 예산 부족은 그 Send를 넣지 않고 `WouldBlock`으로 반환합니다. 재시도·최신 상태로 교체·연결 종료 중 어떤 정책을 택할지는 메시지 의미에 따라 게임이 결정해야 합니다.

## 5. 시간과 종료의 의미

유휴 시간은 **마지막 비어 있지 않은 TCP 바이트 수신** 이후로 셉니다. 완성된 메시지나 인증 성공을 기준으로 하지 않으므로, 조금씩 보내는 부분 프레임도 활동입니다. 따라서 이 타임아웃을 가입 완료 기한이나 프레임 완결 기한으로 대체해서 사용하지 않습니다.

`SendAndDisconnect`의 graceful 기한은 Closing 시작부터 셉니다. 상대가 계속 바이트를 보내도 기한을 연장하지 않으며, 기한 후 남은 송신을 버리고 연결을 정리합니다. 명시적인 `Disconnect`나 Host 종료는 그보다 먼저 drain을 중단할 수 있습니다. 어떤 경우도 마지막 메시지의 상대 애플리케이션 수신을 보증하지 않습니다.

내부 검사는 활성 제한 중 짧은 시간의 1/4을 10~1,000ms로 제한한 주기로 예약합니다. 검사 작업 자체도 JobRunner에서 기다리므로 설정 시간에 정확히 종료된다는 실시간 보장은 없습니다. 긴 처리기는 다른 요청, 주기 작업과 지표 관측까지 지연시킵니다.

## 6. 실패·관측·배포의 경계

`Status::Code()`로 실패를 분류합니다. `Message()`는 사람이 읽을 진단이고 문구 고정을 약속하지 않습니다. `Result`는 `IsOk()` 확인 후에만 `Value()`를 읽습니다. 처리기·작업·관찰자 등 예외 금지 경계에서 던지는 예외는 잘못된 네트워크 입력과 다른 계약 위반이며 프로세스 종료로 이어질 수 있습니다.

지표 스냅숏에는 세션 수와 대기량, 프레임 누계 등이 있지만 전체 값이 한 원자적 시점의 상태는 아닙니다. `errorCount`는 Closed·Timeout·WouldBlock과 게임이 직접 정한 종료를 제외하므로 0이 성공률 100%를 뜻하지 않습니다. 스냅숏은 JobRunner에서 읽고 외부 저장·시각화는 소비자가 구현합니다.

기본 로거는 메시지를 버립니다. 주입하는 `ILogger::Write`는 여러 스레드에서 호출될 수 있고 예외를 던지지 않아야 합니다. 파일 저장·회전·보존 정책, 플레이어 입장·채팅 같은 게임 로그는 소비 서버의 책임입니다. 전역 로거를 공유하므로 서로 독립적인 여러 Host를 같은 프로세스에 띄우는 구성은 지원 계약으로 제공하지 않습니다.

MSVC 빌드는 `/MD` 또는 `/MDd`를 사용하며 소비 타깃과 CRT를 맞춰야 합니다. 정적 `ServerCore.lib`를 링크한다고 CRT까지 정적으로 묶이지는 않습니다. Debug/Release 설치 패키지 분리와 실행 파일 배포 조건은 [빌드 문서](BUILD_TEST_DEPLOY.md)를 따릅니다. 이 저장소의 현재 문서와 버전 표시는 장기 ABI 호환성, 서비스 가용성 또는 특정 동접 수를 보증하지 않습니다.
