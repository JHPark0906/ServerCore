# 공개 API 이전 안내

동작을 명확히 하는 이름을 정식 API로 추가했습니다. 아래 기존 API는 `[[deprecated("Use ...")]]` 경고를 내며 같은 구현으로 전달합니다. 선언·정의와 기존 반환형·인자·`noexcept` 계약은 유지합니다. 삭제 시점은 아직 정하지 않았습니다.

| Deprecated API | 정식 API | 이유 |
| --- | --- | --- |
| `JobRunner::Stop()` | `JobRunner::RequestStop()` | 종료를 요청할 뿐 작업이나 외부 실행 스레드가 끝날 때까지 기다리지 않음 |
| `DatagramTransport::Send(id, bytes)` | `DatagramTransport::SendSerialized(id, bytes)` | 이미 직렬화한 JSON 봉투를 받으며 직렬화·JSON 검증을 수행하지 않음 |
| `HttpServer::Route(method, path, handler)` | `HttpServer::RegisterRoute(method, path, handler)` | 시작 전 라우트 등록이라는 동작을 명시 |
| `HttpServer::WebSocket(path, callbacks)` | `HttpServer::RegisterWebSocket(path, callbacks)` | 시작 전 WebSocket endpoint 등록이라는 동작을 명시 |

새 이름과 기존 이름은 같은 객체의 등록표·송신 순번·오류 처리·종료 상태를 공유합니다. 서로 독립적인 구현이나 별도 설정 모드는 아닙니다. 예를 들어 기존 이름으로 등록한 라우트를 새 이름으로 다시 등록하면 동일한 중복 등록 오류가 발생합니다.

## 종료 요청과 완료 대기

`JobRunner`는 스레드를 소유하지 않습니다. `RequestStop()`은 새 작업을 거절하고 이미 수락한 작업을 `RunUntilStopped()`가 끝까지 실행하도록 요청합니다. 작업 안에서도 호출할 수 있고 반복 호출도 안전합니다. 호출자는 실행 스레드를 join한 후 `JobRunner`를 파괴해야 합니다.

```cpp
ServerCore::Runtime::JobRunner runner;
std::thread worker([&runner] { runner.RunUntilStopped(); });
// runner.Post(...)로 작업을 넣고 반환 Status를 처리합니다.
runner.RequestStop();
worker.join();
```

스레드를 직접 소유하는 `IoContext`, `PeriodicRunner`, `ServerHost`와 종료 완료를 기다리는 `Acceptor`, `HttpServer`의 `Stop()`은 유지합니다. 이들의 종료 계약은 `JobRunner::RequestStop()`과 다릅니다.

## UDP 직렬화 경계

`SendSerialized(id, prepared.Bytes())`에 주는 바이트는 JSON 봉투입니다. TCP 길이 접두사나 UDP 머리를 직접 붙이지 않습니다. `Protocol::SerializeMessage()` 또는 `PreparedMessage`를 사용해 만들고 UDP payload 상한을 지킵니다. 이 호출의 성공은 OS의 datagram 수락이며, 비동기 TCP의 로컬 송신 큐 수락이나 상대의 수신 확인과 같지 않습니다.

`SendSerialized`는 기존 `Send`와 마찬가지로 JSON을 다시 파싱하지 않습니다. 등록되지 않았거나 준비되지 않은 경로, 빈/초과 payload, OS 오류의 판정 순서와 송신 성공 시에만 증가하는 순번 계약도 유지합니다.

## WebSocket 오류 코드 교정

호출자가 `SendText()` 또는 `Close()`에 넘긴 UTF-8이 잘못되면 `InvalidArgument`를 반환하도록 교정했습니다. 앞선 구현의 `InvalidFormat`에 분기하던 코드는 `InvalidArgument`로 옮깁니다. 이는 `JsonValue::Dump()`와 `SerializeMessage()`의 잘못된 송신 인자 규칙과 같습니다. 종료된 연결은 `Closed`, 크기 상한 초과는 `TooLarge`를 먼저 반환하며, 상대가 보내온 잘못된 UTF-8은 계속 WebSocket 종료 코드 1007로 처리합니다. 기존 가상 함수의 시그니처나 슬롯은 바꾸지 않습니다.

## 호환성 범위

기존 API를 호출하는 소스는 유지되지만, `/WX` 또는 `-Werror`로 deprecation 경고를 오류로 취급하는 프로젝트는 위 표에 따라 이전해야 합니다. 라이브러리의 정상 빌드와 예제는 정식 API만 사용합니다. 호환성 검사에서만 deprecation 경고를 제한적으로 억제하고, 별도 컴파일 검사로 각 기존 API의 경고가 실제 발생하는지도 확인합니다.

기존 C++ 가상 인터페이스의 순수 가상 함수나 슬롯은 변경하지 않습니다. `Connection::Send`와 `SendWithOutcome`의 관계도 유지하므로 기존 사용자 정의 연결 구현은 계속 사용할 수 있습니다. `JsonValue::Parse`와 `ParseBytes`는 입력 형태를 명확히 구분하는 이름으로 유지합니다. 내부 파서가 공통이라고 해서 `Parse` 오버로드를 추가해 기존 `Parse({})` 호출을 모호하게 만들지 않습니다.

앞선 HEAD 응답·WebSocket 프로토콜 검증·플랫폼별 수명 관리 수정은 잘못된 동작을 바로잡은 것입니다. 이를 재현하는 legacy 모드는 제공하지 않습니다. 오류 코드의 수치, 설정 구조체의 필드 순서, TCP/UDP 프레임 형식은 변경하지 않았습니다. 정적 라이브러리 소비자는 같은 툴체인·CRT로 다시 빌드하는 기존 배포 계약을 따릅니다.
