# HTTP와 WebSocket

`ServerCore::Web::HttpServer`는 Windows IOCP 또는 Linux epoll 전송 위에서 HTTP/1.1과 WebSocket을 제공합니다. 기존 `Runtime::ServerHost`의 4바이트 길이 + JSON 프로토콜과는 별도 리스너입니다. 두 서버를 함께 사용하려면 서로 다른 포트를 지정합니다. 같은 `ServerCore::ServerCore` CMake 타깃으로 링크합니다.

## 사용 예

아래 코드는 `GET /health`, 경로 매개변수를 받는 `GET /plugins/{id}`, `/echo`와 `/plugins/{id}/events` WebSocket endpoint를 등록합니다. 서버는 기본적으로 loopback에만 열리며, 외부 접속은 `options.listenAddress`를 명시적으로 설정합니다.

```cpp
#include <ServerCore/Web/HttpServer.h>

#include <iostream>
#include <memory>
#include <string>
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

    status = server.RegisterRoutePattern("GET", "/plugins/{id}",
        [](const Web::HttpRequest& request) {
            return Web::HttpResponse{200, {{"Content-Type", "text/plain; charset=utf-8"}},
                std::string("plugin: ") + std::string(request.PathParameter("id"))};
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

    Web::WebSocketCallbacks pluginEvents;
    pluginEvents.onOpenWithRequest = [](
        const std::shared_ptr<Web::WebSocketConnection>& connection,
        const Web::HttpRequest& request) {
        const auto sent = connection->SendText(
            std::string("topic: ") + std::string(request.PathParameter("id")));
        if (!sent.IsOk()) (void)connection->Close(1011, "send failed");
    };
    status = server.RegisterWebSocketPattern("/plugins/{id}/events", std::move(pluginEvents));
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

HTTP 응답은 `curl http://127.0.0.1:8080/health`로 확인합니다. `curl http://127.0.0.1:8080/plugins/weather`는 `plugin: weather`를 반환합니다. WebSocket 클라이언트는 `ws://127.0.0.1:8080/echo`에 연결하고 텍스트 또는 바이너리 메시지를 전송합니다. `ws://127.0.0.1:8080/plugins/weather/events`에 연결하면 첫 메시지로 `topic: weather`를 받습니다. 실제 이벤트 구독·발행은 애플리케이션이 구현합니다.

정확 일치 등록 API는 `RegisterRoute`와 `RegisterWebSocket`, 패턴 등록 API는 `RegisterRoutePattern`과 `RegisterWebSocketPattern`입니다. 정확 일치 API는 계속 사용하며 패턴으로 자동 해석하지 않습니다. 기존 `Route`와 `WebSocket`은 정확 일치 API에 위임하는 deprecated 호환 API입니다. 이번 패턴 지원으로 추가 deprecated API가 생기지는 않습니다.

## 패턴과 경로 매개변수

`RegisterRoutePattern("GET", "/plugins/{id}", handler)`는 `{id}` 위치의 비어 있지 않은 경로 세그먼트 하나를 받습니다. 이름은 ASCII 식별자 `[A-Za-z_][A-Za-z0-9_]*`이며 한 패턴 안에서 중복할 수 없습니다. `{id}.json`처럼 세그먼트 일부만 캡처하거나 여러 세그먼트를 한꺼번에 받는 wildcard·정규식은 지원하지 않습니다. 등록 문자열은 ASCII URL 표기를 사용합니다. 정적 세그먼트의 비ASCII 문자는 `/caf%C3%A9/{id}`처럼 percent-encoding으로 표현합니다. 잘못된 패턴 등록은 `InvalidArgument`입니다.

캡처한 이름과 값은 `HttpRequest::pathParameters`가 경로 순서의 문자열 쌍 벡터로 소유하며, 정확 일치 라우트에서는 비어 있습니다. 요청을 복사하면 캡처도 복사됩니다. `PathParameter("id")`는 이름의 대소문자를 구별해 그 값의 `string_view`를 반환하고, 없는 이름은 빈 view를 반환합니다. view는 요청의 해당 저장소가 유지되는 동안만 유효하므로 요청을 수정하거나 콜백 이후에 보관하려면 문자열로 복사합니다. `target`과 `Path()`는 요청의 원래 표기를 보존하며 `Path()`만 query를 제외합니다.

