# 프로토콜: 바이트 프레임에서 게임 처리기까지

ServerCore의 TCP 와이어 형식은 **리틀 엔디언 4바이트 길이와 UTF-8 JSON 봉투**다. 검증된 JSON 송신 값을 재사용하는 Prepared API와, 소비자가 별도 UDP 소켓에 사용할 수 있는 헤더 전용 DatagramCodec도 제공한다. 이 문서는 각 형식의 검증, 소유권, 오류 처리와 게임 백엔드가 맡아야 할 경계를 설명한다. 특정 게임의 입장·이동·채팅 메시지는 ServerCore의 내장 프로토콜이 아니다.

전체 소개는 [README](../README.md), 모듈별 책임과 스레드·종료 수명은 [아키텍처](ARCHITECTURE.md)에서 이어서 읽을 수 있다.

## 1. 처리 경계와 읽을 코드

```text
TCP 수신 바이트
  → FrameCodec::Reader / Protocol::FrameReader: 길이로 프레임 분리
  → ParseMessage: UTF-8, JSON 문법, 봉투 검증
  → Dispatcher: type 조회, body 존재와 타입별 바이트 상한 검사
  → 게임 처리기: 인증, 게임 스키마, 권한, 상태 변경
  → Session::Send(MessageFields)
  → SerializeMessage → EncodeFrame → Connection 송신 큐
```

| 경계 | 공개 계약 | 구현·검증 근거 |
| --- | --- | --- |
| 공유 프레이밍 | [FrameCodec.h](../include/ServerCore/Protocol/FrameCodec.h) | 헤더만 포함해 쓰는 비할당 구현 |
| 공유 datagram 머리 | [DatagramCodec.h](../include/ServerCore/Protocol/DatagramCodec.h) | 소켓·인증 정책 없이 호출자의 저장소를 쓰는 Encode/Decode |
| 서버 프레임 보관 | [Framing.h](../include/ServerCore/Protocol/Framing.h) | [Framing.cpp](../src/Protocol/Framing.cpp) |
| JSON 값 | [Json.h](../include/ServerCore/Protocol/Json.h) | [Json.cpp](../src/Protocol/Json.cpp) |
| 메시지 봉투 | [Message.h](../include/ServerCore/Protocol/Message.h) | [Message.cpp](../src/Protocol/Message.cpp) |
| 라우팅 | [Dispatcher.h](../include/ServerCore/Dispatch/Dispatcher.h) | [Dispatcher.cpp](../src/Dispatch/Dispatcher.cpp) |
| 연결 상대와 송신 | [Session.h](../include/ServerCore/Session/Session.h) | [ServerHost.cpp](../src/Runtime/ServerHost.cpp)의 NetworkSession |

프레이밍은 JSON을 읽지 않고, JSON 파서는 소켓을 모른다. Dispatcher도 자체 스레드를 만들지 않는다. 이 구분 때문에 TCP 분할 수신, JSON 값 보존, 게임 상태 검증을 각각 시험할 수 있다.

## 2. 바이트 프레임

### 2.1 길이와 인코딩

| 오프셋 | 크기 | 내용 |
| ---: | ---: | --- |
| 0 | 4바이트 | 뒤따르는 JSON 봉투의 바이트 수를 담은 unsigned 32-bit 정수, **리틀 엔디언** |
| 4 | 위 길이만큼 | UTF-8로 인코딩된 JSON 문서 하나 |

길이에는 머리 4바이트를 포함하지 않는다. 문자열 종료용 NUL이나 별도 줄바꿈 구분자는 붙이지 않는다. 클라이언트→서버와 서버→클라이언트가 같은 형식을 쓴다. 일반적인 네트워크 바이트 순서라는 이유로 `htonl()`을 적용하면 이 규격과 달라진다.

예를 들어 `{}` 두 바이트를 프레이밍하면 다음과 같다.

```text
02 00 00 00  7b 7d
└─ 길이 2 ─┘  {  }
```

이 프레임은 프레이밍 계층에서는 정상이다. 그러나 `{}`에는 메시지 `type`과 `body` 또는 `error`가 없으므로 `ParseMessage()`는 거절한다. `EncodeFrame()`도 내용이 JSON인지를 검증하는 함수는 아니다.

`FrameCodec::DefaultMaxBodySize`와 `Protocol::DefaultMaxBodySize`는 **65,536바이트(64 KiB)**다. 길이가 설정 상한과 같으면 허용하고, 초과하면 `TooLarge`다. 머리의 값이 상한을 넘는지는 본문 전체를 기다리지 않고 판정한다. 실제 Host는 `ServerHostOptions::maxBodySize`로 다른 상한을 쓸 수 있으며, 같은 값으로 수신과 송신 프레임을 제한한다.

다음 세 가지 크기를 구분해야 한다.

- **프레임 본문 길이**: `type`, `body`, `seq`, `error` 등을 포함한 JSON 봉투 전체의 UTF-8 바이트 수
- **`Message::RawBodySize()`**: 봉투 안의 `body` 값이 원래 wire에서 차지한 바이트 수
- **게임 문자열 길이**: JSON escape를 해석한 문자열의 UTF-8 바이트 수 또는 게임이 별도로 정한 문자 수

예를 들어 한글을 그대로 UTF-8로 보내는 경우와 `\uXXXX`로 보내는 경우는 같은 JSON 문자열로 해석될 수 있지만 프레임 및 raw body 크기는 다를 수 있다.

