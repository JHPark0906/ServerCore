//! Blocking atomic replacement of application files. Run on a worker, not an I/O
//! callback. A trusted parent directory is required. Drop cleans only an
//! uncommitted temporary; crash durability is separate from rename atomicity.
use crate::{bytes, check, pointer, sys, sys::files as ffi, verify_abi, Error, Result};
use std::{path::Path, ptr::NonNull};
#[derive(Clone, Copy, Debug, Default)]
#[repr(u32)]
pub enum SyncMode {
    None = 0,
    #[default]
    File = 1,
    FileAndDirectory = 2,
}
#[derive(Clone, Copy, Debug)]
pub struct Options {
    pub maximum_bytes: u64,
    pub sync: SyncMode,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            maximum_bytes: 64 * 1024 * 1024,
            sync: SyncMode::File,
        }
    }
}
pub struct AtomicFile(NonNull<ffi::sc_atomic_file>);
// Sole owner, every mutation needs &mut; no native background callbacks.
unsafe impl Send for AtomicFile {}
impl AtomicFile {
    pub fn supports_directory_sync() -> bool {
        unsafe { ffi::sc_file_supports_directory_sync() != 0 }
    }
    pub fn new(target: impl AsRef<Path>, options: Options) -> Result<Self> {
        verify_abi()?;
        let target = target.as_ref().to_str().ok_or(Error::INVALID_ARGUMENT)?;
        let native = ffi::sc_atomic_file_options {
            abi_version: sys::SC_ABI_VERSION,
            struct_size: size_of::<ffi::sc_atomic_file_options>() as u32,
            maximum_bytes: options.maximum_bytes,
            sync: options.sync as u32,
            reserved: 0,
        };
        let mut out = std::ptr::null_mut();
        check(unsafe { ffi::sc_atomic_file_create(bytes(target.as_bytes()), &native, &mut out) })?;
        Ok(Self(pointer(out)?))
    }
    pub fn write(&mut self, data: &[u8]) -> Result<()> {
        check(unsafe { ffi::sc_atomic_file_write(self.0.as_ptr(), bytes(data)) })
    }
    /// After an error inspect committed(): directory sync can fail after publication.
    pub fn commit(&mut self) -> Result<()> {
        check(unsafe { ffi::sc_atomic_file_commit(self.0.as_ptr()) })
    }
    pub fn cancel(&mut self) -> Result<()> {
        check(unsafe { ffi::sc_atomic_file_cancel(self.0.as_ptr()) })
    }
    pub fn committed(&self) -> bool {
        unsafe { ffi::sc_atomic_file_committed(self.0.as_ptr()) != 0 }
    }
    pub fn written(&self) -> u64 {
        unsafe { ffi::sc_atomic_file_written(self.0.as_ptr()) }
    }
}
impl Drop for AtomicFile {
    fn drop(&mut self) {
        unsafe { ffi::sc_atomic_file_destroy(self.0.as_ptr()) };
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn glob_import_keeps_the_sync_marker_trait() {
        // `use super::*` above is the glob import users write for this module.
        fn assert_sync<T: Sync>() {}
        assert_sync::<Options>();
    }
    #[test]
    fn replace_and_abandon_preserve_target() {
        let root = std::env::temp_dir().join(format!(
            "servercore-rust-file-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&root).unwrap();
        let path = root.join("설정.json");
        std::fs::write(&path, b"old").unwrap();
        {
            let mut file = AtomicFile::new(
                &path,
                Options {
                    maximum_bytes: 3,
                    ..Options::default()
                },
            )
            .unwrap();
            file.write(b"new").unwrap();
            assert_eq!(file.write(b"!"), Err(Error::TOO_LARGE));
            assert_eq!(std::fs::read(&path).unwrap(), b"old");
        }
        assert_eq!(std::fs::read_dir(&root).unwrap().count(), 1);
        let mut file = AtomicFile::new(&path, Options::default()).unwrap();
        file.write(b"new").unwrap();
        file.commit().unwrap();
        assert!(file.committed());
        assert_eq!(file.write(b"!"), Err(Error::CLOSED));
        file.cancel().unwrap();
        drop(file);
        assert_eq!(std::fs::read(&path).unwrap(), b"new");
        std::fs::remove_file(&path).unwrap();
        std::fs::remove_dir(root).unwrap();
    }
}
