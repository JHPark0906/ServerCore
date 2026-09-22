//! HTTP request events, response streaming and RFC 6455 WebSocket handles.
use crate::{
    borrowed_bytes, bytes, check, pointer, raw_headers, reactor, sys, timeout_ms, verify_abi,
    CapacityWait, Error, Header, Headers, Result,
};
use std::{
    mem::{size_of, MaybeUninit},
    ptr::NonNull,
    sync::Arc,
    time::Duration,
};

#[derive(Clone, Debug)]
pub struct Options {
    pub listen_address: String,
    pub port: u16,
    pub io_threads: u32,
    pub handler_threads: u32,
    pub max_connections: usize,
    pub max_header_bytes: usize,
    pub max_body_bytes: usize,
    pub max_buffered_response_bytes: usize,
    pub max_active_requests: usize,
    pub max_request_bytes: usize,
    pub max_event_count: usize,
    pub max_event_bytes: usize,
    pub max_ws_event_count: usize,
    pub max_ws_event_bytes: usize,
    pub max_send_bytes: usize,
    pub max_chunk_bytes: usize,
    pub handler_timeout: Duration,
    pub stream_idle_timeout: Duration,
    pub send_stall_timeout: Duration,
}
impl Default for Options {
    fn default() -> Self {
        let mut raw = MaybeUninit::<sys::sc_web_options>::uninit();
        assert_eq!(
            unsafe { sys::sc_web_options_init(raw.as_mut_ptr(), size_of::<sys::sc_web_options>()) },
            sys::SC_OK
        );
        let raw = unsafe { raw.assume_init() };
        Self {
            listen_address: String::from_utf8_lossy(unsafe { borrowed_bytes(raw.listen_address) })
                .into_owned(),
            port: raw.port,
            io_threads: raw.io_threads,
            handler_threads: raw.handler_threads,
            max_connections: raw.max_connections,
            max_header_bytes: raw.max_header_bytes,
            max_body_bytes: raw.max_body_bytes,
            max_buffered_response_bytes: raw.max_buffered_response_bytes,
            max_active_requests: raw.max_active_requests,
            max_request_bytes: raw.max_request_bytes,
            max_event_count: raw.max_event_count,
            max_event_bytes: raw.max_event_bytes,
            max_ws_event_count: raw.max_ws_event_count,
            max_ws_event_bytes: raw.max_ws_event_bytes,
            max_send_bytes: raw.max_send_bytes,
            max_chunk_bytes: raw.max_chunk_bytes,
            handler_timeout: Duration::from_millis(raw.handler_timeout_ms.into()),
            stream_idle_timeout: Duration::from_millis(raw.stream_idle_timeout_ms.into()),
            send_stall_timeout: Duration::from_millis(raw.send_stall_timeout_ms.into()),
        }
    }
}
impl Options {
    fn raw(&self) -> Result<sys::sc_web_options> {
        Ok(sys::sc_web_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_web_options>() as u32,
            listen_address: bytes(self.listen_address.as_bytes()),
            port: self.port,
            reserved: 0,
            io_threads: self.io_threads,
            handler_threads: self.handler_threads,
            max_connections: self.max_connections,
            max_header_bytes: self.max_header_bytes,
            max_body_bytes: self.max_body_bytes,
            max_buffered_response_bytes: self.max_buffered_response_bytes,
            max_active_requests: self.max_active_requests,
            max_request_bytes: self.max_request_bytes,
            max_event_count: self.max_event_count,
            max_event_bytes: self.max_event_bytes,
            max_ws_event_count: self.max_ws_event_count,
            max_ws_event_bytes: self.max_ws_event_bytes,
            max_send_bytes: self.max_send_bytes,
            max_chunk_bytes: self.max_chunk_bytes,
            handler_timeout_ms: timeout_ms(self.handler_timeout)?,
            stream_idle_timeout_ms: timeout_ms(self.stream_idle_timeout)?,
            send_stall_timeout_ms: timeout_ms(self.send_stall_timeout)?,
        })
    }
}

