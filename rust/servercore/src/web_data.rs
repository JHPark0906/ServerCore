//! Bounded request-data parsing backed by the native implementation.
//! Duplicates keep their input order. Multipart output owns its byte credit
//! until dropped, including after the parser is dropped. Filenames are metadata.
use crate::{borrowed_bytes, bytes, check, pointer, sys, verify_abi, Error, Result};
use std::{mem::size_of, ptr::NonNull};

#[derive(Clone, Copy, Debug)]
pub struct FieldLimits {
    pub max_bytes: usize, pub max_fields: usize, pub max_name_bytes: usize, pub max_value_bytes: usize,
}
impl Default for FieldLimits {
    fn default() -> Self { Self { max_bytes: 65536, max_fields: 128, max_name_bytes: 1024, max_value_bytes: 65536 } }
}
/// Immutable UTF-8 fields. No duplicate is silently overwritten.
pub struct Fields { handle: NonNull<sys::sc_web_fields> }
// Native output is immutable; all views borrow this live owner.
unsafe impl Send for Fields {}
unsafe impl Sync for Fields {}
impl Fields {
    pub fn len(&self) -> usize { unsafe { sys::sc_web_fields_count(self.handle.as_ptr()) } }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, index: usize) -> Result<(&str, &str)> {
        let mut field = sys::sc_header { name: bytes(&[]), value: bytes(&[]) };
        check(unsafe { sys::sc_web_fields_at(self.handle.as_ptr(), index, &mut field) })?;
        Ok((std::str::from_utf8(unsafe { borrowed_bytes(field.name) }).map_err(|_| Error::INVALID_FORMAT)?,
            std::str::from_utf8(unsafe { borrowed_bytes(field.value) }).map_err(|_| Error::INVALID_FORMAT)?))
    }
}
impl Drop for Fields { fn drop(&mut self) { unsafe { sys::sc_web_fields_destroy(self.handle.as_ptr()) }; } }
fn parse(kind: u32, input: &[u8], limits: FieldLimits) -> Result<Fields> {
    verify_abi()?;
    let options = sys::sc_web_field_limits { abi_version: sys::SC_ABI_VERSION,
        struct_size: size_of::<sys::sc_web_field_limits>() as u32, max_bytes: limits.max_bytes,
        max_fields: limits.max_fields, max_name_bytes: limits.max_name_bytes, max_value_bytes: limits.max_value_bytes };
    let mut raw = std::ptr::null_mut();
    check(unsafe { sys::sc_web_parse_fields(kind, bytes(input), &options, &mut raw) })?;
    Ok(Fields { handle: pointer(raw)? })
}
/// Parses the query of a whole request target; '+' stays literal.
pub fn parse_query(target: &str, limits: FieldLimits) -> Result<Fields> { parse(sys::SC_FIELDS_QUERY, target.as_bytes(), limits) }
/// Parses an application/x-www-form-urlencoded body; '+' becomes space.
pub fn parse_form(body: &[u8], limits: FieldLimits) -> Result<Fields> { parse(sys::SC_FIELDS_FORM, body, limits) }
/// Cookie values are strict ASCII cookie-octets, without percent decoding.
pub fn parse_cookies(header: &[u8], limits: FieldLimits) -> Result<Fields> { parse(sys::SC_FIELDS_COOKIE, header, limits) }

