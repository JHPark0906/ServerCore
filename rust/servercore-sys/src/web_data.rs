use crate::*;

#[repr(C)] pub struct sc_web_fields { _private: [u8; 0] }
#[repr(C)] pub struct sc_web_data_text { _private: [u8; 0] }
#[repr(C)] pub struct sc_multipart { _private: [u8; 0] }
#[repr(C)] pub struct sc_multipart_event { _private: [u8; 0] }
pub const SC_FIELDS_QUERY: u32 = 0;
pub const SC_FIELDS_FORM: u32 = 1;
pub const SC_FIELDS_COOKIE: u32 = 2;
pub const SC_COOKIE_UNSPECIFIED: u32 = 0;
pub const SC_COOKIE_LAX: u32 = 1;
pub const SC_COOKIE_STRICT: u32 = 2;
pub const SC_COOKIE_NONE: u32 = 3;
pub const SC_MULTIPART_PART_BEGIN: u32 = 0;
pub const SC_MULTIPART_DATA: u32 = 1;
pub const SC_MULTIPART_PART_END: u32 = 2;
#[repr(C)] #[derive(Clone, Copy)]
pub struct sc_web_field_limits {
    pub abi_version: u32, pub struct_size: u32,
    pub max_bytes: usize, pub max_fields: usize, pub max_name_bytes: usize, pub max_value_bytes: usize,
}
#[repr(C)] #[derive(Clone, Copy)]
pub struct sc_web_cookie_options {
    pub abi_version: u32, pub struct_size: u32, pub domain: sc_bytes, pub path: sc_bytes,
    pub max_age_seconds: i64, pub has_max_age: u32, pub same_site: u32,
    pub secure: u32, pub http_only: u32, pub max_bytes: usize,
}
#[repr(C)] #[derive(Clone, Copy)]
pub struct sc_multipart_options {
    pub abi_version: u32, pub struct_size: u32,
    pub max_parts: usize, pub max_files: usize, pub max_headers: usize, pub max_header_bytes: usize,
    pub max_part_bytes: u64, pub max_file_bytes: u64, pub max_total_bytes: u64,
    pub max_retained_bytes: usize, pub max_event_bytes: usize,
}
#[repr(C)] #[derive(Clone, Copy)]
pub struct sc_multipart_event_view {
    pub abi_version: u32, pub struct_size: u32, pub kind: u32, pub part_index: u64,
    pub name: sc_bytes, pub filename: sc_bytes, pub content_type: sc_bytes, pub data: sc_bytes,
    pub has_filename: u32,
}
extern "C" {
    pub fn sc_web_field_limits_init(out: *mut sc_web_field_limits, size: usize) -> sc_status;
    pub fn sc_web_cookie_options_init(out: *mut sc_web_cookie_options, size: usize) -> sc_status;
    pub fn sc_multipart_options_init(out: *mut sc_multipart_options, size: usize) -> sc_status;
    pub fn sc_web_parse_fields(kind: u32, input: sc_bytes, limits: *const sc_web_field_limits, out: *mut *mut sc_web_fields) -> sc_status;
    pub fn sc_web_fields_count(value: *const sc_web_fields) -> usize;
    pub fn sc_web_fields_at(value: *const sc_web_fields, index: usize, out: *mut sc_header) -> sc_status;
    pub fn sc_web_fields_destroy(value: *mut sc_web_fields);
    pub fn sc_web_percent_decode(input: sc_bytes, plus_as_space: u32, max_bytes: usize, out: *mut *mut sc_web_data_text) -> sc_status;
    pub fn sc_web_percent_encode(input: sc_bytes, form_mode: u32, max_bytes: usize, out: *mut *mut sc_web_data_text) -> sc_status;
    pub fn sc_web_set_cookie(name: sc_bytes, value: sc_bytes, options: *const sc_web_cookie_options, out: *mut *mut sc_web_data_text) -> sc_status;
    pub fn sc_web_data_text_view(value: *const sc_web_data_text, out: *mut sc_bytes) -> sc_status;
    pub fn sc_web_data_text_destroy(value: *mut sc_web_data_text);
    pub fn sc_multipart_create(content_type: sc_bytes, options: *const sc_multipart_options, out: *mut *mut sc_multipart) -> sc_status;
    pub fn sc_multipart_feed(value: *mut sc_multipart, input: sc_bytes, consumed: *mut usize) -> sc_status;
    pub fn sc_multipart_read(value: *mut sc_multipart, out: *mut *mut sc_multipart_event) -> sc_status;
    pub fn sc_multipart_finish(value: *mut sc_multipart) -> sc_status;
    pub fn sc_multipart_cancel(value: *mut sc_multipart);
    pub fn sc_multipart_retained_bytes(value: *const sc_multipart) -> usize;
    pub fn sc_multipart_destroy(value: *mut sc_multipart);
    pub fn sc_multipart_event_get(value: *const sc_multipart_event, out: *mut sc_multipart_event_view) -> sc_status;
    pub fn sc_multipart_event_header_count(value: *const sc_multipart_event) -> usize;
    pub fn sc_multipart_event_header_at(value: *const sc_multipart_event, index: usize, out: *mut sc_header) -> sc_status;
    pub fn sc_multipart_event_destroy(value: *mut sc_multipart_event);
}