### 2.2 TCP 수신 단위는 메시지 단위가 아니다

길이 머리가 여러 번에 나뉘어 올 수도 있고, 한 번의 수신에 여러 프레임이 붙어서 올 수도 있다. Reader는 미완성 머리와 본문을 보관하며, 완성된 프레임을 순서대로 돌려준다. 따라서 `recv()` 한 번을 메시지 하나로 해석하거나, `Append()` 뒤 프레임 하나만 꺼내는 구현은 맞지 않는다.

`FrameReader`를 직접 사용하는 호출자는 다음 순서로 처리한다.

1. 연결마다 Reader 하나를 유지한다.
2. 수신 바이트를 `Append()`하고 반환 Status를 확인한다.
3. `TakeNextFrame()` 또는 `NextFrame()`을 `WouldBlock`이 나올 때까지 반복한다.
4. 완성된 각 본문을 `ParseMessage()`에 넘긴다.
5. 연결 끝에서는 `Finish()`로 미완성 바이트가 남았는지 확인한다.

`WouldBlock`은 현재 꺼낼 완성 프레임이 없다는 뜻이며 통상적인 대기 상태다. 빈 스트림의 `Finish()`는 성공하고, 머리나 본문이 덜 온 상태에서 끝나면 `InvalidFormat`이다. 길이가 0인 프레임은 프레이밍 자체가 허용하지만, 그 빈 본문은 JSON 문서가 아니므로 메시지 파싱에서 거절된다.

한 번 어긋난 길이 경계에서 다음 JSON의 시작을 검색해 복구하는 기능은 없다. `FrameCodec::Reader`는 길이 초과 등의 오류를 terminal 상태로 남긴다. Host는 프레이밍·메시지 파싱 오류가 발생한 연결을 종료한다.

실제 바이트 예와 분할 패턴은 [FramingVectors.txt](../tests/Protocol/FramingVectors.txt)에 있다. 같은 벡터를 서버의 `FrameReader`와 공유 `FrameCodec`에 적용하는 코드는 [ProtocolTest.cpp](../tests/Protocol/ProtocolTest.cpp)의 `FramingConformanceVectors()`다.

### 2.3 두 Reader의 소유권 차이

| API | 저장소와 결과 | 수명·동시성 계약 |
| --- | --- | --- |
| `FrameCodec::Reader` | 호출자가 최소 `4 + maxBodySize` 바이트 저장소를 제공. 완성 본문을 `Append()` 중 callback으로 전달 | 연결별 단일 스레드. callback의 span은 그 callback 동안만 유효. callback은 예외·동일 Reader 재진입 금지 |
| `Protocol::FrameReader` | codec 저장소와 완성 본문 큐를 소유. 본문마다 소유 vector를 보관 | 연결별 단일 스레드. Reader 자체는 복사·이동 불가 |
| `FrameReader::NextFrame()` | Reader가 소유하는 현재 본문의 span | 다음 `Append()`, `NextFrame()`, `TakeNextFrame()` 전까지만 유효하다고 취급 |
| `FrameReader::TakeNextFrame()` | 완료 큐의 vector 소유권을 이동 | 이후 Reader 호출과 무관하게 유효. parse worker 등 비동기 전달에 적합 |

`FrameCodec.h`는 운영체제 헤더, ServerCore Status, 동적 할당과 C++ 예외에 의존하지 않는다. 게임 클라이언트가 ServerCore 정적 라이브러리를 링크하지 않고 공유할 수 있는 부분이다. `EncodeTo()`도 호출자가 준 출력 span에 쓰며, 결과의 `required`, `written`, `error`로 부족한 저장소를 알린다. Reader에 넣는 입력 span은 Reader 자신의 작업 저장소와 겹치면 안 된다.

서버용 `FrameReader`의 프레임 하나 상한이 **완성 큐 전체 메모리 상한**까지 뜻하지는 않는다. 일반 `Append()`는 여러 본문을 큐에 보관할 수 있다. 대기열 예산이 필요한 호출자는 `CompletedFrameAdmission` overload로 복사 전에 body 바이트와 task 수를 승인해야 한다. 거절하면 해당 본문을 복사하지 않고 `TooLarge`를 반환하며 Reader가 terminal 상태가 된다. 앞서 승인해 보관한 본문과 reservation의 정리는 호출자 책임이다.

`DiscardCompletedFrames()`는 대기 중인 완성 본문만 버린다. `ReleaseStorage()`는 미완성·완성·현재 본문과 codec 저장소까지 놓으며, 기존 span을 무효화한다. 이후 `Append()`는 빈 Reader처럼 저장소를 다시 준비한다. 이는 연결 오류를 같은 TCP 스트림에서 복구한다는 의미가 아니라, 객체가 남아 있어도 종료된 연결의 큰 버퍼를 해제하는 수명 API다.

### 2.4 DatagramCodec의 독립적인 경계

[DatagramCodec.h](../include/ServerCore/Protocol/DatagramCodec.h)는 TCP FrameCodec과 별개의 datagram 포맷이다. 실제 UDP 소켓, 토큰 발급·등록·폐기, endpoint 연결, 패킷 순서 검증과 수신 pump는 소비자가 구현한다. ServerCore Host가 UDP 소켓을 만들거나 TCP 세션과 자동 연결하지 않는다.

