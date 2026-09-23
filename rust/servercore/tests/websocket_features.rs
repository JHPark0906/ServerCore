use servercore::{block_on, web, Error};
use std::{
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    time::Duration,
};

fn server(options: &web::WebSocketOptions, route: &web::WebSocketRouteOptions) -> web::HttpServer {
    let port = TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    let mut server = web::HttpServer::new(&web::Options {
        port,
        ..Default::default()
    })
    .unwrap();
    server.set_websocket_options(options).unwrap();
    server.websocket_with_options("/ws", route).unwrap();
    server.start().unwrap();
    server
}
fn connect(server: &web::HttpServer) -> TcpStream {
    let mut peer = TcpStream::connect(("127.0.0.1", server.port())).unwrap();
    peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    peer.set_write_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    peer.write_all(b"GET /ws HTTP/1.1\r\nHost: local\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: first, second\r\n\r\n").unwrap();
    peer
}
fn head(peer: &mut TcpStream) -> String {
    let mut result = Vec::new();
    while !result.ends_with(b"\r\n\r\n") {
        let mut byte = [0];
        peer.read_exact(&mut byte).unwrap();
        result.push(byte[0]);
        assert!(result.len() < 16384);
    }
    String::from_utf8(result).unwrap()
}
fn frame(peer: &mut TcpStream) -> (u8, bool, Vec<u8>) {
    let mut header = [0; 2];
    peer.read_exact(&mut header).unwrap();
    assert_eq!(header[1] & 0x80, 0);
    assert!(header[1] < 126, "fixtures use bounded short frames");
    let mut bytes = vec![0; usize::from(header[1])];
    peer.read_exact(&mut bytes).unwrap();
    (header[0] & 15, header[0] & 128 != 0, bytes)
}
fn masked(peer: &mut TcpStream, opcode: u8, payload: &[u8]) {
    assert!(payload.len() < 126);
    let mask = [1, 2, 3, 4];
    let mut wire = vec![128 | opcode, 128 | payload.len() as u8];
    wire.extend_from_slice(&mask);
    wire.extend(
        payload
            .iter()
            .enumerate()
            .map(|(index, byte)| byte ^ mask[index % 4]),
    );
    peer.write_all(&wire).unwrap();
}
fn assert_send<T: Send>(value: T) -> T {
    value
}

#[test]
fn negotiated_protocol_fragment_lane_and_owned_writer() {
    let server = server(
        &web::WebSocketOptions {
            max_frame_bytes: 4,
            max_message_bytes: 6,
            ..Default::default()
        },
        &web::WebSocketRouteOptions {
            subprotocols: vec!["second".into(), "first".into()],
            authorize: true,
        },
    );
    let mut peer = connect(&server);
    let mut policy = server.next_timeout(Duration::from_secs(5)).unwrap();
    assert_eq!(policy.kind().unwrap(), web::EventKind::Policy);
    let request = policy.request().unwrap();
    assert_eq!(
        request.local_endpoint.unwrap().socket_addr(),
        peer.peer_addr().unwrap()
    );
    assert_eq!(
        request.remote_endpoint.unwrap().socket_addr(),
        peer.local_addr().unwrap()
    );
    policy.decision().unwrap().allow(&[], &[]).unwrap();
    drop(policy);
    let mut opened = server.next_timeout(Duration::from_secs(5)).unwrap();
    let socket = opened.socket().unwrap();
    assert_eq!(socket.subprotocol(), "second");
    assert!(head(&mut peer).contains("Sec-WebSocket-Protocol: second\r\n"));
    let mut writer = socket
        .begin_message(web::WebSocketMessageType::Text)
        .unwrap();
    writer.try_write(b"A\xf0", false).unwrap();
    assert_eq!(socket.send_text("busy"), Err(Error::ALREADY_EXISTS));
    assert!(matches!(
        socket.begin_message(web::WebSocketMessageType::Binary),
        Err(Error::ALREADY_EXISTS)
    ));
    socket.ping(b"p").unwrap();
    assert_eq!(writer.try_write(b"x", true), Err(Error::INVALID_ARGUMENT));
    block_on(assert_send(writer.write(b"\x9f\x98\x80B", true))).unwrap();
    assert_eq!(frame(&mut peer), (1, false, b"A\xf0".to_vec()));
    assert_eq!(frame(&mut peer), (9, true, b"p".to_vec()));
    assert_eq!(frame(&mut peer), (0, true, b"\x9f\x98\x80B".to_vec()));
    assert_eq!(writer.try_write(&[], true), Err(Error::CLOSED));
    drop(writer);
    block_on(assert_send(socket.send_fragmented(
        web::WebSocketMessageType::Binary,
        b"abcdef",
        2,
    )))
    .unwrap();
    assert_eq!(frame(&mut peer), (2, false, b"ab".to_vec()));
    assert_eq!(frame(&mut peer), (0, false, b"cd".to_vec()));
    assert_eq!(frame(&mut peer), (0, true, b"ef".to_vec()));
    let mut retained = socket
        .begin_message(web::WebSocketMessageType::Binary)
        .unwrap();
    drop(socket);
    drop(opened);
    retained.try_write(b"owned", true).unwrap_err(); // Frame limit still applies.
    block_on(retained.write(b"done", true)).unwrap();
    assert_eq!(frame(&mut peer), (2, true, b"done".to_vec()));
    drop(retained);
    server.stop().unwrap();
}

