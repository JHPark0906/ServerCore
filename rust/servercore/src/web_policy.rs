//! Owned synchronous policy evaluation over native request metadata. Configure
//! before first evaluation; evaluation freezes registration. HTTP OPTIONS routes
//! must be registered explicitly before startup for CORS preflight admission.
//! Headers remain byte views; conversion to a Rust response validates UTF-8.
use crate::{
    borrowed_bytes, bytes, check, endpoint::Endpoint, pointer, raw_headers, sys, verify_abi, web,
    Error, Header, Headers, Result,
};
use std::{mem::size_of, ptr::NonNull};

#[derive(Clone, Debug)]
pub struct HttpPolicyOptions {
    pub max_rules: usize,
    pub max_metadata_bytes: usize,
    pub max_response_body_bytes: usize,
}
impl Default for HttpPolicyOptions {
    fn default() -> Self {
        Self {
            max_rules: 64,
            max_metadata_bytes: 16384,
            max_response_body_bytes: 256 * 1024,
        }
    }
}
#[derive(Clone, Debug)]
pub struct CorsOptions {
    pub allowed_origins: Vec<String>,
    pub allowed_methods: Vec<String>,
    pub allowed_headers: Vec<String>,
    pub exposed_headers: Vec<String>,
    pub allow_credentials: bool,
    pub max_age_seconds: u32,
}
impl Default for CorsOptions {
    fn default() -> Self {
        Self {
            allowed_origins: Vec::new(),
            allowed_methods: vec!["GET".into(), "HEAD".into(), "POST".into()],
            allowed_headers: Vec::new(),
            exposed_headers: Vec::new(),
            allow_credentials: false,
            max_age_seconds: 600,
        }
    }
}
#[derive(Clone, Debug)]
pub struct RequestIdOptions {
    pub header_name: String,
    pub accept_incoming: bool,
    pub max_bytes: usize,
}
impl Default for RequestIdOptions {
    fn default() -> Self {
        Self {
            header_name: "x-request-id".into(),
            accept_incoming: false,
            max_bytes: 128,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProxyHeaderMode {
    Forwarded,
    XForwarded,
}
#[derive(Clone, Debug)]
pub struct ProxyOptions {
    pub trusted_cidrs: Vec<String>,
    pub mode: ProxyHeaderMode,
    pub accept_proto: bool,
    pub accept_host: bool,
    pub normalize_mapped_ipv4: bool,
    pub max_hops: usize,
    pub max_header_bytes: usize,
}
impl Default for ProxyOptions {
    fn default() -> Self {
        Self {
            trusted_cidrs: Vec::new(),
            mode: ProxyHeaderMode::Forwarded,
            accept_proto: false,
            accept_host: false,
            normalize_mapped_ipv4: true,
            max_hops: 32,
            max_header_bytes: 8192,
        }
    }
}
fn strings(values: &[String]) -> Vec<sys::sc_bytes> {
    values.iter().map(|value| bytes(value.as_bytes())).collect()
}
fn request_headers(request: &web::Request<'_>) -> Vec<sys::sc_header> {
    request
        .headers
        .iter()
        .map(|(name, value)| sys::sc_header {
            name: bytes(name),
            value: bytes(value),
        })
        .collect()
}
fn text(value: &[u8]) -> Result<&str> {
    std::str::from_utf8(value).map_err(|_| Error::INVALID_FORMAT)
}

pub struct TrustedProxy(NonNull<sys::sc_trusted_proxy>);
unsafe impl Send for TrustedProxy {}
unsafe impl Sync for TrustedProxy {}
impl TrustedProxy {
    pub fn new(options: &ProxyOptions) -> Result<Self> {
        verify_abi()?;
        let networks = strings(&options.trusted_cidrs);
        let raw = sys::sc_proxy_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_proxy_options>() as u32,
            trusted_cidrs: networks.as_ptr(),
            trusted_cidr_count: networks.len(),
            mode: match options.mode {
                ProxyHeaderMode::Forwarded => sys::SC_PROXY_FORWARDED,
                ProxyHeaderMode::XForwarded => sys::SC_PROXY_X_FORWARDED,
            },
            accept_proto: options.accept_proto.into(),
            accept_host: options.accept_host.into(),
            normalize_mapped_ipv4: options.normalize_mapped_ipv4.into(),
            max_hops: options.max_hops,
            max_header_bytes: options.max_header_bytes,
        };
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_trusted_proxy_create(&raw, &mut out) })?;
        Ok(Self(pointer(out)?))
    }
    pub fn resolve(&self, peer: Endpoint, headers: &[Header]) -> Result<ProxyPeer> {
        self.resolve_raw(peer, &raw_headers(headers))
    }
    pub fn resolve_request(&self, request: &web::Request<'_>) -> Result<ProxyPeer> {
        self.resolve_raw(
            request.remote_endpoint.ok_or(Error::INVALID_ARGUMENT)?,
            &request_headers(request),
        )
    }
    fn resolve_raw(&self, peer: Endpoint, headers: &[sys::sc_header]) -> Result<ProxyPeer> {
        let peer = peer.to_raw()?;
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_trusted_proxy_resolve(
                self.0.as_ptr(),
                &peer,
                headers.as_ptr(),
                headers.len(),
                &mut out,
            )
        })?;
        ProxyPeer::from_raw(out)
    }
}
impl Drop for TrustedProxy {
    fn drop(&mut self) {
        unsafe { sys::sc_trusted_proxy_destroy(self.0.as_ptr()) };
    }
}
/// Immutable native-owned resolution, independent of its input and policy.
pub struct ProxyPeer {
    handle: NonNull<sys::sc_proxy_peer>,
    view: sys::sc_proxy_peer_view,
    transport: Endpoint,
    client: Endpoint,
}
unsafe impl Send for ProxyPeer {}
unsafe impl Sync for ProxyPeer {}
impl ProxyPeer {
    fn from_raw(raw: *mut sys::sc_proxy_peer) -> Result<Self> {
        let handle = pointer(raw)?;
        let mut view: sys::sc_proxy_peer_view = unsafe { std::mem::zeroed() };
        view.abi_version = sys::SC_ABI_VERSION;
        view.struct_size = size_of::<sys::sc_proxy_peer_view>() as u32;
        let result = (|| {
            check(unsafe { sys::sc_proxy_peer_get(handle.as_ptr(), &mut view) })?;
            Ok(Self {
                handle,
                view,
                transport: Endpoint::from_raw(view.transport_peer).ok_or(Error::INVALID_FORMAT)?,
                client: Endpoint::from_raw(view.client).ok_or(Error::INVALID_FORMAT)?,
            })
        })();
        if result.is_err() {
            unsafe { sys::sc_proxy_peer_destroy(handle.as_ptr()) };
        }
        result
    }
    pub fn transport_peer(&self) -> Endpoint {
        self.transport
    }
    pub fn client(&self) -> Endpoint {
        self.client
    }
    pub fn accepted_hops(&self) -> usize {
        self.view.accepted_hops
    }
    pub fn proto(&self) -> Result<&str> {
        text(unsafe { borrowed_bytes(self.view.proto) })
    }
    pub fn host(&self) -> Result<&str> {
        text(unsafe { borrowed_bytes(self.view.host) })
    }
}
impl Drop for ProxyPeer {
    fn drop(&mut self) {
        unsafe { sys::sc_proxy_peer_destroy(self.handle.as_ptr()) };
    }
}

