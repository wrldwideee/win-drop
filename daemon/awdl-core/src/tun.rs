//! tun.rs — Wintun virtual adapter bridge for awdl0
//!
//! Bridges traffic between the Windows TCP/IP stack (on interface `awdl0`)
//! and raw 802.11 QoS Data frames injected/received by the MT7922 KMDF driver.
//!
//! Ingress path:
//!   Windows TCP/IP -> Wintun Session -> Read Packet -> Encap AWDL Frame -> Radio TX
//!
//! Egress path:
//!   Radio RX -> Decap AWDL Frame -> Write Packet -> Wintun Session -> Windows TCP/IP
//!
//! SPDX-License-Identifier: MIT

use std::sync::Arc;
use tokio::sync::mpsc;
use tracing::{info, trace, warn};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{FreeLibrary, HMODULE};
use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};

use crate::frame::mac_hdr::{encap_awdl_data_frame, AWDL_BSSID};

// Wintun C API function pointer signatures
type WintunOpenAdapterFn = unsafe extern "system" fn(
    pool_name: PCWSTR,
    adapter_name: PCWSTR,
) -> *mut std::ffi::c_void;

type WintunCloseAdapterFn = unsafe extern "system" fn(adapter: *mut std::ffi::c_void);

type WintunStartSessionFn = unsafe extern "system" fn(
    adapter: *mut std::ffi::c_void,
    capacity: u32,
) -> *mut std::ffi::c_void;

type WintunEndSessionFn = unsafe extern "system" fn(session: *mut std::ffi::c_void);

type WintunAllocateSendPacketFn = unsafe extern "system" fn(
    session: *mut std::ffi::c_void,
    packet_size: u32,
) -> *mut u8;

type WintunSendPacketFn = unsafe extern "system" fn(
    session: *mut std::ffi::c_void,
    packet: *const u8,
);

type WintunReceivePacketFn = unsafe extern "system" fn(
    session: *mut std::ffi::c_void,
    packet_size: *mut u32,
) -> *const u8;

type WintunReleaseReceivePacketFn = unsafe extern "system" fn(
    session: *mut std::ffi::c_void,
    packet: *const u8,
);

pub struct WintunApi {
    _module:                      HMODULE,
    pub open_adapter:             WintunOpenAdapterFn,
    pub close_adapter:            WintunCloseAdapterFn,
    pub start_session:            WintunStartSessionFn,
    pub end_session:              WintunEndSessionFn,
    pub allocate_send_packet:     WintunAllocateSendPacketFn,
    pub send_packet:              WintunSendPacketFn,
    pub receive_packet:           WintunReceivePacketFn,
    pub release_receive_packet:   WintunReleaseReceivePacketFn,
}

impl WintunApi {
    pub fn load() -> Option<Self> {
        let dll_name: Vec<u16> = "wintun.dll\0".encode_utf16().collect();
        let module = unsafe { LoadLibraryW(PCWSTR(dll_name.as_ptr())) }.ok()?;

        macro_rules! get_proc {
            ($sym:literal, $typ:ty) => {{
                let c_sym = std::ffi::CString::new($sym).unwrap();
                let ptr = unsafe { GetProcAddress(module, windows::core::PCSTR(c_sym.as_ptr() as _)) };
                match ptr {
                    Some(p) => unsafe { std::mem::transmute::<_, $typ>(p) },
                    None => {
                        warn!("Missing Wintun symbol: {}", $sym);
                        unsafe { let _ = FreeLibrary(module); }
                        return None;
                    }
                }
            }};
        }

        Some(Self {
            _module: module,
            open_adapter:           get_proc!("WintunOpenAdapter", WintunOpenAdapterFn),
            close_adapter:          get_proc!("WintunCloseAdapter", WintunCloseAdapterFn),
            start_session:          get_proc!("WintunStartSession", WintunStartSessionFn),
            end_session:            get_proc!("WintunEndSession", WintunEndSessionFn),
            allocate_send_packet:   get_proc!("WintunAllocateSendPacket", WintunAllocateSendPacketFn),
            send_packet:            get_proc!("WintunSendPacket", WintunSendPacketFn),
            receive_packet:         get_proc!("WintunReceivePacket", WintunReceivePacketFn),
            release_receive_packet: get_proc!("WintunReleaseReceivePacket", WintunReleaseReceivePacketFn),
        })
    }
}

pub struct WintunBridge {
    api:     Option<WintunApi>,
    adapter: *mut std::ffi::c_void,
    session: *mut std::ffi::c_void,
    name:    String,
}

unsafe impl Send for WintunBridge {}
unsafe impl Sync for WintunBridge {}

