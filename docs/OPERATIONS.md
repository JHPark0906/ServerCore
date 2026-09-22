# 운영 로그, 메트릭, 요청 추적

이 기능은 기본 `ServerCore::ServerCore` 타깃에 포함됩니다. 외부 로그 서버나 메트릭 수집기 의존성은 없습니다. 파일 출력, 메트릭 외부 공개, 추적 전송 대상은 애플리케이션이 선택합니다.

관측 기능도 내부 계층을 구분합니다. `Metrics.h`·`RequestTrace.h`는 값·콜백 계약이고, `AsyncLogger`는 Core의 로그 인터페이스 구현입니다. HTTP 응답 수명과 추적 큐를 관리하는 내부 구현은 `src/Web/HttpObservationInternal.*`에 있습니다. `Prometheus.h`는 이미 수집한 지표 값을 읽는 상위 출력 어댑터이며 하위 전송·실행기가 의존하는 관측 기반이 아닙니다. 상세 의존 검사 범위는 [아키텍처](ARCHITECTURE.md)를 따릅니다.

## 비동기 로그

`Observability::AsyncLogger`는 `Core::ILogger` 구현입니다. `ServerHost::SetLogger`와 `HttpServer::SetLogger`로 시작 전에 주입합니다. 서버·Dispatcher·Acceptor는 각자의 sink를 보관하므로 한 프로세스의 여러 서버가 서로의 로거를 교체하지 않습니다. 미설정 인스턴스는 기록을 버립니다. 전역 `SetGlobalLogger/GetGlobalLogger`는 deprecated인 애플리케이션 호환 API이며 서버 설정에 영향을 주지 않습니다.

```cpp
#include <ServerCore/Core/Logging.h>
#include <ServerCore/Observability/AsyncLogger.h>
#include <ServerCore/Web/HttpServer.h>
#include <memory>

using namespace ServerCore;
auto logger = std::make_shared<Observability::AsyncLogger>();
Observability::LoggerOptions options;
options.minimumLevel = Core::LogLevel::Info;
options.console = true;                 // stderr
options.file = "logs/server.log";       // 부모 디렉터리는 미리 생성
options.maxFileBytes = 10 * 1024 * 1024;
options.retainedFiles = 3;
auto started = logger->Start(options);
if (!started.IsOk()) return 1;
Web::HttpServer server;
auto installed = server.SetLogger(logger);
if (!installed.IsOk()) return 1;
logger->Write(Core::LogLevel::Info, "server started");
// 서버와 작업 실행기를 먼저 정지한 뒤 마지막 로그를 비운다.
auto stopped = logger->Stop();
if (!stopped.IsOk()) return 1;
```

`Write`는 메시지 복사와 큐 삽입만 수행합니다. 파일·콘솔 출력은 전용 스레드가 담당합니다. 큐가 차면 새 메시지를 버리며, `TryWrite`를 사용하면 `WouldBlock`을 직접 확인할 수 있습니다. 메시지 상한 초과는 `TooLarge`, 정지 후 호출은 `Closed`입니다. 레벨 필터로 제외된 메시지는 성공으로 처리하고 별도 필터 카운터에 포함합니다. `SetMinimumLevel`은 실행 중 안전하게 호출할 수 있습니다.

`maxQueuedMessages`는 대기 메시지 수, `maxRetainedBytes`는 대기·출력 중 메시지 텍스트의 합, `maxMessageBytes`는 단일 메시지 크기 상한입니다. 컨테이너·할당기 부가 비용과 포맷된 출력 한 줄은 별도입니다. 출력 한 줄은 단일 메시지 상한으로 제한됩니다. 로그 레벨과 Unix epoch 밀리초를 앞에 붙이며, CR/LF/NUL 및 제어 바이트를 `\xNN`으로 이스케이프해 메시지 하나가 물리적 한 줄을 차지하게 합니다. 메시지 내용의 민감 정보 제거는 호출자의 책임입니다.

