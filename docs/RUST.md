# Rust bindings

`rust/` contains two local crates with no crates.io dependencies:

- `servercore-sys`: handwritten ABI v1 declarations matching `include/ServerCore/C/`.
- `servercore`: owned HTTP server, request/response streams, policy decisions, WebSocket, accepted TCP connection, logger, and server metrics handles. Async methods implement standard Rust `Future`; they work with Tokio and other executors without requiring one as a dependency.

This is a focused C boundary for web and protocol-neutral TCP applications. It does not expose every C++ class: `ServerHost`, session registries, the JSON dispatcher, UDP, and the C++ file-task helper are not mapped. TCP bytes carry no implicit length prefix or JSON encoding. Applications choose their protocol and executor.

## Build

Use Rust 1.75 or newer, CMake 3.21 or newer, and the same supported native compiler/toolchain as the C++ library. The checked-in workspace lockfile needs no network dependency resolution.

From `rust/`:

```sh
cargo test --workspace --offline
cargo run -p servercore --example http_echo -- 8080
```

When `SERVERCORE_CABI_DIR` is absent, `servercore-sys/build.rs` builds the repository's C ABI with CMake in Cargo's build output, using `SERVERCORE_BUILD_C_API=ON`, a shared library, and native **Release** configuration even for Cargo's debug profile. It installs only the `CAbi` component. This requires the repository source checkout; a separately copied crate must use a prebuilt library. Cargo artifacts are directed into the repository's ignored `build/rust/` directory when running from `rust/`.

For an existing native build or installation, set `SERVERCORE_CABI_DIR` to the directory containing `ServerCoreCAbi.lib` on Windows or `libServerCoreCAbi.so` on Linux. A Windows DLL must be beside the import library or in the sibling `bin/` directory. The build script stages that DLL inside its own `OUT_DIR`; Cargo supplies that runtime path for `cargo run` and `cargo test`. To run a deployed Windows executable directly, place the DLL beside it or provide its directory in `PATH`. Linux consumers of prebuilt shared libraries must provide the loader path, for example:

```sh
export SERVERCORE_CABI_DIR=/absolute/path/to/servercore/lib
export LD_LIBRARY_PATH="$SERVERCORE_CABI_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd rust
cargo test --workspace --offline
```

Deploy the shared library with the application using the platform's normal loader rules. The C API copies inputs and pairs every native allocation with a native destroy function; C++ containers, exceptions, and allocator ownership do not cross into Rust. An ABI version mismatch returns `Unimplemented`. The C struct version/size checks protect handwritten layout compatibility; they do not make an arbitrary incompatible native binary safe.

ServerCore supplies server-side networking using the standard libraries, compiler runtime and OS facilities. Outbound HTTP, when needed by a backend, belongs to the consuming application and does not introduce a dependency into these crates. See the [dependency policy](DEPENDENCIES.md).

The former `servercore::client` and `servercore-sys::client` APIs, `http-client` feature and `http_client` example have been removed. Remove those feature requests/imports and migrate outbound requests to the application's HTTP implementation. The matching C/C++ client APIs are also removed. The remaining C ABI stays at version 1 with unchanged layouts and numeric values; former client capability value `4` is reserved and never reported by the new library. Binaries linked to removed client symbols must be migrated and rebuilt. Replace an old client-enabled native DLL/SO as well to remove its external dependencies. See [API_MIGRATION.md](API_MIGRATION.md).

Additional build environment variables:

| Variable | Meaning |
| --- | --- |
| `SERVERCORE_CMAKE_GENERATOR` | Explicit CMake generator for source builds. |
| `SERVERCORE_CMAKE_TOOLCHAIN` | Required toolchain file for source cross compilation. |
| `SERVERCORE_CMAKE_JOBS` | Native build parallelism; defaults to 2. |
| `CMAKE`, `CXX`, `CMAKE_PREFIX_PATH` | CMake executable, compiler, and CMake search prefixes. |
| `SERVERCORE_CABI_STATIC=1` | Link a static C ABI, or build one from source. |
| `SERVERCORE_NATIVE_LIBS` | Required ordered, semicolon-separated transitive link libraries for static linking, e.g. `static=ServerCore;dylib=stdc++`. This list depends on the native compiler and platform. |
| `SERVERCORE_NATIVE_SEARCH` | Additional semicolon-separated native library directories. |

For Linux cross compilation, install the matching Rust standard-library target and configure Cargo's target linker separately. A CMake toolchain does not configure Rust's linker. The local validation setup uses `x86_64-unknown-linux-musl`, a matching C++ musl toolchain, and static libraries inside a Linux VM. Static linkage must supply the core, C++ runtime and platform libraries built for that exact target. Do not link host Windows libraries into a Linux target.

## HTTP and streaming