| 오프셋 | 바이트 | 내용 |
| ---: | ---: | --- |
| 0 | 4 | ASCII `SMU1` magic |
| 4 | 16 | opaque token |
| 20 | 8 | **big-endian** unsigned 64-bit 패킷 순번 |
| 28 | 1..1172 | 응용 payload |

`HeaderBytes = 28`, `MaximumDatagramBytes = 1200`, `MaximumPayloadBytes = 1172`다. 이 상한에는 IP/UDP 헤더를 포함하지 않는다. TCP의 길이 접두사를 덧붙이지 않으며, 각 datagram은 독립적으로 decode한다. 보통의 네트워크에서 단편화를 피하려는 보수적인 상한이지만 모든 경로 MTU를 보장하지 않는다.

`Encode(destination, token, sequence, payload)`는 호출자의 저장소에 쓴 바이트 수를 반환한다. 순번 0, 빈 payload, 상한 초과, 부족한 destination은 쓰기 전에 0을 반환한다. `Decode(datagram)`는 길이·magic·0이 아닌 순번을 검사하고 `optional<PacketView>`를 반환한다. 토큰은 반환 값에 복사하지만 `payload` span은 입력 저장소를 빌리므로 그 버퍼를 재사용하기 전에 처리해야 한다. Encode/Decode 자체는 동적 할당이나 운영체제 호출을 하지 않는다.

토큰은 16바이트 배열이며 `TokenToHex`는 32자리 소문자 hex 문자열을 만든다. `TokenFromHex`는 정확히 32자리인 대소문자 hex만 받는다. Codec은 임의 토큰이 등록됐는지, 순번이 이전 패킷보다 최신인지, payload가 JSON인지 검사하지 않는다. 순번의 생성·중복/역순 제거·wrap 방지와 JSON 파싱은 호출자 책임이다. 문자열 변환 함수가 할당하는 것과 Encode/Decode의 비할당 경계도 구분해야 한다.

이 토큰 머리는 암호화·MAC·재전송 ACK가 아니다. 토큰 생성과 안전한 전달, TCP 신원과의 대응, NAT endpoint 변경 정책, 유실 복구와 송신 예산은 소비자가 정의한다. Codec에는 조각 재조립, 과거 패킷 큐, 신뢰 전송이나 혼잡 제어가 없다. 따라서 이 헤더가 추가됐다는 사실만으로 ServerCore Host가 UDP를 지원하거나 특정 게임이 더 많은 사용자를 수용한다고 해석하지 않는다.

## 3. JSON 메시지 봉투

### 3.1 필드 계약

```json
{
  "type": "Move",
  "body": { "x": 1.5, "y": 2.0 },
  "seq": "request-17"
}
```

| 필드 | 현재 검증 | 보존·의미 |
| --- | --- | --- |
| `type` | 필수, 비어 있지 않은 문자열 | Dispatcher의 정확한 조회 키. 대소문자를 구분한다 |
| `body` | 있으면 반드시 객체. `error`가 없으면 필수 | 내부 필드의 의미는 게임 처리기가 검증한다. `{}`는 정상이며 `null`, 배열, 문자열, 숫자는 거절한다 |
| `seq` | 선택, 임의의 JSON 값 | 정수·문자열·배열·객체·`null` 모두 보존한다. 번호 생성·재전송·중복 제거 기능은 없다 |
| `error` | 선택, 객체이며 그 안의 `code`가 문자열 | `code`의 의미와 추가 필드는 게임이 정한다. 현재 검사는 빈 `code` 문자열까지 금지하지는 않는다 |
| 그 밖의 최상위 필드 | JSON 문법은 검사하지만 Message의 필드로 보관하지 않음 | 파싱 뒤 다시 직렬화하면 자동으로 복원되지 않는다 |

루트는 반드시 객체이고 `body`와 `error` 중 하나 이상이 있어야 한다. 두 필드를 동시에 보내는 것도 허용한다. `type`에 ASCII 전용 이름이나 별도 정규식을 강제하지는 않지만, UTF-8 및 JSON 문자열 검사는 통과해야 한다.

**`id`는 ServerCore 봉투의 표준 필드가 아니다.** `Message`에는 `Id()`가 없고, `MessageFields`에도 `id` 자리가 없다. 게임이 상대를 식별하는 ID를 전송하려면 `body.id` 등 게임 스키마에 정의한다. 최상위 `id`를 보냈다고 Host의 세션 ID가 바뀌거나 인증되는 일은 없다.

원격 세션의 실제 `SessionId`는 서버의 `SessionRegistry`가 발급하는 unsigned 64-bit 강타입이며 0은 Invalid다. 서버가 부여한 세션 ID와 클라이언트가 주장하는 게임 필드는 구분해야 한다. 재접속은 새 세션이며, 같은 플레이어의 복원 정책도 게임이 별도로 구현한다.

### 3.2 seq는 자동 응답 기능이 아니다

`ParseMessage()`는 seq 값을 보존하지만 요청의 응답을 자동 생성하지 않는다. 게임이 응답을 만들 때 `MessageFields::sequence`에 `message.Sequence()`를 명시적으로 넣어야 한다.

```cpp
const auto status = session->Send(ServerCore::Protocol::MessageFields{
    "MoveAccepted", &responseBody, message.Sequence(), nullptr });
```

위 코드는 `responseBody`와 현재 `message`가 살아 있는 처리기 안에서 호출하는 예다. seq가 없으면 `Sequence()`는 null pointer이고, JSON `"seq": null`이 있으면 null JSON 값을 가리키는 유효한 포인터다. 두 경우를 구분할 수 있다.

