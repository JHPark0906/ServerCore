use servercore::{block_on, net, web, Error, Header};
use std::{
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    thread,
    time::Duration,
};

fn port() -> u16 {
    TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port()
}
fn connect(port: u16) -> TcpStream {
    let socket = TcpStream::connect(("127.0.0.1", port)).unwrap();
    socket
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    socket
        .set_write_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    socket
}
fn read_head(socket: &mut TcpStream) -> Vec<u8> {
    let mut head = Vec::new();
    while !head.ends_with(b"\r\n\r\n") {
        let mut byte = [0];
        socket.read_exact(&mut byte).unwrap();
        head.push(byte[0]);
        assert!(head.len() <= 16384);
    }
    head
}
fn assert_send<T: Send>(value: T) -> T {
    value
}

#[test]
fn policy_authenticates_before_websocket_101_and_passes_http_attributes() {
    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.enable_policy().unwrap();
    server.websocket_policy_pattern("/private/{id}").unwrap();
    server.route("GET", "/plain").unwrap();
    server.start().unwrap();
    let mut socket = connect(server.port());
    socket.write_all(b"GET /private/42 HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n").unwrap();
    let mut event = block_on(server.next()).unwrap();
    assert_eq!(event.kind().unwrap(), web::EventKind::Policy);
    assert!(event.policy_is_websocket());
    let global = event.decision().unwrap();
    drop(event);
    global.allow(&[], &[]).unwrap();
    let mut event = block_on(server.next()).unwrap();
    assert_eq!(event.request().unwrap().parameter("id"), Some("42"));
    let decision = event.decision().unwrap();
    drop(event);
    socket
        .set_read_timeout(Some(Duration::from_millis(100)))
        .unwrap();
    let mut byte = [0];
    let pending = socket.read(&mut byte).unwrap_err();
    assert!(matches!(
        pending.kind(),
        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
    ));
    socket
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    decision
        .reject(
            &web::ResponseHead {
                status: 403,
                close: true,
                ..Default::default()
            },
            b"denied",
        )
        .unwrap();
    assert_eq!(decision.allow(&[], &[]), Err(Error::CLOSED));
    let mut wire = Vec::new();
    socket.read_to_end(&mut wire).unwrap();
    assert!(wire.starts_with(b"HTTP/1.1 403") && wire.ends_with(b"denied"));
    assert!(!wire.windows(3).any(|s| s == b"101"));

    let mut peer = connect(server.port());
    peer.write_all(b"GET /plain HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n")
        .unwrap();
    let mut event = block_on(server.next()).unwrap();
    let decision = event.decision().unwrap();
    drop(event);
    decision
        .allow(
            &[Header::new("X-Policy", "ok")],
            &[Header::new("UserId", "42")],
        )
        .unwrap();
    let mut event = block_on(server.next()).unwrap();
    assert_eq!(
        event.attributes().unwrap().get_exact("UserId"),
        Some(b"42".as_slice())
    );
    assert_eq!(event.attributes().unwrap().get_exact("userid"), None);
    let response = event.response().unwrap();
    drop(event);
    response
        .complete(
            &web::ResponseHead {
                close: true,
                ..Default::default()
            },
            b"ok",
        )
        .unwrap();
    let mut wire = String::new();
    peer.read_to_string(&mut wire).unwrap();
    assert!(wire.contains("X-Policy: ok\r\n") && wire.ends_with("ok"));
}