/// Registration precedes `start`; polling dispatches immutable events to Rust.
/// `stop` and Drop join native workers. Previously returned events stay valid.
pub struct HttpServer {
    handle: NonNull<sys::sc_web_server>,
    _readiness: reactor::Lease,
}
// ABI handles allow concurrent methods; Rust ownership excludes destruction
// while any borrowed method/future is running.
unsafe impl Send for HttpServer {}
unsafe impl Sync for HttpServer {}
impl HttpServer {
    pub fn new(options: &Options) -> Result<Self> {
        verify_abi()?;
        let readiness = reactor::Lease::new()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_web_server_create(&options.raw()?, &mut out) })?;
        Ok(Self {
            handle: pointer(out)?,
            _readiness: readiness,
        })
    }
    pub fn route(&mut self, method: &str, path: &str) -> Result<()> {
        self.register_route(method, path, false)
    }
    pub fn route_pattern(&mut self, method: &str, pattern: &str) -> Result<()> {
        self.register_route(method, pattern, true)
    }
    /// Dispatch request headers before buffering the complete upload. Claim
    /// event.body() and event.response(); consume/drop bounded owned chunks.
    pub fn stream_route(&mut self, method: &str, path: &str) -> Result<()> {
        self.register_stream_route(method, path, false)
    }
    pub fn stream_route_pattern(&mut self, method: &str, pattern: &str) -> Result<()> {
        self.register_stream_route(method, pattern, true)
    }
    fn register_stream_route(&mut self, method: &str, path: &str, pattern: bool) -> Result<()> {
        check(unsafe {
            sys::sc_web_server_stream_route(
                self.handle.as_ptr(),
                bytes(method.as_bytes()),
                bytes(path.as_bytes()),
                pattern.into(),
            )
        })
    }
    pub fn set_body_limits(&mut self, options: &BodyOptions) -> Result<()> {
        let raw = sys::sc_body_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_body_options>() as u32,
            max_buffered_bytes: options.max_buffered_bytes,
            max_body_bytes: options.max_body_bytes,
        };
        check(unsafe { sys::sc_web_server_set_body_limits(self.handle.as_ptr(), &raw) })
    }
    /// Enable an async gate before selected handlers and WebSocket 101. Buffered
    /// routes already own their body; streaming routes gate at headers.
    /// Protocol/routing errors bypass policy.
    pub fn enable_policy(&mut self) -> Result<()> {
        check(unsafe { sys::sc_web_server_enable_policy(self.handle.as_ptr()) })
    }
    pub fn websocket_policy(&mut self, path: &str) -> Result<()> {
        self.register_websocket_policy(path, false)
    }
    pub fn websocket_policy_pattern(&mut self, pattern: &str) -> Result<()> {
        self.register_websocket_policy(pattern, true)
    }
    fn register_websocket_policy(&mut self, path: &str, pattern: bool) -> Result<()> {
        check(unsafe {
            sys::sc_web_server_websocket_policy(
                self.handle.as_ptr(),
                bytes(path.as_bytes()),
                pattern.into(),
            )
        })
    }
    fn register_route(&mut self, method: &str, path: &str, pattern: bool) -> Result<()> {
        check(unsafe {
            sys::sc_web_server_route(
                self.handle.as_ptr(),
                bytes(method.as_bytes()),
                bytes(path.as_bytes()),
                pattern.into(),
            )
        })
    }
    pub fn websocket(&mut self, path: &str) -> Result<()> {
        self.register_websocket(path, false)
    }
    pub fn websocket_pattern(&mut self, pattern: &str) -> Result<()> {
        self.register_websocket(pattern, true)
    }
    fn register_websocket(&mut self, path: &str, pattern: bool) -> Result<()> {
        check(unsafe {
            sys::sc_web_server_websocket(
                self.handle.as_ptr(),
                bytes(path.as_bytes()),
                pattern.into(),
            )
        })
    }
    pub fn start(&mut self) -> Result<()> {
        check(unsafe { sys::sc_web_server_start(self.handle.as_ptr()) })
    }
    pub fn port(&self) -> u16 {
        unsafe { sys::sc_web_server_port(self.handle.as_ptr()) }
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { sys::sc_web_server_stop(self.handle.as_ptr()) })
    }
    pub fn begin_drain(&self) -> Result<()> {
        check(unsafe { sys::sc_web_server_begin_drain(self.handle.as_ptr()) })
    }
    pub fn drain_status(&self) -> Result<()> {
        check(unsafe { sys::sc_web_server_drain_status(self.handle.as_ptr()) })
    }
    /// Blocks the calling thread. Expiry forces shutdown and reports Timeout.
    pub fn stop_gracefully(&self, timeout: Duration) -> Result<()> {
        check(unsafe {
            sys::sc_web_server_stop_gracefully(self.handle.as_ptr(), timeout_ms(timeout)?)
        })
    }
    /// Await admitted work without blocking for its duration, then join native
    /// shutdown. Dropping this Future leaves the server draining; Drop/stop can
    /// force shutdown. Timeout aborts remaining work and returns Timeout.
    pub async fn drain(&self, timeout: Duration) -> Result<()> {
        timeout_ms(timeout)?;
        let deadline = std::time::Instant::now()
            .checked_add(timeout)
            .ok_or(Error::INVALID_ARGUMENT)?;
        self.begin_drain()?;
        reactor::poll_fn(move || match self.drain_status() {
            Ok(()) => self.stop(),
            Err(Error::WOULD_BLOCK) if std::time::Instant::now() >= deadline => {
                self.stop_gracefully(Duration::ZERO)
            }
            result => result,
        })?
        .await
    }
    pub fn set_logger(&mut self, logger: &crate::observability::Logger) -> Result<()> {
        check(unsafe {
            sys::sc_web_server_set_logger(self.handle.as_ptr(), logger.handle.as_ptr())
        })
    }
    pub fn metrics(&self) -> Result<crate::observability::WebMetrics> {
        let mut value = crate::observability::WebMetrics {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_web_metrics>() as u32,
            ..Default::default()
        };
        check(unsafe { sys::sc_web_server_get_metrics(self.handle.as_ptr(), &mut value) })?;
        Ok(value)
    }
    pub fn metrics_prometheus(&self) -> Result<crate::observability::MetricsText> {
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_web_server_metrics_prometheus(self.handle.as_ptr(), &mut out) })?;
        crate::observability::MetricsText::from_raw(out)
    }
    /// Zero timeout returns WouldBlock when empty; a positive timeout returns
    /// Timeout. Closed means the server's queue is stopped and drained.
    pub fn next_timeout(&self, timeout: Duration) -> Result<Event> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_web_server_next(self.handle.as_ptr(), timeout_ms(timeout)?, &mut out)
        })?;
        Ok(Event(pointer(out)?))
    }
    pub async fn next(&mut self) -> Result<Event> {
        reactor::poll_fn(|| self.next_timeout(Duration::ZERO))?.await
    }
}
impl Drop for HttpServer {
    fn drop(&mut self) {
        unsafe { sys::sc_web_server_destroy(self.handle.as_ptr()) };
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventKind {
    Request,
    WebSocket,
    Policy,
}
/// Owns the immutable request and its queue-budget charge. Dropping an unclaimed
/// request aborts it; dropping an unclaimed upgrade closes the socket.
pub struct Event(NonNull<sys::sc_web_event>);
unsafe impl Send for Event {}
unsafe impl Sync for Event {}
impl Event {
    pub fn kind(&self) -> Result<EventKind> {
        match unsafe { sys::sc_web_event_kind(self.0.as_ptr()) } {
            sys::SC_WEB_REQUEST => Ok(EventKind::Request),
            sys::SC_WEB_WEBSOCKET => Ok(EventKind::WebSocket),
            sys::SC_WEB_POLICY => Ok(EventKind::Policy),
            _ => Err(Error::INVALID_FORMAT),
        }
    }
    pub fn request(&self) -> Result<Request<'_>> {
        let mut view: sys::sc_request_view = unsafe { std::mem::zeroed() };
        view.abi_version = sys::SC_ABI_VERSION;
        view.struct_size = size_of::<sys::sc_request_view>() as u32;
        check(unsafe { sys::sc_web_event_request(self.0.as_ptr(), &mut view) })?;
        // Immutable view remains alive through this Event's borrow.
        unsafe {
            Ok(Request {
                method: std::str::from_utf8(borrowed_bytes(view.method))
                    .map_err(|_| Error::INVALID_FORMAT)?,
                target: std::str::from_utf8(borrowed_bytes(view.target))
                    .map_err(|_| Error::INVALID_FORMAT)?,
                body: borrowed_bytes(view.body),
                headers: Headers::from_raw(view.headers, view.header_count),
                parameters: Headers::from_raw(view.parameters, view.parameter_count),
            })
        }
    }
    pub fn response(&mut self) -> Result<Response> {
        let readiness = reactor::Lease::new()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_web_event_response(self.0.as_ptr(), &mut out) })?;
        Ok(Response(Arc::new(ResponseInner {
            handle: pointer(out)?,
            _readiness: readiness,
        })))
    }
    pub fn socket(&mut self) -> Result<WebSocket> {
        let readiness = reactor::Lease::new()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_web_event_socket(self.0.as_ptr(), &mut out) })?;
        Ok(WebSocket(Arc::new(SocketInner {
            handle: pointer(out)?,
            _readiness: readiness,
        })))
    }
    pub fn attributes(&self) -> Result<Headers<'_>> {
        let mut view = sys::sc_headers_view {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_headers_view>() as u32,
            headers: std::ptr::null(),
            count: 0,
        };
        check(unsafe { sys::sc_web_event_attributes(self.0.as_ptr(), &mut view) })?;
        Ok(unsafe { Headers::from_raw(view.headers, view.count) })
    }
    pub fn body(&mut self) -> Result<Body> {
        let readiness = reactor::Lease::new()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_web_event_body(self.0.as_ptr(), &mut out) })?;
        Ok(Body {
            handle: pointer(out)?,
            _readiness: readiness,
        })
    }
    pub fn decision(&mut self) -> Result<Decision> {
        let readiness = reactor::Lease::new()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_web_event_decision(self.0.as_ptr(), &mut out) })?;
        Ok(Decision(Arc::new(DecisionInner {
            handle: pointer(out)?,
            _readiness: readiness,
        })))
    }
    pub fn policy_is_websocket(&self) -> bool {
        unsafe { sys::sc_web_event_policy_is_websocket(self.0.as_ptr()) != 0 }
    }
}
impl Drop for Event {
    fn drop(&mut self) {
        unsafe { sys::sc_web_event_destroy(self.0.as_ptr()) };
    }
}

