//! frame/mac_hdr.rs — 802.11 MAC header and LLC/SNAP encap/decap for AWDL
//!
//! Provides:
//!   - 802.11 Management Action frame header serialization
//!   - 802.11 QoS Data frame header serialization
//!   - LLC/SNAP encapsulation for IPv6 payloads over AWDL
//!
//! SPDX-License-Identifier: MIT

use bytes::{BufMut, BytesMut};

/// AWDL broadcast BSSID: 00:17:f2:00:00:00
pub const AWDL_BSSID: [u8; 6] = [0x00, 0x17, 0xf2, 0x00, 0x00, 0x00];

/// 802.11 Frame Control bits
pub const FC_TYPE_MGMT: u16      = 0x0000;
pub const FC_SUBTYPE_ACTION: u16 = 0x00D0; // Management Action frame (0b1101 0000)
pub const FC_TYPE_DATA: u16      = 0x0008; // Data
pub const FC_SUBTYPE_QOS_DATA: u16 = 0x0088; // QoS Data (0b1000 1000)

/// LLC/SNAP header for IPv6 over AWDL
/// DSAP: 0xAA, SSAP: 0xAA, Control: 0x03, Apple OUI: 00:17:F2, EtherType: 0x86DD (IPv6)
pub const AWDL_SNAP_IPV6: [u8; 8] = [0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x86, 0xdd];

/// Standard 802.2 LLC/SNAP for regular Ethernet protocol encapsulation
pub const LLC_SNAP_STANDARD_IPV6: [u8; 8] = [0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x86, 0xdd];

/// Builds a 24-byte 802.11 Action frame header
pub fn build_action_mac_hdr(
    da: [u8; 6],
    sa: [u8; 6],
    bssid: [u8; 6],
    seq_num: u16,
) -> [u8; 24] {
    let mut buf = [0u8; 24];
    let fc = FC_TYPE_MGMT | FC_SUBTYPE_ACTION;
    buf[0..2].copy_from_slice(&fc.to_le_bytes());
    buf[2..4].copy_from_slice(&0u16.to_le_bytes()); // Duration
    buf[4..10].copy_from_slice(&da);
    buf[10..16].copy_from_slice(&sa);
    buf[16..22].copy_from_slice(&bssid);
    let seq_ctrl = (seq_num & 0x0FFF) << 4;
    buf[22..24].copy_from_slice(&seq_ctrl.to_le_bytes());
    buf
}

/// Builds a 26-byte or 30-byte 802.11 QoS Data frame header
pub fn build_qos_data_mac_hdr(
    da: [u8; 6],
    sa: [u8; 6],
    bssid: [u8; 6],
    seq_num: u16,
    tid: u8,
) -> Vec<u8> {
    let mut buf = BytesMut::with_capacity(26);
    let fc = FC_SUBTYPE_QOS_DATA; // To-DS = 0, From-DS = 0
    buf.put_u16_le(fc);
    buf.put_u16_le(0); // Duration
    buf.put_slice(&da);
    buf.put_slice(&sa);
    buf.put_slice(&bssid);
    let seq_ctrl = (seq_num & 0x0FFF) << 4;
    buf.put_u16_le(seq_ctrl);
    // QoS Control: TID in bits 0-3
    let qos_ctrl = (tid as u16) & 0x000F;
    buf.put_u16_le(qos_ctrl);
    buf.to_vec()
}

/// Encapsulate an IPv6 packet into an AWDL QoS Data frame ready for injection
pub fn encap_awdl_data_frame(
    da: [u8; 6],
    sa: [u8; 6],
    ipv6_packet: &[u8],
    seq_num: u16,
) -> Vec<u8> {
    let mut frame = BytesMut::with_capacity(26 + AWDL_SNAP_IPV6.len() + ipv6_packet.len());
    let hdr = build_qos_data_mac_hdr(da, sa, AWDL_BSSID, seq_num, 0);
    frame.put_slice(&hdr);
    frame.put_slice(&AWDL_SNAP_IPV6);
    frame.put_slice(ipv6_packet);
    frame.to_vec()
}

/// Decapsulate an AWDL 802.11 data frame, returning the inner IPv6 payload slice if valid
pub fn decap_awdl_data_frame(raw: &[u8]) -> Option<(&[u8], [u8; 6], [u8; 6])> {
    if raw.len() < 24 + 8 {
        return None;
    }
    let fc = u16::from_le_bytes([raw[0], raw[1]]);
    let type_sub = fc & 0x00FC;
    let is_qos = type_sub == 0x0088;
    let is_data = type_sub == 0x0008;
    if !is_qos && !is_data {
        return None;
    }

    let da: [u8; 6] = raw[4..10].try_into().ok()?;
    let sa: [u8; 6] = raw[10..16].try_into().ok()?;

    let hdr_len = if is_qos { 26 } else { 24 };
    if raw.len() < hdr_len + 8 {
        return None;
    }

    let snap = &raw[hdr_len..hdr_len + 8];
    if snap == AWDL_SNAP_IPV6 || snap == LLC_SNAP_STANDARD_IPV6 {
        Some((&raw[hdr_len + 8..], da, sa))
    } else {
        None
    }
}