보존하는 것은 JSON **값**이다. 입력의 공백, 키 순서, `1e0` 같은 숫자 철자, `\uXXXX` 사용 여부까지 그대로 돌려준다는 뜻은 아니다.

### 3.3 error 봉투와 로컬 Status

다음은 정상적인 error-only 메시지다.

```json
{
  "type": "MoveRejected",
  "error": { "code": "not_joined", "message": "먼저 입장해야 합니다." },
  "seq": "request-17"
}
```

wire의 `error.code` 문자열과 C++의 `Core::ErrorCode`는 별개의 계약이다. 예를 들어 게임은 `not_joined`를 응답으로 보내면서 연결을 유지할 수 있다. `Status`를 반환하는 것만으로 JSON 오류 응답을 만들거나 전송하지는 않는다.

또한 **파싱 가능한 봉투와 Dispatcher 처리기가 받는 봉투의 범위는 다르다.** `ParseMessage()`는 error-only 메시지를 허용하지만, 등록된 `MessageHandler`는 body가 있어야 한다. body 없는 메시지가 등록된 타입에 도달하면 Dispatcher는 처리기를 호출하지 않고 `InvalidFormat`을 반환한다. 서버 처리기에서 받아야 하는 응답이라면 `{}` body를 함께 두는 등의 게임 프로토콜 약정이 필요하다.

### 3.4 원래 body 바이트 수

`RawBodySize()`는 다시 Dump한 크기가 아니라 수신 JSON에서 `body` **값 토큰의 시작부터 끝까지**다. 값 앞의 콜론 뒤 공백과 값 뒤의 구분 공백은 제외하며, 객체 내부 공백과 escape 표기는 포함한다. body가 없는 error-only 메시지의 값은 0이다.

```text
body 값 {"x":1}     → 7바이트
body 값 { "x": 1 }  → 10바이트
```

Dispatcher의 `maximumRawBodySize`는 이 값에 적용된다. `seq`, `error` 또는 다른 봉투 필드를 이 타입별 한도로 제한하는 것은 아니다. 이들도 포함하는 절대 상한은 프레이밍의 `maxBodySize`가 담당한다.

## 4. JSON 값과 검증

### 4.1 UTF-8와 문법

`JsonValue::Parse()`와 `ParseMessage()`는 입력 전체가 유효한 UTF-8인지 먼저 검사한다. 구현은 [Utf8.cpp](../src/Core/Utf8.cpp)의 공통 검사기를 사용한다. 잘못된 continuation byte, 과도하게 긴 인코딩, Unicode surrogate를 직접 인코딩한 바이트 등은 허용하지 않는다.

JSON 파서는 다음 규칙을 적용한다.

- 문서 하나를 읽고 남은 비공백 데이터가 있으면 거절한다. 한 프레임에 JSON 문서 두 개를 이어 붙일 수 없다.
- 문서 시작의 UTF-8 BOM은 허용하며, 문법 공백은 space·tab·CR·LF다. 출력은 BOM을 붙이지 않는다.
- 문자열의 일반 JSON escape와 `\uXXXX`를 해석한다. 보충 문자용 high/low surrogate 쌍을 합치며, 잘못된 쌍과 단독 surrogate는 거절한다.
- 모든 객체 깊이에서 중복 키를 거절한다. 마지막 값 우선이나 첫 값 우선으로 넘어가지 않는다.
- 후행 쉼표, 주석, 선행 `+`, 불필요한 선행 0, 빠진 소수부·지수부를 허용하지 않는다.
- 파싱과 Dump의 재귀 값 깊이에는 `MaximumJsonNestingDepth = 256` 검사가 있다. 루트 값을 깊이 0으로 세며, 깊은 사용자 생성 JsonValue도 출력 때 검사를 받는다.

유효한 JSON 문자열에 들어갈 수 있다는 사실이 게임 필드의 허용을 뜻하지는 않는다. 예를 들어 escape된 줄바꿈은 JSON으로는 정상이다. 닉네임·채팅에서 줄바꿈을 금지할지, 공백을 trim할지, Unicode 정규화나 금칙어 검사를 할지는 게임 책임이다. ServerCore는 문자열을 자동 정규화하지 않는다.

### 4.2 정수와 부동소수점

| 입력 토큰 | 보관 방식 | 정확한 조회 |
| --- | --- | --- |
| `0`, `42`, `18446744073709551615` | `uint64_t` | `TryUInt64()` |
| `-1`, `-9223372036854775808` | `int64_t` | `TryInt64()` |
| `1.0`, `1e0`, `-0`, `-0.0` | 유한 `double` | `TryNumber()` |

소수점·지수 표기 없는 정수는 signed/unsigned 64-bit 범위 안에서 정확히 보관한다. `18446744073709551616`이나 `-9223372036854775809`를 double로 반올림해 받아들이지 않는다. 반면 같은 크기를 지수 또는 소수 표기로 보내면 double 범위와 정밀도로 해석한다. 변환이 범위를 벗어나거나 결과가 유한하지 않으면 거절한다.

`TryNumber()`는 정수에도 double 근사값을 제공하는 호환 접근자다. ID·seq·카운터를 정확히 읽으려면 정수 접근자를 써야 한다. 양의 wire 정수는 작은 값도 `TryUInt64()` 쪽이므로 `TryInt64()`만 검사해 모든 정수를 읽을 수 있다고 가정하지 않는다. C++에서 signed 양수를 만들어 전송한 뒤 다시 파싱해도 wire의 비음수 정수 규칙을 따른다.

