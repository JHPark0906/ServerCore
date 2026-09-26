//! Token-bound UDP messages, bounded owned receive events, and native readiness.
//! Tokens require a protected reliable control channel. This adds no encryption,
//! acknowledgement, retransmission, fragmentation, or ordered delivery.
use crate::endpoint::Endpoint;
use crate::sys::datagram as ffi;
use crate::{
    borrowed_bytes, bytes, check, pointer, reactor, sys, timeout_ms, verify_abi, Error, Result,
};
use std::{
    mem::{size_of, MaybeUninit},
    ptr::NonNull,
    time::Duration,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PayloadMode {
    Json,
    Binary,
}
#[derive(Clone, Copy, Debug)]
pub struct SendRate {
    pub bytes_per_interval: u64,
    pub burst_bytes: u64,
    pub interval: Duration,
}
impl Default for SendRate {
    fn default() -> Self {
        Self {
            bytes_per_interval: 0,
            burst_bytes: 0,
            interval: Duration::from_secs(1),
        }
    }
}
impl SendRate {
    fn raw(self) -> Result<ffi::sc_udp_rate> {
        Ok(ffi::sc_udp_rate {
            bytes_per_interval: self.bytes_per_interval,
            burst_bytes: self.burst_bytes,
            interval_ms: timeout_ms(self.interval)?,
            reserved: 0,
        })
    }
}
#[derive(Clone, Debug)]
pub struct Options {
    pub listen_address: String,
    pub port: u16,
    pub ipv6_only: bool,
    pub payload_mode: PayloadMode,
    pub max_sessions: usize,
    pub max_event_count: usize,
    pub max_event_bytes: usize,
    pub max_datagrams_per_batch: usize,
    pub max_bytes_per_batch: usize,
    pub total_send_rate: SendRate,
    pub peer_send_rate: SendRate,
    /// How far one packet may advance a session's receive sequence (native
    /// default 1024; 0 is rejected). Larger jumps are dropped and counted in
    /// `sequence_jump_datagrams`.
    pub max_sequence_jump: u64,
    /// Lets a valid packet from a new source move the session's return
    /// endpoint (native default false). There is no path validation, so this
    /// permits reflection; enable it only for NAT rebinding, with
    /// `peer_send_rate`. Refused moves count in `endpoint_mismatch_datagrams`.
    pub allow_endpoint_migration: bool,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            listen_address: "127.0.0.1".into(),
            port: 0,
            ipv6_only: true,
            payload_mode: PayloadMode::Json,
            max_sessions: 256,
            max_event_count: 1024,
            max_event_bytes: 4 * 1024 * 1024,
            max_datagrams_per_batch: 256,
            max_bytes_per_batch: 256 * 1024,
            total_send_rate: SendRate::default(),
            peer_send_rate: SendRate::default(),
            max_sequence_jump: 1024,
            allow_endpoint_migration: false,
        }
    }
}
impl Options {
    fn raw(&self) -> Result<ffi::sc_udp_options> {
        Ok(ffi::sc_udp_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<ffi::sc_udp_options>() as u32,
            listen_address: bytes(self.listen_address.as_bytes()),
            port: self.port,
            reserved: 0,
            ipv6_only: u32::from(self.ipv6_only),
            payload_mode: if self.payload_mode == PayloadMode::Json {
                0
            } else {
                1
            },
            max_sessions: self.max_sessions,
            max_event_count: self.max_event_count,
            max_event_bytes: self.max_event_bytes,
            max_datagrams_per_batch: self.max_datagrams_per_batch,
            max_bytes_per_batch: self.max_bytes_per_batch,
            total_send_rate: self.total_send_rate.raw()?,
            peer_send_rate: self.peer_send_rate.raw()?,
            max_sequence_jump: self.max_sequence_jump,
            allow_endpoint_migration: u32::from(self.allow_endpoint_migration),
            reserved2: 0,
        })
    }
}
/// A token is a bearer secret; do not log it or distribute it over untrusted UDP.
#[derive(Clone, Copy, Eq, PartialEq)]
pub struct Token(pub [u8; 16]);
pub type Metrics = ffi::sc_udp_metrics;
pub struct Transport {
    handle: NonNull<ffi::sc_udp_transport>,
    _readiness: reactor::Lease,
}
unsafe impl Send for Transport {}
unsafe impl Sync for Transport {}
impl Transport {
    fn native_handle(&self) -> *mut ffi::sc_udp_transport {
        self.handle.as_ptr()
    }
    pub fn new(options: &Options) -> Result<Self> {
        verify_abi()?;
        let readiness = reactor::Lease::new()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { ffi::sc_udp_transport_create(&options.raw()?, &mut out) })?;
        Ok(Self {
            handle: pointer(out)?,
            _readiness: readiness,
        })
    }
    pub fn start(&mut self) -> Result<()> {
        check(unsafe { ffi::sc_udp_transport_start(self.native_handle()) })
    }
    pub fn stop(&self) -> Result<()> {
        check(unsafe { ffi::sc_udp_transport_stop(self.native_handle()) })
    }
    pub fn local_endpoint(&self) -> Result<Endpoint> {
        let mut value = sys::sc_ip_endpoint::default();
        check(unsafe { ffi::sc_udp_transport_local_endpoint(self.native_handle(), &mut value) })?;
        Endpoint::from_raw(value).ok_or(Error::PLATFORM)
    }
    pub fn remote_endpoint(&self, session: u64) -> Result<Endpoint> {
        let mut value = sys::sc_ip_endpoint::default();
        check(unsafe {
            ffi::sc_udp_transport_remote_endpoint(self.native_handle(), session, &mut value)
        })?;
        Endpoint::from_raw(value).ok_or(Error::PLATFORM)
    }
    pub fn register(&self, session: u64) -> Result<Token> {
        let mut token = ffi::sc_udp_token::default();
        check(unsafe {
            ffi::sc_udp_transport_register(self.native_handle(), session, &mut token)
        })?;
        Ok(Token(token.bytes))
    }
    pub fn unregister(&self, session: u64) {
        unsafe { ffi::sc_udp_transport_unregister(self.native_handle(), session) }
    }
    /// WouldBlock means no datagram was accepted; retry under an application timer.
    pub fn send_json(&self, session: u64, envelope: &[u8]) -> Result<()> {
        check(unsafe {
            ffi::sc_udp_transport_send_json(self.native_handle(), session, bytes(envelope))
        })
    }
    pub fn send_binary(&self, session: u64, kind: u32, payload: &[u8]) -> Result<()> {
        check(unsafe {
            ffi::sc_udp_transport_send_binary(self.native_handle(), session, kind, bytes(payload))
        })
    }
    pub fn next_timeout(&self, timeout: Duration) -> Result<Event> {
        let mut out = std::ptr::null_mut();
        check(unsafe {
            ffi::sc_udp_transport_next(self.native_handle(), timeout_ms(timeout)?, &mut out)
        })?;
        let event = Event(pointer(out)?);
        event.view()?;
        Ok(event)
    }
    pub async fn next(&mut self) -> Result<Event> {
        reactor::poll_fn(
            || self.next_timeout(Duration::ZERO),
            |notifier, key, out| unsafe {
                ffi::sc_udp_transport_subscribe(self.native_handle(), notifier, key, out)
            },
        )?
        .await
    }
    pub fn metrics(&self) -> Result<Metrics> {
        let mut value = Metrics {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<Metrics>() as u32,
            ..Default::default()
        };
        check(unsafe { ffi::sc_udp_transport_get_metrics(self.native_handle(), &mut value) })?;
        Ok(value)
    }
    pub fn observation(&self) -> Result<crate::observability::Observation> {
        crate::observability::Observation::read(|out| unsafe {
            ffi::sc_udp_transport_get_observation(self.native_handle(), out)
        })
    }
}
impl Drop for Transport {
    fn drop(&mut self) {
        unsafe { ffi::sc_udp_transport_destroy(self.native_handle()) }
    }
}
/// Owns its bytes and queue credit even after the transport is dropped.
pub struct Event(NonNull<ffi::sc_udp_event>);
unsafe impl Send for Event {}
unsafe impl Sync for Event {}
impl Event {
    fn view(&self) -> Result<ffi::sc_udp_event_view> {
        let mut out = MaybeUninit::<ffi::sc_udp_event_view>::zeroed();
        unsafe {
            (*out.as_mut_ptr()).abi_version = sys::SC_ABI_VERSION;
            (*out.as_mut_ptr()).struct_size = size_of::<ffi::sc_udp_event_view>() as u32;
            check(ffi::sc_udp_event_get(self.0.as_ptr(), out.as_mut_ptr()))?;
            Ok(out.assume_init())
        }
    }
    pub fn is_closed(&self) -> bool {
        self.view().expect("owned event").kind == 1
    }
    pub fn status(&self) -> Result<()> {
        check(self.view()?.status)
    }
    pub fn session_id(&self) -> u64 {
        self.view().expect("owned event").session_id
    }
    pub fn sequence(&self) -> u64 {
        self.view().expect("owned event").sequence
    }
    pub fn payload_mode(&self) -> PayloadMode {
        if self.view().expect("owned event").payload_mode == 0 {
            PayloadMode::Json
        } else {
            PayloadMode::Binary
        }
    }
    pub fn binary_type(&self) -> u32 {
        self.view().expect("owned event").binary_type
    }
    pub fn remote_endpoint(&self) -> Option<Endpoint> {
        Endpoint::from_raw(self.view().expect("owned event").remote_endpoint)
    }
    pub fn payload(&self) -> &[u8] {
        unsafe { borrowed_bytes(self.view().expect("owned event").payload) }
    }
}
impl Drop for Event {
    fn drop(&mut self) {
        unsafe { ffi::sc_udp_event_destroy(self.0.as_ptr()) }
    }
}
/// Uses the native codec; payload is the JSON envelope or full binary envelope.
pub fn encode_packet(token: Token, sequence: u64, payload: &[u8]) -> Result<Vec<u8>> {
    let mut output = vec![0u8; 1200];
    let mut written = 0;
    check(unsafe {
        ffi::sc_udp_packet_encode(
            &ffi::sc_udp_token { bytes: token.0 },
            sequence,
            bytes(payload),
            output.as_mut_ptr(),
            output.len(),
            &mut written,
        )
    })?;
    output.truncate(written);
    Ok(output)
}
pub struct Packet<'a> {
    pub token: Token,
    pub sequence: u64,
    pub payload: &'a [u8],
}
pub fn decode_packet(packet: &[u8]) -> Result<Packet<'_>> {
    let mut out = MaybeUninit::<ffi::sc_udp_packet_view>::zeroed();
    unsafe {
        (*out.as_mut_ptr()).abi_version = sys::SC_ABI_VERSION;
        (*out.as_mut_ptr()).struct_size = size_of::<ffi::sc_udp_packet_view>() as u32;
    }
    check(unsafe { ffi::sc_udp_packet_decode(bytes(packet), out.as_mut_ptr()) })?;
    let out = unsafe { out.assume_init() };
    Ok(Packet {
        token: Token(out.token.bytes),
        sequence: out.sequence,
        payload: unsafe { borrowed_bytes(out.payload) },
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::UdpSocket;
    #[test]
    fn owned_udp_roundtrip_and_close() {
        let mut server = Transport::new(&Options::default()).unwrap();
        server.start().unwrap();
        let token = server.register(42).unwrap();
        let peer = UdpSocket::bind("127.0.0.1:0").unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let payload = br#"{"type":"Ping","body":{}}"#;
        let packet = encode_packet(token, 1, payload).unwrap();
        peer.send_to(&packet, server.local_endpoint().unwrap().socket_addr())
            .unwrap();
        let event = server.next_timeout(Duration::from_secs(5)).unwrap();
        assert_eq!(event.session_id(), 42);
        assert_eq!(event.payload(), payload);
        assert_eq!(
            event.remote_endpoint().unwrap().socket_addr(),
            peer.local_addr().unwrap()
        );
        server.send_json(42, payload).unwrap();
        let mut buffer = [0u8; 1200];
        let (count, _) = peer.recv_from(&mut buffer).unwrap();
        let reply = decode_packet(&buffer[..count]).unwrap();
        assert_eq!(reply.sequence, 1);
        assert_eq!(reply.payload, payload);
        server.stop().unwrap();
        assert!(server.next_timeout(Duration::ZERO).unwrap().is_closed());
        assert!(matches!(
            server.next_timeout(Duration::ZERO),
            Err(Error::CLOSED)
        ));
        drop(server);
        assert_eq!(event.payload(), payload);
    }
    #[test]
    fn option_defaults_match_the_native_initializer() {
        let mut raw = MaybeUninit::<ffi::sc_udp_options>::uninit();
        assert_eq!(
            unsafe { ffi::sc_udp_options_init(raw.as_mut_ptr(), size_of::<ffi::sc_udp_options>()) },
            sys::SC_OK
        );
        let raw = unsafe { raw.assume_init() };
        let defaults = Options::default();
        assert_eq!(defaults.max_sequence_jump, raw.max_sequence_jump);
        assert_eq!(
            u32::from(defaults.allow_endpoint_migration),
            raw.allow_endpoint_migration
        );
        assert!(matches!(
            Transport::new(&Options {
                max_sequence_jump: 0,
                ..Default::default()
            }),
            Err(Error::INVALID_ARGUMENT)
        ));
    }
    #[test]
    fn sequence_jumps_and_source_changes_follow_the_options() {
        let payload = br#"{"type":"Ping","body":{}}"#;
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        let mut server = Transport::new(&Options {
            max_sequence_jump: 4,
            ..Default::default()
        })
        .unwrap();
        server.start().unwrap();
        let token = server.register(9).unwrap();
        let endpoint = server.local_endpoint().unwrap().socket_addr();
        let peer = UdpSocket::bind("127.0.0.1:0").unwrap();
        let other = UdpSocket::bind("127.0.0.1:0").unwrap();
        let send = |socket: &UdpSocket, sequence| {
            socket
                .send_to(&encode_packet(token, sequence, payload).unwrap(), endpoint)
                .unwrap();
        };
        send(&peer, 1);
        assert_eq!(server.next_timeout(Duration::from_secs(5)).unwrap().sequence(), 1);
        // Each is refused whichever of them arrives first: 10 is more than 4
        // past both 1 and 2, and 3 is new from an unbound source.
        send(&peer, 10);
        send(&other, 3);
        send(&peer, 2);
        assert_eq!(server.next_timeout(Duration::from_secs(5)).unwrap().sequence(), 2);
        loop {
            let metrics = server.metrics().unwrap();
            if metrics.sequence_jump_datagrams == 1 && metrics.endpoint_mismatch_datagrams == 1 {
                assert!(metrics.rejected_datagrams >= 2);
                break;
            }
            assert!(
                std::time::Instant::now() < deadline,
                "jump {} mismatch {}",
                metrics.sequence_jump_datagrams,
                metrics.endpoint_mismatch_datagrams
            );
            std::thread::sleep(Duration::from_millis(1));
        }
        server.stop().unwrap();

        let mut server = Transport::new(&Options {
            allow_endpoint_migration: true,
            ..Default::default()
        })
        .unwrap();
        server.start().unwrap();
        let token = server.register(9).unwrap();
        let endpoint = server.local_endpoint().unwrap().socket_addr();
        let send = |socket: &UdpSocket, sequence| {
            socket
                .send_to(&encode_packet(token, sequence, payload).unwrap(), endpoint)
                .unwrap();
        };
        send(&peer, 1);
        assert_eq!(server.next_timeout(Duration::from_secs(5)).unwrap().sequence(), 1);
        send(&other, 2);
        let moved = server.next_timeout(Duration::from_secs(5)).unwrap();
        assert_eq!(moved.sequence(), 2);
        assert_eq!(
            moved.remote_endpoint().unwrap().socket_addr(),
            other.local_addr().unwrap()
        );
        assert_eq!(server.metrics().unwrap().endpoint_mismatch_datagrams, 0);
        server.stop().unwrap();
    }
    #[test]
    fn held_udp_event_limits_admission_and_future_is_send() {
        fn assert_send<T: Send>(_: T) {}
        let mut server = Transport::new(&Options {
            max_event_count: 1,
            ..Default::default()
        })
        .unwrap();
        server.start().unwrap();
        assert_send(server.next());
        let token = server.register(7).unwrap();
        let peer = UdpSocket::bind("127.0.0.1:0").unwrap();
        let endpoint = server.local_endpoint().unwrap().socket_addr();
        let payload = br#"{"type":"Ping","body":{}}"#;
        peer.send_to(&encode_packet(token, 1, payload).unwrap(), endpoint)
            .unwrap();
        let event = server.next_timeout(Duration::from_secs(5)).unwrap();
        peer.send_to(&encode_packet(token, 2, payload).unwrap(), endpoint)
            .unwrap();
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while server.metrics().unwrap().event_queue_drops == 0 {
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(1));
        }
        drop(event);
        peer.send_to(&encode_packet(token, 2, payload).unwrap(), endpoint)
            .unwrap();
        assert_eq!(
            server
                .next_timeout(Duration::from_secs(5))
                .unwrap()
                .sequence(),
            2
        );
        server.stop().unwrap();
    }
}