/// Native bounded builder, immutable after its first evaluation. The C owner
/// serializes compilation; independent evaluations can then run concurrently.
pub struct HttpPolicy(NonNull<sys::sc_http_policy>);
unsafe impl Send for HttpPolicy {}
unsafe impl Sync for HttpPolicy {}
impl HttpPolicy {
    pub fn new(options: &HttpPolicyOptions) -> Result<Self> {
        verify_abi()?;
        let raw = sys::sc_http_policy_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_http_policy_options>() as u32,
            max_rules: options.max_rules,
            max_metadata_bytes: options.max_metadata_bytes,
            max_response_body_bytes: options.max_response_body_bytes,
        };
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_http_policy_create(&raw, &mut out) })?;
        Ok(Self(pointer(out)?))
    }
    pub fn add_headers(&mut self, prefix: &str, headers: &[Header]) -> Result<()> {
        let fields = raw_headers(headers);
        check(unsafe {
            sys::sc_http_policy_add_headers(
                self.0.as_ptr(),
                bytes(prefix.as_bytes()),
                fields.as_ptr(),
                fields.len(),
            )
        })
    }
    pub fn add_cors(&mut self, prefix: &str, options: &CorsOptions) -> Result<()> {
        let origins = strings(&options.allowed_origins);
        let methods = strings(&options.allowed_methods);
        let allowed = strings(&options.allowed_headers);
        let exposed = strings(&options.exposed_headers);
        let raw = sys::sc_cors_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_cors_options>() as u32,
            origins: origins.as_ptr(),
            origin_count: origins.len(),
            methods: methods.as_ptr(),
            method_count: methods.len(),
            allowed_headers: allowed.as_ptr(),
            allowed_header_count: allowed.len(),
            exposed_headers: exposed.as_ptr(),
            exposed_header_count: exposed.len(),
            allow_credentials: options.allow_credentials.into(),
            max_age_seconds: options.max_age_seconds,
        };
        check(unsafe {
            sys::sc_http_policy_add_cors(self.0.as_ptr(), bytes(prefix.as_bytes()), &raw)
        })
    }
    pub fn add_request_id(&mut self, prefix: &str, options: &RequestIdOptions) -> Result<()> {
        check(unsafe {
            sys::sc_http_policy_add_request_id(
                self.0.as_ptr(),
                bytes(prefix.as_bytes()),
                bytes(options.header_name.as_bytes()),
                options.accept_incoming.into(),
                options.max_bytes,
            )
        })
    }
    /// Copies configuration; the proxy owner may be dropped after this call.
    pub fn add_proxy(&mut self, prefix: &str, proxy: &TrustedProxy) -> Result<()> {
        check(unsafe {
            sys::sc_http_policy_add_proxy(
                self.0.as_ptr(),
                bytes(prefix.as_bytes()),
                proxy.0.as_ptr(),
            )
        })
    }
    pub fn evaluate(
        &self,
        request: &web::Request<'_>,
        websocket_upgrade: bool,
    ) -> Result<PolicyResult> {
        self.evaluate_raw(
            request.method,
            request.target,
            &request_headers(request),
            request.remote_endpoint,
            websocket_upgrade,
        )
    }
    pub fn evaluate_fields(
        &self,
        method: &str,
        target: &str,
        headers: &[Header],
        peer: Option<Endpoint>,
        websocket_upgrade: bool,
    ) -> Result<PolicyResult> {
        self.evaluate_raw(
            method,
            target,
            &raw_headers(headers),
            peer,
            websocket_upgrade,
        )
    }
    fn evaluate_raw(
        &self,
        method: &str,
        target: &str,
        headers: &[sys::sc_header],
        peer: Option<Endpoint>,
        upgrade: bool,
    ) -> Result<PolicyResult> {
        let peer = peer.map(Endpoint::to_raw).transpose()?;
        let raw = sys::sc_request_view {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_request_view>() as u32,
            method: bytes(method.as_bytes()),
            target: bytes(target.as_bytes()),
            body: bytes(&[]),
            headers: headers.as_ptr(),
            header_count: headers.len(),
            parameters: std::ptr::null(),
            parameter_count: 0,
        };
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_http_policy_evaluate(
                self.0.as_ptr(),
                &raw,
                peer.as_ref().map_or(std::ptr::null(), |value| value),
                upgrade.into(),
                &mut out,
            )
        })?;
        PolicyResult::from_raw(out)
    }
    pub fn evaluate_event(&self, event: &web::Event) -> Result<PolicyResult> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_http_policy_evaluate_event(self.0.as_ptr(), event.native_handle(), &mut out)
        })?;
        PolicyResult::from_raw(out)
    }
}
impl Drop for HttpPolicy {
    fn drop(&mut self) {
        unsafe { sys::sc_http_policy_destroy(self.0.as_ptr()) };
    }
}

