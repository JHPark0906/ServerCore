use crate::*;

#[repr(C)]
pub struct sc_channel { _private: [u8; 0] }
#[repr(C)]
pub struct sc_latest_value { _private: [u8; 0] }
#[repr(C)]
pub struct sc_channel_value { _private: [u8; 0] }
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sc_channel_options {
    pub abi_version: u32,
    pub struct_size: u32,
    pub max_messages: usize,
    pub max_retained_bytes: usize,
}

extern "C" {
    pub fn sc_channel_create(options: *const sc_channel_options, out: *mut *mut sc_channel) -> sc_status;
    pub fn sc_channel_retain(channel: *const sc_channel, out: *mut *mut sc_channel) -> sc_status;
    pub fn sc_channel_destroy(channel: *mut sc_channel);
    pub fn sc_channel_close(channel: *mut sc_channel);
    pub fn sc_channel_try_send(channel: *mut sc_channel, value: sc_bytes) -> sc_status;
    pub fn sc_channel_try_receive(channel: *mut sc_channel, out: *mut *mut sc_channel_value) -> sc_status;
    pub fn sc_channel_size(channel: *const sc_channel) -> usize;
    pub fn sc_channel_retained_bytes(channel: *const sc_channel) -> usize;
    pub fn sc_channel_subscribe_read(channel: *mut sc_channel, notifier: *mut sc_notifier,
        key: u64, out: *mut *mut sc_subscription) -> sc_status;
    pub fn sc_channel_subscribe_write(channel: *mut sc_channel, required_bytes: usize,
        notifier: *mut sc_notifier, key: u64, out: *mut *mut sc_subscription) -> sc_status;
    pub fn sc_latest_value_create(max_retained_bytes: usize, out: *mut *mut sc_latest_value) -> sc_status;
    pub fn sc_latest_value_retain(value: *const sc_latest_value, out: *mut *mut sc_latest_value) -> sc_status;
    pub fn sc_latest_value_destroy(value: *mut sc_latest_value);
    pub fn sc_latest_value_close(value: *mut sc_latest_value);
    pub fn sc_latest_value_publish(value: *mut sc_latest_value, bytes: sc_bytes) -> sc_status;
    pub fn sc_latest_value_read_after(value: *mut sc_latest_value, version: u64,
        out: *mut *mut sc_channel_value) -> sc_status;
    pub fn sc_latest_value_retained_bytes(value: *const sc_latest_value) -> usize;
    pub fn sc_latest_value_subscribe(value: *mut sc_latest_value, version: u64,
        notifier: *mut sc_notifier, key: u64, out: *mut *mut sc_subscription) -> sc_status;
    pub fn sc_channel_value_view(value: *const sc_channel_value, out: *mut sc_bytes) -> sc_status;
    pub fn sc_channel_value_version(value: *const sc_channel_value) -> u64;
    pub fn sc_channel_value_destroy(value: *mut sc_channel_value);
}
