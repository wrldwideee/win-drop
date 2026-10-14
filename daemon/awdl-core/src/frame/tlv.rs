//! frame/tlv.rs — AWDL Action Frame TLV parser and serializer
//!
//! AWDL TLVs follow the standard format:
//!   - Tag (1 byte)
//!   - Length (2 bytes, little-endian)
//!   - Value (Length bytes)
//!
//! Standard AWDL TLVs:
//!   - 0x01: Service Parameters
//!   - 0x02: Synchronization Parameters
//!   - 0x03: Master Election Parameters
//!   - 0x04: Service Response
//!   - 0x05: Channel Sequence
//!   - 0x06: Synchronization Tree
//!   - 0x07: Version / Capabilities
//!   - 0x08: Data Path State
//!   - 0x09: Arp / IPv6 Address / Hostname
//!
//! SPDX-License-Identifier: MIT

use bytes::{BufMut, BytesMut};
use nom::{
    bytes::complete::take,
    number::complete::{le_u16, u8},
    IResult,
};

pub const TLV_SERVICE_PARAMS: u8 = 0x01;
pub const TLV_SYNC_PARAMS: u8    = 0x02;
pub const TLV_ELECTION_PARAMS: u8 = 0x03;
pub const TLV_SERVICE_RESP: u8   = 0x04;
pub const TLV_CHANNEL_SEQ: u8    = 0x05;
pub const TLV_SYNC_TREE: u8      = 0x06;
pub const TLV_VERSION: u8        = 0x07;
pub const TLV_DATA_PATH_ST: u8   = 0x08;
pub const TLV_PEER_ADDR: u8      = 0x09;

/// Synchronization Parameters (TLV 0x02)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SyncParams {
    pub next_channel: u8,
    pub tx_counter:   u8,
    pub master_chan:  u8,
    pub guard_time:   u8,
    pub aw_period:    u16, // Availability window period (typically 16 ms)
    pub aw_seq:       u16,
}

/// Master Election Parameters (TLV 0x03)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ElectionParams {
    pub flags:           u8,
    pub master_id:       [u8; 6], // MAC address of current master
    pub master_metric:   u32,     // Metric (higher priority wins)
    pub master_counter:  u32,     // Monotonic master sequence counter
    pub self_metric:     u32,     // Sender's own metric
    pub self_counter:    u32,     // Sender's own counter
}

/// Channel Sequence (TLV 0x05)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChannelSequence {
    pub channel_step: u8,
    pub channels:     Vec<u8>,
}

/// Data Path State (TLV 0x08)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataPathState {
    pub aw_flags:      u8,
    pub aw_slots:      u8,
    pub unicast_step:  u8,
}

/// Peer Address / Host Info (TLV 0x09)
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeerAddressTlv {
    pub ipv6_addr: [u8; 16],
    pub hostname:  String,
}

/// Parsed AWDL TLV
#[derive(Debug, Clone)]
pub enum AwdlTlv {
    Sync(SyncParams),
    Election(ElectionParams),
    ChannelSeq(ChannelSequence),
    DataPath(DataPathState),
    PeerAddr(PeerAddressTlv),
    Raw { tag: u8, data: Vec<u8> },
}

impl AwdlTlv {
    /// Parse all TLVs from a raw byte slice
    pub fn parse_all(mut input: &[u8]) -> Vec<AwdlTlv> {
        let mut list = Vec::new();
        while !input.is_empty() {
            match parse_single_tlv(input) {
                Ok((rem, tlv)) => {
                    list.push(tlv);
                    input = rem;
                }
                Err(_) => break,
            }
        }
        list
    }