/// Immutable decision data; its byte/header views live until this owner drops.
/// The result can outlive the originating request event and policy builder.
pub struct PolicyResult {
    handle: NonNull<sys::sc_http_policy_result>,
    view: sys::sc_http_policy_view,
}
unsafe impl Send for PolicyResult {}
unsafe impl Sync for PolicyResult {}
impl PolicyResult {
    fn from_raw(raw: *mut sys::sc_http_policy_result) -> Result<Self> {
        let mut value = Self {
            handle: pointer(raw)?,
            view: unsafe { std::mem::zeroed() },
        };
        value.view.abi_version = sys::SC_ABI_VERSION;
        value.view.struct_size = size_of::<sys::sc_http_policy_view>() as u32;
        check(unsafe { sys::sc_http_policy_result_get(value.handle.as_ptr(), &mut value.view) })?;
        Ok(value)
    }
    pub fn has_response(&self) -> bool {
        self.view.has_response != 0
    }
    pub fn status(&self) -> Option<u32> {
        self.has_response().then_some(self.view.status)
    }
    pub fn headers(&self) -> Headers<'_> {
        unsafe { Headers::from_raw(self.view.response_headers, self.view.response_header_count) }
    }
    pub fn attributes(&self) -> Headers<'_> {
        unsafe { Headers::from_raw(self.view.attributes, self.view.attribute_count) }
    }
    pub fn body(&self) -> &[u8] {
        unsafe { borrowed_bytes(self.view.body) }
    }
    pub fn apply(&self, decision: &web::Decision) -> Result<()> {
        check(unsafe {
            sys::sc_http_policy_result_apply(self.handle.as_ptr(), decision.native_handle())
        })
    }
    pub fn response_head(&self) -> Result<Option<web::ResponseHead>> {
        if !self.has_response() {
            return Ok(None);
        }
        let headers = self
            .headers()
            .iter()
            .map(|(name, value)| Ok(Header::new(text(name)?, text(value)?)))
            .collect::<Result<Vec<_>>>()?;
        Ok(Some(web::ResponseHead {
            status: self.view.status,
            headers,
            close: self.view.close != 0,
            ..Default::default()
        }))
    }
    pub fn complete(&self, response: &web::Response) -> Result<()> {
        response.complete(
            &self.response_head()?.ok_or(Error::INVALID_ARGUMENT)?,
            self.body(),
        )
    }
}
impl Drop for PolicyResult {
    fn drop(&mut self) {
        unsafe { sys::sc_http_policy_result_destroy(self.handle.as_ptr()) };
    }
}
pub fn json_response(json: &[u8], status: u32, max_bytes: usize) -> Result<PolicyResult> {
    let mut out = std::ptr::null_mut();
    check(unsafe { sys::sc_http_json_response(bytes(json), status, max_bytes, &mut out) })?;
    PolicyResult::from_raw(out)
}
pub fn error_response(error: Error, request_id: &str, status: Option<u32>) -> Result<PolicyResult> {
    let mut out = std::ptr::null_mut();
    check(unsafe {
        sys::sc_http_error_response(
            error.0,
            bytes(request_id.as_bytes()),
            status.unwrap_or(0),
            &mut out,
        )
    })?;
    PolicyResult::from_raw(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{Read, Write},
        net::{TcpListener, TcpStream},
        time::Duration,
    };
    #[test]
    fn pipeline_prefix_cors_freeze_and_owned_results() {
        let mut policy = HttpPolicy::new(&HttpPolicyOptions::default()).unwrap();
        policy
            .add_headers("/", &[Header::new("X-Common", "one")])
            .unwrap();
        policy
            .add_headers("/api", &[Header::new("X-Common", "two")])
            .unwrap();
        policy
            .add_request_id("/api", &RequestIdOptions::default())
            .unwrap();
        policy
            .add_cors(
                "/api",
                &CorsOptions {
                    allowed_origins: vec!["https://app.example".into()],
                    allowed_headers: vec!["x-token".into()],
                    allow_credentials: true,
                    ..Default::default()
                },
            )
            .unwrap();
        let response = policy
            .evaluate_fields(
                "OPTIONS",
                "/api/item",
                &[
                    Header::new("Origin", "https://app.example"),
                    Header::new("Access-Control-Request-Method", "POST"),
                    Header::new("Access-Control-Request-Headers", "X-Token"),
                ],
                None,
                false,
            )
            .unwrap();
        assert_eq!(response.status(), Some(204));
        assert_eq!(response.headers().get("x-common"), Some(b"two".as_slice()));
        assert_eq!(
            response.headers().get("access-control-allow-origin"),
            Some(b"https://app.example".as_slice())
        );
        assert_eq!(
            response.headers().get("access-control-allow-credentials"),
            Some(b"true".as_slice())
        );
        assert!(response
            .attributes()
            .get_exact("request_id")
            .unwrap()
            .starts_with(b"sc-"));
        assert_eq!(policy.add_headers("/", &[]), Err(Error::CLOSED));
        let outside = policy
            .evaluate_fields("GET", "/apix", &[], None, false)
            .unwrap();
        assert_eq!(outside.headers().get("x-common"), Some(b"one".as_slice()));
        assert!(outside.attributes().is_empty());
        assert!(matches!(
            policy.evaluate_fields("GET", "/api/%2fprivate", &[], None, false),
            Err(Error::INVALID_FORMAT)
        ));
        drop(policy);
        assert_eq!(response.response_head().unwrap().unwrap().status, 204);
        assert!(response.body().is_empty());
        let mut invalid = HttpPolicy::new(&HttpPolicyOptions::default()).unwrap();
        assert_eq!(
            invalid.add_cors(
                "/",
                &CorsOptions {
                    allowed_origins: vec!["*".into()],
                    allow_credentials: true,
                    ..Default::default()
                }
            ),
            Err(Error::INVALID_ARGUMENT)
        );
    }
    #[test]
    fn trusted_proxy_boundary_normalization_and_owned_peer() {
        let proxy = TrustedProxy::new(&ProxyOptions {
            trusted_cidrs: vec!["127.0.0.0/8".into()],
            accept_proto: true,
            accept_host: true,
            ..Default::default()
        })
        .unwrap();
        let headers = [Header::new(
            "Forwarded",
            "for=192.0.2.123, for=198.51.100.7;proto=https;host=public.example",
        )];
        let peer = Endpoint::parse("::ffff:127.0.0.1", 7000).unwrap();
        let resolved = proxy.resolve(peer, &headers).unwrap();
        assert_eq!(resolved.transport_peer(), peer);
        assert_eq!(resolved.client().address.to_string(), "198.51.100.7");
        assert_eq!(
            resolved.accepted_hops(),
            1,
            "untrusted next hop stops attacker leftmost traversal"
        );
        assert_eq!(resolved.proto().unwrap(), "https");
        let untrusted = Endpoint::parse("203.0.113.4", 8000).unwrap();
        let ignored = proxy
            .resolve(untrusted, &[Header::new("Forwarded", "malformed")])
            .unwrap();
        assert_eq!(ignored.client(), untrusted);
        let mut policy = HttpPolicy::new(&HttpPolicyOptions::default()).unwrap();
        policy.add_proxy("/", &proxy).unwrap();
        drop(proxy);
        assert_eq!(resolved.host().unwrap(), "public.example");
        let result = policy
            .evaluate_fields("GET", "/", &headers, Some(peer), false)
            .unwrap();
        assert_eq!(
            result.attributes().get_exact("client_ip"),
            Some(b"198.51.100.7".as_slice())
        );
        let invalid = Endpoint {
            address: "127.0.0.1".parse().unwrap(),
            port: 1,
            scope_id: 9,
        };
        assert!(matches!(
            policy.evaluate_fields("GET", "/", &[], Some(invalid), false),
            Err(Error::INVALID_ARGUMENT)
        ));
    }
    #[test]
    fn event_evaluation_apply_and_json_response_helpers() {
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
        server.enable_policy().unwrap();
        server.route("GET", "/api").unwrap();
        server.start().unwrap();
        let mut peer = TcpStream::connect(("127.0.0.1", server.port())).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        peer.write_all(b"GET /api HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n")
            .unwrap();
        let mut event = server.next_timeout(Duration::from_secs(5)).unwrap();
        let decision = event.decision().unwrap();
        let mut policy = HttpPolicy::new(&HttpPolicyOptions::default()).unwrap();
        policy
            .add_request_id("/", &RequestIdOptions::default())
            .unwrap();
        let value = policy.evaluate_event(&event).unwrap();
        drop(event);
        drop(policy);
        value.apply(&decision).unwrap();
        assert_eq!(value.apply(&decision), Err(Error::CLOSED));
        let mut request = server.next_timeout(Duration::from_secs(5)).unwrap();
        assert_eq!(
            request.attributes().unwrap().get_exact("request_id"),
            value.attributes().get_exact("request_id")
        );
        let response = request.response().unwrap();
        drop(request);
        let json = json_response(br#" { "ready" : true } "#, 201, 1024).unwrap();
        assert_eq!(json.status(), Some(201));
        json.complete(&response).unwrap();
        let mut wire = String::new();
        peer.read_to_string(&mut wire).unwrap();
        assert!(wire.starts_with("HTTP/1.1 201 ") && wire.ends_with(r#"{"ready": true}"#));
        assert!(matches!(
            json_response(b"{", 200, 1024),
            Err(Error::INVALID_FORMAT)
        ));
        let error = error_response(Error::NOT_FOUND, "request-1", None).unwrap();
        assert_eq!(error.status(), Some(404));
        assert_eq!(
            error.headers().get("content-type"),
            Some(b"application/problem+json".as_slice())
        );
        assert!(text(error.body()).unwrap().contains("request-1"));
        server.stop().unwrap();
    }
}