pub struct Request<'a> {
    pub method: &'a str,
    pub target: &'a str,
    pub body: &'a [u8],
    pub headers: Headers<'a>,
    pub parameters: Headers<'a>,
}

#[derive(Clone, Debug)]
pub struct BodyOptions {
    pub max_buffered_bytes: usize,
    /// Must be at least Options::max_body_bytes (the buffered-route body limit).
    pub max_body_bytes: usize,
}
impl Default for BodyOptions {
    fn default() -> Self {
        Self {
            max_buffered_bytes: 64 * 1024,
            max_body_bytes: 1024 * 1024 * 1024,
        }
    }
}
/// Owns the upload reader independently of the initial request event. Drop
/// aborts before clean EOF; a consumed or parser-complete body is harmless.
pub struct Body {
    handle: NonNull<sys::sc_http_body>,
    _readiness: reactor::Lease,
}
unsafe impl Send for Body {}
unsafe impl Sync for Body {}
impl Body {
    pub fn try_read(&self) -> Result<Option<BodyChunk>> {
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_http_body_read(self.handle.as_ptr(), &mut out) })?;
        Ok(NonNull::new(out).map(|handle| BodyChunk { handle }))
    }
    pub async fn next(&mut self) -> Result<Option<BodyChunk>> {
        reactor::poll_fn(|| self.try_read())?.await
    }
    pub fn retained_bytes(&self) -> usize {
        unsafe { sys::sc_http_body_retained_bytes(self.handle.as_ptr()) }
    }
    pub fn is_cancelled(&self) -> bool {
        unsafe { sys::sc_http_body_cancelled(self.handle.as_ptr()) != 0 }
    }
    pub fn cancel(&self) {
        unsafe { sys::sc_http_body_cancel(self.handle.as_ptr()) };
    }
}
impl Drop for Body {
    fn drop(&mut self) {
        unsafe { sys::sc_http_body_destroy(self.handle.as_ptr()) };
    }
}
/// Payload remains charged against native upload capacity until this chunk is
/// dropped, including after its reader/server is destroyed. Release promptly.
pub struct BodyChunk {
    handle: NonNull<sys::sc_body_chunk>,
}
unsafe impl Send for BodyChunk {}
unsafe impl Sync for BodyChunk {}
impl BodyChunk {
    pub fn bytes(&self) -> &[u8] {
        unsafe { borrowed_bytes(sys::sc_body_chunk_view(self.handle.as_ptr())) }
    }
}
impl Drop for BodyChunk {
    fn drop(&mut self) {
        unsafe { sys::sc_body_chunk_destroy(self.handle.as_ptr()) };
    }
}

