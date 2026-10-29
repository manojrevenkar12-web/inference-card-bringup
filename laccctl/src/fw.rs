//! Firmware images: the format the card's boot ROM verifies (include/lacc_hw.h, fw/mkfw.py).

use anyhow::{bail, Result};
use sha2::{Digest, Sha256};

pub const FW_MAGIC: u32 = 0x3157_464C; // "LFW1"
pub const CFG_MAGIC: u32 = 0x4746_434C; // "LCFG"
pub const HEADER_LEN: usize = 56;
pub const SLOT_SIZE: usize = 0x10_0000;

pub const TLV_NAMES: [&str; 6] = ["?", "lns-lut", "power", "thermal", "clocks", "build"];

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct Header {
    pub magic: u32,
    pub version: u32,
    pub rollback_index: u32,
    pub payload_len: u32,
    pub sha256: [u8; 32],
    pub flags: u32,
    pub header_crc32: u32,
}

#[derive(Debug, Clone, serde::Serialize)]
pub struct Tlv {
    pub kind: u16,
    pub name: &'static str,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, serde::Serialize)]
pub struct Report {
    pub header: Header,
    pub magic_ok: bool,
    pub crc_ok: bool,
    pub sha256_ok: bool,
    pub tlvs_ok: bool,
    pub tlvs: Vec<Tlv>,
    pub build: Option<String>,
}

impl Report {
    pub fn valid(&self) -> bool {
        self.magic_ok && self.crc_ok && self.sha256_ok && self.tlvs_ok
    }
    /// Why the boot ROM would refuse this image, or None.
    pub fn problem(&self) -> Option<&'static str> {
        if !self.magic_ok {
            Some("bad magic")
        } else if !self.crc_ok {
            Some("header CRC-32 mismatch")
        } else if !self.sha256_ok {
            Some("payload SHA-256 mismatch")
        } else if !self.tlvs_ok {
            Some("malformed TLV records")
        } else {
            None
        }
    }
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

pub fn parse_header(img: &[u8]) -> Result<Header> {
    if img.len() < HEADER_LEN {
        bail!(
            "image is {} bytes, shorter than the {HEADER_LEN}-byte header",
            img.len()
        );
    }
    let mut sha = [0u8; 32];
    sha.copy_from_slice(&img[16..48]);
    Ok(Header {
        magic: u32le(img, 0),
        version: u32le(img, 4),
        rollback_index: u32le(img, 8),
        payload_len: u32le(img, 12),
        sha256: sha,
        flags: u32le(img, 48),
        header_crc32: u32le(img, 52),
    })
}

pub fn header_crc(img: &[u8]) -> u32 {
    let mut h = img[..HEADER_LEN].to_vec();
    h[52..56].fill(0);
    crc32fast::hash(&h)
}

pub fn inspect(img: &[u8]) -> Result<Report> {
    let header = parse_header(img)?;
    let body_end = HEADER_LEN.checked_add(header.payload_len as usize);
    let body = match body_end {
        Some(end) if end <= img.len() && end <= SLOT_SIZE => &img[HEADER_LEN..end],
        _ => &[][..],
    };
    let sha256_ok = !body.is_empty() && Sha256::digest(body).as_slice() == header.sha256;
    let (tlvs, tlvs_ok) = parse_tlvs(body);
    let build = tlvs
        .iter()
        .find(|t| t.kind == 5)
        .map(|t| String::from_utf8_lossy(&t.data).into_owned());
    Ok(Report {
        magic_ok: header.magic == FW_MAGIC,
        crc_ok: header_crc(img) == header.header_crc32,
        sha256_ok,
        tlvs_ok,
        tlvs,
        build,
        header,
    })
}

fn parse_tlvs(mut b: &[u8]) -> (Vec<Tlv>, bool) {
    let mut out = Vec::new();
    while !b.is_empty() {
        if b.len() < 4 {
            return (out, false);
        }
        let kind = u16::from_le_bytes([b[0], b[1]]);
        let len = u16::from_le_bytes([b[2], b[3]]) as usize;
        let padded = (len + 3) & !3;
        if b.len() < 4 + padded {
            return (out, false);
        }
        out.push(Tlv {
            kind,
            name: TLV_NAMES.get(kind as usize).copied().unwrap_or("?"),
            data: b[4..4 + len].to_vec(),
        });
        b = &b[4 + padded..];
    }
    let has_lut = out.iter().any(|t| t.kind == 1 && t.data.len() == 32);
    (out, has_lut)
}

/// The 16-byte config record: boot slot (1 = A, 2 = B, 0 = newest) and the rollback floor mirror.
pub fn config(boot_slot: u32, min_version: u32) -> [u8; 16] {
    let mut c = [0u8; 16];
    c[0..4].copy_from_slice(&CFG_MAGIC.to_le_bytes());
    c[4..8].copy_from_slice(&boot_slot.to_le_bytes());
    c[8..12].copy_from_slice(&min_version.to_le_bytes());
    let crc = crc32fast::hash(&c[..12]);
    c[12..16].copy_from_slice(&crc.to_le_bytes());
    c
}

pub fn parse_config(c: &[u8]) -> Option<(u32, u32)> {
    if c.len() < 16 || u32le(c, 0) != CFG_MAGIC || crc32fast::hash(&c[..12]) != u32le(c, 12) {
        return None;
    }
    Some((u32le(c, 4), u32le(c, 8)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    fn mkfw(args: &[&str]) -> Vec<u8> {
        let dir = std::env::temp_dir().join(format!(
            "laccctl-test-{}-{}",
            std::process::id(),
            args.join("_")
        ));
        let out = dir.with_extension("bin");
        let script = concat!(env!("CARGO_MANIFEST_DIR"), "/../fw/mkfw.py");
        let st = Command::new("python3")
            .arg(script)
            .args(args)
            .arg("-o")
            .arg(&out)
            .status()
            .unwrap();
        assert!(st.success());
        let v = std::fs::read(&out).unwrap();
        std::fs::remove_file(&out).ok();
        v
    }

    #[test]
    fn good_image_from_mkfw_verifies() {
        let r = inspect(&mkfw(&["image", "--version", "4", "--rollback", "2"])).unwrap();
        assert!(r.valid(), "{:?}", r.problem());
        assert_eq!(r.header.version, 4);
        assert_eq!(r.header.rollback_index, 2);
        assert_eq!(r.build.as_deref(), Some("lacc-fw 4"));
        assert_eq!(
            r.tlvs.iter().map(|t| t.kind).collect::<Vec<_>>(),
            vec![1, 2, 3, 4, 5]
        );
    }

    #[test]
    fn corrupt_payload_fails_sha() {
        let r = inspect(&mkfw(&["image", "--version", "9", "--corrupt", "payload"])).unwrap();
        assert_eq!(r.problem(), Some("payload SHA-256 mismatch"));
    }

    #[test]
    fn corrupt_header_fails_crc() {
        let r = inspect(&mkfw(&["image", "--version", "9", "--corrupt", "header"])).unwrap();
        assert_eq!(r.problem(), Some("header CRC-32 mismatch"));
    }

    #[test]
    fn truncated_and_oversized_images_are_invalid() {
        let img = mkfw(&["image", "--version", "1"]);
        assert!(!inspect(&img[..img.len() - 3]).unwrap().valid());
        assert!(inspect(&img[..20]).is_err());
        let mut big = img.clone();
        big[12..16].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(!inspect(&big).unwrap().valid());
    }

    #[test]
    fn config_matches_mkfw() {
        let flash = mkfw(&["flash", "--boot-slot", "2", "--min-version", "5"]);
        let c = &flash[0x20_0000..0x20_0010];
        assert_eq!(c, &config(2, 5));
        assert_eq!(parse_config(c), Some((2, 5)));
        assert_eq!(parse_config(&[0xff; 16]), None);
    }
}