double 출력은 `std::to_chars()`를 사용한다. 최단 출력이 정수처럼 보이면 `.0`을 붙여 double 경로임을 보존한다. 이는 유한 double이 64-bit 정수 경계를 넘을 때, 출력한 문자열을 상대가 범위 밖 정수로 오인해 거절하는 문제를 막는다. `-0.0`의 부호, 정수 경계 양옆 값, 최대 유한 값과 subnormal 값은 `JsonDumpRejectsUnsafeValues()`의 왕복 회귀 대상이다.

서버의 64-bit 보존이 다른 언어 클라이언트의 정밀도까지 보장하지는 않는다. 모든 JSON 숫자를 double로 읽는 클라이언트와 큰 ID를 공유한다면 게임 스키마에서 십진 문자열을 사용하는 등의 표현을 정해야 한다. ServerCore는 `body.id`의 숫자·문자열 여부를 결정하지 않는다.

### 4.3 결정적 출력과 오류 구분

`JsonValue::Object`의 메모리 보관 순서는 정해져 있지 않지만 `Dump()`는 키를 정렬한다. 문자열의 따옴표·역슬래시·제어문자를 escape하고, 배열·객체에 일관된 구분 공백을 넣는다. 직렬화 결과 길이는 반환된 UTF-8 바이트로 계산해야 하며, 직접 조립한 원본 JSON의 길이를 재사용해서는 안 된다.

직접 만든 JsonValue에 잘못된 UTF-8 키·문자열, NaN, infinity 또는 과도한 깊이가 있으면 `Dump()`와 이를 사용하는 `SerializeMessage()`도 거절한다. C++ 값 생성 자체가 입력 검증의 대체 수단은 아니다.

| 실패 지점 | 일반적인 결과 | 의미 |
| --- | --- | --- |
| 수신 JSON 문법·UTF-8·숫자·봉투 | `InvalidFormat` | 원격 바이트가 수신 계약을 위반 |
| 송신 필드·JSON 값 | `InvalidArgument` | 호출자가 직렬화할 수 없는 값을 제공 |
| 프레임 또는 등록 body 상한 초과 | `TooLarge` | 해당 계층의 바이트 상한 위반 |
| 현재 완성 프레임 없음 | `WouldBlock` | 추가 수신을 기다리는 상태 |
| 통상적인 할당·플랫폼 처리 실패 | `PlatformError` | `Result`/`Status`로 전달하는 자원 실패 |

이 표가 모든 API에 `noexcept`를 선언한다는 뜻은 아니다. JsonValue 객체나 컨테이너를 직접 구성하는 코드는 일반 C++ 할당 예외를 낼 수 있다. 공개 파싱·직렬화 함수는 형식 오류와 통상적인 할당 실패를 Result로 변환하며, 게임 처리기와 관찰자는 자신의 예외 금지 경계를 지켜야 한다.

## 5. Dispatcher와 게임 처리기의 경계

`Dispatcher::Register(type, handler, maximumRawBodySize)`는 한 타입에 처리기 하나를 등록한다. 빈 타입·빈 처리기는 `InvalidArgument`, 중복 타입은 `AlreadyExists`, `Freeze()` 뒤의 등록은 `Closed`다. 기본 타입별 상한인 `UnlimitedRawBodySize`는 **프레임 상한 외의 추가 제한 없음**을 뜻한다.

등록과 Freeze는 게이트로 직렬화한다. Freeze 전 Dispatch는 처리기 참조를 얻는 동안만 공유 게이트를 잡고, 처리기는 잠금 밖에서 호출한다. Freeze가 반환한 뒤에는 표가 불변이므로 등록표를 잠그지 않고 조회한다. `ServerHost::Start()`는 수락 전에 Dispatcher를 Freeze한다.

Dispatcher 자체는 다음을 하지 않는다.

- 처리기의 실행 순서나 별도 스레드를 만들지 않는다. 호출한 스레드에서 즉시 실행한다.
- 인증 여부, 세션별 허용 type, 플레이어 권한을 검사하지 않는다.
- 처리기 실패 Status를 자동 오류 응답이나 연결 종료로 바꾸지 않는다.
- 처리기 예외를 통상적인 라우팅 실패로 숨기지 않는다. 처리기에는 예외 금지 계약이 있다.

현재 ServerHost는 게임 Dispatch를 JobRunner의 직렬 문맥에 모은다. 선택적 parse worker가 켜져도 JSON 파싱만 worker에서 하며, 같은 세션의 프레임 순서를 유지한 뒤 JobRunner로 전달한다. Dispatcher를 별도 실행 환경에서 직접 쓰면 그 실행 환경이 동시성·순서를 책임져야 한다. 여러 세션 사이의 전역 수신 시각 순서를 약속하는 것은 아니다.

등록되지 않은 타입은 기본 `LogAndIgnore` 정책에서 기록 후 성공으로 무시한다. `Disconnect` 정책에서는 `Session::Disconnect()`를 호출하고 `UnknownType`을 돌려준다. 등록된 타입의 body 부재·크기 초과나 처리기 실패는 다르다. Dispatcher는 Status만 반환하며, 현재 Host도 이를 기록하고 자동으로 연결을 끊지 않는다. 연결을 끝내야 하는 게임 정책은 처리기가 명시적으로 수행한다.