#[test]
fn streamed_upload_chunks_retain_capacity_and_outlive_server() {
    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        max_chunk_bytes: 4,
        max_body_bytes: 64,
        ..Default::default()
    })
    .unwrap();
    // Each held chunk is also charged a fixed bookkeeping cost against
    // max_buffered_bytes (WEB-4): 160 bytes at 0.3.0, the native
    // RequestBodyState::PieceOverheadBytes, which the C ABI does not expose.
    // 8 + 2 * 160 admits two held 4-byte chunks and holds back the third.
    const PIECE_OVERHEAD_BYTES: usize = 160;
    server
        .set_body_limits(&web::BodyOptions {
            max_buffered_bytes: 8 + 2 * PIECE_OVERHEAD_BYTES,
            max_body_bytes: 64,
        })
        .unwrap();
    server.stream_route_pattern("POST", "/upload/{id}").unwrap();
    server.start().unwrap();
    let mut socket = connect(server.port());
    socket.write_all(b"POST /upload/file HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n10\r\nabcdefghijklmnop\r\n0\r\n\r\n").unwrap();
    let mut event = block_on(server.next()).unwrap();
    assert!(event.request().unwrap().body.is_empty());
    let response = event.response().unwrap();
    let mut body = event.body().unwrap();
    drop(event);
    let first = block_on(assert_send(body.next())).unwrap().unwrap();
    let second = block_on(body.next()).unwrap().unwrap();
    assert_eq!(first.bytes(), b"abcd");
    assert_eq!(second.bytes(), b"efgh");
    assert_eq!(body.retained_bytes(), 8);
    assert!(matches!(body.try_read(), Err(Error::WOULD_BLOCK)));
    drop(first);
    drop(second);
    let third = block_on(body.next()).unwrap().unwrap();
    assert_eq!(third.bytes(), b"ijkl");
    drop(third);
    let last = block_on(body.next()).unwrap().unwrap();
    assert_eq!(last.bytes(), b"mnop");
    assert!(block_on(body.next()).unwrap().is_none());
    drop(body);
    response
        .complete(
            &web::ResponseHead {
                close: true,
                ..Default::default()
            },
            b"uploaded",
        )
        .unwrap();
    let mut wire = Vec::new();
    socket.read_to_end(&mut wire).unwrap();
    assert!(wire.ends_with(b"uploaded"));
    drop(server);
    assert_eq!(last.bytes(), b"mnop");

    // Dropping a live reader must cancel even when the response is still owned.
    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.stream_route("POST", "/incomplete").unwrap();
    server.start().unwrap();
    let mut peer = connect(server.port());
    peer.write_all(b"POST /incomplete HTTP/1.1\r\nHost: test\r\nContent-Length: 12\r\n\r\n")
        .unwrap();
    let mut event = block_on(server.next()).unwrap();
    let response = event.response().unwrap();
    let body = event.body().unwrap();
    drop(event);
    drop(body);
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    while !response.is_cancelled() && std::time::Instant::now() < deadline {
        thread::sleep(Duration::from_millis(1));
    }
    assert!(response.is_cancelled());
}

#[test]
fn graceful_drain_metrics_and_instance_logger() {
    use servercore::observability::{LogLevel, Logger, LoggerOptions};
    let logger = Logger::new(&LoggerOptions {
        max_message_bytes: 16,
        ..Default::default()
    })
    .unwrap();
    logger.try_write(LogLevel::Debug, b"filtered").unwrap();
    logger.try_write(LogLevel::Info, b"Rust logger").unwrap();
    assert_eq!(
        logger.try_write(LogLevel::Info, b"01234567890123456"),
        Err(Error::TOO_LARGE)
    );
    logger.stop().unwrap();
    let logs = logger.metrics().unwrap();
    assert_eq!(logs.filtered_messages, 1);
    assert_eq!(logs.written_messages, 1);
    assert_eq!(logs.retained_bytes, 0);
    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.set_logger(&logger).unwrap();
    drop(logger);
    server.route("GET", "/drain").unwrap();
    server.start().unwrap();
    let mut socket = connect(server.port());
    socket
        .write_all(b"GET /drain HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n")
        .unwrap();
    let mut event = block_on(server.next()).unwrap();
    let response = event.response().unwrap();
    drop(event);
    server.begin_drain().unwrap();
    assert_eq!(server.drain_status(), Err(Error::WOULD_BLOCK));
    response
        .complete(
            &web::ResponseHead {
                close: true,
                ..Default::default()
            },
            b"done",
        )
        .unwrap();
    let mut wire = Vec::new();
    socket.read_to_end(&mut wire).unwrap();
    assert!(wire.ends_with(b"done"));
    // After closing its send side the server reads until the peer closes
    // (NET-2 lingering close); the connection, and the drain, end only then.
    drop(socket);
    block_on(assert_send(server.drain(Duration::from_secs(5)))).unwrap();
    let metrics = server.metrics().unwrap();
    assert_eq!(metrics.accepted_requests, 1);
    assert_eq!(metrics.completed_requests, 1);
    assert_eq!(metrics.latency_buckets.iter().sum::<u64>(), 1);
    let export = server.metrics_prometheus().unwrap();
    drop(server);
    assert!(export.as_str().unwrap().contains("servercore_http_"));

    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.route("GET", "/timeout").unwrap();
    server.start().unwrap();
    let mut socket = connect(server.port());
    socket
        .write_all(b"GET /timeout HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n")
        .unwrap();
    let mut event = block_on(server.next()).unwrap();
    let response = event.response().unwrap();
    drop(event);
    assert_eq!(block_on(server.drain(Duration::ZERO)), Err(Error::TIMEOUT));
    assert!(response.is_cancelled());
}