```rust,no_run
use servercore::{web::{HttpServer, Options, ResponseHead}, Result};

async fn run() -> Result<()> {
    let mut server = HttpServer::new(&Options { port: 8080, ..Default::default() })?;
    server.route_pattern("POST", "/plugins/{id}")?;
    server.start()?;
    loop {
        let mut event = server.next().await?;
        let response = event.response()?;
        let request = event.request()?;
        let body = request.body;
        response.start_async(&ResponseHead {
            content_length: Some(body.len() as u64),
            ..Default::default()
        }).await?;
        response.write_all(body).await?;
        response.finish_async().await?;
    }
}
```

Registration occurs before `start()`. Exact paths and explicit `{parameter}` patterns preserve the behavior documented in [WEB.md](WEB.md), including method selection, HEAD, percent decoding, and the separate WebSocket namespace. Choose a nonzero listen port. Request methods, targets, headers, decoded route parameters and body are borrowed from an **owned event**. Keep that event in an async task to retain the request across suspension; copy only data that must outlive it. The returned response is an independent owning handle and may outlive the event.

`complete()` sends one bounded buffered response. `start()`/`write()`/`finish()` provide streaming; omitting `content_length` uses chunked framing where a response body is allowed. `write_all()` splits data into the server's maximum chunk size and waits for capacity. A successful write means locally queued. Concurrent producers must serialize their writes when order matters, and each response supports one pending capacity registration.

SSE uses the same streaming response with `Content-Type: text/event-stream`; write UTF-8 event records and finish or abort deliberately. The C++ SSE encoder and file helper are not duplicated as a Rust API. For files, read bounded chunks using the application's async file facilities or bounded blocking pool, then pass them to `write_all()`. Filesystem reads do not become asynchronous merely because the response writer is async.

For uploads, register `stream_route()` or `stream_route_pattern()` before startup. These dispatch headers with an empty `request.body`; claim **both** `event.body()` and `event.response()` before dropping the event. `Body::next().await` returns an owned `BodyChunk`, `None` at clean EOF, or a terminal error. Fixed-length and chunked requests share this API. A popped chunk still consumes native receive capacity until Drop, so retaining chunks applies backpressure rather than allocating without bound. Use `set_body_limits()` to bound buffered upload bytes and total body size; the total must be at least `Options::max_body_bytes`, the buffered-route limit. `Options::max_chunk_bytes` also caps received chunk sizes.

```rust,no_run
async fn consume_upload(mut event: servercore::web::Event) -> servercore::Result<()> {
    let response = event.response()?;
    let mut body = event.body()?;
    drop(event);
    let mut count = 0_u64;
    while let Some(chunk) = body.next().await? {
        count += chunk.bytes().len() as u64;
        // Process or write this bounded slice before dropping the chunk.
    }
    response.complete(&Default::default(), count.to_string().as_bytes())
}
```

Dropping the final reader before the parser reaches clean EOF aborts the upload and response. Dropping after parser EOF is harmless, including when unread final chunks remain queued. An already-returned chunk remains readable after reader/server shutdown.

## Middleware and WebSocket authorization

`enable_policy()` registers an async gate for matched routes. `websocket_policy()` and `websocket_policy_pattern()` register an upgrade route with an additional gate. The server emits `EventKind::Policy` before dispatching the selected HTTP handler and before sending a WebSocket `101`. Streaming routes gate at headers; ordinary buffered routes already have their bounded body. Protocol errors, unmatched paths and unsupported methods are rejected before policy. Inspect `event.request()` and `event.policy_is_websocket()`, then claim `event.decision()` before dropping the event. Authorization may await an application service on the application's executor; native `handler_timeout` and disconnect cancellation still apply.

```rust,no_run
async fn authorize(mut event: servercore::web::Event, permitted: bool)
    -> servercore::Result<()> {
    use servercore::{Header, web::ResponseHead};
    let decision = event.decision()?;
    drop(event);
    if permitted {
        decision.allow(&[Header::new("X-Policy", "accepted")],
                       &[Header::new("UserId", "42")])
    } else {
        decision.reject(&ResponseHead { status: 403, close: true,
                                       ..Default::default() }, b"Forbidden")
    }
}
```

The application determines `permitted` using its credentials, Origin rules and permissions. `Decision` is cloneable but single use: after allow/reject, further decisions return `Closed`; last unresolved clone Drop aborts. `cancelled().await` observes timeout/disconnect/abort, not successful decision completion. If global and route-specific policies are both registered, two sequential policy events must each allow before the WebSocket opens. The ordinary `WebSocket` event is still post-upgrade.

Response headers and attributes are copied and bounded by native policy limits. The admitted HTTP event exposes `attributes()`; use `get_exact("UserId")` because metadata keys are case-sensitive. Opened WebSocket events expose only the original request, not policy attributes. Native framing and handshake headers cannot be overridden by policy response headers. Policy events retain the same event budget as request/open events: release one after claiming its decision to return queue capacity before continuing admission.

