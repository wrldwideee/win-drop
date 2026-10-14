//! airdrop/ble.rs — Apple Continuity Bluetooth LE Advertiser & Scanner
//!
//! Features:
//!   1. Continuous Apple Continuity AirDrop Advertisement (Company ID 0x004C, Type 0x05)
//!      broadcast via Windows WinRT BluetoothLEAdvertisementPublisher.
//!   2. Passive/Active Continuity Scanner listening for nearby Apple devices (iPhones, Macs, iPads)
//!      via BluetoothLEAdvertisementWatcher and populating the AWDL state machine peer table.
//!
//! SPDX-License-Identifier: MIT

use std::sync::Arc;
use tokio::sync::Mutex;
use tracing::{info, warn};

use windows::core::Result;
use windows::Devices::Bluetooth::Advertisement::{
    BluetoothLEAdvertisementPublisher, BluetoothLEAdvertisementReceivedEventArgs,
    BluetoothLEAdvertisementWatcher, BluetoothLEManufacturerData, BluetoothLEScanningMode,
};
use windows::Foundation::TypedEventHandler;
use windows::Storage::Streams::{DataReader, DataWriter};

use crate::state::AwdlStateMachine;

pub const APPLE_COMPANY_ID: u16 = 0x004C;

pub struct AirDropBleService {
    publisher: Option<BluetoothLEAdvertisementPublisher>,
    watcher:   Option<BluetoothLEAdvertisementWatcher>,
}

impl AirDropBleService {
    /// Start both the AirDrop BLE advertiser and Apple Continuity watcher
    pub fn start(state: Arc<Mutex<AwdlStateMachine>>) -> Self {
        let publisher = match Self::start_advertiser() {
            Ok(publ) => {
                info!("Apple Continuity AirDrop BLE advertisement broadcasting active (0x004C)");
                Some(publ)
            }
            Err(e) => {
                warn!("Bluetooth LE advertiser unavailable (Bluetooth may be off): {:?}", e);
                None
            }
        };

        let watcher = match Self::start_scanner(state) {
            Ok(wat) => {
                info!("Apple Continuity BLE scanner listening for nearby Apple devices");
                Some(wat)
            }
            Err(e) => {
                warn!("Bluetooth LE scanner unavailable: {:?}", e);
                None
            }
        };

        Self { publisher, watcher }
    }

    /// Broadcast Apple Continuity AirDrop advertisement (Type 0x05, 18-byte payload)
    fn start_advertiser() -> Result<BluetoothLEAdvertisementPublisher> {
        let publisher = BluetoothLEAdvertisementPublisher::new()?;

        // Apple Continuity AirDrop payload:
        // Byte 0: Type = 0x05 (AirDrop)
        // Byte 1: Length = 0x12 (18 bytes)
        // Bytes 2..9: Short contact hashes (all zeros for public broadcast)
        // Byte 10: 0x01 (Flags: Discoverable)
        // Bytes 11..19: Reserved / zero padding
        let payload: [u8; 20] = [
            0x05, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        ];

        let writer = DataWriter::new()?;
        writer.WriteBytes(&payload)?;
        let buffer = writer.DetachBuffer()?;

        let mfg_data = BluetoothLEManufacturerData::Create(APPLE_COMPANY_ID, &buffer)?;
        let advertisement = publisher.Advertisement()?;
        advertisement.ManufacturerData()?.Append(&mfg_data)?;
        publisher.Start()?;

        Ok(publisher)
    }

    /// Scan for Apple Continuity advertisements (Company ID 0x004C)
    fn start_scanner(state: Arc<Mutex<AwdlStateMachine>>) -> Result<BluetoothLEAdvertisementWatcher> {
        let watcher = BluetoothLEAdvertisementWatcher::new()?;
        watcher.SetScanningMode(BluetoothLEScanningMode::Active)?;

        // Channel to forward BLE peer detections to Tokio async worker
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<([u8; 6], String, i8)>();

        let state_worker = Arc::clone(&state);
        tokio::spawn(async move {
            while let Some((mac, host, rssi)) = rx.recv().await {
                let mut st = state_worker.lock().await;
                st.upsert_peer(mac, Some(host), rssi);
            }
        });

        let handler = TypedEventHandler::new(
            move |_sender: &Option<BluetoothLEAdvertisementWatcher>,
                  args: &Option<BluetoothLEAdvertisementReceivedEventArgs>| {
                if let Some(args) = args {
                    let addr = match args.BluetoothAddress() {
                        Ok(a) => a,
                        Err(_) => return Ok(()),
                    };

                    let rssi = args.RawSignalStrengthInDBm().unwrap_or(-80) as i8;

                    let ad = match args.Advertisement() {
                        Ok(ad) => ad,
                        Err(_) => return Ok(()),
                    };

                    let mfg_list = match ad.ManufacturerData() {
                        Ok(list) => list,
                        Err(_) => return Ok(()),
                    };

                    for mfg in mfg_list {
                        if mfg.CompanyId().unwrap_or(0) == APPLE_COMPANY_ID {
                            let data = match mfg.Data() {
                                Ok(d) => d,
                                Err(_) => continue,
                            };

                            let reader = match DataReader::FromBuffer(&data) {
                                Ok(r) => r,
                                Err(_) => continue,
                            };

                            let len = reader.UnconsumedBufferLength().unwrap_or(0) as usize;
                            if len == 0 { continue; }

                            let mut bytes = vec![0u8; len];
                            if reader.ReadBytes(&mut bytes).is_err() { continue; }

                            let type_byte = bytes[0];
                            let dev_name = match type_byte {
                                0x05 => "Apple iPhone (AirDrop)",
                                0x10 => "Apple Device (Nearby)",
                                0x07 => "Apple Device (Proximity)",
                                0x09 => "Apple Mac (AirDrop)",
                                0x0f => "Apple Device (Continuity)",
                                _    => "Apple Device",
                            };

                            let mac = [
                                ((addr >> 40) & 0xFF) as u8,
                                ((addr >> 32) & 0xFF) as u8,
                                ((addr >> 24) & 0xFF) as u8,
                                ((addr >> 16) & 0xFF) as u8,
                                ((addr >> 8) & 0xFF) as u8,
                                (addr & 0xFF) as u8,
                            ];

                            info!(
                                bt_addr = format!("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]),
                                rssi = rssi,
                                continuity_type = format!("{:#04X}", type_byte),
                                name = dev_name,
                                "Detected real Apple device over BLE!"
                            );

                            let _ = tx.send((mac, dev_name.to_string(), rssi));
                        }
                    }
                }
                Ok(())
            },
        );

        watcher.Received(&handler)?;
        watcher.Start()?;

        Ok(watcher)
    }

    /// Stop advertiser and scanner
    pub fn stop(&mut self) {
        if let Some(publ) = &self.publisher {
            let _ = publ.Stop();
        }
        if let Some(wat) = &self.watcher {
            let _ = wat.Stop();
        }
    }
}

impl Drop for AirDropBleService {
    fn drop(&mut self) {
        self.stop();
    }
}