파일은 append로 열고 다음 줄이 상한을 넘기기 전에 회전합니다. `server.log.1`이 가장 최근 백업이며 `.N`까지만 보관합니다. 한 경로는 로거 하나가 단독으로 사용해야 합니다. 외부 파일 교체 및 여러 프로세스의 같은 파일 쓰기는 지원하지 않습니다. 기존에 있던 큰 파일의 크기를 소급해서 줄이지는 않습니다. 파일 설정 시 최악의 이스케이프 크기를 고려해 `maxMessageBytes <= (maxFileBytes - 80) / 4`가 필요합니다.

`RequestStop`은 새 입력을 막고 비우기를 요청합니다. `Stop`은 받아 둔 메시지를 모두 처리하고 스레드를 합류합니다. 출력 실패는 `outputErrors`에 기록하고 `Stop`에서 `PlatformError`로 알립니다. 콘솔이나 파일 시스템 자체가 멈추면 종료 시간은 보장하지 않습니다. 매 줄 flush하지만 디스크 영구 기록을 위한 fsync는 보장하지 않습니다.

`GetMetrics()`의 `acceptedMessages`, `writtenMessages`, `filteredMessages`, `droppedMessages`, `outputErrors`로 기록 손실을 확인합니다. 정상 종료 뒤에는 받아 둔 메시지마다 성공 출력 또는 출력 오류 하나가 대응합니다.

## 고정 메트릭 스냅샷

`Runtime::JobRunner::GetMetrics()`, `TaskExecutor::GetMetrics()`와 `HttpServer::GetMetrics()`는 스레드 안전한 값 스냅샷을 반환합니다. 수명 전체의 카운터는 초기화하지 않으며 `UINT64_MAX`에서 포화합니다. 외부에서 일정 주기로 읽어 차이를 계산할 수 있습니다. HTTP 스냅샷의 서로 다른 필드는 하나의 원자적 시점을 나타내지 않습니다. Host의 `SnapshotMetrics()`는 기존처럼 JobRunner 문맥에서 호출하며 `jobs`에 실행기 지표를 포함합니다.

| 대상 | 주요 게이지 | 주요 카운터 |
| --- | --- | --- |
| 작업 실행기 | 대기·실행 작업 수, 선언된 보관 바이트 | 수락·완료·실패·취소·시간 초과·거절 |
| 직렬 실행기 | 대기·예약·실행 작업 수와 선언 바이트 | 실제 투입·완료 작업 수, 큐 투입부터 캡처 해제까지 지연 |
| HTTP 서버 | 연결·활성 요청 수, 보관 요청·송신 바이트, 대기 추적 이벤트 | 수락·종료 연결, 수락·완료·실패·취소·시간 초과·거절 요청, 파서 오류, 추적 손실·콜백 오류 |

`completedTasks`와 `completedRequests`에는 모든 종료 결과가 포함됩니다. 실패·취소·시간 초과는 서로 겹치지 않는 부분 집합입니다. HTTP 4xx/5xx 응답도 정상적으로 로컬 송신 큐에 들어갔다면 성공한 응답 전송입니다. `failedRequests`를 HTTP 500 개수로 해석하면 안 됩니다. 요청 핸들러 풀의 통계는 HTTP 스냅샷의 `handlers`에 들어 있습니다.

HTTP 요청 카운터는 등록된 HTTP 라우트에 수락된 요청을 대상으로 합니다. 경로 미발견, 메서드 불일치, WebSocket 업그레이드는 이 요청 카운터에 포함하지 않습니다. WebSocket 연결은 연결 및 송신 바이트 게이지에 포함합니다. `protocolErrors`는 HTTP 파서의 거절 횟수입니다. 요청 예산 또는 핸들러 풀의 수락 실패는 `rejectedRequests`에 포함하며, 예산 수락 후 핸들러 큐에서 거절된 경우에는 수락·거절 모두 증가할 수 있습니다.