/// An owned native UTF-8 string, borrowing without another payload copy.
pub struct Text { handle: NonNull<sys::sc_web_data_text>, view: sys::sc_bytes }
unsafe impl Send for Text {}
unsafe impl Sync for Text {}
impl Text {
    fn from_raw(raw: *mut sys::sc_web_data_text) -> Result<Self> {
        let handle = pointer(raw)?;
        let mut view = bytes(&[]);
        if let Err(error) = check(unsafe { sys::sc_web_data_text_view(handle.as_ptr(), &mut view) }) {
            unsafe { sys::sc_web_data_text_destroy(handle.as_ptr()) }; return Err(error);
        }
        Ok(Self { handle, view })
    }
    pub fn as_str(&self) -> &str {
        // Native percent codecs validate UTF-8; cookie serialization emits ASCII.
        unsafe { std::str::from_utf8_unchecked(borrowed_bytes(self.view)) }
    }
}
impl Drop for Text { fn drop(&mut self) { unsafe { sys::sc_web_data_text_destroy(self.handle.as_ptr()) }; } }
pub fn percent_decode(input: &[u8], plus_as_space: bool, max_bytes: usize) -> Result<Text> {
    verify_abi()?; let mut raw = std::ptr::null_mut();
    check(unsafe { sys::sc_web_percent_decode(bytes(input), plus_as_space as u32, max_bytes, &mut raw) })?;
    Text::from_raw(raw)
}
pub fn percent_encode(input: &str, form_mode: bool, max_bytes: usize) -> Result<Text> {
    verify_abi()?; let mut raw = std::ptr::null_mut();
    check(unsafe { sys::sc_web_percent_encode(bytes(input.as_bytes()), form_mode as u32, max_bytes, &mut raw) })?;
    Text::from_raw(raw)
}
#[derive(Clone, Copy, Debug)]
pub enum SameSite { Unspecified, Lax, Strict, None }
#[derive(Clone, Debug)]
pub struct CookieOptions {
    pub domain: String, pub path: String, pub max_age_seconds: Option<i64>,
    pub same_site: SameSite, pub secure: bool, pub http_only: bool, pub max_bytes: usize,
}
impl Default for CookieOptions {
    fn default() -> Self { Self { domain: String::new(), path: "/".into(), max_age_seconds: None,
        same_site: SameSite::Lax, secure: false, http_only: true, max_bytes: 4096 } }
}
/// Produces one Set-Cookie field value; callers append separate header fields.
pub fn set_cookie(name: &str, value: &str, options: &CookieOptions) -> Result<Text> {
    verify_abi()?;
    let native = sys::sc_web_cookie_options { abi_version: sys::SC_ABI_VERSION,
        struct_size: size_of::<sys::sc_web_cookie_options>() as u32, domain: bytes(options.domain.as_bytes()),
        path: bytes(options.path.as_bytes()), max_age_seconds: options.max_age_seconds.unwrap_or(0),
        has_max_age: options.max_age_seconds.is_some() as u32, same_site: match options.same_site {
            SameSite::Unspecified => sys::SC_COOKIE_UNSPECIFIED, SameSite::Lax => sys::SC_COOKIE_LAX,
            SameSite::Strict => sys::SC_COOKIE_STRICT, SameSite::None => sys::SC_COOKIE_NONE },
        secure: options.secure as u32, http_only: options.http_only as u32, max_bytes: options.max_bytes };
    let mut raw = std::ptr::null_mut();
    check(unsafe { sys::sc_web_set_cookie(bytes(name.as_bytes()), bytes(value.as_bytes()), &native, &mut raw) })?;
    Text::from_raw(raw)
}