라우트는 등록 순서와 관계없이 다음 규칙으로 선택합니다.

1. 먼저 query를 제외한 원래 경로의 정확 일치를 찾습니다. 해당 경로에 다른 HTTP 메서드만 등록돼 있어도 패턴으로 내려가지 않습니다.
2. 정확 일치가 없으면 패턴을 비교합니다. 왼쪽부터 처음 다른 위치에서 정적 세그먼트가 매개변수보다 우선합니다. 예를 들어 `/plugins/new`는 `/plugins/{id}`보다 우선하고, `/plugins/{id}`는 `/{group}/settings`보다 `/plugins/settings`에 대해 우선합니다. 대소문자와 마지막 `/`는 구별합니다.
3. 가장 구체적인 경로를 정한 뒤 HTTP 메서드를 선택합니다. 명시적 `HEAD`가 우선하고 없으면 `GET` 처리기를 사용해 본문 없이 응답합니다. 경로는 있지만 메서드가 없으면 `405 Method Not Allowed`와 `Allow`를 반환합니다. `GET`이 있으면 `Allow`에 `HEAD`도 포함합니다. 경로 자체가 없으면 `404`입니다.

예를 들어 `GET /plugins/new`와 `POST /plugins/{id}`가 등록되어 있을 때 `POST /plugins/new`는 더 구체적인 경로의 허용 메서드가 아니므로 `405`입니다. 덜 구체적인 POST 처리기로 우회하지 않습니다. 같은 HTTP 메서드에서 `/plugins/{id}`와 `/plugins/{name}`은 같은 구조의 중복 패턴이며 두 번째 등록은 `AlreadyExists`입니다. 다른 메서드는 같은 구조에 서로 다른 매개변수 이름을 사용할 수 있습니다. WebSocket도 매개변수 이름만 다른 동일 구조를 중복 등록할 수 없습니다.

패턴 비교는 경로를 `/`로 나눈 뒤 각 세그먼트를 **한 번만** percent-decode합니다. 디코딩 결과는 유효한 UTF-8이어야 하며 잘못된 escape, ASCII 제어 문자·NUL, 슬래시·역슬래시, `.`·`..` 세그먼트는 `400`으로 거절합니다. `+`는 공백으로 바꾸지 않습니다. 해당 HTTP 또는 WebSocket 등록표에 패턴이 있고 정확 일치가 없다면, 일치하는 패턴 접두사가 없어도 이 검증을 적용합니다. 정확 일치 경로와 `OPTIONS *`는 패턴 디코딩을 거치지 않습니다. query는 이 검증·디코딩의 대상이 아닙니다.

| 요청 경로 | `/plugins/{id}` 처리 |
| --- | --- |
| `/plugins/weather?lang=ko` | `id`는 `weather`; query는 `target`에 보존 |
| `/plugins/a+b` | `id`는 `a+b` |
| `/plugins/%ED%95%9C%EA%B8%80` | `id`는 `한글` |
| `/plugins/%252F` | `id`는 `%2F`; 다시 디코딩하지 않음 |
| `/plugins/a%2Fb`, `/plugins/%2E%2E`, `/plugins/%ZZ` | `400` |
| `/plugins/`, `/plugins/weather/` | 이 패턴과 일치하지 않음 |

WebSocket 경로도 `RegisterWebSocketPattern("/plugins/{id}/events", callbacks)`로 등록합니다. `callbacks.accept`는 upgrade 전 요청을 받고, `callbacks.onOpenWithRequest`는 upgrade 후 연결 객체와 요청을 함께 받습니다. 두 콜백 모두 `PathParameter("id")`와 요청 헤더를 읽을 수 있습니다. 연결 ID에 주제를 연결해 보관하려면 콜백 안에서 매개변수 문자열을 복사합니다.

HTTP와 WebSocket은 등록표가 분리되어 같은 경로를 함께 등록할 수 있습니다. 일반 HTTP 요청은 HTTP 라우트로, WebSocket upgrade 요청은 WebSocket 라우트로 보냅니다. HTTP 라우트가 없고 WebSocket 경로만 일치하는 일반 요청은 기존처럼 upgrade 누락 `400`을 반환합니다.

