//! Accepted raw TCP connections. Chunk boundaries are not message boundaries;
//! applications provide their own framing and serialization.
use crate::{
    borrowed_bytes, bytes, check, pointer, reactor, sys, timeout_ms, verify_abi, CapacityWait,
    Error, Result,
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
    pub max_connections: usize,
    pub max_event_count: usize,
    pub max_event_bytes: usize,
    pub connection_send_bytes: usize,
    pub total_send_bytes: usize,
    /// Each connection's own receive budget (native defaults 64 events and
    /// 1 MiB; count at most 65536). Held events count until dropped.
    pub max_connection_event_count: usize,
    pub max_connection_event_bytes: usize,
    /// Explicitly defaults to IPv6-only on both operating systems. False permits
    /// IPv4-mapped peers on an IPv6 listener; ignored for IPv4 literals.
    pub ipv6_only: bool,
}
impl Default for Options {
    fn default() -> Self {
        let mut raw = MaybeUninit::<sys::sc_tcp_options>::uninit();
        assert_eq!(
            unsafe { sys::sc_tcp_options_init(raw.as_mut_ptr(), size_of::<sys::sc_tcp_options>()) },
            sys::SC_OK
        );
        let raw = unsafe { raw.assume_init() };
        Self {
            listen_address: String::from_utf8_lossy(unsafe { borrowed_bytes(raw.listen_address) })
                .into_owned(),
            port: raw.port,
            io_threads: raw.io_threads,
            max_connections: raw.max_connections,
            max_event_count: raw.max_event_count,
            max_event_bytes: raw.max_event_bytes,
            connection_send_bytes: raw.connection_send_bytes,
            total_send_bytes: raw.total_send_bytes,
            max_connection_event_count: raw.max_connection_event_count,
            max_connection_event_bytes: raw.max_connection_event_bytes,
            ipv6_only: true,
        }
    }
}
impl Options {
    fn raw(&self) -> sys::sc_tcp_options {
        sys::sc_tcp_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_tcp_options>() as u32,
            listen_address: bytes(self.listen_address.as_bytes()),
            port: self.port,
            reserved: 0,
            io_threads: self.io_threads,
            max_connections: self.max_connections,
            max_event_count: self.max_event_count,
            max_event_bytes: self.max_event_bytes,
            connection_send_bytes: self.connection_send_bytes,
            total_send_bytes: self.total_send_bytes,
            max_connection_event_count: self.max_connection_event_count,
            max_connection_event_bytes: self.max_connection_event_bytes,
        }
    }
}
pub struct TcpServer(NonNull<sys::sc_tcp_server>, reactor::Lease);
// C ABI operations are thread-safe; borrowed Rust methods exclude destruction.
unsafe impl Send for TcpServer {}
unsafe impl Sync for TcpServer {}
impl TcpServer {
    pub(crate) fn native_handle(&self) -> *mut sys::sc_tcp_server {
        self.0.as_ptr()
    }
    pub fn new(options: &Options) -> Result<Self> {
        verify_abi()?;
        let readiness = reactor::Lease::new()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { sys::sc_tcp_server_create_ex(&options.raw(), u32::from(options.ipv6_only), &mut out) })?;
        Ok(Self(pointer(out)?, readiness))
    }
    pub fn start(&mut self) -> Result<()> {
        check(unsafe { sys::sc_tcp_server_start(self.0.as_ptr()) })
    }
    pub fn port(&self) -> u16 {
        unsafe { sys::sc_tcp_server_port(self.0.as_ptr()) }
    }
    pub fn local_endpoint(&self) -> Result<crate::endpoint::Endpoint> {
        let mut raw = sys::sc_ip_endpoint::default();
        check(unsafe { sys::sc_tcp_server_local_endpoint(self.0.as_ptr(), &mut raw) })?;
        crate::endpoint::Endpoint::from_raw(raw).ok_or(Error::PLATFORM)
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { sys::sc_tcp_server_stop(self.0.as_ptr()) })
    }
    pub fn accept_timeout(&self, timeout: Duration) -> Result<Connection> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_tcp_server_accept(self.0.as_ptr(), timeout_ms(timeout)?, &mut out)
        })?;
        Ok(Connection(Arc::new(ConnectionInner {
            handle: pointer(out)?,
            _readiness: self.1.clone(),
        })))
    }
    pub async fn accept(&mut self) -> Result<Connection> {
        reactor::poll_fn(
            || self.accept_timeout(Duration::ZERO),
            |notifier, key, out| unsafe {
                sys::sc_tcp_server_subscribe(self.native_handle(), notifier, key, out)
            },
        )?
        .await
    }
}
impl Drop for TcpServer {
    fn drop(&mut self) {
        unsafe { sys::sc_tcp_server_destroy(self.0.as_ptr()) };
    }
}
struct ConnectionInner {
    handle: NonNull<sys::sc_tcp_connection>,
    _readiness: reactor::Lease,
}
unsafe impl Send for ConnectionInner {}
unsafe impl Sync for ConnectionInner {}
impl Drop for ConnectionInner {
    fn drop(&mut self) {
        unsafe { sys::sc_tcp_connection_destroy(self.handle.as_ptr()) };
    }
}
/// Last clone closes the connection. A full receive budget (this connection's
/// or the server-wide one) pauses its reads until held events are dropped; it
/// does not close the connection. pause/resume is a separate caller pause that
/// cannot lift a full budget. Either way one already-posted read is allowed.
#[derive(Clone)]
pub struct Connection(Arc<ConnectionInner>);
impl Connection {
    pub(crate) fn native_handle(&self) -> *mut sys::sc_tcp_connection {
        self.0.handle.as_ptr()
    }
    /// Immutable transport metadata, still available after the socket closes.
    pub fn endpoints(&self) -> Result<(crate::endpoint::Endpoint, crate::endpoint::Endpoint)> {
        let mut local = sys::sc_ip_endpoint::default();
        let mut remote = sys::sc_ip_endpoint::default();
        check(unsafe { sys::sc_tcp_connection_endpoints(self.0.handle.as_ptr(), &mut local, &mut remote) })?;
        Ok((crate::endpoint::Endpoint::from_raw(local).ok_or(Error::PLATFORM)?,
            crate::endpoint::Endpoint::from_raw(remote).ok_or(Error::PLATFORM)?))
    }
    pub fn id(&self) -> u64 {
        unsafe { sys::sc_tcp_connection_id(self.0.handle.as_ptr()) }
    }
    pub fn send(&self, data: &[u8]) -> Result<()> {
        check(unsafe { sys::sc_tcp_connection_send(self.0.handle.as_ptr(), bytes(data)) })
    }
    /// Sends this entire slice atomically or waits; an impossible slice returns
    /// TooLarge. Applications split large transfers according to their framing.
    pub async fn send_async(&self, data: &[u8]) -> Result<()> {
        loop {
            match self.send(data) {
                Err(Error::WOULD_BLOCK) => self.wait_capacity(data.len())?.wait().await?,
                result => return result,
            }
        }
    }
    pub fn close(&self) {
        unsafe { sys::sc_tcp_connection_close(self.0.handle.as_ptr()) };
    }
    pub fn pause_receive(&self) -> Result<()> {
        check(unsafe { sys::sc_tcp_connection_pause(self.0.handle.as_ptr()) })
    }
    pub fn resume_receive(&self) -> Result<()> {
        check(unsafe { sys::sc_tcp_connection_resume(self.0.handle.as_ptr()) })
    }
    pub fn wait_capacity(&self, bytes: usize) -> Result<CapacityWait> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_tcp_connection_wait_capacity(self.0.handle.as_ptr(), bytes, &mut out)
        })?;
        CapacityWait::from_raw(out)
    }
    pub fn next_timeout(&self, timeout: Duration) -> Result<Event> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            sys::sc_tcp_connection_next(self.0.handle.as_ptr(), timeout_ms(timeout)?, &mut out)
        })?;
        let mut event = Event {
            handle: pointer(out)?,
            view: unsafe { std::mem::zeroed() },
        };
        event.view.abi_version = sys::SC_ABI_VERSION;
        event.view.struct_size = size_of::<sys::sc_tcp_event_view>() as u32;
        check(unsafe { sys::sc_tcp_event_get_view(event.handle.as_ptr(), &mut event.view) })?;
        Ok(event)
    }
    pub async fn next(&mut self) -> Result<Event> {
        reactor::poll_fn(
            || self.next_timeout(Duration::ZERO),
            |notifier, key, out| unsafe {
                sys::sc_tcp_connection_subscribe(self.0.handle.as_ptr(), notifier, key, out)
            },
        )?
        .await
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventKind {
    Bytes,
    Closed,
}
pub struct Event {
    handle: NonNull<sys::sc_tcp_event>,
    view: sys::sc_tcp_event_view,
}
// Views are immutable, point into the native owned event, and are only borrowed
// through &self. Drop requires exclusive access and returns its queue charge.
unsafe impl Send for Event {}
unsafe impl Sync for Event {}
impl Event {
    pub fn kind(&self) -> Result<EventKind> {
        match self.view.kind {
            sys::SC_TCP_BYTES => Ok(EventKind::Bytes),
            sys::SC_TCP_CLOSED => Ok(EventKind::Closed),
            _ => Err(Error::INVALID_FORMAT),
        }
    }
    pub fn bytes(&self) -> &[u8] {
        unsafe { borrowed_bytes(self.view.bytes) }
    }
    pub fn status(&self) -> Result<()> {
        check(self.view.status)
    }
}
impl Drop for Event {
    fn drop(&mut self) {
        unsafe { sys::sc_tcp_event_destroy(self.handle.as_ptr()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, TcpListener, TcpStream};

    /// Explicitly skips a test when IPv6 loopback is unavailable. The notice is
    /// written to stderr directly so the test harness does not capture it, and
    /// SERVERCORE_REQUIRE_IPV6 (set by CI) turns the skip into a failure.
    fn ipv6_loopback_or_skip(test: &str) -> bool {
        if std::net::UdpSocket::bind("[::1]:0").is_ok() {
            return true;
        }
        if std::env::var_os("SERVERCORE_REQUIRE_IPV6").is_some_and(|v| !v.is_empty() && v != "0") {
            panic!("{test}: IPv6 loopback ::1 is unavailable while SERVERCORE_REQUIRE_IPV6 requires it");
        }
        let _ = std::io::Write::write_all(
            &mut std::io::stderr(),
            format!("SKIPPED {test}: IPv6 loopback ::1 is unavailable\n").as_bytes(),
        );
        false
    }

    #[test]
    fn connection_event_budget_reaches_native_options() {
        // Defaults are read back from sc_tcp_options_init (Net.h: 64, 1 MiB).
        let defaults = Options::default();
        assert_eq!(defaults.max_connection_event_count, 64);
        assert_eq!(defaults.max_connection_event_bytes, 1024 * 1024);
        for invalid in [
            Options { max_connection_event_count: 0, ..Options::default() },
            Options { max_connection_event_count: 65537, ..Options::default() },
            Options { max_connection_event_bytes: 0, ..Options::default() },
        ] {
            assert!(matches!(TcpServer::new(&invalid), Err(Error::INVALID_ARGUMENT)));
        }
        let mut limits = Options::default();
        limits.max_connection_event_count = 65536;
        assert!(TcpServer::new(&limits).is_ok());
    }

    #[test]
    fn ipv6_and_dual_stack_endpoints_survive_close() {
        if !ipv6_loopback_or_skip("ipv6_and_dual_stack_endpoints_survive_close") {
            return;
        }
        for only in [true, false] {
            let reserved = TcpListener::bind((Ipv6Addr::LOCALHOST, 0)).unwrap();
            let port = reserved.local_addr().unwrap().port();
            drop(reserved);
            let options = Options {
                listen_address: if only { "::1" } else { "::" }.into(),
                port,
                ipv6_only: only,
                ..Options::default()
            };
            let mut server = TcpServer::new(&options).unwrap();
            server.start().unwrap();
            assert_eq!(server.local_endpoint().unwrap().port, port);
            let ip = if only { IpAddr::V6(Ipv6Addr::LOCALHOST) } else { IpAddr::V4(Ipv4Addr::LOCALHOST) };
            let peer = TcpStream::connect(SocketAddr::new(ip, port)).unwrap();
            let connection = server.accept_timeout(Duration::from_secs(3)).unwrap();
            let endpoints = connection.endpoints().unwrap();
            assert_eq!(endpoints.0.port, port);
            assert_eq!(endpoints.1.port, peer.local_addr().unwrap().port());
            assert!(endpoints.1.address.is_ipv6());
            if !only { assert_eq!(endpoints.1.normalized().address, IpAddr::V4(Ipv4Addr::LOCALHOST)); }
            connection.close();
            server.stop().unwrap();
            assert_eq!(connection.endpoints().unwrap(), endpoints);
        }
    }
}
