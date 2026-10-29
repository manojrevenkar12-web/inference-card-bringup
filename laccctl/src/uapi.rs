//! The driver's ioctls (include/uapi/lacc.h), enough to run a workload while tracing.

use anyhow::{bail, Context, Result};
use std::fs::{File, OpenOptions};
use std::os::unix::io::AsRawFd;
use std::time::Instant;

#[repr(C)]
#[derive(Default)]
struct Bo {
    size: u64,
    offset: u64,
}
#[repr(C)]
#[derive(Default)]
struct Fill {
    dev_offset: u64,
    size: u64,
    value: u32,
    pad: u32,
    fence: u64,
}
#[repr(C)]
#[derive(Default)]
struct Gemm {
    a: u64,
    b: u64,
    c: u64,
    m: u32,
    n: u32,
    k: u32,
    kind: u32,
    fence: u64,
}
#[repr(C)]
#[derive(Default)]
pub struct Wait {
    pub fence: u64,
    pub timeout_ms: u32,
    pub result: u32,
    pub status: u32,
    pub pad: u32,
    pub submit_ns: u64,
    pub done_ns: u64,
}

const fn ioc(dir: u64, nr: u8, size: usize) -> u64 {
    (dir << 30) | ((size as u64) << 16) | ((b'L' as u64) << 8) | nr as u64
}
const ALLOC: u64 = ioc(3, 1, std::mem::size_of::<Bo>());
const FREE: u64 = ioc(1, 2, std::mem::size_of::<Bo>());
const GEMM: u64 = ioc(3, 4, std::mem::size_of::<Gemm>());
const FILL: u64 = ioc(3, 5, std::mem::size_of::<Fill>());
const WAIT: u64 = ioc(3, 6, std::mem::size_of::<Wait>());

pub struct Dev(File);

impl Dev {
    pub fn open(path: &str) -> Result<Dev> {
        Ok(Dev(OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .with_context(|| format!("open {path}"))?))
    }
    fn ioctl<T>(&self, req: u64, arg: &mut T, what: &str) -> Result<()> {
        // SAFETY: each request is paired with the struct the driver expects for it.
        if unsafe { libc::ioctl(self.0.as_raw_fd(), req as _, arg as *mut T) } < 0 {
            bail!("{what}: {}", std::io::Error::last_os_error());
        }
        Ok(())
    }
    pub fn alloc(&self, size: u64) -> Result<u64> {
        let mut b = Bo { size, offset: 0 };
        self.ioctl(ALLOC, &mut b, "LACC_IOC_ALLOC")?;
        Ok(b.offset)
    }
    pub fn free(&self, offset: u64, size: u64) -> Result<()> {
        self.ioctl(FREE, &mut Bo { size, offset }, "LACC_IOC_FREE")
    }
    pub fn fill(&self, off: u64, size: u64, value: u32) -> Result<u64> {
        let mut f = Fill {
            dev_offset: off,
            size,
            value,
            ..Default::default()
        };
        self.ioctl(FILL, &mut f, "LACC_IOC_FILL")?;
        Ok(f.fence)
    }
    pub fn gemm(&self, a: u64, b: u64, c: u64, s: u32, kind: u32) -> Result<u64> {
        let mut g = Gemm {
            a,
            b,
            c,
            m: s,
            n: s,
            k: s,
            kind,
            fence: 0,
        };
        self.ioctl(GEMM, &mut g, "LACC_IOC_GEMM")?;
        Ok(g.fence)
    }
    pub fn wait(&self, fence: u64) -> Result<Wait> {
        let mut w = Wait {
            fence,
            timeout_ms: 10_000,
            ..Default::default()
        };
        self.ioctl(WAIT, &mut w, "LACC_IOC_WAIT")?;
        Ok(w)
    }
}

/// Bursts of fills and LNS8/INT8 GEMMs of a few sizes until the deadline.
pub fn workload(deadline: Instant) -> Result<()> {
    let d = Dev::open("/dev/lacc0")?;
    let s = 128u32;
    let (a, b, c) = (
        d.alloc((s * s) as u64)?,
        d.alloc((s * s) as u64)?,
        d.alloc((s * s * 4) as u64)?,
    );
    let buf = d.alloc(1 << 20)?;
    d.wait(d.fill(a, (s * s) as u64, 0x4040_4040)?)?;
    d.wait(d.fill(b, (s * s) as u64, 0x3f41_3f41)?)?;
    let mut i = 0u32;
    while Instant::now() < deadline {
        let mut fences = Vec::new();
        for j in 0..8 {
            fences.push(match (i + j) % 3 {
                0 => d.fill(buf, 4096 << (j % 8), i)?,
                1 => d.gemm(a, b, c, s / (1 + j % 2), 0)?,
                _ => d.gemm(a, b, c, s, 1)?,
            });
        }
        for f in fences {
            d.wait(f)?;
        }
        i += 1;
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    for (o, sz) in [(a, s * s), (b, s * s), (c, s * s * 4), (buf, 1 << 20)] {
        d.free(o, sz as u64)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn struct_sizes_match_the_uapi_header() {
        assert_eq!(std::mem::size_of::<Bo>(), 16);
        assert_eq!(std::mem::size_of::<Fill>(), 32);
        assert_eq!(std::mem::size_of::<Gemm>(), 48);
        assert_eq!(std::mem::size_of::<Wait>(), 40);
        assert_eq!(WAIT, 0xc028_4c06);
    }
}