## HTTP 계약

- HTTP/1.1 요청, 정확 일치·패턴 기반 라우팅, 지속 연결과 순서대로 처리하는 pipelining을 제공합니다. 경로 매개변수는 위 규칙으로 해석하며 query 해석은 애플리케이션이 맡습니다.
- `Content-Length`와 chunked 본문을 처리합니다. 헤더 이름은 소문자로 정규화하며 요청·헤더·본문의 크기와 완료 기한을 제한합니다.
- 모호한 본문 길이, 잘못된 헤더와 요청 구문은 거절합니다. 응답 길이와 연결 헤더는 서버가 관리하며 응답 헤더에 줄바꿈을 삽입할 수 없습니다.
- `HEAD`는 오류 응답을 포함해 본문을 보내지 않습니다. `204`·`304`도 본문과 `Content-Length`를 생략하며, `205`는 빈 본문과 `Content-Length: 0`을 보냅니다. `304`의 원래 표현 길이를 빈 응답에서 추측하지 않습니다.
- 애플리케이션이 `Date`를 지정하지 않으면 서버가 UTC 날짜를 생성합니다. 지정한 값은 보존하고 중복 `Date`는 거절합니다. `Upgrade`의 프로토콜 목록은 요청·응답에서 같은 규칙으로 검증하며, 응답의 연결 옵션은 서버가 추가합니다. `426`에는 `Upgrade`가 필요하고, 지원하지 않는 WebSocket 버전에는 지원 버전도 함께 알립니다.
- 일반 라우트의 요청 본문과 `HttpResponse`는 메모리에 보관합니다. 별도 streaming 라우트는 요청 본문도 제한된 조각으로 받습니다. `HttpResponseWriter`는 응답을 제한된 조각으로 보내며 전체 표현 크기는 64비트로 다룹니다. multipart/form-data 해석, 압축과 프록시 기능은 제공하지 않습니다.

## 비동기 처리와 스트리밍

`RegisterAsyncRoute`·`RegisterAsyncRoutePattern`은 `shared_ptr<const HttpRequestContext>`를 받습니다. 문맥은 요청 문자열·본문·경로 매개변수를 소유하므로 콜백이 반환한 뒤 다른 작업이나 소비 애플리케이션이 시작한 외부 요청의 완료 콜백에서 응답할 수 있습니다. `context->response`도 별도로 보관할 수 있습니다. 연결 종료·서버 종료·처리 기한 만료는 작성기의 `GetCancellationToken()`으로 전달됩니다. 외부 작업의 취소 체계에 연결하고 취소되면 보유 문맥을 해제합니다.

```cpp
server.RegisterAsyncRoute("GET", "/deferred",
    [](std::shared_ptr<const ServerCore::Web::HttpRequestContext> context) {
        // context를 애플리케이션의 제한된 비동기 작업에 전달할 수 있습니다.
        const auto result = context->response->Complete({200, {}, "ready"});
        if (!result.IsOk()) context->response->Abort();
    });
```

작성기는 다음 순서를 따릅니다.

1. `Start(HttpResponseHead)`로 헤더를 보냅니다. `contentLength`가 있으면 고정 길이, 없으면 HTTP/1.1 chunked 전송입니다. `Content-Length`·`Transfer-Encoding`·`Connection`을 직접 헤더에 넣지 않습니다.
2. `Write(span<const byte>)`로 `MaxWriteBytes()` 이하의 본문 조각을 보냅니다. 입력은 성공 시 복사됩니다. `WouldBlock`이면 아무 바이트도 수락하지 않았으므로 같은 조각을 재시도할 수 있습니다.
3. `Finish()`로 응답을 끝냅니다. 고정 길이와 실제 본문 길이가 다르면 `InvalidArgument`이며 부족한 본문을 더 보낸 뒤 재시도할 수 있습니다. chunked 응답은 마지막 chunk가 큐에 들어가야 완료됩니다.

