//! driver.rs — Windows kernel IOCTL interface to mt7922_awdl.sys
//!
//! Wraps CreateFile / DeviceIoControl calls to \\.\AwdlRadioDevice.
//! Provides:
//!   - RadioDevice::open()          — open the kernel device
//!   - RadioDevice::get_status()    — IOCTL_AWDL_GET_STATUS
//!   - RadioDevice::set_channel()   — IOCTL_AWDL_SET_CHANNEL
//!   - RadioDevice::inject_frame()  — IOCTL_AWDL_INJECT_FRAME
//!   - rx_loop()                    — blocking IOCTL_AWDL_READ_FRAMES loop
//!   - tx_loop()                    — drain mpsc::Receiver, call inject_frame
//!
//! SPDX-License-Identifier: MIT

use std::sync::Arc;
use tokio::sync::mpsc;
use tracing::{debug, error, trace, warn};
use windows::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_NONE,
    OPEN_EXISTING,
};
use windows::Win32::System::IO::DeviceIoControl;
use windows::core::PCWSTR;

/// Device path — matches the DOS symlink in mt7922_init.c
const DEVICE_PATH: &[u16] = &[
    b'\\' as u16, b'\\' as u16, b'.' as u16, b'\\' as u16,
    b'A' as u16, b'w' as u16, b'd' as u16, b'l' as u16,
    b'R' as u16, b'a' as u16, b'd' as u16, b'i' as u16,
    b'o' as u16, b'D' as u16, b'e' as u16, b'v' as u16,
    b'i' as u16, b'c' as u16, b'e' as u16, 0u16,
];

// IOCTL codes — must match mt7922_awdl.h
const AWDL_IOCTL_TYPE: u32 = 0x9944;
const AWDL_IOCTL_BASE: u32 = 0x800;

#[inline(always)]
const fn ctl_code(typ: u32, func: u32, method: u32, access: u32) -> u32 {
    (typ << 16) | (access << 14) | (func << 2) | method
}

const METHOD_BUFFERED:    u32 = 0;
const FILE_READ_DATA:     u32 = 0x0001;
const FILE_WRITE_DATA:    u32 = 0x0002;

const IOCTL_AWDL_SET_CHANNEL: u32 = ctl_code(
    AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 0, METHOD_BUFFERED,
    FILE_READ_DATA | FILE_WRITE_DATA);
const IOCTL_AWDL_INJECT_FRAME: u32 = ctl_code(
    AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 1, METHOD_BUFFERED, FILE_WRITE_DATA);
const IOCTL_AWDL_READ_FRAMES: u32 = ctl_code(
    AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 2, METHOD_BUFFERED, FILE_READ_DATA);
const IOCTL_AWDL_GET_STATUS: u32 = ctl_code(
    AWDL_IOCTL_TYPE, AWDL_IOCTL_BASE + 3, METHOD_BUFFERED, FILE_READ_DATA);

// ---- Payload types (must match mt7922_awdl.h, #[repr(C, packed)]) ----
#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct SetChannelIn {
    pub channel: u8,
    pub band:    u8,   // 1 = 5 GHz
    pub width:   u8,   // 0 = 20 MHz
    pub _pad:    u8,
}

#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct InjectFrameHeader {
    pub frame_length: u32,
    pub no_ack:       u8,
    pub _pad:         [u8; 3],
    // frame data follows immediately
}

#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct RxFrameHeader {
    pub total_length: u32,
    pub timestamp:    u64,
    pub rssi:         i8,
    pub channel:      u8,
    pub _pad:         [u8; 2],
    // frame data follows immediately
}

#[repr(C, packed)]
#[derive(Debug, Clone, Copy)]
pub struct DriverStatus {
    pub chip_id:          u32,
    pub firmware_version: u32,
    pub firmware_loaded:  u8,
    pub radio_enabled:    u8,
    pub current_channel:  u8,
    pub _pad:             u8,
}

// ---- RadioDevice ----

pub struct RadioDevice {
    handle: HANDLE,
}

// SAFETY: HANDLE is Send if we don't share it across threads unsafely.
// We serialise all calls through &self methods which are not called concurrently.
unsafe impl Send for RadioDevice {}
unsafe impl Sync for RadioDevice {}

impl RadioDevice {
    /// Open \\.\AwdlRadioDevice.  Panics if driver is not loaded.
    pub fn open() -> windows::core::Result<Self> {
        let handle = unsafe {
            CreateFileW(
                PCWSTR(DEVICE_PATH.as_ptr()),
                (FILE_READ_DATA | FILE_WRITE_DATA) as u32,
                FILE_SHARE_NONE,
                None,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                HANDLE::default(),
            )?
        };
        if handle == INVALID_HANDLE_VALUE {
            return Err(windows::core::Error::from_win32());
        }
        Ok(Self { handle })
    }

    /// IOCTL_AWDL_GET_STATUS
    pub fn get_status(&self) -> windows::core::Result<DriverStatus> {
        let mut out = DriverStatus {
            chip_id: 0, firmware_version: 0,
            firmware_loaded: 0, radio_enabled: 0,
            current_channel: 0, _pad: 0,
        };
        let mut bytes_returned: u32 = 0;
        unsafe {
            DeviceIoControl(
                self.handle,
                IOCTL_AWDL_GET_STATUS,
                None, 0,
                Some(&mut out as *mut _ as *mut _),
                std::mem::size_of::<DriverStatus>() as u32,
                Some(&mut bytes_returned),
                None,
            )?;
        }
        Ok(out)
    }