#[test]
fn heartbeat_requires_matching_pong_and_stops_after_close() {
    let server = server(
        &web::WebSocketOptions {
            ping_interval: Duration::from_millis(30),
            pong_timeout: Duration::from_millis(120),
            ..Default::default()
        },
        &web::WebSocketRouteOptions::default(),
    );
    let mut peer = connect(&server);
    let mut opened = server.next_timeout(Duration::from_secs(5)).unwrap();
    let socket = opened.socket().unwrap();
    assert_eq!(socket.subprotocol(), "");
    assert!(!head(&mut peer).contains("Sec-WebSocket-Protocol:"));
    let first = frame(&mut peer);
    assert_eq!((first.0, first.1, first.2.len()), (9, true, 16));
    masked(&mut peer, 10, &first.2);
    let second = frame(&mut peer);
    assert_eq!(second.0, 9);
    assert_ne!(first.2, second.2);
    masked(&mut peer, 10, &first.2);
    let close = frame(&mut peer);
    assert_eq!(close.0, 8);
    assert_eq!(&close.2[..2], &[3, 233]);
    assert_eq!(socket.ping(b"late"), Err(Error::CLOSED));
    masked(&mut peer, 8, &close.2);
    let terminal = socket.next_timeout(Duration::from_secs(5)).unwrap();
    assert_eq!(terminal.kind().unwrap(), web::WebSocketKind::Closed);
    server.stop().unwrap();
}

#[test]
fn http_ipv6_and_explicit_dual_stack_preserve_transport_endpoints() {
    let Ok(probe) = TcpListener::bind("[::1]:0") else {
        return;
    };
    drop(probe);
    for dual_stack in [false, true] {
        let port = TcpListener::bind("[::1]:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        let mut server = web::HttpServer::new(&web::Options {
            listen_address: if dual_stack { "::" } else { "::1" }.into(),
            ipv6_only: !dual_stack,
            port,
            ..Default::default()
        })
        .unwrap();
        server.route("GET", "/address").unwrap();
        server.start().unwrap();
        let mut peer =
            TcpStream::connect((if dual_stack { "127.0.0.1" } else { "::1" }, port)).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        peer.write_all(b"GET /address HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n")
            .unwrap();
        let mut event = server.next_timeout(Duration::from_secs(5)).unwrap();
        let request = event.request().unwrap();
        let local = request.local_endpoint.unwrap();
        let remote = request.remote_endpoint.unwrap();
        assert_eq!(local.normalized().socket_addr(), peer.peer_addr().unwrap());
        assert_eq!(
            remote.normalized().socket_addr(),
            peer.local_addr().unwrap()
        );
        event
            .response()
            .unwrap()
            .complete(&web::ResponseHead::default(), b"ok")
            .unwrap();
        let mut response = String::new();
        peer.read_to_string(&mut response).unwrap();
        assert!(response.ends_with("ok"));
        server.stop().unwrap();
        assert_eq!(event.request().unwrap().local_endpoint, Some(local));
        assert_eq!(event.request().unwrap().remote_endpoint, Some(remote));
    }
}