struct DecisionInner {
    handle: NonNull<sys::sc_request_decision>,
    _readiness: reactor::Lease,
}
unsafe impl Send for DecisionInner {}
unsafe impl Sync for DecisionInner {}
impl Drop for DecisionInner {
    fn drop(&mut self) {
        unsafe { sys::sc_request_decision_destroy(self.handle.as_ptr()) };
    }
}
/// Single-use pre-handler/pre-upgrade gate. Last clone aborts an unresolved
/// decision. Allow copies metadata; Reject sends HTTP before any WebSocket 101.
#[derive(Clone)]
pub struct Decision(Arc<DecisionInner>);
impl Decision {
    pub fn allow(&self, response_headers: &[Header], attributes: &[Header]) -> Result<()> {
        let response_headers = raw_headers(response_headers);
        let attributes = raw_headers(attributes);
        let options = sys::sc_policy_allow {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_policy_allow>() as u32,
            response_headers: response_headers.as_ptr(),
            response_header_count: response_headers.len(),
            attributes: attributes.as_ptr(),
            attribute_count: attributes.len(),
        };
        check(unsafe { sys::sc_request_decision_allow(self.0.handle.as_ptr(), &options) })
    }
    pub fn reject(&self, head: &ResponseHead, body: &[u8]) -> Result<()> {
        let headers = raw_headers(&head.headers);
        check(unsafe {
            sys::sc_request_decision_reject(
                self.0.handle.as_ptr(),
                &head.raw(&headers),
                bytes(body),
            )
        })
    }
    pub fn abort(&self) {
        unsafe { sys::sc_request_decision_abort(self.0.handle.as_ptr()) };
    }
    pub fn is_cancelled(&self) -> bool {
        unsafe { sys::sc_request_decision_cancelled(self.0.handle.as_ptr()) != 0 }
    }
    /// Waits for disconnect/timeout/abort, not normal decision completion.
    pub async fn cancelled(&self) -> Result<()> {
        reactor::poll_fn(|| {
            if self.is_cancelled() {
                Ok(())
            } else {
                Err(Error::WOULD_BLOCK)
            }
        })?
        .await
    }
}
impl<'a> Request<'a> {
    pub fn parameter(&self, name: &str) -> Option<&'a str> {
        // Parameter names are case-sensitive even though HTTP header names are not.
        self.parameters
            .iter()
            .find(|(key, _)| *key == name.as_bytes())
            .and_then(|(_, value)| std::str::from_utf8(value).ok())
    }
}

