//! airdrop/cpio.rs — POSIX CPIO Archive Builder & Extractor for Apple AirDrop
//!
//! Apple iOS, iPadOS, and macOS AirDrop daemons transmit files packaged as
//! gzip-compressed POSIX CPIO archives (.cpio.gz) with ODC (070707) or
//! SVR4 (070701) format headers.
//!
//! SPDX-License-Identifier: MIT

use std::io::{Read, Write};
use flate2::read::GzDecoder;
use flate2::write::GzEncoder;
use flate2::Compression;

/// Build a POSIX ODC (070707) CPIO archive containing a single file, compressed with gzip (.cpio.gz)
pub fn build_cpio_gz(file_name: &str, file_data: &[u8]) -> anyhow::Result<Vec<u8>> {
    let clean_name = {
        let p = std::path::Path::new(file_name);
        let base = p.file_name().and_then(|f| f.to_str()).unwrap_or("file.bin");
        format!("./{}", base)
    };
    let name_bytes = clean_name.as_bytes();
    let name_len = name_bytes.len() + 1; // includes terminating NUL

    let mut cpio_buf = Vec::with_capacity(76 + name_len + file_data.len() + 128);
    let mtime = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();

    // 76 bytes ODC header:
    // c_magic    [6]  = "070707"
    // c_dev      [6]  = 0
    // c_ino      [6]  = 1
    // c_mode     [6]  = 0100644 (regular file)
    // c_uid      [6]  = 0
    // c_gid      [6]  = 0
    // c_nlink    [6]  = 1
    // c_rdev     [6]  = 0
    // c_mtime    [11] = octal mtime
    // c_namesize [6]  = octal name length
    // c_filesize [11] = octal file data length
    let header = format!(
        "070707{:06o}{:06o}{:06o}{:06o}{:06o}{:06o}{:06o}{:011o}{:06o}{:011o}",
        0,              // dev
        1,              // ino
        0o100644,       // mode (regular file rw-r--r--)
        0,              // uid
        0,              // gid
        1,              // nlink
        0,              // rdev
        mtime,          // mtime (11 chars)
        name_len,       // namesize (6 chars)
        file_data.len() // filesize (11 chars)
    );
    cpio_buf.extend_from_slice(header.as_bytes());
    cpio_buf.extend_from_slice(name_bytes);
    cpio_buf.push(0); // NUL terminator
    cpio_buf.extend_from_slice(file_data);

    // Trailer entry ("TRAILER!!!\0")
    let trailer_name = b"TRAILER!!!\0";
    let trailer_hdr = format!(
        "070707{:06o}{:06o}{:06o}{:06o}{:06o}{:06o}{:06o}{:011o}{:06o}{:011o}",
        0, 0, 0, 0, 0, 1, 0, 0, trailer_name.len(), 0
    );
    cpio_buf.extend_from_slice(trailer_hdr.as_bytes());
    cpio_buf.extend_from_slice(trailer_name);

    // Pad archive to 512-byte POSIX boundary
    let rem = cpio_buf.len() % 512;
    if rem != 0 {
        let pad = 512 - rem;
        cpio_buf.extend(std::iter::repeat(0).take(pad));
    }

    // Compress to .cpio.gz
    let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
    encoder.write_all(&cpio_buf)?;
    let gz_data = encoder.finish()?;

    Ok(gz_data)
}

/// Extract original filename and content from incoming AirDrop payload (.cpio.gz, .cpio, or raw)
pub fn extract_cpio_gz_or_raw(data: &[u8]) -> Option<(String, Vec<u8>)> {
    if data.is_empty() {
        return None;
    }

    // 1. Decompress if gzipped (magic bytes 0x1F, 0x8B)
    let is_gzip = data.len() >= 2 && data[0] == 0x1F && data[1] == 0x8B;
    let payload = if is_gzip {
        let mut decoder = GzDecoder::new(data);
        let mut decomp = Vec::new();
        if decoder.read_to_end(&mut decomp).is_ok() && !decomp.is_empty() {
            decomp
        } else {
            data.to_vec()
        }
    } else {
        data.to_vec()
    };

    // 2. Parse CPIO archive (ODC "070707" or SVR4 "070701" / "070702")
    if payload.len() >= 76 && payload.starts_with(b"070707") {
        if let Ok(hdr_str) = std::str::from_utf8(&payload[..76]) {
            let namesize = usize::from_str_radix(hdr_str[59..65].trim(), 8).ok()?;
            let filesize = usize::from_str_radix(hdr_str[65..76].trim(), 8).ok()?;
            let name_end = 76 + namesize;
            if payload.len() >= name_end + filesize {
                let raw_name = std::str::from_utf8(&payload[76..name_end]).unwrap_or("file.bin");
                let clean_name = raw_name
                    .trim_matches('\0')
                    .trim_start_matches("./")
                    .trim_start_matches('/')
                    .to_string();

                let file_content = payload[name_end..name_end + filesize].to_vec();
                if clean_name != "TRAILER!!!" && !clean_name.is_empty() {
                    return Some((clean_name, file_content));
                }
            }
        }
    } else if payload.len() >= 110 && (payload.starts_with(b"070701") || payload.starts_with(b"070702")) {
        if let Ok(hdr_str) = std::str::from_utf8(&payload[..110]) {
            let filesize = usize::from_str_radix(hdr_str[54..62].trim(), 16).ok()?;
            let namesize = usize::from_str_radix(hdr_str[94..102].trim(), 16).ok()?;
            // SVR4 pads header + name to 4-byte alignment
            let name_end = 110 + namesize;
            let data_offset = (name_end + 3) & !3;
            if payload.len() >= data_offset + filesize {
                let raw_name = std::str::from_utf8(&payload[110..name_end]).unwrap_or("file.bin");
                let clean_name = raw_name
                    .trim_matches('\0')
                    .trim_start_matches("./")
                    .trim_start_matches('/')
                    .to_string();

                let file_content = payload[data_offset..data_offset + filesize].to_vec();
                if clean_name != "TRAILER!!!" && !clean_name.is_empty() {
                    return Some((clean_name, file_content));
                }
            }
        }
    }

    // 3. Fallback: if it was gzipped archive but couldn't parse CPIO header, return payload
    if is_gzip {
        return Some(("archive.tar".to_string(), payload));
    }

    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cpio_roundtrip() {
        let original_name = "test_photo.jpg";
        let original_data = b"JFIF_MOCK_IMAGE_DATA_123456789";

        let archive = build_cpio_gz(original_name, original_data).expect("build cpio gz");
        assert!(!archive.is_empty());

        let (extracted_name, extracted_data) =
            extract_cpio_gz_or_raw(&archive).expect("extract cpio gz");
        assert_eq!(extracted_name, original_name);
        assert_eq!(extracted_data, original_data);
    }
}