핵심 회귀는 [DispatcherTest.cpp](../tests/Dispatch/DispatcherTest.cpp)의 `RegisterRoutesAndEnforcesRawBodyLimit()`, `HandlerStatusIsPropagatedWithoutDisconnect()`, `ErrorEnvelopeWithoutBodyIsNotRouted()`, `FreezeSerializesConcurrentRegistration()`에 있다.

## 6. Session 송신과 수명

`Session`은 소켓 핸들 대신 게임이 사용하는 상대의 경계다. `Connected → Authenticated` 전이는 `MarkAuthenticated()`로 요청하지만 이 함수는 자격 증명을 검증하지 않는다. 게임이 검증한 뒤 호출하는 상태 전이이며, `Send()`를 쓰는 데 모든 게임이 동일한 인증 절차를 거쳐야 한다는 내장 정책도 없다.

`Message`는 파싱 결과를 자체 소유한다. 수신 span이 사라져도 결과 Message는 유효하지만, `Type()`, `Body()`, `Sequence()`, `Error()`에서 얻은 참조는 해당 Message 수명에 묶인다. 처리기 뒤에도 필요하면 게임 값으로 복사한다.

반대로 `MessageFields`는 `string_view`와 JSON 포인터를 **빌려 쓴다**. 원본은 `SerializeMessage()` 또는 `Session::Send()` 호출이 끝날 때까지 유지하면 된다. NetworkSession은 호출 중 JSON과 길이 머리를 만들고, 비동기 송신 큐에는 바이트 사본을 넣는다. 게임은 Session 송신 전에 `EncodeFrame()`을 별도로 적용하지 않는다.

| 호출 | 성공의 의미 | 보장하지 않는 것 |
| --- | --- | --- |
| `Send(fields)` | 이 프레임이 로컬 송신 큐에 들어감 | 상대 프로그램의 수신·처리 확인 |
| `SendAndDisconnect(fields, reason)` | 마지막 봉투를 큐에 넣고 drain 후 종료를 요청함 | peer 종료·Host Stop·즉시 Disconnect·graceful timeout 뒤의 최종 바이트 도착 |
| `Disconnect(reason)` | 즉시 종료 요청. 반복 호출 가능 | 남은 송신 큐의 전달 |

Send 계열과 Disconnect는 스레드 안전하지만 동시 호출에는 세션 안에서 정해진 수락 순서가 적용된다. `SendAndDisconnect()`보다 먼저 수락된 Send는 앞에 남고, 이후 Send는 `Closed`로 거절된다. MarkAuthenticated와 게임 상태 변경은 게임의 직렬 문맥에서 수행한다.

송신 큐에는 연결별 및 Host 공유 예산이 있다. 꽉 차면 `WouldBlock`이며, 성공하지 않은 프레임이 언젠가 자동으로 다시 전송되지는 않는다. 게임은 다시 보낼 수 있는 상태 업데이트인지, 명단 누락을 허용할 수 없어 연결을 종료해야 하는 이벤트인지 결정해야 한다. 큰 명단이나 파일을 한 프레임에 넣기 위해 상한만 늘리는 대신, 필요한 분할·페이지·완료 조건을 게임 프로토콜로 정해야 한다.

수신·파싱 대기열과 종료 drain의 흐름은 [아키텍처](ARCHITECTURE.md), 예산의 기본값과 결합 조건은 [지원 범위와 설정](SUPPORT_AND_LIMITS.md)을 참고한다.

### 6.1 준비된 JSON과 봉투 재사용

[Message.h](../include/ServerCore/Protocol/Message.h)의 Prepared API는 같은 최신 상태를 여러 수신자에게 보낼 때 JSON escape·숫자 인코딩을 반복하는 비용을 줄이는 값 타입이다.

| API | 결과와 검증 |
| --- | --- |
| `PrepareJsonValue(value)` | 원본을 빌리지 않는 `PreparedJsonValue`. UTF-8·유한 숫자·깊이를 검증하고 바이트를 소유한다 |
| `PrepareMessage(fields)` | 일반 `SerializeMessage` 검증을 거친 `PreparedMessage`. body/seq/error 규칙은 동일하다 |
| `PrepareArrayMessage(type, arrayKey, items)` | `{"body":{arrayKey:[...]},"type":type}` 봉투를 조립한다. 준비된 항목 바이트를 복사하고 항목 JSON을 다시 파싱·직렬화하지 않는다 |
| `Session::SendPrepared(message)` | 일반 Send와 같은 로컬 수락·역압·종료 계약으로 보낸다 |

`PreparedJsonValue::Bytes()/Size()`는 값 하나의 UTF-8 바이트이고, `PreparedMessage::Bytes()/Size()`는 **봉투 전체의 UTF-8 바이트**다. 둘 다 전송 머리를 포함하지 않는다. TCP 프레임 예산은 `prepared.Size() + 4`, 위 DatagramCodec을 사용하는 응용 예산은 `prepared.Size() + 28`이다. IP·UDP/TCP 커널 헤더와 TCP 재전송 트래픽까지 이 값으로 계산하지 않는다.

