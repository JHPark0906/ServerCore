//! Checked binary cursors and explicit protocol selection. All native operations
//! are bounded and leave cursor/output unchanged on failure. No wire handshake
//! is inserted automatically and no unsafe typed views of input are created.
use crate::{borrowed_bytes, bytes, check, pointer, sys::binary as ffi, verify_abi, Error, Result};
use std::ptr::NonNull;
#[derive(Clone, Copy, Debug, Default)]
#[repr(u32)]
pub enum ByteOrder {
    #[default]
    BigEndian = 0,
    LittleEndian = 1,
}
pub struct Reader(NonNull<ffi::sc_binary_reader>);
pub struct Writer(NonNull<ffi::sc_binary_writer>);
// Each handle is exclusively owned. Mutation needs &mut; native owns reader bytes.
unsafe impl Send for Reader {}
unsafe impl Send for Writer {}
macro_rules! read_number {
    ($name:ident,$ty:ty,$native:ident) => {
        pub fn $name(&mut self) -> Result<$ty> {
            let mut value = <$ty>::default();
            check(unsafe { ffi::$native(self.0.as_ptr(), &mut value) })?;
            Ok(value)
        }
    };
}
macro_rules! read_bytes {
    ($name:ident,$native:ident) => {
        pub fn $name(&mut self, length: usize) -> Result<&[u8]> {
            let mut value = bytes(&[]);
            check(unsafe { ffi::$native(self.0.as_ptr(), length, &mut value) })?;
            Ok(unsafe { borrowed_bytes(value) })
        }
    };
}
impl Reader {
    /// Copies input; maximum is 64 MiB. Returned slices borrow this reader.
    pub fn new(input: &[u8], order: ByteOrder, maximum_bytes: usize) -> Result<Self> {
        verify_abi()?;
        let mut out = std::ptr::null_mut();
        check(unsafe {
            ffi::sc_binary_reader_create(bytes(input), order as u32, maximum_bytes, &mut out)
        })?;
        Ok(Self(pointer(out)?))
    }
    pub fn position(&self) -> usize {
        unsafe { ffi::sc_binary_reader_position(self.0.as_ptr()) }
    }
    pub fn remaining(&self) -> usize {
        unsafe { ffi::sc_binary_reader_remaining(self.0.as_ptr()) }
    }
    pub fn skip(&mut self, length: usize) -> Result<()> {
        check(unsafe { ffi::sc_binary_skip(self.0.as_ptr(), length) })
    }
    /// Width is 1, 2, 4 or 8 bytes; no implicit truncation.
    pub fn unsigned(&mut self, width: usize) -> Result<u64> {
        let mut value = 0;
        check(unsafe { ffi::sc_binary_read_unsigned(self.0.as_ptr(), width, &mut value) })?;
        Ok(value)
    }
    pub fn signed(&mut self, width: usize) -> Result<i64> {
        let mut value = 0;
        check(unsafe { ffi::sc_binary_read_signed(self.0.as_ptr(), width, &mut value) })?;
        Ok(value)
    }
    pub fn boolean(&mut self) -> Result<bool> {
        let mut value = 0;
        check(unsafe { ffi::sc_binary_read_bool(self.0.as_ptr(), &mut value) })?;
        Ok(value != 0)
    }
    read_number!(f32, f32, sc_binary_read_f32);
    read_number!(f64, f64, sc_binary_read_f64);
    read_bytes!(bytes, sc_binary_read_bytes);
    read_bytes!(blob, sc_binary_read_blob);
    pub fn utf8(&mut self, maximum_length: usize) -> Result<&str> {
        let mut value = bytes(&[]);
        check(unsafe { ffi::sc_binary_read_utf8(self.0.as_ptr(), maximum_length, &mut value) })?;
        std::str::from_utf8(unsafe { borrowed_bytes(value) }).map_err(|_| Error::INVALID_FORMAT)
    }
}
impl Drop for Reader {
    fn drop(&mut self) {
        unsafe { ffi::sc_binary_reader_destroy(self.0.as_ptr()) };
    }
}
impl Writer {
    pub fn new(maximum_bytes: usize, order: ByteOrder) -> Result<Self> {
        verify_abi()?;
        let mut out = std::ptr::null_mut();
        check(unsafe { ffi::sc_binary_writer_create(maximum_bytes, order as u32, &mut out) })?;
        Ok(Self(pointer(out)?))
    }
    pub fn clear(&mut self) {
        unsafe { ffi::sc_binary_writer_clear(self.0.as_ptr()) };
    }
    pub fn as_bytes(&self) -> &[u8] {
        let mut value = bytes(&[]);
        // A valid owned handle and output pointer make view infallible.
        let status = unsafe { ffi::sc_binary_writer_view(self.0.as_ptr(), &mut value) };
        debug_assert_eq!(status, crate::sys::SC_OK);
        unsafe { borrowed_bytes(value) }
    }
    pub fn unsigned(&mut self, value: u64, width: usize) -> Result<()> {
        check(unsafe { ffi::sc_binary_write_unsigned(self.0.as_ptr(), value, width) })
    }
    pub fn signed(&mut self, value: i64, width: usize) -> Result<()> {
        check(unsafe { ffi::sc_binary_write_signed(self.0.as_ptr(), value, width) })
    }
    pub fn boolean(&mut self, value: bool) -> Result<()> {
        check(unsafe { ffi::sc_binary_write_bool(self.0.as_ptr(), u32::from(value)) })
    }
    pub fn f32(&mut self, value: f32) -> Result<()> {
        check(unsafe { ffi::sc_binary_write_f32(self.0.as_ptr(), value) })
    }
    pub fn f64(&mut self, value: f64) -> Result<()> {
        check(unsafe { ffi::sc_binary_write_f64(self.0.as_ptr(), value) })
    }
    pub fn bytes(&mut self, value: &[u8]) -> Result<()> {
        check(unsafe { ffi::sc_binary_write_bytes(self.0.as_ptr(), bytes(value)) })
    }
    pub fn blob(&mut self, value: &[u8], maximum_length: usize) -> Result<()> {
        check(unsafe { ffi::sc_binary_write_blob(self.0.as_ptr(), bytes(value), maximum_length) })
    }
    pub fn utf8(&mut self, value: &str, maximum_length: usize) -> Result<()> {
        check(unsafe {
            ffi::sc_binary_write_utf8(self.0.as_ptr(), bytes(value.as_bytes()), maximum_length)
        })
    }
}
impl Drop for Writer {
    fn drop(&mut self) {
        unsafe { ffi::sc_binary_writer_destroy(self.0.as_ptr()) };
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Offer {
    pub version: u32,
    pub features: u64,
}
pub fn negotiate(server: &[Offer], peer: &[Offer], required_features: u64) -> Result<Offer> {
    if server.is_empty() || peer.is_empty() || server.len() > 64 || peer.len() > 64 {
        return Err(Error::INVALID_ARGUMENT);
    }
    let raw = |values: &[Offer]| {
        values
            .iter()
            .map(|v| ffi::sc_protocol_offer {
                version: v.version,
                reserved: 0,
                features: v.features,
            })
            .collect::<Vec<_>>()
    };
    let (server, peer) = (raw(server), raw(peer));
    let mut out = ffi::sc_protocol_offer::default();
    check(unsafe {
        ffi::sc_protocol_negotiate(
            server.as_ptr(),
            server.len(),
            peer.as_ptr(),
            peer.len(),
            required_features,
            &mut out,
        )
    })?;
    Ok(Offer {
        version: out.version,
        features: out.features,
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bounded_roundtrip_and_cursor_transactions() {
        for order in [ByteOrder::BigEndian, ByteOrder::LittleEndian] {
            let mut writer = Writer::new(64, order).unwrap();
            writer.unsigned(u64::MAX, 8).unwrap();
            writer.signed(-123, 2).unwrap();
            writer.f32(-0.0).unwrap();
            writer.f64(1.25).unwrap();
            writer.utf8("한글", 6).unwrap();
            let before = writer.as_bytes().to_vec();
            assert_eq!(writer.unsigned(256, 1), Err(Error::INVALID_ARGUMENT));
            assert_eq!(writer.as_bytes(), before);
            let mut reader = Reader::new(writer.as_bytes(), order, 64).unwrap();
            drop(writer);
            assert_eq!(reader.unsigned(8).unwrap(), u64::MAX);
            assert_eq!(reader.signed(2).unwrap(), -123);
            assert_eq!(reader.f32().unwrap().to_bits(), (-0.0f32).to_bits());
            assert_eq!(reader.f64().unwrap(), 1.25);
            let position = reader.position();
            assert_eq!(reader.utf8(5), Err(Error::TOO_LARGE));
            assert_eq!(reader.position(), position);
            assert_eq!(reader.utf8(6).unwrap(), "한글");
            assert_eq!(reader.remaining(), 0);
            assert_eq!(reader.unsigned(8), Err(Error::INVALID_FORMAT));
        }
        let mut malformed = Reader::new(&[2], ByteOrder::BigEndian, 1).unwrap();
        assert_eq!(malformed.boolean(), Err(Error::INVALID_FORMAT));
        assert_eq!(malformed.position(), 0);
    }
    #[test]
    fn explicit_negotiation_requires_capabilities() {
        let local = [
            Offer {
                version: 3,
                features: 7,
            },
            Offer {
                version: 2,
                features: 3,
            },
        ];
        let peer = [
            Offer {
                version: 2,
                features: 3,
            },
            Offer {
                version: 3,
                features: 1,
            },
        ];
        assert_eq!(negotiate(&local, &peer, 2).unwrap().version, 2);
        assert_eq!(negotiate(&local, &peer, 8), Err(Error::UNIMPLEMENTED));
        assert_eq!(
            negotiate(&[local[0], local[0]], &peer, 0),
            Err(Error::INVALID_ARGUMENT)
        );
    }
}