`Complete(HttpResponse)`는 작은 완성 응답을 한 번에 보내는 편의 API입니다. 기존 `maxResponseBodyBytes`와 단일 송신 상한이 적용됩니다. `Start`/`Write` 경로의 전체 본문에는 이 제한을 적용하지 않습니다. `HEAD`·204·205·304는 공통 헤더 인코더로 처리하며 본문을 쓰지 않고 `Finish()`합니다. 성공한 Finish·Complete 후 추가 쓰기는 `Closed`, `Abort()`는 무효 동작입니다. 전송 성공은 로컬 큐 수락이며 상대 수신 확인은 아닙니다.

송신 예산 부족 시 `WaitForWriteCapacity(bodyBytes, callback)`의 이동 전용 등록을 보관합니다. 헤더 또는 Finish 재시도에는 0을 넘깁니다. 알림은 용량 예약이 아니므로 `Ok` 뒤에도 재시도가 `WouldBlock`일 수 있습니다. 응답 종료는 이전 응답의 대기를 취소하여 다음 응답의 대기를 막지 않습니다. 등록 Reset·소멸은 미완료 콜백을 억제하고 실행 중 콜백을 join합니다. 콜백은 등록 호출 안 또는 I/O·취소 스레드에서 실행될 수 있으므로 오래 기다리지 않습니다.

작성기는 동시 호출에 안전하지만 애플리케이션의 생산자와 재시도는 직렬화해야 합니다. 다른 쓰기가 진행 중이면 쓰기·대기 등록 자체도 `WouldBlock`일 수 있습니다. 진행 없이 즉시 콜백을 재등록하는 재귀 루프를 만들지 않습니다. `Abort()`는 미완료 응답을 취소하고 연결을 닫습니다. 헤더를 보낸 뒤 오류가 생기면 새 오류 응답으로 교체할 수 없습니다.

## SSE와 파일

`Web/HttpStreaming.h`의 `EncodeSseEvent`와 `WriteSseEvent`는 UTF-8 SSE 이벤트를 인코딩합니다. `Content-Type: text/event-stream`, 선택적 `Cache-Control: no-cache`로 길이 없는 응답을 시작한 뒤 이벤트를 씁니다. data의 CR·LF는 여러 `data:` 행으로 변환하며 event/id의 줄바꿈 삽입을 거절합니다. 빈 id는 마지막 이벤트 ID를 초기화하고, 없는 id는 유지합니다. `WriteSseEvent`도 `WouldBlock`을 반환하므로 생산 속도·재시도 큐를 제한해야 합니다. 이벤트 저장·재접속 재생·구독 관리는 애플리케이션 책임입니다.

`SendFile(executor, context, path, head)`는 호출자가 제공한 `TaskExecutor`에서 파일을 열고 실제 크기를 구한 뒤 최대 64 KiB씩 전송합니다. 반환된 `TaskHandle`로 취소·완료를 확인합니다. HEAD는 파일 메타데이터만 보내고 본문을 읽지 않으며 Range도 무시합니다. 파일 경로의 접근 권한과 URL 매핑은 호출자가 검증합니다. 파일 축소 등 헤더 이후 I/O 실패는 연결을 종료합니다.

기본 200 파일 응답에서는 `If-Match`, `If-Unmodified-Since`, `If-None-Match`, `If-Modified-Since`를 HTTP 우선순위에 따라 평가합니다. 캐시 일치는 본문 없는 304, 실패한 사전 조건은 412입니다. 그 후 단일 GET `Range: bytes=시작-끝`, `시작-`, `-길이`를 처리해 206과 `Content-Range`를 보냅니다. 유효하지만 충족할 수 없는 범위는 빈 416과 `Content-Range: bytes */전체크기`입니다. 잘못되거나 지원하지 않는 범위 및 multipart 요청은 전체 200 응답으로 처리합니다.