준비된 값은 원시 바이트로 공개 생성할 수 없으며, 준비 함수만 검증된 바이트를 만든다. 원본 JsonValue와 문자열은 준비가 끝나면 변경·해제할 수 있다. 배열 조립의 항목 포인터는 그 함수 호출 동안만 유효하면 되고 결과 봉투는 항목을 빌리지 않는다. null pointer와 이동 후 빈 항목은 거절한다. 빈 항목 배열은 정상적인 빈 JSON 배열이다. type과 배열 키도 JSON 문자열로 escape하며, 배열 키가 비었다는 이유만으로 거절하지는 않는다.

`PrepareJsonValue`는 나중의 `envelope → body → array` 컨테이너 깊이 3을 이미 사용한 것으로 검증한다. 개별 값이 단독으로 유효하더라도 조립 뒤 최대 깊이 256을 넘을 수 있으므로, 일반 Dump보다 이 용도에 필요한 깊이 여유를 먼저 요구한다. 이 경계를 우회하기 위해 준비된 바이트를 수정할 공개 API는 없다.

실제 ServerHost의 `NetworkSession::SendPrepared`는 다시 파싱하거나 직렬화하지 않고 기존 `EncodeFrame`과 송신 큐에 전달한다. 프레임 크기, 세션 상태, 연결별·Host 전체 큐 상한은 그대로 적용한다. 사용자 정의 Session의 **기본 구현은 호환성을 위해 ParseMessage → 기존 가상 Send로 위임**하므로 그 구현까지 재직렬화 비용이 없다고 약속하지 않는다. 작은 fake session이 기존 Send override만 가지고도 준비된 메시지를 검사할 수 있는 이유다.

`QueuedSendBytes()`는 미완료 송신 큐의 관측값이며 예약이나 다음 Send의 성공 보장이 아니다. 사용자 정의 Session의 기본값 0은 큐 계측을 제공하지 않는다는 뜻이다. 소비자는 상태의 지연·병합·우선순위와 송신 예산을 별도로 결정해야 한다. Prepared API는 과거 패킷을 자동으로 최신화하거나 전역 방송을 자동으로 묶지 않는다.

```cpp
// state와 session은 호출자가 가진 최신 JsonValue와 상대 Session이다.
auto item = ServerCore::Protocol::PrepareJsonValue(state);
if (!item.IsOk()) return std::move(item).TakeStatus();
const ServerCore::Protocol::PreparedJsonValue* items[] = { &item.Value() };
auto batch = ServerCore::Protocol::PrepareArrayMessage("StateBatch", "states", items);
if (!batch.IsOk()) return std::move(batch).TakeStatus();
return session->SendPrepared(batch.Value());
```

이 예는 Status를 반환하는 함수 본문에서 사용하는 조각이다. 바깥의 게임 처리기는 JsonValue 구성과 자신의 작업에 대한 예외 금지 경계도 유지해야 한다. `StateBatch`는 예제 이름이며 코어 내장 타입이 아니다.

## 7. 연결해 보는 예제

### 7.1 C++ Echo 처리기

아래 함수는 생성한 Host의 Dispatcher에 **Start 전에** 호출하는 예다. Echo는 설명을 위한 게임 타입이며 ServerCore가 기본 등록하지 않는다. 인증 정책은 넣지 않았고, 응답에는 요청의 seq 값을 명시적으로 복사한다. `maximumRawBodySize = 1024`는 Echo의 body에만 적용된다.

```cpp
#include <ServerCore/Dispatch/Dispatcher.h>
#include <ServerCore/Protocol/Message.h>
#include <ServerCore/Session/Session.h>

ServerCore::Core::Status RegisterEcho(ServerCore::Dispatch::Dispatcher& dispatcher)
{
    return dispatcher.Register("Echo",
        [](const std::shared_ptr<ServerCore::Session::Session>& session,
           const ServerCore::Protocol::Message& request) -> ServerCore::Core::Status
        {
            try
            {
                // 등록된 MessageHandler에는 객체 body가 있는 요청만 도달한다.
                return session->Send(ServerCore::Protocol::MessageFields{
                    "EchoReply", request.Body(), request.Sequence(), nullptr });
            }
            catch (...)
            {
                // 게임 처리기의 예외를 실행자 밖으로 보내지 않는다.
                return ServerCore::Core::Status::FailWithoutMessage(
                    ServerCore::Core::ErrorCode::PlatformError);
            }
        },
        1024);
}
```

호스트를 조립하는 쪽에서 `RegisterEcho(host.GetDispatcher())`의 Status를 확인하고 Start한다. 포트·실행·정상 종료를 포함한 호스트 조립은 [README](../README.md), 라이브러리를 링크하는 방법은 [빌드·테스트·배포](BUILD_TEST_DEPLOY.md)에서 다룬다.

### 7.2 Python TCP 클라이언트

[README의 Echo 서버](../README.md)를 `127.0.0.1:17891`에서 실행한 경우의 클라이언트다. 표준 라이브러리만 사용하며, 소켓 수신이 요청한 바이트 수보다 짧을 수 있다는 점을 처리한다. 이 예의 본문 상한 8 KiB는 README의 Host 설정과 맞춘 로컬 정책이다.

