# 실행·취소·흐름 제어

웹 서버와 게임 서버가 공유하는 기반 계약입니다. 공개 구현은 `Runtime/TaskExecutor.h`, `Net/ConnectionFlowControl.h`와 `Net/Acceptor.h`입니다. [HTTP 비동기 응답·스트리밍](WEB.md)은 이 실행기와 흐름 제어 위에 구성합니다.

## 모듈 경계

| 계층 | 의존 방향과 책임 |
| --- | --- |
| Core | 상태·오류·버퍼·시계·설정·로그 인터페이스. 상위 프로토콜과 네트워크 I/O를 모릅니다. |
| 관측 계약·로그 구현 | Metrics·RequestTrace 값과 AsyncLogger를 구분합니다. 서버·실행기는 값과 ILogger 인터페이스를 사용하며 출력 어댑터에 의존하지 않습니다. |
| Net | Core 위에서 연결·바이트 전송·보유 메모리 예산을 관리합니다. HTTP 요청, 게임 메시지, 세션 ID를 모릅니다. |
| Runtime의 실행기 | Core 위에서 작업 수명과 실행 문맥을 관리합니다. TaskExecutor는 Net·Web·Session에 의존하지 않습니다. |
| Web | Net 위에 HTTP·WebSocket·라우팅을 조립합니다. 게임 Session·Dispatch를 요구하지 않습니다. |
| Protocol·Session·Dispatch | 게임 메시지 표현과 직렬 처리 계약을 제공합니다. 웹 처리는 이 계층을 거칠 필요가 없습니다. |
| Runtime의 ServerHost | 게임 전송·프레이밍·실행기를 조립하는 상위 host입니다. 범용 실행기와 책임을 구분합니다. |
| 관측 출력·C ABI | Prometheus는 완성된 snapshot을 변환하고 C ABI는 서버 객체를 외부 언어 handle로 감쌉니다. 하위 실행·전송 계층에서 역의존하지 않습니다. |
| 애플리케이션 | 인증, 방·월드·AOI, 플러그인 등록·복구, 작업 상태 조회, DB·배포 정책을 소유합니다. |

기본 패키지는 `ServerCore::ServerCore` 정적 타깃입니다. 외부 HTTP 요청은 소비 애플리케이션이 처리하며 코어에는 서드파티 라이브러리 의존성이 없습니다. Windows IOCP와 Linux epoll은 커널 요청 수명을 분리하고 송신 저장소·예산·용량 알림 정책을 공유합니다.

`Runtime`·`Observability` 디렉터리 내부의 세부 의존 방향도 [아키텍처의 계층 구분](ARCHITECTURE.md)에 따릅니다. `ServerCore.Architecture.LayerBoundaries`는 저장소의 직접 include가 이 경계를 넘지 않는지 검사합니다.

## 소유권·오류

송신 성공은 로컬 큐 수락이며 원격 처리 완료가 아닙니다. `Connection::Send`는 입력을 복사하고, 수신 span은 콜백 동안만 유효합니다. 요청·작업 데이터를 나중에 사용하려면 소유 값을 보관합니다.

| 상태 | 의미 |
| --- | --- |
| `WouldBlock` | 현재 예산 부족 또는 미완료 상태. 송신 알림은 이후 재시도 기회를 제공합니다. |
| `TooLarge` | 작업/용량 요청이 설정된 최대 예산에도 들어가지 않습니다. 기존 raw `Connection::Send`의 초과 payload는 원래 계약대로 `WouldBlock`입니다. |
| `Closed` | 더 이상 새 작업·송신을 받지 않습니다. |
| `Cancelled` | 수동·부모 토큰·실행기 종료에 의한 취소입니다. 기존 오류 값은 유지하고 새 값 12를 추가했습니다. |
| `Timeout` | 작업 마감 시간 또는 대기 기한이 지났습니다. `WaitUntil`의 만료만으로 작업을 취소하지 않습니다. |
| `InvalidArgument` | 잘못된 인자 또는 자기 실행 문맥을 기다리는 호출입니다. |
| `PlatformError` | OS·할당 실패 또는 작업 함수의 예외입니다. |

취소 사유는 먼저 확정한 값을 유지합니다. 생성자와 호출 인자를 만드는 C++ 할당은 예외를 던질 수 있습니다. 작업 함수와 송신 용량 콜백의 예외는 실행기·전송 경계를 넘어가지 않습니다. 기존 수신 observer의 예외 계약은 바꾸지 않습니다.