기본 ETag는 파일 크기·수정 시각에 기반한 약한 태그이며 `Last-Modified`와 `Accept-Ranges`도 생성합니다. 호출자가 head에 제공한 올바른 ETag는 보존합니다. `If-Range`는 호출자가 보장한 강한 ETag가 정확히 일치할 때만 부분 응답을 허용합니다. 약한 태그나 날짜 If-Range는 전체 응답으로 처리합니다. 파일 시스템 시각만으로 강한 표현 식별을 보장하지 않기 때문입니다. 전송 중 파일은 변경하지 않고 새 버전을 원자적으로 게시하는 방식을 사용하며, 강한 태그를 지정하면 그 버전의 바이트를 정확히 식별해야 합니다. 이 규칙은 [RFC 9110의 조건부 요청과 Range](https://www.rfc-editor.org/rfc/rfc9110.html#section-13.2)를 따릅니다.

파일 작업은 읽기와 용량 대기 동안 worker 하나를 점유하며 메모리에 전체 파일을 적재하지 않습니다. 조각 버퍼와 보관하는 파일 경로·응답 헤더를 실행기 예산에 포함합니다. 파일 읽기 자체를 강제 중단하지는 않습니다. 제출 거절은 응답을 건드리지 않으며 수락 후 대기 중 취소는 응답도 중단합니다. 파일 helper가 응답 쓰기와 완료를 맡는 동안 다른 생산자가 같은 작성기에 쓰면 안 됩니다.

## 요청 본문 스트리밍

`RegisterStreamingRoute`·`RegisterStreamingRoutePattern`은 기존 `AsyncHandler` 형식을 사용하되, 헤더가 검증되면 본문 수신 완료 전에 문맥을 전달합니다. `request.body`는 비어 있고 `context->body`가 `HttpRequestBody`를 소유합니다. 일반·async 라우트의 기존 버퍼링 동작은 유지됩니다.

`body->Read()`는 소유한 `shared_ptr<const vector<byte>>` 청크, 성공한 빈 포인터(정상 EOF), 또는 `WouldBlock`을 반환합니다. `WouldBlock`에는 기다리는 동안 다른 작업을 실행하고 외부 이벤트 루프에서 재시도합니다. 진행 없는 즉시 반복은 피합니다. 반환된 청크를 보관하는 동안에도 바이트 예산을 유지하므로, 오래 보관하면 다음 입력이 정지됩니다. 청크를 해제하면 유지관리 주기 내에 수신을 재개합니다. 문자열 또는 별도 벡터로 복사한 데이터의 예산은 애플리케이션이 관리합니다.

`maxRequestBodyBufferBytes` 기본값은 64 KiB이며 큐와 이미 꺼낸 청크를 합친 본문 용량입니다. 조각 하나는 `maxStreamChunkBytes`도 넘지 않습니다. 전체 업로드 상한 `maxStreamedBodyBytes`는 기본 1 GiB이며 기존 버퍼링의 `maxBodyBytes`와 구분합니다. 독립적인 수신·파이프라인 버퍼와 OS 소켓 버퍼는 별도입니다. 본문 reader와 청크가 살아 있는 동안 전체 본문 용량을 서버 요청 바이트 예산에 예약합니다.

본문 길이와 chunked 구문, 요청 수신의 절대 기한을 검증합니다. `Cancel()`은 미완료 업로드와 해당 연결을 취소하고 reader·응답 토큰에 알립니다. 정상 EOF 후에는 무효 동작이며, 이미 받은 청크는 서버 종료 후에도 읽을 수 있습니다. 요청 본문을 끝까지 읽기 전에 응답을 완료하면 남은 업로드를 다음 요청으로 오인하지 않도록 연결을 닫습니다. pipelining을 유지하려면 먼저 본문 EOF까지 소비합니다.

## 요청 정책과 비동기 WebSocket 승인

시작 전 `SetRequestPolicy(policy)`로 공통 정책 하나를 설치합니다. 정책 안에서 인증·권한·Origin·CORS 규칙을 순서대로 조합할 수 있습니다. `HttpPolicyContext`는 요청과 `webSocketUpgrade` 구분, 소유한 `HttpRequestDecision`을 제공합니다. HTTP 경로 매개변수도 이미 선택된 상태입니다. 일반 라우트에서는 제한된 본문을 수신한 뒤, streaming 라우트에서는 본문을 버퍼링하지 않고 정책을 실행합니다.

`decision->Allow(responseHeaders, attributes)`는 라우트 실행을 허용합니다. 응답 헤더는 기본값이며 실제 핸들러가 같은 이름을 제공하면 핸들러 값이 우선합니다. CORS preflight에 직접 응답하려면 OPTIONS 라우트를 등록하고 정책에서 `Reject`로 완성 응답을 반환할 수 있습니다. `attributes`는 대소문자를 구별하는 애플리케이션 메타데이터이며 허용된 HTTP `HttpRequestContext::attributes`로 전달합니다. 자동 로그인·토큰 해석이나 허용 Origin 기본값은 없습니다.

`decision->Reject(HttpResponse)`는 JSON 오류 등 애플리케이션의 표준 응답을 보내고 연결을 닫습니다. 정책은 본문·송신 framing을 직접 바꾸지 못하며, 정책 결정은 한 번만 성공합니다. `Abort()`는 보류 중 결정을 취소합니다. 정책 출력은 헤더·속성 합계 128개와 `maxHeaderBytes`, 거절 본문은 `maxResponseBodyBytes`로 제한합니다. 대기 정책은 최악의 거절 응답 저장소까지 `maxTotalRequestBytes`에 예약합니다. 요청·정책 문맥을 오래 보관하면 그 예약도 유지됩니다.

정책 콜백은 기존 제한된 handler pool에서 실행하며 결정 객체만 보관하고 반환할 수 있습니다. 따라서 외부 인증 응답을 기다리며 worker를 점유할 필요가 없습니다. `handlerTimeout`은 최초 정책 단계부터의 절대 기한이며 새 입력이나 후속 정책 단계가 연장하지 않습니다. 연결 종료·서버 종료·시간 초과는 결정의 취소 토큰으로 전달됩니다.

`WebSocketCallbacks::authorize`는 같은 정책 API를 해당 WS 라우트의 101 전 단계에 적용합니다. 서버 공통 정책, 라우트 authorize, 기존 동기 accept 순서로 모두 허용되어야 101을 보냅니다. 보류 중에는 WebSocket 연결이 아직 열리지 않습니다. 거절은 정상 HTTP 오류 응답입니다. WS 승인 속성은 열림 이벤트에 자동 전달하지 않으므로, 필요한 앱 상태는 애플리케이션에서 명시적으로 연결합니다. C ABI의 정책 이벤트와 Rust 래퍼도 이 소유권 계약을 따릅니다.

## WebSocket 계약

- HTTP Upgrade를 통한 RFC 6455 버전 13 연결, 텍스트·바이너리 메시지, 수신 메시지 분할·재조립, ping/pong과 close handshake를 제공합니다.
- 클라이언트의 마스킹, 프레임 길이·opcode·제어 프레임 규칙, 텍스트의 UTF-8과 종료 코드를 검증합니다. 프레임 크기와 조립된 메시지 크기를 별도로 제한합니다.
- `SendText`, `SendBinary`, `Ping`, `Close`는 스레드 안전합니다. 송신 성공은 로컬 큐가 수락했다는 의미이며 상대의 수신 확인이 아닙니다. 큐 상한에 도달하면 `WouldBlock`을 처리해야 합니다.
- 종료가 끝난 연결은 입력 검증보다 먼저 `Closed`를 반환합니다. 열린 연결에서는 크기 상한을 먼저 검사해 `TooLarge`를 반환하고, 그 안에 있는 잘못된 호출자 텍스트·종료 사유 UTF-8은 `InvalidArgument`로 거절합니다. 이전의 `InvalidFormat` 반환은 수신 형식 오류와 호출자 인수 오류를 구분하는 공통 계약에 맞게 수정되었습니다. 이 송신 오류를 검사하는 코드는 `InvalidArgument`로 분기해야 합니다. 수신한 잘못된 UTF-8의 WebSocket 종료 코드는 계속 `1007`입니다. 데이터 송신 한 번에는 프레임 상한이 적용되며, ping은 125바이트, 종료 사유는 123바이트가 상한입니다.
- `onMessage`의 바이트는 콜백 동안만 유효합니다. 나중에 사용할 데이터는 복사합니다. 연결 객체의 `shared_ptr`는 보관할 수 있으며, 종료된 연결에 대한 송신은 실패합니다.
- `accept`에서 Origin, 로그인 자격과 접근 권한을 검사할 수 있습니다. 기본은 올바른 handshake를 수락하며, 자동 로그인 검증이나 Origin 정책은 없습니다.
- 연결 열림 알림은 기존 `onOpen(connection)` 또는 선택적 `onOpenWithRequest(connection, request)` 중 하나를 사용합니다. 둘을 함께 지정하면 정확 일치·패턴 등록 모두 `InvalidArgument`입니다. 새 콜백의 요청과 그 안에서 얻은 view는 콜백 동안만 빌려 쓰며, 나중에 필요한 값은 복사합니다. 두 콜백의 실행 문맥은 같고 예외가 발생하면 해당 연결을 종료합니다. 기존 `onOpen`은 deprecated가 아닙니다.
- WebSocket 압축 확장과 subprotocol 협상은 제공하지 않습니다.

## 실행과 수명

정확 일치·패턴 라우트 모두 `Start()` 전에 등록하며 실행 중 추가·교체·삭제나 hot reload를 지원하지 않습니다. 모든 HTTP 처리기는 서버 소유의 제한된 TaskExecutor에서 실행됩니다. 기존 `RegisterRoute`·`RegisterRoutePattern`도 같은 비동기 응답 경로에 어댑터로 연결하며 반환한 `HttpResponse`를 완료합니다. 처리기 풀의 작업 수·바이트 한도나 활성 요청 한도를 넘으면 503과 연결 종료로 거절합니다. 요청 하나의 응답이 끝날 때까지 같은 연결의 다음 요청을 실행하지 않아 pipelining 순서를 지킵니다.

이미 버퍼에 쌓인 다음 요청의 진행과 시간 제한 확인은 25ms 주기의 유지관리에서도 수행합니다. 응답 완료와 다음 처리기 호출 사이에 그만큼의 스케줄링 지연이 생길 수 있습니다. 별도 연결의 요청은 이 순서를 기다리지 않습니다.

WebSocket 콜백은 전송 또는 유지관리 스레드에서 같은 연결마다 직렬 실행됩니다. 다른 연결과 HTTP worker는 병렬이므로 공유 상태를 동기화해야 합니다. WebSocket 콜백도 빠르게 반환합니다. HTTP 처리기를 I/O 스레드라고 가정하던 thread-local·스레드 친화적 코드는 이전해야 합니다.

`Stop()`은 수락을 중단하고 연결·콜백·전송 작업을 정리한 뒤 반환합니다. 콜백에서 직접 호출하면 `InvalidArgument`이며, 서버 객체도 콜백 밖의 제어 스레드에서 파괴해야 합니다. 시작 실패 또는 종료 후 재시작하려면 새 `HttpServer`를 생성합니다.

`BeginDrain()`은 새 연결과 새 HTTP 요청을 막고 이미 수락한 요청·정책·응답을 마무리하도록 둡니다. 유휴 HTTP 연결은 닫고 WebSocket에는 1001 종료 handshake를 시작합니다. 제어 스레드에서 호출하며 콜백·I/O 스레드 호출은 `InvalidArgument`입니다. 반복 호출은 안전합니다. `DrainStatus()`는 연결이 남았으면 `WouldBlock`, 모두 정리됐으면 `Ok`입니다. 시작 전에는 `Closed`입니다. `StopGracefully(steady_clock::time_point)`는 제어 스레드에서 마감까지 기다린 뒤 정리합니다. 기한 내 완료는 `Ok`, 남은 연결을 강제로 취소해야 했다면 `Timeout`입니다. 이후에도 스레드 합류는 필요하므로 취소를 무시하는 애플리케이션 콜백의 종료 시간을 강제 보장하지 않습니다. 즉시 종료가 필요하면 기존 `Stop()`을 사용합니다.

`SetLogger(shared_ptr<Core::ILogger>)`는 시작 전에 인스턴스 로거를 지정합니다. 리스너와 HTTP 프로토콜 거절에 사용하며 헤더·본문·인증 정보를 자동 기록하지 않습니다. 설정하지 않으면 전역 로거로 우회하지 않습니다. I/O 지연을 피하려면 [비동기 운영 로거](OPERATIONS.md)를 사용합니다.

기본 상한은 연결 256개, 요청 헤더 16 KiB, 요청 본문 64 KiB, 완성 응답 본문 256 KiB, WebSocket 프레임 64 KiB와 조립 메시지 256 KiB입니다. HTTP worker는 2개, 대기 처리기는 128개, 선언된 처리기 보유 예산은 8 MiB, 활성 HTTP 요청은 256개, 소유 요청 예산은 16 MiB입니다. 요청 바이트 예산은 전달 문맥의 수명에 연결되며 allocator·애플리케이션의 별도 사본까지 측정하지 않습니다. 처리기 실행 시간은 강제 제한하지 않으므로 취소를 무시하는 처리기는 Stop을 지연할 수 있습니다.

응답 대기 중 수신은 EOF 감지를 위해 유지하되 파이프라인 버퍼가 `maxPipelinedBytes`(기본 64 KiB)에 가까워지면 정지합니다. 이미 진행 중인 수신 한 개의 여유를 남기며 넘치는 입력은 연결을 닫습니다. 수신이 정지된 경우 EOF 감지가 늦어질 수 있어 마감 시간도 적용합니다.

요청 수신 기한은 30초, 응답 시작 전 처리 기한 `handlerTimeout`은 30초, 응답 진행 중 쓰기 유휴 `streamIdleTimeout`은 120초, 송신 큐가 빠져나가지 않는 `sendStallTimeout`은 30초입니다. 활성 스트림에는 일반 수신 유휴 기한을 적용하지 않습니다. 응답 후 일반 연결 유휴는 120초, 연결을 닫는 응답의 송신 대기는 5초(`responseDrainTimeout`), WebSocket 종료 기한은 3초입니다. 응답 종료 대기와 WebSocket 종료 기한은 추가 입력으로 연장되지 않습니다.

전송 계층의 연결별 송신 payload 상한은 1 MiB입니다. `maxTotalSendQueueCapacityBytes`는 HTTP·WebSocket 전체 연결이 공유하는 보유 송신 payload 예산이며 기본 256 MiB, 유효 범위는 1바이트~512 MiB입니다. 부분 송신 벡터의 이미 보낸 앞부분도 저장소를 해제할 때까지 포함합니다. 완성 응답 어댑터나 upgrade가 큐에 넣지 못하면 연결을 닫고, 스트리밍 작성기와 WebSocket 데이터 송신은 `WouldBlock`을 반환합니다. 스트리밍 조각 기본값은 64 KiB이며 실제 `MaxWriteBytes()`는 송신 예산과 framing 여유도 반영합니다. 이는 원격 수신량·OS 소켓 메모리 상한과 다릅니다.

`GetWebSocketFlowControl(connection)`은 `RetainedSendBytes`와 `WaitForSendCapacity(payloadBytes, callback, token)`을 제공합니다. TCP와 같은 용량 알림·취소·등록 수명 계약을 공유합니다. 연결별 미완료 대기 하나만 허용하며 프레임 헤더 여유까지 예약 조건에 반영합니다. 사용자 정의 연결이 기능을 구현하지 않으면 빈 포인터를 반환합니다. 자세한 의미는 [공통 흐름 제어](EXECUTION_AND_FLOW_CONTROL.md)를 따릅니다.

## 지원 범위와 배포

서버의 HTTP/2·HTTP/3·HTTPS·WSS는 내장하지 않습니다. TLS가 필요한 배포에서는 reverse proxy에서 TLS를 종료하고 내부 리스너로 전달합니다. 외부 HTTP·HTTPS 요청은 소비 애플리케이션이 처리합니다. 웹 서버는 [C ABI](C_ABI.md)와 [Rust 래퍼](RUST.md)로도 사용할 수 있습니다. 이 계층에는 인증·DB·정적 파일 디렉터리 노출을 자동으로 추가하지 않습니다.

프로토콜 기준은 [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html), [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html)와 [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html)입니다. 구현에 대한 검증 범위와 실제 실행 환경은 [VALIDATION.md](VALIDATION.md)를 참고합니다.