```python
import json
import socket
import struct

MAX_BODY = 8 * 1024

def recv_exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError("프레임이 완성되기 전에 연결이 닫혔습니다.")
        result.extend(chunk)
    return bytes(result)

request = {
    "type": "Echo",
    "body": {"text": "안녕하세요"},
    "seq": "example-1",
}
body = json.dumps(request, ensure_ascii=False, allow_nan=False,
                  separators=(",", ":")).encode("utf-8")
if len(body) > MAX_BODY:
    raise ValueError("송신 본문 상한 초과")

with socket.create_connection(("127.0.0.1", 17891), timeout=5) as sock:
    sock.sendall(struct.pack("<I", len(body)) + body)
    size, = struct.unpack("<I", recv_exact(sock, 4))
    if size > MAX_BODY:
        raise ValueError("수신 본문 상한 초과")
    response = json.loads(recv_exact(sock, size).decode("utf-8"))
    assert response["type"] == "EchoReply"
    assert response["seq"] == request["seq"]
    assert response["body"] == request["body"]
    print(response)
```

이 코드는 작은 정상 응답을 확인하는 예이며 ServerCore JSON 검증기의 대체 구현은 아니다. 다른 JSON 라이브러리는 중복 키나 숫자 범위에 다른 정책을 쓸 수 있다. 완전한 클라이언트는 자기 JSON 라이브러리의 동작도 와이어 계약과 맞춰야 한다.

## 8. 게임 스키마와 지원하지 않는 부분

ServerCore가 제공하는 것은 길이 프레이밍, JSON 봉투, 타입 라우팅, 세션과 제한된 송수신 수명이다. 다음은 소비자 프로토콜이 정하거나 별도 구현해야 한다.

- 버전 협상, 최초 Join 또는 로그인 순서, 게임별 error code와 body 필수 필드
- 닉네임·캐릭터·좌표·채팅·방 목록·관심 영역 등 게임 데이터의 검증과 권한
- seq 기반 응답 대기, 타임아웃, 중복 제거, 재시도, 요청 멱등성
- 큰 데이터의 페이지·청크 구성, 명단 동기화 완료 표시, 재동기화
- 재접속한 사용자의 신원과 상태 복원, 영속 저장
- TLS, 메시지 서명·암호화·압축과 HTTP/WebSocket 전송
- UDP 소켓/수신 pump, 토큰 발급과 신원 연결, endpoint 관리, replay 방지·유실 복구·혼잡 제어(공유 DatagramCodec은 머리의 Encode/Decode만 제공)

TCP는 바이트 순서를 제공하지만 게임 이벤트의 처리 성공을 상대에게 확인해 주지는 않는다. 현재 구현에 일반적인 RPC 호출 계층이나 메시지당 ACK는 없다. 오류를 보내고 유지할지, 즉시 끊을지, 마지막 프레임을 보낸 뒤 끊을지 역시 게임이 Session API로 선택한다.

지원 범위 전체와 운영상 제한은 [지원 범위와 제한](SUPPORT_AND_LIMITS.md)에 모은다. 이 문서의 Echo 예를 실제 게임의 기본 기능이나 프로토콜 버전으로 취급하지 않는다.

## 9. 코드와 회귀 테스트로 확인하는 방법

문서의 계약을 변경할 때는 공개 헤더와 구현뿐 아니라 아래 회귀를 함께 읽는다. 여기서 목록은 존재하는 검증 근거이며, 이 문서 작성 과정에서 테스트를 실행했다는 뜻은 아니다.

| 확인할 계약 | 기존 근거 |
| --- | --- |
| 분할·합쳐진 수신, 최대 길이, 잘린 EOF | [ProtocolTest.cpp](../tests/Protocol/ProtocolTest.cpp)의 `FramingConformanceVectors()`, `FramingRejectsInvalidLengths()` 및 [공유 벡터](../tests/Protocol/FramingVectors.txt) |
| shared codec의 비할당/noexcept 경계 | 같은 파일의 `FrameCodecSharedApiIsNoThrow()` |
| 소유 본문 이전·완료 큐 폐기·저장소 해제 | 같은 파일의 `FramingTransfersAndDiscardsCompletedBodies()`, `FrameReaderReleaseStorageResetsReader()` |
| seq/error 및 raw body 바이트 보존 | 같은 파일의 `MessagePreservesEnvelopeFields()` |
| 정확한 64-bit 정수·범위 밖 수 거절 | 같은 파일의 `MessagePreserves64BitIntegers()`, `JsonRejectsUnrepresentableNumbers()` |
| double 왕복·UTF-8·직접 만든 송신 값 | 같은 파일의 `JsonDumpRejectsUnsafeValues()`, `MessageRejectsInvalidUtf8()`, `MessageRejectsUnsafeOutboundJson()` |
| body가 객체여야 한다는 봉투 규칙 | 같은 파일의 `MessageRejectsNonObjectBodies()` |
| Prepared 소유권·거절·깊이·기존 Session 위임 | 같은 ProtocolTest.cpp의 `PreparedMessagesOwnImmutableJsonAndEnvelope()`, `PreparedMessagesRejectInvalidInputs()`, `PreparedMessagesAdaptLegacySessions()`, `PreparedArrayReservesEnvelopeDepth()` |
| datagram 상한·순번 byte order·token 변환 | 같은 파일의 `DatagramBoundariesAndWireOrder()` |
| 등록·Freeze·unknown type·처리기 거부 | [DispatcherTest.cpp](../tests/Dispatch/DispatcherTest.cpp) |
| 실제 Host의 파싱·송신·순서·역압·종료 통합 | [ServerHostTest.cpp](../tests/Runtime/ServerHostTest.cpp) |

실행 방법과 환경별 검증 범위는 [빌드·테스트·배포](BUILD_TEST_DEPLOY.md)를 따른다.