## 실행기 선택과 취소

`JobRunner`는 외부 스레드 하나에서 게임 상태를 직렬 처리합니다. 기본 일반 예산은 예약·대기·실행 중 합계 4,096개와 선언된 보유 바이트 16 MiB입니다. `Configure(JobRunnerOptions)`는 작업 투입·실행 전에 호출하며 동일 설정의 재적용은 무해합니다. `Post(job, retainedBytes)`는 포화 시 `WouldBlock`, 단일 작업 초과 시 `TooLarge`를 반환합니다. 캡처가 파괴될 때까지 예산을 유지합니다. 0바이트 선언도 작업 수 제한을 받으며 실제 임의 할당량을 자동 측정하지는 않습니다.

내부 수신·완료·종료 처리는 별도의 제한된 `PostControl` 예산(기본 1,024개·4 MiB)을 사용합니다. 두 종류는 실제 투입 순서대로 같은 스레드에서 실행합니다. Host의 수신·파싱 예산도 별도로 유지합니다.

`CloseAdmission()`은 일반 투입만 닫습니다. 기존 예약과 `AcquireControlLease()`로 만든 유지 관리 타이머는 계속 진행합니다. `RequestStop()`은 모든 투입을 닫고 이미 큐에 들어온 작업을 끝까지 실행합니다. 아직 투입하지 않은 예약을 기다리지 않으며, 뒤늦은 예약 `Post`는 `Closed`를 반환합니다. 실행기 스레드는 호출자가 join합니다.

`TaskExecutor`는 고정 worker와 제한된 대기열을 소유합니다. `Start`는 한 번만 가능하고 `Submit`은 `Result<TaskHandle>`을 반환하며 성공한 handle은 복사할 수 있습니다. 기본값은 worker 2개, 대기 작업 128개, 대기·실행 중 보유 바이트 합계 4 MiB입니다. 별도 coordinator 1개가 취소·마감 시간을 처리합니다. `TaskOptions::retainedBytes`는 호출자가 선언한 값이며 실행 중 임의 할당을 측정하지 않습니다. 제출 거절은 대기열·예산을 소비하지 않습니다.

부모 `std::stop_token`, `TaskHandle::RequestCancel()`, 실행기의 `RequestStop()`과 `steady_clock` 마감 시간을 연결합니다. 최초 취소 사유가 결과를 결정합니다. 취소된 대기 작업은 worker가 모두 바빠도 coordinator가 꺼내 캡처와 예산을 반환합니다. 실행 중 작업은 전달된 토큰을 확인하고 반환해야 하며, 그 전에는 예산을 회수하거나 완료로 표시하지 않습니다. 작업 함수와 캡처 정리 후 결과를 한 번 게시합니다.

`RequestStop()`과 부모 토큰 콜백은 coordinator를 깨웁니다. `RequestCancel()`은 표준 stop callback을 호출 스레드에서 동기 실행할 수 있습니다. stop callback과 캡처 소멸자는 빨리 끝나야 하고 실행기의 작업 완료를 기다려서는 안 됩니다. 중첩 취소 문맥도 추적하여 관련 실행기의 Stop·미완료 handle 대기를 `InvalidArgument`로 거절합니다.

`Stop()`과 소멸자는 worker·coordinator를 join합니다. 강제 스레드 종료를 제공하지 않으므로 비협력적 작업은 종료를 무기한 지연시킬 수 있습니다. 실행기 파괴는 제어 스레드에서 수행하되, 그 스레드에서 동기 실행 중인 stop callback 안에서도 파괴해서는 안 됩니다. handle은 실행기 파괴 후에도 완료 상태를 조회할 수 있지만 실행기를 계속 살려 두지는 않습니다. `WaitUntil` 만료는 대기만 끝내며 취소 요청과 별개입니다.

풀에서 동기 네트워크 호출을 수행하면 해당 worker가 기다립니다. HTTP 비동기 처리기는 소유 요청 문맥을 넘긴 뒤 반환하고, 소비 애플리케이션이 시작한 비동기 외부 작업의 완료 콜백에서 응답할 수 있습니다. 응답 작성기의 취소 토큰을 외부 작업의 취소 체계에 연결합니다.

