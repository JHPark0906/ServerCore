# HTTP와 WebSocket

`ServerCore::Web::HttpServer`는 Windows IOCP 또는 Linux epoll 전송 위에서 HTTP/1.1과 WebSocket을 제공합니다. 기존 `Runtime::ServerHost`의 4바이트 길이 + JSON 프로토콜과는 별도 리스너입니다. 두 서버를 함께 사용하려면 서로 다른 포트를 지정합니다. 같은 `ServerCore::ServerCore` CMake 타깃으로 링크합니다.

## 사용 예

아래 코드는 `GET /health`와 `/echo` WebSocket endpoint를 등록합니다. 서버는 기본적으로 loopback에만 열리며, 외부 접속은 `options.listenAddress`를 명시적으로 설정합니다.

```cpp
#include <ServerCore/Web/HttpServer.h>

#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

int main()
{
    using namespace ServerCore;
    Web::HttpServer server;
    auto status = server.RegisterRoute("GET", "/health", [](const Web::HttpRequest&) {
        return Web::HttpResponse{200, {{"Content-Type", "application/json"}},
            R"({"status":"ok"})"};
    });
    if (!status.IsOk()) return 1;

    Web::WebSocketCallbacks callbacks;
    callbacks.onMessage = [](const std::shared_ptr<Web::WebSocketConnection>& connection,
                             const Web::WebSocketMessage& message) {
        Core::Status sent = message.type == Web::WebSocketMessageType::Text
            ? connection->SendText(std::string_view(
                  reinterpret_cast<const char*>(message.bytes.data()), message.bytes.size()))
            : connection->SendBinary(message.bytes);
        if (!sent.IsOk()) (void)connection->Close(1011, "send failed");
    };
    status = server.RegisterWebSocket("/echo", std::move(callbacks));
    if (!status.IsOk()) return 1;

    Web::HttpServerOptions options;
    options.port = 8080;
    status = server.Start(options);
    if (!status.IsOk()) {
        std::cerr << status.Message() << '\n';
        return 1;
    }
    std::cout << "Listening on http://127.0.0.1:8080. Press Enter to stop.\n";
    std::cin.get();
    return server.Stop().IsOk() ? 0 : 1;
}
```

HTTP 응답은 `curl http://127.0.0.1:8080/health`로 확인합니다. WebSocket 클라이언트는 `ws://127.0.0.1:8080/echo`에 연결하고 텍스트 또는 바이너리 메시지를 전송합니다.

등록 API는 `RegisterRoute`와 `RegisterWebSocket`입니다. 기존 `Route`와 `WebSocket`은 같은 구현에 위임하는 deprecated 호환 API로 남아 있습니다. 새 이름으로 바꾸면 등록 시점, 중복 키의 `AlreadyExists`와 기존 처리기 보존 동작은 그대로 유지됩니다.

## HTTP 계약

- HTTP/1.1 요청, 정확한 메서드·경로 기반 라우팅, 지속 연결과 순서대로 처리하는 pipelining을 제공합니다. query는 `target`에 보존되고 `Path()`는 query 앞의 경로를 반환합니다. URL decoding이나 경로 매개변수 추출은 애플리케이션이 맡습니다.
- `Content-Length`와 chunked 본문을 처리합니다. 헤더 이름은 소문자로 정규화하며 요청·헤더·본문의 크기와 완료 기한을 제한합니다.
- 모호한 본문 길이, 잘못된 헤더와 요청 구문은 거절합니다. 응답 길이와 연결 헤더는 서버가 관리하며 응답 헤더에 줄바꿈을 삽입할 수 없습니다.
- `HEAD`는 오류 응답을 포함해 본문을 보내지 않습니다. `204`·`304`도 본문과 `Content-Length`를 생략하며, `205`는 빈 본문과 `Content-Length: 0`을 보냅니다. `304`의 원래 표현 길이를 빈 응답에서 추측하지 않습니다.
- 애플리케이션이 `Date`를 지정하지 않으면 서버가 UTC 날짜를 생성합니다. 지정한 값은 보존하고 중복 `Date`는 거절합니다. `Upgrade`의 프로토콜 목록은 요청·응답에서 같은 규칙으로 검증하며, 응답의 연결 옵션은 서버가 추가합니다. `426`에는 `Upgrade`가 필요하고, 지원하지 않는 WebSocket 버전에는 지원 버전도 함께 알립니다.
- 요청과 응답의 본문은 메모리에 보관합니다. 파일 전송, 스트리밍 응답, multipart/form-data 해석, 압축과 프록시 기능은 제공하지 않습니다.