#[test]
fn http_owned_request_streaming_and_async_send() {
    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        max_send_bytes: 16 * 1024,
        max_chunk_bytes: 1024,
        ..Default::default()
    })
    .unwrap();
    server.route_pattern("POST", "/echo/{id}").unwrap();
    server.start().unwrap();
    let endpoint = server.port();
    let peer = thread::spawn(move || {
        let mut socket = connect(endpoint);
        socket.write_all(b"POST /echo/%ED%95%9C?raw=%2f HTTP/1.1\r\nHost: test\r\nX-Owned: retained\r\nContent-Length: 3\r\nConnection: close\r\n\r\na\0b").unwrap();
        let mut response = Vec::new();
        socket.read_to_end(&mut response).unwrap();
        response
    });
    let mut event = block_on(assert_send(server.next())).unwrap();
    assert_eq!(event.kind().unwrap(), web::EventKind::Request);
    let response = event.response().unwrap();
    let request = event.request().unwrap();
    assert_eq!(request.parameter("id"), Some("한"));
    assert_eq!(request.headers.get("x-owned"), Some(b"retained".as_slice()));
    assert_eq!(request.body, b"a\0b");
    let body = vec![b'x'; 70 * 1024];
    block_on(assert_send(async {
        response
            .start_async(&web::ResponseHead {
                content_length: Some(body.len() as u64),
                close: true,
                headers: vec![Header::new("X-Route", request.parameter("id").unwrap())],
                ..Default::default()
            })
            .await
            .unwrap();
        response.write_all(&body).await.unwrap();
        response.finish_async().await.unwrap();
    }));
    let wire = peer.join().unwrap();
    let split = wire
        .windows(4)
        .position(|part| part == b"\r\n\r\n")
        .unwrap()
        + 4;
    assert!(wire.starts_with(b"HTTP/1.1 200"));
    assert_eq!(&wire[split..], body.as_slice());
    drop(server);
    // Returned immutable events survive native shutdown and still own their
    // request bytes. No request copy is required merely to cross an await.
    assert_eq!(event.request().unwrap().body, b"a\0b");
}

#[test]
fn abandoned_request_and_cancelled_future_release_ownership() {
    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.route("GET", "/").unwrap();
    server.start().unwrap();
    // Poll once to register a Waker, then cancel only the Future. The server
    // must remain usable and no worker may later access this dropped Future.
    let mut future = Box::pin(server.next());
    struct Wake;
    impl std::task::Wake for Wake {
        fn wake(self: std::sync::Arc<Self>) {}
    }
    let waker = std::task::Waker::from(std::sync::Arc::new(Wake));
    assert!(std::future::Future::poll(
        future.as_mut(),
        &mut std::task::Context::from_waker(&waker)
    )
    .is_pending());
    drop(future);
    let mut socket = connect(server.port());
    socket
        .write_all(b"GET / HTTP/1.1\r\nHost: test\r\n\r\n")
        .unwrap();
    let event = server.next_timeout(Duration::from_secs(3)).unwrap();
    assert_eq!(event.request().unwrap().target, "/");
    drop(event); // Unclaimed response is aborted.
    let mut byte = [0];
    match socket.read(&mut byte) {
        Ok(0) => {}
        Err(error) if error.kind() == std::io::ErrorKind::ConnectionReset => {}
        other => panic!("not closed: {other:?}"),
    }
    let mut pending_peer = connect(server.port());
    pending_peer
        .write_all(b"GET / HTTP/1.1\r\nHost: test\r\n\r\n")
        .unwrap();
    let mut pending_event = server.next_timeout(Duration::from_secs(3)).unwrap();
    let response = pending_event.response().unwrap();
    let mut cancellation = Box::pin(assert_send(response.cancelled()));
    assert!(std::future::Future::poll(
        cancellation.as_mut(),
        &mut std::task::Context::from_waker(&waker)
    )
    .is_pending());
    drop(pending_peer);
    block_on(cancellation).unwrap();
    assert!(response.is_cancelled());
    server.stop().unwrap();
    assert!(matches!(
        server.next_timeout(Duration::ZERO),
        Err(Error::CLOSED)
    ));
}

