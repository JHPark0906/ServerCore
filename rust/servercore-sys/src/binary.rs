use crate::{sc_bytes, sc_status};
#[repr(C)]
pub struct sc_binary_reader {
    _private: [u8; 0],
}
#[repr(C)]
pub struct sc_binary_writer {
    _private: [u8; 0],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct sc_protocol_offer {
    pub version: u32,
    pub reserved: u32,
    pub features: u64,
}
unsafe extern "C" {
    pub fn sc_binary_reader_create(
        input: sc_bytes,
        order: u32,
        limit: usize,
        out: *mut *mut sc_binary_reader,
    ) -> sc_status;
    pub fn sc_binary_reader_destroy(value: *mut sc_binary_reader);
    pub fn sc_binary_reader_position(value: *const sc_binary_reader) -> usize;
    pub fn sc_binary_reader_remaining(value: *const sc_binary_reader) -> usize;
    pub fn sc_binary_read_unsigned(
        value: *mut sc_binary_reader,
        width: usize,
        out: *mut u64,
    ) -> sc_status;
    pub fn sc_binary_read_signed(
        value: *mut sc_binary_reader,
        width: usize,
        out: *mut i64,
    ) -> sc_status;
    pub fn sc_binary_read_bool(value: *mut sc_binary_reader, out: *mut u32) -> sc_status;
    pub fn sc_binary_read_f32(value: *mut sc_binary_reader, out: *mut f32) -> sc_status;
    pub fn sc_binary_read_f64(value: *mut sc_binary_reader, out: *mut f64) -> sc_status;
    pub fn sc_binary_read_bytes(
        value: *mut sc_binary_reader,
        length: usize,
        out: *mut sc_bytes,
    ) -> sc_status;
    pub fn sc_binary_read_blob(
        value: *mut sc_binary_reader,
        length: usize,
        out: *mut sc_bytes,
    ) -> sc_status;
    pub fn sc_binary_read_utf8(
        value: *mut sc_binary_reader,
        length: usize,
        out: *mut sc_bytes,
    ) -> sc_status;
    pub fn sc_binary_skip(value: *mut sc_binary_reader, length: usize) -> sc_status;
    pub fn sc_binary_writer_create(
        limit: usize,
        order: u32,
        out: *mut *mut sc_binary_writer,
    ) -> sc_status;
    pub fn sc_binary_writer_destroy(value: *mut sc_binary_writer);
    pub fn sc_binary_writer_clear(value: *mut sc_binary_writer);
    pub fn sc_binary_writer_view(value: *const sc_binary_writer, out: *mut sc_bytes) -> sc_status;
    pub fn sc_binary_write_unsigned(
        value: *mut sc_binary_writer,
        number: u64,
        width: usize,
    ) -> sc_status;
    pub fn sc_binary_write_signed(
        value: *mut sc_binary_writer,
        number: i64,
        width: usize,
    ) -> sc_status;
    pub fn sc_binary_write_bool(value: *mut sc_binary_writer, number: u32) -> sc_status;
    pub fn sc_binary_write_f32(value: *mut sc_binary_writer, number: f32) -> sc_status;
    pub fn sc_binary_write_f64(value: *mut sc_binary_writer, number: f64) -> sc_status;
    pub fn sc_binary_write_bytes(value: *mut sc_binary_writer, data: sc_bytes) -> sc_status;
    pub fn sc_binary_write_blob(
        value: *mut sc_binary_writer,
        data: sc_bytes,
        limit: usize,
    ) -> sc_status;
    pub fn sc_binary_write_utf8(
        value: *mut sc_binary_writer,
        data: sc_bytes,
        limit: usize,
    ) -> sc_status;
    pub fn sc_protocol_negotiate(
        server: *const sc_protocol_offer,
        server_count: usize,
        peer: *const sc_protocol_offer,
        peer_count: usize,
        required: u64,
        out: *mut sc_protocol_offer,
    ) -> sc_status;
}