#[derive(Clone, Debug)]
pub struct ResponseHead {
    pub status: u32,
    pub headers: Vec<Header>,
    pub content_length: Option<u64>,
    pub close: bool,
}
impl Default for ResponseHead {
    fn default() -> Self {
        Self {
            status: 200,
            headers: Vec::new(),
            content_length: None,
            close: false,
        }
    }
}
impl ResponseHead {
    fn raw(&self, headers: &[sys::sc_header]) -> sys::sc_response_head {
        sys::sc_response_head {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_response_head>() as u32,
            status: self.status,
            flags: if self.close {
                sys::SC_RESPONSE_CLOSE
            } else {
                0
            } | if self.content_length.is_some() {
                sys::SC_RESPONSE_HAS_LENGTH
            } else {
                0
            },
            content_length: self.content_length.unwrap_or(0),
            headers: headers.as_ptr(),
            header_count: headers.len(),
        }
    }
}
struct ResponseInner {
    handle: NonNull<sys::sc_http_response>,
    _readiness: reactor::Lease,
}
unsafe impl Send for ResponseInner {}
unsafe impl Sync for ResponseInner {}
impl Drop for ResponseInner {
    fn drop(&mut self) {
        unsafe { sys::sc_http_response_destroy(self.handle.as_ptr()) };
    }
}
/// Clone shares ownership; the last clone aborts an unfinished response.
/// Serialize producers when byte order matters. Successful writes are queued
/// locally, not acknowledged by the remote peer.
#[derive(Clone)]
pub struct Response(Arc<ResponseInner>);
impl Response {
    pub fn start(&self, head: &ResponseHead) -> Result<()> {
        let headers = raw_headers(&head.headers);
        check(unsafe { sys::sc_http_response_start(self.0.handle.as_ptr(), &head.raw(&headers)) })
    }
    pub fn write(&self, data: &[u8]) -> Result<()> {
        check(unsafe { sys::sc_http_response_write(self.0.handle.as_ptr(), bytes(data)) })
    }
    pub fn finish(&self) -> Result<()> {
        check(unsafe { sys::sc_http_response_finish(self.0.handle.as_ptr()) })
    }
    pub fn complete(&self, head: &ResponseHead, body: &[u8]) -> Result<()> {
        let headers = raw_headers(&head.headers);
        check(unsafe {
            sys::sc_http_response_complete(self.0.handle.as_ptr(), &head.raw(&headers), bytes(body))
        })
    }
    pub fn abort(&self) {
        unsafe { sys::sc_http_response_abort(self.0.handle.as_ptr()) };
    }
    pub fn is_cancelled(&self) -> bool {
        unsafe { sys::sc_http_response_cancelled(self.0.handle.as_ptr()) != 0 }
    }
    /// Resolves on abort/disconnect/shutdown, so an executor can select this
    /// against an outbound operation and drop that operation when cancelled.
    /// Normal Finish does not cancel user work; this is not a completion wait.
    pub async fn cancelled(&self) -> Result<()> {
        reactor::poll_fn(|| {
            if self.is_cancelled() {
                Ok(())
            } else {
                Err(Error::WOULD_BLOCK)
            }
        })?
        .await
    }
    pub fn is_head(&self) -> bool {
        unsafe { sys::sc_http_response_is_head(self.0.handle.as_ptr()) != 0 }
    }
    pub fn max_write_bytes(&self) -> usize {
        unsafe { sys::sc_http_response_max_write(self.0.handle.as_ptr()) }
    }
    /// Only one pending capacity registration per response. Zero bytes waits
    /// for header/Finish framing capacity. A notification is advisory.
    pub fn wait_capacity(&self, bytes: usize) -> Result<CapacityWait> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_http_response_wait_capacity(self.0.handle.as_ptr(), bytes, &mut out)
        })?;
        CapacityWait::from_raw(out)
    }
    pub async fn start_async(&self, head: &ResponseHead) -> Result<()> {
        loop {
            match self.start(head) {
                Err(Error::WOULD_BLOCK) => self.wait_capacity(0)?.wait().await?,
                result => return result,
            }
        }
    }
    /// Splits into bounded writes and awaits backpressure. Cancellation of this
    /// Rust Future may leave an accepted prefix; abort the response if abandoning
    /// the producer while another clone remains alive.
    pub async fn write_all(&self, mut data: &[u8]) -> Result<()> {
        let max = self.max_write_bytes();
        if max == 0 {
            return Err(Error::CLOSED);
        }
        while !data.is_empty() {
            let length = data.len().min(max);
            match self.write(&data[..length]) {
                Ok(()) => data = &data[length..],
                Err(Error::WOULD_BLOCK) => self.wait_capacity(length)?.wait().await?,
                Err(error) => return Err(error),
            }
        }
        Ok(())
    }
    pub async fn finish_async(&self) -> Result<()> {
        loop {
            match self.finish() {
                Err(Error::WOULD_BLOCK) => self.wait_capacity(0)?.wait().await?,
                result => return result,
            }
        }
    }
}

