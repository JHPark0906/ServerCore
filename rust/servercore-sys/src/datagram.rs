use crate::{sc_bytes, sc_ip_endpoint, sc_notifier, sc_observation, sc_status, sc_subscription};
#[repr(C)]
pub struct sc_udp_transport {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_udp_event {
    _private: [u8; 0],
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct sc_udp_token {
    pub bytes: [u8; 16],
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sc_udp_rate {
    pub bytes_per_interval: u64,
    pub burst_bytes: u64,
    pub interval_ms: u32,
    pub reserved: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_udp_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub listen_address: sc_bytes,
    pub port: u16,
    pub reserved: u16,
    pub ipv6_only: u32,
    pub payload_mode: u32,
    pub max_sessions: usize,
    pub max_event_count: usize,
    pub max_event_bytes: usize,
    pub max_datagrams_per_batch: usize,
    pub max_bytes_per_batch: usize,
    pub total_send_rate: sc_udp_rate,
    pub peer_send_rate: sc_udp_rate,
    pub max_sequence_jump: u64,
    pub allow_endpoint_migration: u32,
    pub reserved2: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_udp_event_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub kind: u32,
    pub payload_mode: u32,
    pub status: sc_status,
    pub binary_type: u32,
    pub session_id: u64,
    pub sequence: u64,
    pub remote_endpoint: sc_ip_endpoint,
    pub payload: sc_bytes,
}
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct sc_udp_metrics {
    pub abi_version: u32,
    pub struct_size: u32,
    pub received_datagrams: u64,
    pub received_bytes: u64,
    pub sent_datagrams: u64,
    pub sent_bytes: u64,
    pub rejected_datagrams: u64,
    pub send_would_block: u64,
    pub socket_errors: u64,
    pub truncated_datagrams: u64,
    pub malformed_datagrams: u64,
    pub unknown_token_datagrams: u64,
    pub replayed_datagrams: u64,
    pub invalid_payload_datagrams: u64,
    pub admission_rejected_datagrams: u64,
    pub stale_datagrams: u64,
    pub callback_failures: u64,
    pub send_rate_limited: u64,
    pub send_limited_global: u64,
    pub send_limited_peer: u64,
    pub send_not_ready: u64,
    pub pump_batches: u64,
    pub pump_failures: u64,
    pub poll_contentions: u64,
    pub event_queue_drops: u64,
    pub registered_sessions: usize,
    pub pending_batches: usize,
    pub retained_events: usize,
    pub retained_event_bytes: usize,
    pub bound: u32,
    pub receiving: u32,
    pub endpoint_mismatch_datagrams: u64,
    pub sequence_jump_datagrams: u64,
}
/// Output: initialize abi_version and struct_size before the call.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_udp_packet_view {
    pub abi_version: u32,
    pub struct_size: u32,
    pub token: sc_udp_token,
    pub sequence: u64,
    pub payload: sc_bytes,
}
// ABI 2 layout of Datagram.h on 64-bit targets.
#[cfg(target_pointer_width = "64")]
const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<sc_udp_options>() == 144);
    assert!(offset_of!(sc_udp_options, peer_send_rate) == 104);
    assert!(offset_of!(sc_udp_options, max_sequence_jump) == 128);
    assert!(offset_of!(sc_udp_options, allow_endpoint_migration) == 136);
    assert!(offset_of!(sc_udp_options, reserved2) == 140);
    assert!(size_of::<sc_udp_metrics>() == 248);
    assert!(offset_of!(sc_udp_metrics, receiving) == 228);
    assert!(offset_of!(sc_udp_metrics, endpoint_mismatch_datagrams) == 232);
    assert!(offset_of!(sc_udp_metrics, sequence_jump_datagrams) == 240);
    assert!(size_of::<sc_udp_packet_view>() == 48);
    assert!(offset_of!(sc_udp_packet_view, token) == 8);
    assert!(offset_of!(sc_udp_packet_view, sequence) == 24);
    assert!(offset_of!(sc_udp_packet_view, payload) == 32);
};
extern "C" {
    pub fn sc_udp_options_init(options: *mut sc_udp_options, size: usize) -> sc_status;
    pub fn sc_udp_transport_create(
        options: *const sc_udp_options,
        out: *mut *mut sc_udp_transport,
    ) -> sc_status;
    pub fn sc_udp_transport_start(value: *mut sc_udp_transport) -> sc_status;
    pub fn sc_udp_transport_stop(value: *mut sc_udp_transport) -> sc_status;
    pub fn sc_udp_transport_destroy(value: *mut sc_udp_transport);
    pub fn sc_udp_transport_local_endpoint(
        value: *const sc_udp_transport,
        out: *mut sc_ip_endpoint,
    ) -> sc_status;
    pub fn sc_udp_transport_remote_endpoint(
        value: *const sc_udp_transport,
        session: u64,
        out: *mut sc_ip_endpoint,
    ) -> sc_status;
    pub fn sc_udp_transport_register(
        value: *mut sc_udp_transport,
        session: u64,
        out: *mut sc_udp_token,
    ) -> sc_status;
    pub fn sc_udp_transport_unregister(value: *mut sc_udp_transport, session: u64);
    pub fn sc_udp_transport_send_json(
        value: *mut sc_udp_transport,
        session: u64,
        payload: sc_bytes,
    ) -> sc_status;
    pub fn sc_udp_transport_send_binary(
        value: *mut sc_udp_transport,
        session: u64,
        kind: u32,
        payload: sc_bytes,
    ) -> sc_status;
    pub fn sc_udp_transport_next(
        value: *mut sc_udp_transport,
        timeout_ms: u32,
        out: *mut *mut sc_udp_event,
    ) -> sc_status;
    pub fn sc_udp_transport_subscribe(
        value: *mut sc_udp_transport,
        notifier: *mut sc_notifier,
        key: u64,
        out: *mut *mut sc_subscription,
    ) -> sc_status;
    pub fn sc_udp_event_get(value: *const sc_udp_event, out: *mut sc_udp_event_view) -> sc_status;
    pub fn sc_udp_event_destroy(value: *mut sc_udp_event);
    pub fn sc_udp_transport_get_metrics(
        value: *const sc_udp_transport,
        out: *mut sc_udp_metrics,
    ) -> sc_status;
    pub fn sc_udp_transport_get_observation(
        value: *const sc_udp_transport,
        out: *mut sc_observation,
    ) -> sc_status;
    pub fn sc_udp_packet_encode(
        token: *const sc_udp_token,
        sequence: u64,
        payload: sc_bytes,
        out: *mut u8,
        capacity: usize,
        written: *mut usize,
    ) -> sc_status;
    pub fn sc_udp_packet_decode(packet: sc_bytes, out: *mut sc_udp_packet_view) -> sc_status;
}