## WebSocket 계약

- HTTP Upgrade를 통한 RFC 6455 버전 13 연결, 텍스트·바이너리 메시지, 수신 메시지 분할·재조립, ping/pong과 close handshake를 제공합니다.
- 클라이언트의 마스킹, 프레임 길이·opcode·제어 프레임 규칙, 텍스트의 UTF-8과 종료 코드를 검증합니다. 프레임 크기와 조립된 메시지 크기를 별도로 제한합니다.
- `SendText`, `SendBinary`, `Ping`, `Close`는 스레드 안전합니다. 송신 성공은 로컬 큐가 수락했다는 의미이며 상대의 수신 확인이 아닙니다. 큐 상한에 도달하면 `WouldBlock`을 처리해야 합니다.
- 종료가 끝난 연결은 입력 검증보다 먼저 `Closed`를 반환합니다. 열린 연결에서는 크기 상한을 먼저 검사해 `TooLarge`를 반환하고, 그 안에 있는 잘못된 호출자 텍스트·종료 사유 UTF-8은 `InvalidArgument`로 거절합니다. 이전의 `InvalidFormat` 반환은 수신 형식 오류와 호출자 인수 오류를 구분하는 공통 계약에 맞게 수정되었습니다. 이 송신 오류를 검사하는 코드는 `InvalidArgument`로 분기해야 합니다. 수신한 잘못된 UTF-8의 WebSocket 종료 코드는 계속 `1007`입니다. 데이터 송신 한 번에는 프레임 상한이 적용되며, ping은 125바이트, 종료 사유는 123바이트가 상한입니다.
- `onMessage`의 바이트는 콜백 동안만 유효합니다. 나중에 사용할 데이터는 복사합니다. 연결 객체의 `shared_ptr`는 보관할 수 있으며, 종료된 연결에 대한 송신은 실패합니다.
- `accept`에서 Origin, 로그인 자격과 접근 권한을 검사할 수 있습니다. 기본은 올바른 handshake를 수락하며, 자동 로그인 검증이나 Origin 정책은 없습니다.
- WebSocket 압축 확장과 subprotocol 협상은 제공하지 않습니다.

## 실행과 수명

라우트는 `Start()` 전에 등록합니다. 처리기는 I/O 실행 문맥에서 동기 호출됩니다. 같은 연결의 콜백은 직렬로 실행되고, 다른 연결은 병렬로 실행될 수 있으므로 공유 애플리케이션 상태는 동기화해야 합니다. DB 대기나 긴 계산은 별도의 실행 문맥으로 넘깁니다.

`Stop()`은 수락을 중단하고 연결·콜백·전송 작업을 정리한 뒤 반환합니다. 콜백에서 직접 호출하면 `InvalidArgument`이며, 서버 객체도 콜백 밖의 제어 스레드에서 파괴해야 합니다. 시작 실패 또는 종료 후 재시작하려면 새 `HttpServer`를 생성합니다.

기본 상한은 연결 256개, 요청 헤더 16 KiB, 요청 본문 64 KiB, 응답 본문 256 KiB, WebSocket 프레임 64 KiB와 조립 메시지 256 KiB입니다. 요청 완료 기한은 30초, 유휴 기한은 120초, HTTP 응답 종료 시 송신 대기는 5초(`responseDrainTimeout`), WebSocket 종료 기한은 3초입니다. 응답 종료 대기와 WebSocket 종료 기한은 추가 입력으로 연장되지 않습니다. 실제 송신 큐 상한은 전송 계층에도 적용됩니다. 콜백 자체의 실행 시간을 강제로 제한하지는 않습니다.

## 지원 범위와 배포

HTTP/2·HTTP/3·HTTPS·WSS는 내장하지 않습니다. TLS가 필요한 배포에서는 reverse proxy에서 TLS를 종료하고 내부 리스너로 전달합니다. 이 계층에는 인증·DB·정적 파일 디렉터리 노출을 자동으로 추가하지 않습니다.

프로토콜 기준은 [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html), [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html)와 [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html)입니다. 구현에 대한 검증 범위와 실제 실행 환경은 [VALIDATION.md](VALIDATION.md)를 참고합니다.