struct SocketInner {
    handle: NonNull<sys::sc_websocket>,
    _readiness: reactor::Lease,
}
unsafe impl Send for SocketInner {}
unsafe impl Sync for SocketInner {}
impl Drop for SocketInner {
    fn drop(&mut self) {
        unsafe { sys::sc_websocket_destroy(self.handle.as_ptr()) };
    }
}
#[derive(Clone)]
pub struct WebSocket(Arc<SocketInner>);
impl WebSocket {
    pub fn id(&self) -> u64 {
        unsafe { sys::sc_websocket_id(self.0.handle.as_ptr()) }
    }
    pub fn send_text(&self, text: &str) -> Result<()> {
        self.send(sys::SC_WS_TEXT, text.as_bytes())
    }
    pub fn send_binary(&self, data: &[u8]) -> Result<()> {
        self.send(sys::SC_WS_BINARY, data)
    }
    fn send(&self, kind: u32, data: &[u8]) -> Result<()> {
        check(unsafe { sys::sc_websocket_send(self.0.handle.as_ptr(), kind, bytes(data)) })
    }
    pub async fn send_text_async(&self, text: &str) -> Result<()> {
        self.send_async(sys::SC_WS_TEXT, text.as_bytes()).await
    }
    pub async fn send_binary_async(&self, data: &[u8]) -> Result<()> {
        self.send_async(sys::SC_WS_BINARY, data).await
    }
    async fn send_async(&self, kind: u32, data: &[u8]) -> Result<()> {
        loop {
            match self.send(kind, data) {
                Err(Error::WOULD_BLOCK) => self.wait_capacity(data.len())?.wait().await?,
                result => return result,
            }
        }
    }
    pub fn close(&self, code: u16, reason: &str) -> Result<()> {
        check(unsafe {
            sys::sc_websocket_close(self.0.handle.as_ptr(), code, bytes(reason.as_bytes()))
        })
    }
    pub fn wait_capacity(&self, bytes: usize) -> Result<CapacityWait> {
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_websocket_wait_capacity(self.0.handle.as_ptr(), bytes, &mut out) })?;
        CapacityWait::from_raw(out)
    }
    pub fn next_timeout(&self, timeout: Duration) -> Result<WebSocketEvent> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_websocket_next(self.0.handle.as_ptr(), timeout_ms(timeout)?, &mut out)
        })?;
        let event = WebSocketEvent {
            handle: pointer(out)?,
            view: unsafe { std::mem::zeroed() },
        };
        event.initialize()
    }
    pub async fn next(&mut self) -> Result<WebSocketEvent> {
        reactor::poll_fn(|| self.next_timeout(Duration::ZERO))?.await
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WebSocketKind {
    Text,
    Binary,
    Closed,
}
pub struct WebSocketEvent {
    handle: NonNull<sys::sc_websocket_event>,
    view: sys::sc_websocket_event_view,
}
unsafe impl Send for WebSocketEvent {}
unsafe impl Sync for WebSocketEvent {}
impl WebSocketEvent {
    fn initialize(mut self) -> Result<Self> {
        self.view.abi_version = sys::SC_ABI_VERSION;
        self.view.struct_size = size_of::<sys::sc_websocket_event_view>() as u32;
        check(unsafe { sys::sc_websocket_event_get_view(self.handle.as_ptr(), &mut self.view) })?;
        Ok(self)
    }
    pub fn kind(&self) -> Result<WebSocketKind> {
        match self.view.kind {
            sys::SC_WS_TEXT => Ok(WebSocketKind::Text),
            sys::SC_WS_BINARY => Ok(WebSocketKind::Binary),
            sys::SC_WS_CLOSED => Ok(WebSocketKind::Closed),
            _ => Err(Error::INVALID_FORMAT),
        }
    }
    pub fn bytes(&self) -> &[u8] {
        unsafe { borrowed_bytes(self.view.bytes) }
    }
    pub fn text(&self) -> Result<&str> {
        std::str::from_utf8(self.bytes()).map_err(|_| Error::INVALID_FORMAT)
    }
    pub fn close_code(&self) -> u16 {
        self.view.close_code
    }
}
impl Drop for WebSocketEvent {
    fn drop(&mut self) {
        unsafe { sys::sc_websocket_event_destroy(self.handle.as_ptr()) };
    }
}