    /// IOCTL_AWDL_SET_CHANNEL
    pub fn set_channel(&self, channel: u8, band: u8, width: u8)
        -> windows::core::Result<()>
    {
        let inp = SetChannelIn { channel, band, width, _pad: 0 };
        let mut bytes_returned: u32 = 0;
        unsafe {
            DeviceIoControl(
                self.handle,
                IOCTL_AWDL_SET_CHANNEL,
                Some(&inp as *const _ as *const _),
                std::mem::size_of::<SetChannelIn>() as u32,
                None, 0,
                Some(&mut bytes_returned),
                None,
            )?;
        }
        Ok(())
    }

    /// IOCTL_AWDL_INJECT_FRAME
    pub fn inject_frame(&self, frame: &[u8], no_ack: bool)
        -> windows::core::Result<()>
    {
        let hdr_size = std::mem::size_of::<InjectFrameHeader>();
        let mut buf  = Vec::with_capacity(hdr_size + frame.len());

        let hdr = InjectFrameHeader {
            frame_length: frame.len() as u32,
            no_ack:       no_ack as u8,
            _pad:         [0u8; 3],
        };
        // SAFETY: packed repr, plain bytes
        let hdr_bytes: &[u8] = unsafe {
            std::slice::from_raw_parts(&hdr as *const _ as *const u8, hdr_size)
        };
        buf.extend_from_slice(hdr_bytes);
        buf.extend_from_slice(frame);

        let mut bytes_returned: u32 = 0;
        unsafe {
            DeviceIoControl(
                self.handle,
                IOCTL_AWDL_INJECT_FRAME,
                Some(buf.as_ptr() as *const _),
                buf.len() as u32,
                None, 0,
                Some(&mut bytes_returned),
                None,
            )?;
        }
        Ok(())
    }

    /// IOCTL_AWDL_READ_FRAMES — fills provided buffer with zero or more
    /// RxFrameHeader-prefixed raw 802.11 frames.
    pub fn read_frames(&self, out: &mut Vec<u8>) -> windows::core::Result<usize> {
        out.resize(64 * 1024, 0u8);
        let mut bytes_returned: u32 = 0;
        unsafe {
            DeviceIoControl(
                self.handle,
                IOCTL_AWDL_READ_FRAMES,
                None, 0,
                Some(out.as_mut_ptr() as *mut _),
                out.len() as u32,
                Some(&mut bytes_returned),
                None,
            )?;
        }
        out.truncate(bytes_returned as usize);
        Ok(bytes_returned as usize)
    }
}

impl Drop for RadioDevice {
    fn drop(&mut self) {
        unsafe { let _ = CloseHandle(self.handle); }
    }
}

// ---- Async loops ----

/// Continuously reads frames from the kernel driver and forwards them to
/// the AWDL state machine via the `tx` sender channel.
pub async fn rx_loop(radio: Arc<RadioDevice>, tx: mpsc::Sender<Vec<u8>>) {
    loop {
        // Offload blocking IOCTL to Tokio's blocking thread pool so the
        // async executor isn't stalled during the kernel wait.
        let radio_clone = Arc::clone(&radio);
        let result = tokio::task::spawn_blocking(move || {
            let mut b = Vec::with_capacity(64 * 1024);
            radio_clone.read_frames(&mut b).map(|_| b)
        })
        .await;

        match result {
            Ok(Ok(data)) if !data.is_empty() => {
                // Demux packed frames
                let mut ptr = 0usize;
                while ptr + std::mem::size_of::<RxFrameHeader>() <= data.len() {
                    let hdr = unsafe {
                        *(data.as_ptr().add(ptr) as *const RxFrameHeader)
                    };
                    let total = hdr.total_length as usize;
                    if total == 0 || ptr + total > data.len() { break; }

                    let hdr_sz = std::mem::size_of::<RxFrameHeader>();
                    let frame  = data[ptr + hdr_sz .. ptr + total].to_vec();

                    trace!(len = frame.len(), rssi = hdr.rssi, ch = hdr.channel,
                           "RX frame");

                    if tx.send(frame).await.is_err() { return; }
                    ptr += total;
                }
            }
            Ok(Ok(_)) => {
                // No data available — yield to scheduler
                tokio::time::sleep(tokio::time::Duration::from_millis(5)).await;
            }
            Ok(Err(e)) => {
                warn!("read_frames error: {}", e);
                tokio::time::sleep(tokio::time::Duration::from_millis(100)).await;
            }
            Err(e) => {
                error!("spawn_blocking panic: {}", e);
                break;
            }
        }
    }
}

/// Drains the TX channel and injects each frame via the kernel driver.
pub async fn tx_loop(radio: Arc<RadioDevice>, mut rx: mpsc::Receiver<Vec<u8>>) {
    while let Some(frame) = rx.recv().await {
        let radio_clone = Arc::clone(&radio);
        let result = tokio::task::spawn_blocking(move || {
            radio_clone.inject_frame(&frame, true)
        })
        .await;

        match result {
            Ok(Ok(_))  => debug!("Frame injected"),
            Ok(Err(e)) => warn!("inject_frame error: {}", e),
            Err(e)     => error!("spawn_blocking panic in tx_loop: {}", e),
        }
    }
}
