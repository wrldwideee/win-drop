//! frame/mod.rs — AWDL 802.11 Action Frame parser
//!
//! Parses the raw 802.11 frame bytes delivered by the kernel driver into
//! typed Rust structures.  Uses `nom` for zero-copy slice parsing.
//!
//! AWDL frame structure:
//!   [802.11 MAC header — 24 bytes]
//!   [Action Category = 0x7F (Vendor-Specific)]
//!   [Apple OUI: 00:17:F2]
//!   [AWDL Action Type: 0x08 (PSF) or 0x03 (MIF)]
//!   [AWDL Action Code: 0x00]
//!   [AWDL Header: version, sub_type, phy_tx, target_tx, counter, data_path, seq]
//!   [TLV sequence ...]
//!
//! Reference: OWL project (https://github.com/seemoo-lab/owl)
//!            Apple80211 framework reverse engineering
//!
//! SPDX-License-Identifier: MIT

pub mod tlv;
pub mod mac_hdr;

use nom::{
    bytes::complete::{tag, take},
    number::complete::{le_u16, le_u32, u8},
    IResult,
};
use tracing::trace;

/// Apple OUI used in vendor-specific AWDL Action Frames
pub const APPLE_OUI: [u8; 3] = [0x00, 0x17, 0xF2];
/// 802.11 Action frame category for vendor-specific
pub const ACTION_CAT_VENDOR: u8 = 0x7F;
/// AWDL Action type byte
pub const AWDL_ACTION_TYPE: u8 = 0x08;

/// AWDL Periodic Synchronisation Frame (PSF) action code
pub const AWDL_FRAME_PSF: u8 = 0x00;
/// AWDL Master Indication Frame (MIF) action code
pub const AWDL_FRAME_MIF: u8 = 0x03;

/// Parsed 802.11 MAC header (24-byte fixed)
#[derive(Debug, Clone)]
pub struct MacHeader {
    pub frame_ctrl: u16,
    pub duration:   u16,
    pub addr1:      [u8; 6],  // DA
    pub addr2:      [u8; 6],  // SA
    pub addr3:      [u8; 6],  // BSSID
    pub seq_ctrl:   u16,
}

/// Parsed AWDL frame header (follows the Action body preamble)
#[derive(Debug, Clone)]
pub struct AwdlHeader {
    pub version:      u8,
    pub sub_type:     u8,
    pub phy_tx:       u16,   // PHY Tx time (µs since last beacon)
    pub target_tx:    u32,   // Target Tx time
    pub counter:      u8,
    pub data_path_st: u8,
    pub seq:          u16,
}

/// A fully parsed AWDL Action Frame
#[derive(Debug, Clone)]
pub struct AwdlFrame {
    pub mac:      MacHeader,
    pub action:   u8,         // PSF or MIF
    pub hdr:      AwdlHeader,
    /// Raw TLV bytes (parsed further by tlv module)
    pub tlv_data: Vec<u8>,
}

impl AwdlFrame {
    /// Parse a raw 802.11 frame byte slice into an AwdlFrame.
    /// Returns `None` if the frame is not an AWDL Action Frame.
    pub fn parse(raw: &[u8]) -> Option<Self> {
        match parse_awdl_frame(raw) {
            Ok((_, frame)) => {
                trace!("Parsed AWDL frame action={:#04x} seq={}", frame.action, frame.hdr.seq);
                Some(frame)
            }
            Err(_) => None,
        }
    }

    /// Is this frame from the same peer as `mac`?
    pub fn from_peer(&self, mac: &[u8; 6]) -> bool {
        &self.mac.addr2 == mac
    }
}

// ---- nom parsers ----

fn parse_mac_header(i: &[u8]) -> IResult<&[u8], MacHeader> {
    let (i, fc)    = le_u16(i)?;
    let (i, dur)   = le_u16(i)?;
    let (i, a1)    = take(6usize)(i)?;
    let (i, a2)    = take(6usize)(i)?;
    let (i, a3)    = take(6usize)(i)?;
    let (i, seq)   = le_u16(i)?;
    Ok((i, MacHeader {
        frame_ctrl: fc,
        duration:   dur,
        addr1:      a1.try_into().unwrap(),
        addr2:      a2.try_into().unwrap(),
        addr3:      a3.try_into().unwrap(),
        seq_ctrl:   seq,
    }))
}

fn parse_awdl_hdr(i: &[u8]) -> IResult<&[u8], AwdlHeader> {
    let (i, version)      = u8(i)?;
    let (i, sub_type)     = u8(i)?;
    let (i, phy_tx)       = le_u16(i)?;
    let (i, target_tx)    = le_u32(i)?;
    let (i, counter)      = u8(i)?;
    let (i, data_path_st) = u8(i)?;
    let (i, seq)          = le_u16(i)?;
    Ok((i, AwdlHeader { version, sub_type, phy_tx, target_tx,
                         counter, data_path_st, seq }))
}

fn parse_awdl_frame(i: &[u8]) -> IResult<&[u8], AwdlFrame> {
    // 802.11 Management frame must be ≥ 24 bytes
    let (i, mac) = parse_mac_header(i)?;

    // Vendor-Specific Action body
    let (i, _cat) = tag([ACTION_CAT_VENDOR])(i)?;
    let (i, _oui) = tag(APPLE_OUI)(i)?;
    let (i, _atyp) = tag([AWDL_ACTION_TYPE])(i)?;
    let (i, action) = u8(i)?;

    // AWDL frame header
    let (i, hdr) = parse_awdl_hdr(i)?;

    // Remaining bytes are TLVs
    let tlv_data = i.to_vec();

    Ok((&[], AwdlFrame { mac, action, hdr, tlv_data }))
}

impl AwdlFrame {
    /// Parse inner TLVs
    pub fn tlvs(&self) -> Vec<tlv::AwdlTlv> {
        tlv::AwdlTlv::parse_all(&self.tlv_data)
    }
}

/// Builds a complete AWDL Periodic Synchronisation Frame (PSF) ready for driver injection
pub fn build_psf_frame(
    sa: [u8; 6],
    seq_num: u16,
    awdl_seq: u16,
    target_tx: u32,
    tlvs: &[tlv::AwdlTlv],
) -> Vec<u8> {
    use bytes::{BufMut, BytesMut};
    let mut frame = BytesMut::with_capacity(128);

    // 1. MAC header (24 bytes) - destination is AWDL multicast/broadcast
    let da = mac_hdr::AWDL_BSSID;
    let bssid = mac_hdr::AWDL_BSSID;
    let mac = mac_hdr::build_action_mac_hdr(da, sa, bssid, seq_num);
    frame.put_slice(&mac);

    // 2. Vendor Specific Action frame preamble
    frame.put_u8(ACTION_CAT_VENDOR);
    frame.put_slice(&APPLE_OUI);
    frame.put_u8(AWDL_ACTION_TYPE);
    frame.put_u8(AWDL_FRAME_PSF);

    // 3. AWDL fixed header (12 bytes)
    frame.put_u8(0x10); // version 1.0
    frame.put_u8(0x01); // sub_type
    frame.put_u16_le(0); // phy_tx
    frame.put_u32_le(target_tx);
    frame.put_u8(0);    // counter
    frame.put_u8(0);    // data_path_st
    frame.put_u16_le(awdl_seq);

    // 4. TLVs
    for tlv in tlvs {
        frame.put_slice(&tlv.serialize());
    }

    frame.to_vec()
}