    /// Serialize this TLV into wire format [Tag: 1B, Len: 2B LE, Value]
    pub fn serialize(&self) -> Vec<u8> {
        let mut buf = BytesMut::new();
        match self {
            AwdlTlv::Sync(s) => {
                buf.put_u8(TLV_SYNC_PARAMS);
                buf.put_u16_le(8);
                buf.put_u8(s.next_channel);
                buf.put_u8(s.tx_counter);
                buf.put_u8(s.master_chan);
                buf.put_u8(s.guard_time);
                buf.put_u16_le(s.aw_period);
                buf.put_u16_le(s.aw_seq);
            }
            AwdlTlv::Election(e) => {
                buf.put_u8(TLV_ELECTION_PARAMS);
                buf.put_u16_le(23); // 1 + 6 + 4*4
                buf.put_u8(e.flags);
                buf.put_slice(&e.master_id);
                buf.put_u32_le(e.master_metric);
                buf.put_u32_le(e.master_counter);
                buf.put_u32_le(e.self_metric);
                buf.put_u32_le(e.self_counter);
            }
            AwdlTlv::ChannelSeq(c) => {
                let len = (1 + c.channels.len()) as u16;
                buf.put_u8(TLV_CHANNEL_SEQ);
                buf.put_u16_le(len);
                buf.put_u8(c.channel_step);
                buf.put_slice(&c.channels);
            }
            AwdlTlv::DataPath(d) => {
                buf.put_u8(TLV_DATA_PATH_ST);
                buf.put_u16_le(3);
                buf.put_u8(d.aw_flags);
                buf.put_u8(d.aw_slots);
                buf.put_u8(d.unicast_step);
            }
            AwdlTlv::PeerAddr(p) => {
                let host_bytes = p.hostname.as_bytes();
                let len = (16 + host_bytes.len()) as u16;
                buf.put_u8(TLV_PEER_ADDR);
                buf.put_u16_le(len);
                buf.put_slice(&p.ipv6_addr);
                buf.put_slice(host_bytes);
            }
            AwdlTlv::Raw { tag, data } => {
                buf.put_u8(*tag);
                buf.put_u16_le(data.len() as u16);
                buf.put_slice(data);
            }
        }
        buf.to_vec()
    }
}

fn parse_single_tlv(i: &[u8]) -> IResult<&[u8], AwdlTlv> {
    let (i, tag) = u8(i)?;
    let (i, len) = le_u16(i)?;
    let (rem, val) = take(len as usize)(i)?;

    let tlv = match tag {
        TLV_SYNC_PARAMS if len >= 8 => {
            let next_channel = val[0];
            let tx_counter   = val[1];
            let master_chan  = val[2];
            let guard_time   = val[3];
            let aw_period    = u16::from_le_bytes([val[4], val[5]]);
            let aw_seq       = u16::from_le_bytes([val[6], val[7]]);
            AwdlTlv::Sync(SyncParams {
                next_channel,
                tx_counter,
                master_chan,
                guard_time,
                aw_period,
                aw_seq,
            })
        }
        TLV_ELECTION_PARAMS if len >= 23 => {
            let flags = val[0];
            let mut master_id = [0u8; 6];
            master_id.copy_from_slice(&val[1..7]);
            let master_metric  = u32::from_le_bytes(val[7..11].try_into().unwrap());
            let master_counter = u32::from_le_bytes(val[11..15].try_into().unwrap());
            let self_metric    = u32::from_le_bytes(val[15..19].try_into().unwrap());
            let self_counter   = u32::from_le_bytes(val[19..23].try_into().unwrap());
            AwdlTlv::Election(ElectionParams {
                flags,
                master_id,
                master_metric,
                master_counter,
                self_metric,
                self_counter,
            })
        }
        TLV_CHANNEL_SEQ if len >= 1 => {
            let channel_step = val[0];
            let channels = val[1..].to_vec();
            AwdlTlv::ChannelSeq(ChannelSequence { channel_step, channels })
        }
        TLV_DATA_PATH_ST if len >= 3 => {
            AwdlTlv::DataPath(DataPathState {
                aw_flags: val[0],
                aw_slots: val[1],
                unicast_step: val[2],
            })
        }
        TLV_PEER_ADDR if len >= 16 => {
            let mut ipv6_addr = [0u8; 16];
            ipv6_addr.copy_from_slice(&val[0..16]);
            let hostname = String::from_utf8_lossy(&val[16..]).to_string();
            AwdlTlv::PeerAddr(PeerAddressTlv { ipv6_addr, hostname })
        }
        _ => AwdlTlv::Raw {
            tag,
            data: val.to_vec(),
        },
    };

    Ok((rem, tlv))
}
