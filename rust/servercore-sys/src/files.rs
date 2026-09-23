use crate::{sc_bytes, sc_status};
#[repr(C)]
pub struct sc_atomic_file {
    _private: [u8; 0],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct sc_atomic_file_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub maximum_bytes: u64,
    pub sync: u32,
    pub reserved: u32,
}
unsafe extern "C" {
    pub fn sc_atomic_file_options_init(
        value: *mut sc_atomic_file_options,
        size: usize,
    ) -> sc_status;
    pub fn sc_file_supports_directory_sync() -> i32;
    pub fn sc_atomic_file_create(
        target: sc_bytes,
        options: *const sc_atomic_file_options,
        out: *mut *mut sc_atomic_file,
    ) -> sc_status;
    pub fn sc_atomic_file_write(value: *mut sc_atomic_file, data: sc_bytes) -> sc_status;
    pub fn sc_atomic_file_commit(value: *mut sc_atomic_file) -> sc_status;
    pub fn sc_atomic_file_committed(value: *const sc_atomic_file) -> i32;
    pub fn sc_atomic_file_written(value: *const sc_atomic_file) -> u64;
    pub fn sc_atomic_file_cancel(value: *mut sc_atomic_file) -> sc_status;
    pub fn sc_atomic_file_destroy(value: *mut sc_atomic_file);
}
