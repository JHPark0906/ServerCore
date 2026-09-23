use crate::{sc_bytes, sc_status, sc_tcp_connection, sc_tcp_options, sc_tcp_server};
use std::ffi::c_char;
pub const SC_IP_UNSPECIFIED: u32 = 0;
pub const SC_IP_V4: u32 = 4;
pub const SC_IP_V6: u32 = 6;
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct sc_ip_endpoint {
    pub family: u32,
    pub scope_id: u32,
    pub address: [u8; 16],
    pub port: u16,
    pub reserved: u16,
}
extern "C" {
    pub fn sc_ip_endpoint_parse(address: sc_bytes, port: u16, out: *mut sc_ip_endpoint) -> sc_status;
    pub fn sc_ip_endpoint_format(endpoint: *const sc_ip_endpoint, buffer: *mut c_char, capacity: usize, written: *mut usize) -> sc_status;
    pub fn sc_tcp_server_create_ex(options: *const sc_tcp_options, ipv6_only: u32, out: *mut *mut sc_tcp_server) -> sc_status;
    pub fn sc_tcp_server_local_endpoint(server: *const sc_tcp_server, out: *mut sc_ip_endpoint) -> sc_status;
    pub fn sc_tcp_connection_endpoints(connection: *const sc_tcp_connection, local: *mut sc_ip_endpoint, remote: *mut sc_ip_endpoint) -> sc_status;
}