#[test]
fn websocket_binary_event_outlives_socket_and_server() {
    let mut server = web::HttpServer::new(&web::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.websocket_pattern("/rooms/{id}").unwrap();
    server.start().unwrap();
    let mut peer = connect(server.port());
    peer.write_all(b"GET /rooms/lobby HTTP/1.1\r\nHost: test\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n").unwrap();
    let head = read_head(&mut peer);
    assert!(head.starts_with(b"HTTP/1.1 101"));
    let mut upgrade = block_on(server.next()).unwrap();
    assert_eq!(upgrade.request().unwrap().parameter("id"), Some("lobby"));
    let mut socket = upgrade.socket().unwrap();
    drop(upgrade);
    // Final binary frame carrying [0, 255, 3], with mandatory client masking.
    peer.write_all(&[0x82, 0x83, 1, 2, 3, 4, 1, 253, 0])
        .unwrap();
    let event = block_on(assert_send(socket.next())).unwrap();
    assert_eq!(event.kind().unwrap(), web::WebSocketKind::Binary);
    assert_eq!(event.bytes(), &[0, 255, 3]);
    block_on(assert_send(socket.send_binary_async(event.bytes()))).unwrap();
    let mut echoed = [0; 5];
    peer.read_exact(&mut echoed).unwrap();
    assert_eq!(echoed, [0x82, 3, 0, 255, 3]);
    drop(socket);
    server.stop().unwrap();
    drop(server);
    assert_eq!(event.bytes(), &[0, 255, 3]);
}

#[test]
fn raw_tcp_preserves_binary_bytes_and_reports_close() {
    let mut server = net::TcpServer::new(&net::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.start().unwrap();
    let mut peer = connect(server.port());
    let mut connection = block_on(assert_send(server.accept())).unwrap();
    connection.pause_receive().unwrap();
    connection.resume_receive().unwrap();
    let original = [0, 255, 42, 0, 3];
    peer.write_all(&original).unwrap();
    let mut received = Vec::new();
    let mut held = Vec::new();
    while received.len() < original.len() {
        let event = block_on(assert_send(connection.next())).unwrap();
        assert_eq!(event.kind().unwrap(), net::EventKind::Bytes);
        received.extend_from_slice(event.bytes());
        held.push(event);
    }
    assert_eq!(received, original);
    block_on(assert_send(connection.send_async(&received))).unwrap();
    let mut echoed = [0; 5];
    peer.read_exact(&mut echoed).unwrap();
    assert_eq!(echoed, original);
    drop(peer);
    let close = connection.next_timeout(Duration::from_secs(3)).unwrap();
    assert_eq!(close.kind().unwrap(), net::EventKind::Closed);
    assert!(matches!(
        connection.next_timeout(Duration::ZERO),
        Err(Error::CLOSED)
    ));
    drop(connection);
    drop(server);
    assert_eq!(
        held.iter()
            .flat_map(|event| event.bytes())
            .copied()
            .collect::<Vec<_>>(),
        original
    );
}

#[test]
fn cancelled_capacity_wait_and_invalid_options_are_safe() {
    let mut invalid = net::Options::default();
    invalid.max_connections = 0;
    assert!(matches!(
        net::TcpServer::new(&invalid),
        Err(Error::INVALID_ARGUMENT)
    ));
    let mut server = net::TcpServer::new(&net::Options {
        port: port(),
        ..Default::default()
    })
    .unwrap();
    server.start().unwrap();
    let peer = connect(server.port());
    let connection = server.accept_timeout(Duration::from_secs(3)).unwrap();
    let wait = connection.wait_capacity(1).unwrap();
    // Immediate readiness is allowed; cancel after readiness remains safe.
    wait.cancel();
    drop(wait);
    drop(peer);
    drop(connection);
    server.stop().unwrap();
}