`SubmitWithCompletion`은 수락한 작업마다 한 번, 작업 캡처 정리 후 worker/coordinator에서 완료 콜백을 실행합니다. 대기 중 취소도 포함하며 거절된 제출은 호출하지 않습니다. 완료 콜백은 빠르게 반환해야 하고 자체 캡처도 `retainedBytes`에 포함합니다. 콜백 예외는 격리하며 작업 결과를 바꾸지 않습니다. `TaskHandle::Wait`는 완료 콜백과 그 캡처 정리까지 기다립니다.

## 세션 작업과 결과 복귀

`Runtime::SubmitSessionTask(executor, host.GetJobRunner(), session, work, apply, options)`는 작업을 시작하기 전에 `completionRetainedBytes`와 완료 슬롯을 예약합니다. 작업 수락 실패 시 슬롯도 해제하며, 일반 큐가 포화되어도 이미 예약한 결과는 전달할 수 있습니다. `apply(Session&, const Status&)`는 세션의 직렬 실행 문맥에서 실행됩니다. 작업과 결과가 공유할 소유 컨테이너는 호출자가 준비하고 최대 크기를 실행기와 완료 양쪽의 보유 바이트에 선언합니다.

세션 종료 토큰과 호출자의 부모 토큰을 함께 연결합니다. 세션이 닫히면 대기·실행 중 작업을 취소하고, 늦게 도착한 결과는 세션을 변경하지 않습니다. 작업 함수는 게임 상태를 직접 만지지 않으며 `apply`는 예외를 내보내지 않습니다. 사용자 정의 Session이 즉시 취소를 원하면 `GetCancellationToken()`을 구현해야 합니다. 반환 handle의 완료는 배경 작업 종료와 결과 투입까지이며, `apply` 실행 완료를 뜻하지 않습니다.

일반 비동기 연동도 `Lease::Reserve(bytes)`를 먼저 호출한 뒤 이동 전용 `Reservation::Post`를 사용할 수 있습니다. 예약에는 큐 노드까지 포함되므로 결과 투입 시 큐 노드 할당이 필요하지 않습니다. 같은 예약 handle의 조작은 호출자가 직렬화합니다.

## 서버 단위 종료

`HttpServer`와 `ServerHost`는 `BeginDrain()` → `DrainStatus()` 또는 `StopGracefully(steady_clock::time_point)`를 제공합니다. 신규 요청·접속을 닫고 수락한 작업과 송신을 처리한 후 종료합니다. `DrainStatus`의 `WouldBlock`은 아직 진행 중이라는 뜻입니다. 마감 시간에는 남은 연결을 취소하고 `Timeout`을 반환합니다. 기존 `Stop`은 즉시 취소 경로로 유지합니다.

기한은 네트워크/작업의 대기 종료 정책입니다. 임의의 C++ 처리기·stop callback을 강제 중단할 수 없으므로 비협력적인 사용자 코드가 있으면 스레드 join은 기한 이후에도 지연될 수 있습니다. 종료는 소유 worker와 콜백 바깥의 제어 스레드에서 수행합니다. 프로토콜별 상세 규칙은 [웹](WEB.md)과 [게임 프로토콜](PROTOCOL.md)을 따릅니다.

## TCP 수신 제어

`GetConnectionFlowControl(connection)`으로 기능을 얻습니다. ServerCore의 TCP 연결은 모두 제공하고, 지원하지 않는 사용자 정의 연결은 빈 포인터를 반환합니다. 별도 capability 인터페이스이므로 모든 연결 구현에 가짜 구현을 요구하지 않습니다.

`PauseReceive()`는 새 수신을 중지합니다. 이미 제출했거나 처리 중인 수신 한 개, 최대 16 KiB는 이후 observer에 전달될 수 있습니다. 두 플랫폼 모두 같은 여유를 허용하므로 상위 계층이 예산에 반영해야 합니다. 수락 처리기에서 Start 전에 정지하면 최초 수신도 막습니다. 같은 지점의 Resume은 수락 처리기가 돌아오기 전에 수신을 시작하지 않습니다.

`ResumeReceive()`는 중복 I/O를 만들지 않습니다. 수신 콜백 안에서 호출해도 버퍼를 덮어쓰거나 같은 연결의 다음 콜백을 겹쳐 실행하지 않습니다. 정지 중에도 송신할 수 있습니다. 닫힌 연결의 Pause·Resume은 `Closed`이며 정지 상태 조회는 마지막 플래그를 보여 줍니다.

