//! MTD character devices (the card's SPI NOR, through spi-nor) and BAR0 through sysfs.

use anyhow::{bail, Context, Result};
use std::fs::{self, File, OpenOptions};
use std::os::unix::fs::FileExt;
use std::os::unix::io::AsRawFd;
use std::path::Path;

#[repr(C)]
#[derive(Default, Debug, Clone, Copy)]
pub struct MtdInfo {
    pub kind: u8,
    pub flags: u32,
    pub size: u32,
    pub erasesize: u32,
    pub writesize: u32,
    pub oobsize: u32,
    pub padding: u64,
}

#[repr(C)]
struct EraseInfo {
    start: u32,
    length: u32,
}

const fn ioc(dir: u64, ty: u8, nr: u8, size: usize) -> u64 {
    (dir << 30) | ((size as u64) << 16) | ((ty as u64) << 8) | nr as u64
}
const MEMGETINFO: u64 = ioc(2, b'M', 1, std::mem::size_of::<MtdInfo>());
const MEMERASE: u64 = ioc(1, b'M', 2, std::mem::size_of::<EraseInfo>());

pub struct Partition {
    pub dev: String,
    pub info: MtdInfo,
    file: File,
}

impl Partition {
    /// Find a partition by name in /proc/mtd (`mtd1: 00100000 00001000 "lacc-fw-a"`).
    pub fn find(name: &str) -> Result<Partition> {
        let table = fs::read_to_string("/proc/mtd").context("read /proc/mtd")?;
        let dev = table
            .lines()
            .find(|l| l.ends_with(&format!("\"{name}\"")))
            .and_then(|l| l.split(':').next())
            .map(|d| format!("/dev/{d}"));
        let Some(dev) = dev else {
            bail!("no MTD partition named {name}")
        };
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&dev)
            .with_context(|| format!("open {dev}"))?;
        let mut info = MtdInfo::default();
        // SAFETY: MEMGETINFO fills a struct mtd_info_user, which MtdInfo mirrors.
        if unsafe { libc::ioctl(file.as_raw_fd(), MEMGETINFO as _, &mut info) } < 0 {
            bail!("MEMGETINFO on {dev}: {}", std::io::Error::last_os_error());
        }
        Ok(Partition { dev, info, file })
    }

    pub fn erase(&self, start: u32, len: u32) -> Result<()> {
        let es = self.info.erasesize.max(1);
        let len = len.div_ceil(es) * es;
        let e = EraseInfo { start, length: len };
        // SAFETY: MEMERASE reads a struct erase_info_user.
        if unsafe { libc::ioctl(self.file.as_raw_fd(), MEMERASE as _, &e) } < 0 {
            bail!(
                "erase {} [{start:#x}, +{len:#x}): {}",
                self.dev,
                std::io::Error::last_os_error()
            );
        }
        Ok(())
    }

    pub fn read(&self, off: u64, len: usize) -> Result<Vec<u8>> {
        let mut v = vec![0u8; len];
        self.file
            .read_exact_at(&mut v, off)
            .with_context(|| format!("read {}", self.dev))?;
        Ok(v)
    }

    pub fn write_verify(&self, off: u64, data: &[u8]) -> Result<()> {
        self.file
            .write_all_at(data, off)
            .with_context(|| format!("write {}", self.dev))?;
        let back = self.read(off, data.len())?;
        if let Some(i) = back.iter().zip(data).position(|(a, b)| a != b) {
            bail!(
                "{} verify failed at offset {:#x}: wrote {:#04x}, read {:#04x}",
                self.dev,
                off as usize + i,
                data[i],
                back[i]
            );
        }
        Ok(())
    }
}

/// BAR0 mapped read-only from sysfs `resource0`.
pub struct Bar0 {
    ptr: *const u32,
    len: usize,
}

impl Bar0 {
    pub fn open(path: &Path) -> Result<Bar0> {
        let f = File::open(path).with_context(|| format!("open {}", path.display()))?;
        let len = f.metadata()?.len() as usize;
        // SAFETY: a shared read-only mapping of the device's register BAR.
        let p = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                len,
                libc::PROT_READ,
                libc::MAP_SHARED,
                f.as_raw_fd(),
                0,
            )
        };
        if p == libc::MAP_FAILED {
            bail!(
                "mmap {}: {}",
                path.display(),
                std::io::Error::last_os_error()
            );
        }
        Ok(Bar0 {
            ptr: p as *const u32,
            len,
        })
    }
    pub fn read(&self, off: u64) -> u32 {
        assert!((off as usize) + 4 <= self.len && off % 4 == 0);
        // SAFETY: in bounds and aligned; MMIO must be read with a volatile 32-bit access.
        unsafe { std::ptr::read_volatile(self.ptr.add(off as usize / 4)) }
    }
}

impl Drop for Bar0 {
    fn drop(&mut self) {
        // SAFETY: unmapping the mapping created in open().
        unsafe { libc::munmap(self.ptr as *mut _, self.len) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ioctl_numbers_match_linux() {
        // from <mtd/mtd-abi.h>: MEMGETINFO = _IOR('M', 1, struct mtd_info_user), MEMERASE = _IOW('M', 2, ...)
        assert_eq!(std::mem::size_of::<MtdInfo>(), 32);
        assert_eq!(MEMGETINFO, 0x8020_4d01);
        assert_eq!(MEMERASE, 0x4008_4d02);
    }
}