지연 시간은 작업/라우트 수락에서 종료까지의 `totalLatencyNanoseconds`, `maxLatencyNanoseconds`입니다. HTTP는 로컬 응답 완료까지이며 상대의 수신 시간은 아닙니다. 작업은 큐 대기와 취소 정리를 포함합니다. 평균은 총 지연을 완료 횟수로 나눠 계산합니다. 응답 종료 후에도 애플리케이션이 요청 컨텍스트를 보관하면 `retainedRequestBytes`는 남습니다. 이는 누수가 아니라 실제 보관 수명입니다.

`latencyHistogram`은 12개 고정 비누적 구간입니다. 상한은 0.1·0.25·0.5·1·2.5·5·10·25·50·100·1,000 ms와 무한대이며 유한 상한 값은 해당 구간에 포함됩니다. 요청 수에 비례하는 추가 저장소가 없습니다.

`Observability/Prometheus.h`의 `RenderPrometheus`는 HTTP·TaskExecutor·JobRunner·ServerHost 스냅샷을 [Prometheus 텍스트 형식](https://prometheus.io/docs/instrumenting/exposition_formats/)으로 변환합니다. 지연은 초 단위 누적 histogram bucket과 count/sum으로 내보내며, 이름과 라벨은 고정입니다. 게임 송신량은 세션별 시계열 대신 합계로 내보냅니다. 수집기 주소, 인스턴스 라벨, 접근 인증과 `text/plain; version=0.0.4` 엔드포인트는 소비자가 설정합니다. 자동 관리 서버나 전역 지표 레지스트리는 만들지 않습니다.

## 요청 종료 추적

```cpp
#include <ServerCore/Web/HttpServer.h>
#include <ServerCore/Observability/AsyncLogger.h>

using namespace ServerCore;
Web::HttpServer server;
auto traced = server.SetRequestTraceHandler(
    [logger](Observability::RequestTrace event) {
        logger->Write(Core::LogLevel::Info,
            "request=" + std::to_string(event.requestId) +
            " status=" + std::to_string(event.status));
    }, 256);
if (!traced.IsOk()) return 1;
// 라우트 등록 후 server.Start(options).
```

`SetRequestTraceHandler`는 시작 전에 설정합니다. 이벤트는 소유하는 값이며 서버 수명 내 요청 ID·연결 ID, 최대 64바이트의 메서드 토큰, HTTP 상태, 종료 결과, 지연 시간만 담습니다. 경로·쿼리·헤더·Authorization·본문이나 외부 요청의 상관 ID를 자동 수집하지 않습니다. 64바이트보다 긴 메서드는 잘리므로 메서드 문자열을 고유 키로 사용하지 않습니다. 아직 응답 헤더를 전송하지 않았다면 `status`는 0입니다.

수락된 HTTP 요청의 종료는 한 번 집계합니다. 추적 이벤트는 최대 `maxPendingEvents`개의 대기 슬롯과 실행 중인 이벤트 하나를 사용합니다. 큐 포화 또는 할당 실패 시 새 이벤트를 버리고 `droppedTraceEvents`를 늘립니다. 콜백은 별도 스레드에서 직렬 실행하므로 네트워크나 응답 잠금을 쥐고 호출하지 않습니다. 콜백 예외는 밖으로 전달하지 않고 `traceCallbackErrors`에 포함합니다. 콜백은 신속히 반환해야 하며 별도 무제한 큐로 넘겨 이 상한을 무력화하지 않아야 합니다.

콜백에서 `GetMetrics()`와 비동기 로거를 호출할 수 있습니다. 같은 서버의 `Stop()`은 자기 스레드를 기다릴 수 없으므로 `InvalidArgument`입니다. 제어 스레드의 `Stop()`은 전송·핸들러 종료 후 받아 둔 추적 이벤트까지 비우고 합류합니다. 서버는 모든 콜백 바깥에서 파괴해야 합니다.