정지 중에는 커널에 읽지 않은 데이터·EOF가 남아 종료 발견이 늦어질 수 있습니다. 상위의 유휴·작업 마감 시간과 `Close()`를 연결합니다. TCP half-close를 별도로 노출하지 않으며 EOF는 기존처럼 전체 연결 종료로 처리합니다. Host의 수신 초과 정책을 자동 watermark로 바꾸지는 않습니다.

## TCP 송신 예산과 알림

`Acceptor::SetSendQueueLimits`는 시작 전에 연결별·전체 송신 payload 예산을 설정합니다. 기본값은 1 MiB·256 MiB이며 각각 1바이트~1 MiB, 1바이트~512 MiB 범위입니다. Stop 후 재설정하면 새 연결은 새 예산을 공유하고 기존 연결은 기존 예산을 유지합니다. ServerHost·HttpServer도 각자의 전체 예산 옵션으로 같은 기반을 사용합니다. 여러 host 사이의 프로세스 전체 예산은 아닙니다.

`QueuedSendBytes()`는 미송신 바이트, `RetainedSendBytes()`는 실제 보유 payload 바이트입니다. 부분 송신 벡터의 이미 전송한 앞부분도 저장소 해제 전까지 보유 예산에 포함합니다. 객체·allocator·커널 소켓 버퍼는 포함하지 않습니다.

`WaitForSendCapacity(requiredBytes, callback, token)`은 연결별 미완료 등록 하나를 허용합니다. 0·빈 콜백은 `InvalidArgument`, 절대 들어갈 수 없는 크기는 `TooLarge`, 기존 대기는 `AlreadyExists`, 새 송신을 받지 않는 연결은 `Closed`로 거절하며 콜백을 호출하지 않습니다. Reset·소멸로 해제하지 않은 수락 등록은 `Ok`·`Cancelled`·`Closed` 중 하나로 한 번 완료합니다. `CloseAfterSend()`도 송신 접수를 닫으므로 기존 대기를 `Closed`로 완료합니다.

알림은 예약이 아닙니다. `Ok` 후 다른 송신자가 용량을 차지하면 재시도도 `WouldBlock`일 수 있습니다. 콜백은 등록 호출 안, I/O 또는 취소 스레드에서 실행되고 수신 observer와 겹칠 수 있습니다. 전송 잠금 밖이므로 Send·Close·후속 등록은 가능하지만 길게 기다리거나 I/O 소유자의 Stop으로 join하면 안 됩니다. 즉시 완료한 등록을 콜백에서 무조건 재등록하면 재귀할 수 있으므로 실제 전송 진행 여부에 따라 재등록합니다.

반환된 `SendCapacitySubscription`을 보관합니다. Reset·소멸은 미완료 콜백을 억제하고 다른 스레드의 실행 중 콜백 종료를 기다립니다. 자기 콜백의 Reset은 자신을 기다리지 않습니다. Cancel은 억제 대신 `Cancelled` 완료를 전달합니다. 같은 이동 전용 handle의 조작은 호출자가 직렬화합니다. 토큰 취소·전송 완료와 Reset/Cancel의 경쟁은 내부에서 처리합니다.

## C ABI·Rust 경계

현재 C++ API는 ABI 안정성을 약속하지 않습니다. 공개 구조·클래스 변경 후 라이브러리와 소비자를 함께 빌드합니다. 선택적 C ABI 1은 opaque handle, 고정 폭 상태 코드, 명시적 생성·해제, pointer+length와 버전·크기 필드를 사용하며 STL·예외·OS 구조체를 노출하지 않습니다.

Rust 래퍼는 소유 handle과 빌린 버퍼를 구분합니다. 네이티브 I/O 스레드는 외부 언어 콜백을 호출하지 않고 제한된 이벤트 큐에 소유 값을 전달합니다. Rust의 공유 대기 작업자는 표준 Waker를 깨우며 C handle을 직접 보관하지 않습니다. 이벤트 조회 Future의 drop은 그 대기만 취소하고, 마지막 응답·연결 handle의 drop은 해당 작업을 중단합니다. capacity wait의 해제는 네이티브 알림이 끝날 때까지 기다립니다. `Send`/`Sync`, 버퍼 수명·자원 상한과 지원 API는 [C ABI](C_ABI.md)와 [Rust](RUST.md)를 따릅니다.