impl WintunBridge {
    pub fn open(adapter_name: &str) -> anyhow::Result<Self> {
        let api = WintunApi::load();
        if api.is_none() {
            warn!("wintun.dll not found in PATH — running in simulation/mock TUN mode");
            return Ok(Self {
                api: None,
                adapter: std::ptr::null_mut(),
                session: std::ptr::null_mut(),
                name: adapter_name.to_string(),
            });
        }

        let api = api.unwrap();
        let pool: Vec<u16> = "WinDrop\0".encode_utf16().collect();
        let name_u16: Vec<u16> = format!("{}\0", adapter_name).encode_utf16().collect();

        let adapter = unsafe { (api.open_adapter)(PCWSTR(pool.as_ptr()), PCWSTR(name_u16.as_ptr())) };
        if adapter.is_null() {
            warn!("Wintun adapter '{}' not currently open — create it with Setup-Wintun.ps1", adapter_name);
            return Ok(Self {
                api: Some(api),
                adapter: std::ptr::null_mut(),
                session: std::ptr::null_mut(),
                name: adapter_name.to_string(),
            });
        }

        // Start session with 0x400000 ring capacity (4 MB)
        let session = unsafe { (api.start_session)(adapter, 0x400000) };
        if session.is_null() {
            warn!("Failed to start Wintun session");
        } else {
            info!("Attached to Wintun adapter '{}' successfully", adapter_name);
        }

        Ok(Self {
            api: Some(api),
            adapter,
            session,
            name: adapter_name.to_string(),
        })
    }

    /// Deliver an incoming IPv6 packet from the radio to Windows TCP/IP
    pub fn write_packet(&self, pkt: &[u8]) {
        if let (Some(api), false) = (&self.api, self.session.is_null()) {
            let buf = unsafe { (api.allocate_send_packet)(self.session, pkt.len() as u32) };
            if !buf.is_null() {
                unsafe {
                    std::ptr::copy_nonoverlapping(pkt.as_ptr(), buf, pkt.len());
                    (api.send_packet)(self.session, buf);
                }
                trace!("Injected {} bytes into Wintun {}", pkt.len(), self.name);
            }
        }
    }

    /// Read an outgoing IPv6 packet from Windows TCP/IP
    pub fn read_packet(&self) -> Option<Vec<u8>> {
        if let (Some(api), false) = (&self.api, self.session.is_null()) {
            let mut size: u32 = 0;
            let ptr = unsafe { (api.receive_packet)(self.session, &mut size) };
            if !ptr.is_null() && size > 0 {
                let slice = unsafe { std::slice::from_raw_parts(ptr, size as usize) };
                let vec = slice.to_vec();
                unsafe { (api.release_receive_packet)(self.session, ptr) };
                return Some(vec);
            }
        }
        None
    }
}

impl Drop for WintunBridge {
    fn drop(&mut self) {
        if let Some(api) = &self.api {
            if !self.session.is_null() {
                unsafe { (api.end_session)(self.session) };
            }
            if !self.adapter.is_null() {
                unsafe { (api.close_adapter)(self.adapter) };
            }
        }
    }
}

/// Task that connects Wintun ingress/egress to the daemon channels
pub async fn tun_bridge_loop(
    tun: Arc<WintunBridge>,
    mut tun_rx: mpsc::Receiver<Vec<u8>>,
    tx_tun: mpsc::Sender<Vec<u8>>,
) {
    let mut seq: u16 = 0;

    loop {
        tokio::select! {
            // Egress from radio to Windows
            in_pkt = tun_rx.recv() => {
                match in_pkt {
                    Some(pkt) => {
                        tun.write_packet(&pkt);
                    }
                    None => break,
                }
            }

            // Ingress poll from Windows to radio
            _ = tokio::time::sleep(std::time::Duration::from_millis(5)) => {
                while let Some(pkt) = tun.read_packet() {
                    // Extract destination IPv6 from header (bytes 24..40)
                    if pkt.len() >= 40 && (pkt[0] >> 4) == 6 {
                        // Map IPv6 to AWDL MAC (EUI-64 mapping or broadcast)
                        let mut da = AWDL_BSSID;
                        // If link-local fe80::, EUI-64 MAC is in bytes 32..40
                        if pkt[24] == 0xfe && pkt[25] == 0x80 {
                            da[0] = pkt[32] ^ 0x02;
                            da[1] = pkt[33];
                            da[2] = pkt[34];
                            da[3] = pkt[37];
                            da[4] = pkt[38];
                            da[5] = pkt[39];
                        }
                        seq = (seq + 1) & 0x0FFF;
                        let raw_frame = encap_awdl_data_frame(da, AWDL_BSSID, &pkt, seq);
                        let _ = tx_tun.send(raw_frame).await;
                    }
                }
            }
        }
    }
}