#[derive(Clone, Copy, Debug)]
pub struct MultipartOptions {
    pub max_parts: usize, pub max_files: usize, pub max_headers: usize, pub max_header_bytes: usize,
    pub max_part_bytes: u64, pub max_file_bytes: u64, pub max_total_bytes: u64,
    pub max_retained_bytes: usize, pub max_event_bytes: usize,
}
impl Default for MultipartOptions {
    fn default() -> Self { Self { max_parts: 128, max_files: 32, max_headers: 32, max_header_bytes: 16384,
        max_part_bytes: 1048576, max_file_bytes: 67108864, max_total_bytes: 134217728,
        max_retained_bytes: 262144, max_event_bytes: 32768 } }
}
/// Incremental parser. Mutable methods enforce one producer at a time.
/// Drain events and release them before retrying WouldBlock. Finish marks input
/// EOF; only a subsequent successful `read()` returning None validates completion.
pub struct Multipart { handle: NonNull<sys::sc_multipart> }
// The C implementation synchronizes operations; Rust mutation additionally
// requires exclusive access. A shared reference exposes only the byte gauge.
unsafe impl Send for Multipart {}
unsafe impl Sync for Multipart {}
impl Multipart {
    pub fn new(content_type: &str, options: MultipartOptions) -> Result<Self> {
        verify_abi()?;
        let native = sys::sc_multipart_options { abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_multipart_options>() as u32,
            max_parts: options.max_parts, max_files: options.max_files, max_headers: options.max_headers,
            max_header_bytes: options.max_header_bytes, max_part_bytes: options.max_part_bytes,
            max_file_bytes: options.max_file_bytes, max_total_bytes: options.max_total_bytes,
            max_retained_bytes: options.max_retained_bytes, max_event_bytes: options.max_event_bytes };
        let mut raw = std::ptr::null_mut();
        check(unsafe { sys::sc_multipart_create(bytes(content_type.as_bytes()), &native, &mut raw) })?;
        Ok(Self { handle: pointer(raw)? })
    }
    /// Copies a bounded prefix and returns its length. WouldBlock consumes zero.
    pub fn feed(&mut self, input: &[u8]) -> Result<usize> {
        let mut consumed = 0;
        check(unsafe { sys::sc_multipart_feed(self.handle.as_ptr(), bytes(input), &mut consumed) })?;
        Ok(consumed)
    }
    pub fn read(&mut self) -> Result<Option<PartEvent>> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { sys::sc_multipart_read(self.handle.as_ptr(), &mut raw) })?;
        if raw.is_null() { Ok(None) } else { PartEvent::from_raw(raw).map(Some) }
    }
    pub fn finish(&mut self) -> Result<()> { check(unsafe { sys::sc_multipart_finish(self.handle.as_ptr()) }) }
    pub fn cancel(&mut self) { unsafe { sys::sc_multipart_cancel(self.handle.as_ptr()) }; }
    pub fn retained_bytes(&self) -> usize { unsafe { sys::sc_multipart_retained_bytes(self.handle.as_ptr()) } }
}
impl Drop for Multipart { fn drop(&mut self) { unsafe { sys::sc_multipart_destroy(self.handle.as_ptr()) }; } }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PartKind { Begin, Data, End }
/// An immutable owned event. Holding it also holds its parser byte credit.
pub struct PartEvent { handle: NonNull<sys::sc_multipart_event>, view: sys::sc_multipart_event_view }
unsafe impl Send for PartEvent {}
unsafe impl Sync for PartEvent {}
impl PartEvent {
    fn from_raw(raw: *mut sys::sc_multipart_event) -> Result<Self> {
        let handle = pointer(raw)?;
        let mut view = sys::sc_multipart_event_view { abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<sys::sc_multipart_event_view>() as u32, kind: 0, part_index: 0,
            name: bytes(&[]), filename: bytes(&[]), content_type: bytes(&[]), data: bytes(&[]), has_filename: 0 };
        if let Err(error) = check(unsafe { sys::sc_multipart_event_get(handle.as_ptr(), &mut view) }) {
            unsafe { sys::sc_multipart_event_destroy(handle.as_ptr()) }; return Err(error);
        }
        Ok(Self { handle, view })
    }
    pub fn kind(&self) -> PartKind { match self.view.kind {
        sys::SC_MULTIPART_PART_BEGIN => PartKind::Begin, sys::SC_MULTIPART_DATA => PartKind::Data, _ => PartKind::End } }
    pub fn part_index(&self) -> u64 { self.view.part_index }
    pub fn name(&self) -> &str { unsafe { std::str::from_utf8_unchecked(borrowed_bytes(self.view.name)) } }
    pub fn filename(&self) -> Option<&str> {
        (self.view.has_filename != 0).then(|| unsafe { std::str::from_utf8_unchecked(borrowed_bytes(self.view.filename)) })
    }
    /// Raw header bytes: media-type parameters may contain non-UTF-8 obs-text.
    pub fn content_type(&self) -> &[u8] { unsafe { borrowed_bytes(self.view.content_type) } }
    pub fn data(&self) -> &[u8] { unsafe { borrowed_bytes(self.view.data) } }
    pub fn header_count(&self) -> usize { unsafe { sys::sc_multipart_event_header_count(self.handle.as_ptr()) } }
    pub fn header(&self, index: usize) -> Result<(&str, &[u8])> {
        let mut field = sys::sc_header { name: bytes(&[]), value: bytes(&[]) };
        check(unsafe { sys::sc_multipart_event_header_at(self.handle.as_ptr(), index, &mut field) })?;
        Ok((unsafe { std::str::from_utf8_unchecked(borrowed_bytes(field.name)) }, unsafe { borrowed_bytes(field.value) }))
    }
}
impl Drop for PartEvent { fn drop(&mut self) { unsafe { sys::sc_multipart_event_destroy(self.handle.as_ptr()) }; } }

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fields_and_cookie_contracts() {
        let query = parse_query("/?a=one+two&a=%252F", FieldLimits::default()).unwrap();
        assert_eq!(query.len(), 2);
        assert_eq!(query.get(0).unwrap(), ("a", "one+two"));
        assert_eq!(query.get(1).unwrap(), ("a", "%2F"));
        let form = parse_form(b"a=one+two", FieldLimits::default()).unwrap();
        assert_eq!(form.get(0).unwrap().1, "one two");
        assert_eq!(percent_decode(b"%00", false, 32).err(), Some(Error::INVALID_FORMAT));
        assert_eq!(percent_encode("a +", true, 32).unwrap().as_str(), "a+%2B");
        assert_eq!(percent_encode("*~", true, 32).unwrap().as_str(), "*%7E");
        assert_eq!(percent_encode("*~", false, 32).unwrap().as_str(), "%2A~");
        let cookies = parse_cookies(b"sid=one; sid=two; raw=%2F", FieldLimits::default()).unwrap();
        assert_eq!(cookies.get(1).unwrap(), ("sid", "two"));
        assert_eq!(cookies.get(2).unwrap().1, "%2F");
        let mut options = CookieOptions { same_site: SameSite::None, ..Default::default() };
        assert_eq!(set_cookie("sid", "x", &options).err(), Some(Error::INVALID_ARGUMENT));
        options.secure = true;
        assert!(set_cookie("sid", "x", &options).unwrap().as_str().contains("SameSite=None"));
    }
    #[test]
    fn multipart_split_input_owned_output_and_eof() {
        let mut parser = Multipart::new("multipart/form-data; boundary=x", MultipartOptions::default()).unwrap();
        let wire = b"--x\r\nContent-Disposition: form-data; name=upload; filename=\"../x.bin\"\r\n\r\na\0b\r\n--x--\r\n";
        let mut data = Vec::new();
        let mut held = None;
        let mut ends = 0;
        for byte in wire {
            assert_eq!(parser.feed(&[*byte]).unwrap(), 1);
            loop {
                match parser.read() {
                    Ok(Some(event)) => match event.kind() {
                        PartKind::Begin => { assert_eq!(event.filename(), Some("../x.bin")); held = Some(event); }
                        PartKind::Data => data.extend_from_slice(event.data()),
                        PartKind::End => ends += 1,
                    },
                    Err(Error::WOULD_BLOCK) => break,
                    _ => panic!("only explicit Finish can validate EOF"),
                }
            }
        }
        parser.finish().unwrap();
        assert!(parser.read().unwrap().is_none());
        assert_eq!(data, b"a\0b"); assert_eq!(ends, 1);
        drop(parser);
        let owned = held.unwrap();
        assert_eq!(owned.name(), "upload");
        assert_eq!(owned.header(0).unwrap().0, "content-disposition");
        fn assert_send_sync<T: Send + Sync>(_: &T) {}
        assert_send_sync(&owned);
    }
    #[test]
    fn multipart_terminal_and_capacity_contracts() {
        let options = MultipartOptions { max_header_bytes: 128, max_retained_bytes: 336,
            max_event_bytes: 64, ..Default::default() };
        let mut parser = Multipart::new("multipart/form-data; boundary=x", options).unwrap();
        let prefix = b"--x\r\nContent-Disposition: form-data; name=x\r\n\r\n";
        parser.feed(prefix).unwrap();
        let begin = parser.read().unwrap().unwrap();
        let charge = parser.retained_bytes();
        assert!(charge > 0);
        drop(begin);
        assert!(parser.retained_bytes() < charge);
        parser.finish().unwrap();
        assert_eq!(parser.read().err(), Some(Error::INVALID_FORMAT));
        parser.cancel();
        assert_eq!(parser.feed(b"later").err(), Some(Error::INVALID_FORMAT));
        let mut cancelled = Multipart::new("multipart/form-data; boundary=x", options).unwrap();
        cancelled.cancel();
        assert_eq!(cancelled.read().err(), Some(Error::CANCELLED));
    }
}