## Graceful shutdown, metrics and logging

`begin_drain()` stops admission and starts WebSocket close 1001 while admitted HTTP responses finish. `drain_status()` polls for completion. `drain(Duration).await` waits through the existing readiness worker, then joins native shutdown; expiry forces remaining work to stop and returns `Timeout`. Dropping that Future leaves the server draining. `stop_gracefully(Duration)` blocks the calling thread for the wait, so use it from a control thread. Ordinary `stop()` and Drop force shutdown. All final shutdown paths join native workers; they are not hard real-time operations.

`server.metrics()` returns a fixed-field snapshot including handler-task counters and twelve noncumulative latency bins. `observability::LATENCY_UPPER_BOUNDS_NS` defines eleven finite inclusive bounds; the last bin is infinity. Fields may be sampled at different instants. `server.metrics_prometheus()` owns an immutable export with fixed names/labels and cumulative histogram buckets in seconds; keep the object alive while borrowing its text. There are no automatic per-URL, token or user labels.

`observability::Logger::new(&LoggerOptions)` starts a bounded native console/file logger. `try_write()` copies bytes, applies level filtering and reports capacity/size failures. `server.set_logger(&logger)` attaches an instance before start; the server independently retains it and no process-global logger is replaced. File names are UTF-8, and output rotation is bounded by the configured file size/count. Explicit logger `stop()` and last native owner Drop drain and join the output worker, which may block on console/filesystem I/O. Call them outside executor workers where blocking is inappropriate. `request_stop()` only signals shutdown; `metrics()` reports queue and output accounting.

## WebSocket and TCP

`websocket()` and `websocket_pattern()` register an upgrade route. A `WebSocket` event carries the original request and route parameters; claim `event.socket()` and then receive its owned message events with `socket.next().await`. Text, binary and close are distinct event kinds. `send_text_async()` and `send_binary_async()` await native send capacity. Message events remain valid after socket/server destruction and retain their queue charge until dropped.

`net::TcpServer::accept().await` returns an owned `Connection`. `next().await` yields bytes or a terminal close event. Receive chunks are not application message boundaries. `send_async()` sends the entire supplied slice or waits; split an impossible oversized send according to the application's framing. Pause/resume controls subsequent native receives; one already-posted receive may still complete. Queue overflow closes the connection instead of allowing unbounded memory growth.

Runnable examples are `http_echo`, `websocket_echo`, and `tcp_echo`. The small echo examples process one event/connection at a time; a production application can spawn a bounded number of tasks on its chosen executor.

## Lifetimes, cancellation and scheduling

- Native workers never invoke Rust handlers or callbacks. Rust pulls immutable events. Held events remain charged against native count/byte limits; copying a body into application memory is a separate application-owned allocation.
- Dropping an unclaimed HTTP event aborts its response. The last `Response` clone aborts unfinished work; the last WebSocket/TCP handle closes its connection. Server Drop stops admission, closes connections and joins native workers. Held immutable events remain readable.
- Dropping a pending `next()`/`accept()` Future only cancels that wait; its borrowed owner remains usable. Dropping a capacity wait unregisters it and waits for native notification quiescence. If `write_all()` is cancelled after a prefix was accepted, abort the response when abandoning the producer while another response clone remains alive.
- `Response::cancelled().await` resolves on abort, disconnect or shutdown. An executor can select it against an application-owned operation and cancel that operation when the frontend goes away. Normal `finish()` does not cancel user work, so this Future is not a response-completion wait and can remain pending after successful completion.
- A process-shared Rust readiness worker uses a nominal 10 ms scheduling interval while work is pending. This is not a strict latency bound: OS scheduling and a slow custom Waker can delay delivery. It never holds C handles. Live owner leases keep the worker available across sequential awaits; releasing the last lease/future stops and joins it. If a custom Waker releases the last lease on that worker itself, the worker exits after the current wake instead of joining itself. There is a limit of 4,096 concurrent readiness registrations; exhaustion returns `WouldBlock`. Custom Waker panics are contained, and Wakers are invoked outside registry locks. As with any executor, a wake already in flight may follow Future cancellation.
- `next_timeout(Duration::ZERO)` is a poll returning `WouldBlock`; a positive blocking timeout returns `Timeout`. These blocking methods occupy their caller and should not run on an async executor worker. Native timeouts continue while Rust delays polling or holds body events. `block_on()` is a small parking executor for examples, not a task scheduler.

The safe crate's `Send`/`Sync` implementations rely on the documented native concurrent-call contract. Rust borrows and owning handles prevent destruction from racing an in-flight method. They do not make concurrent application writes ordered or make application copies part of the native byte budget.
