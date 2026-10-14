//! awdl-daemon — WIN-DROP Phase 3 AWDL Protocol Engine
//!
//! Architecture:
//!   main()
//!     └── tokio::main (multi-thread, high-priority)
//!           ├── radio_rx_task   — reads raw 802.11 frames from driver via IOCTL
//!           ├── radio_tx_task   — drains the TX channel and injects via IOCTL
//!           ├── channel_scheduler_task — fires every 102.4 ms to hop channels
//!           ├── awdl_state_task — parses AWDL frames, runs master election
//!           └── tun_bridge_task — bridges decapsulated data to/from awdl0 Wintun
//!
//! SPDX-License-Identifier: MIT

#![allow(dead_code, unused_variables)]

use std::sync::Arc;
use tokio::sync::mpsc;
use tracing::{error, info, warn};
use tracing_subscriber::EnvFilter;

mod driver;
mod frame;
mod state;
mod scheduler;
mod tun;
mod airdrop;

use driver::RadioDevice;
use state::AwdlStateMachine;
use scheduler::ChannelScheduler;
use tun::WintunBridge;
use airdrop::{AirDropBonjour, AirDropServer, AirDropBleService};
use std::path::PathBuf;

/// Shared raw-frame channel capacities
const RX_CHAN_DEPTH: usize = 512;
const TX_CHAN_DEPTH: usize = 256;

/// Helper to discover all local IPv4 LAN addresses
fn get_all_local_ipv4s() -> Vec<String> {
    let mut ips = Vec::new();
    if let Ok(socket) = std::net::UdpSocket::bind("0.0.0.0:0") {
        if socket.connect("8.8.8.8:80").is_ok() {
            if let Ok(addr) = socket.local_addr() {
                if let std::net::IpAddr::V4(ipv4) = addr.ip() {
                    ips.push(ipv4.to_string());
                }
            }
        }
    }
    if let Ok(output) = std::process::Command::new("ipconfig").output() {
        let text = String::from_utf8_lossy(&output.stdout);
        for line in text.lines() {
            if line.contains("IPv4 Address") || line.contains("IPv4-Adresse") || line.contains("IPv4") {
                if let Some(pos) = line.find(':') {
                    let ip = line[pos + 1..].trim();
                    if !ip.is_empty() && !ips.contains(&ip.to_string()) && ip != "127.0.0.1" {
                        ips.push(ip.to_string());
                    }
                }
            }
        }
    }
    ips
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // --- Logging ---
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("info,awdl_core=debug")),
        )
        .init();

    info!("WIN-DROP awdl-daemon starting");

    // --- Probe kernel driver device or fallback to Hybrid Wi-Fi mode ---
    let radio_opt: Option<Arc<RadioDevice>> = match RadioDevice::open() {
        Ok(r) => match r.get_status() {
            Ok(status) if status.firmware_loaded != 0 => {
                let chip_id = status.chip_id;
                let fw_ver = status.firmware_version;
                let fw_loaded = status.firmware_loaded;
                info!(
                    chip_id   = format!("{:#010X}", chip_id),
                    fw_ver    = format!("{:#010X}", fw_ver),
                    fw_loaded = fw_loaded,
                    "Radio status (Kernel AWDL Mode active)"
                );
                let _ = r.set_channel(44, 1, 0);
                info!("Initial channel: 44 (5 GHz / 20 MHz)");
                Some(Arc::new(r))
            }
            _ => {
                warn!("Radio opened but firmware not loaded — running in Hybrid Wi-Fi Mode");
                None
            }
        },
        Err(e) => {
            info!(
                "Kernel AWDL device unavailable ({}) — Running in Hybrid Wi-Fi + Hardware BLE Mode (normal Wi-Fi active)",
                e
            );
            None
        }
    };

    // Print all local LAN IPv4 addresses so the user can easily open them on their iPad
    let local_ips = get_all_local_ipv4s();
    if local_ips.is_empty() {
        info!("Open http://localhost:8770 to access the Win-Drop Web UI");
    } else {
        for ip in &local_ips {
            info!("Open http://{}:8770 on your iPad Safari to AirDrop files to this PC!", ip);
        }
    }

    // --- Inter-task channels ---
    let (rx_tx, rx_rx) = mpsc::channel::<Vec<u8>>(RX_CHAN_DEPTH);
    let (tx_tx, tx_rx) = mpsc::channel::<Vec<u8>>(TX_CHAN_DEPTH);
    let (tun_tx, tun_rx) = mpsc::channel::<Vec<u8>>(RX_CHAN_DEPTH);

    // --- Shared state machine ---
    let state = Arc::new(tokio::sync::Mutex::new(
        AwdlStateMachine::new(tx_tx.clone()),
    ));

    // --- Spawn driver & scheduler tasks (if kernel driver bound) ---
    let mut task_handles = Vec::new();

    if let Some(radio) = &radio_opt {
        let radio_rx = Arc::clone(radio);
        task_handles.push(tokio::spawn(async move {
            driver::rx_loop(radio_rx, rx_tx).await;
        }));

        let radio_tx = Arc::clone(radio);
        task_handles.push(tokio::spawn(async move {
            driver::tx_loop(radio_tx, tx_rx).await;
        }));

        let sched_radio = Arc::clone(radio);
        let sched_state = Arc::clone(&state);
        task_handles.push(tokio::spawn(async move {
            ChannelScheduler::new(sched_radio, sched_state).run().await;
        }));

        match WintunBridge::open("awdl0") {
            Ok(tun) => {
                let tun_arc = Arc::new(tun);
                let tx_tun = tx_tx.clone();
                task_handles.push(tokio::spawn(async move {
                    tun::tun_bridge_loop(tun_arc, tun_rx, tx_tun).await;
                }));
            }
            Err(e) => {
                warn!("Wintun adapter 'awdl0' could not be initialized: {:?}", e);
            }
        }
    }

    let state_rx = Arc::clone(&state);
    let tun_sink = tun_tx.clone();
    task_handles.push(tokio::spawn(async move {
        state::awdl_state_loop(state_rx, rx_rx, tun_sink).await;
    }));

    // --- Phase 4: AirDrop Server & Bonjour Discovery ---
    let download_dir = PathBuf::from("downloads").join("AirDrop");
    let server = Arc::new(AirDropServer::new(
        download_dir,
        radio_opt.clone(),
        Arc::clone(&state),
    ));

    task_handles.push(tokio::spawn(async move {
        let addr = "[::]:8770".parse().expect("Valid IPv6 bind address");
        if let Err(e) = server.run(addr).await {
            error!("AirDrop server error: {:?}", e);
        }
    }));

    let self_ip = {
        let st = state.lock().await;
        std::net::Ipv6Addr::from(st.self_ipv6)
    };

    if let Ok(bonjour) = AirDropBonjour::new("Win-Drop-PC") {
        let _ = bonjour.register_service("Win-Drop-PC", self_ip);
        let _ = bonjour.start_browsing();
        info!("AirDrop Bonjour service announced as 'Win-Drop-PC'");
    }

    // --- Apple Continuity BLE Advertiser & Scanner ---
    let _ble = AirDropBleService::start(Arc::clone(&state));

    info!("All tasks running — AWDL daemon and AirDrop services active");

    // Wait for Ctrl+C or task exit
    tokio::select! {
        _ = tokio::signal::ctrl_c() => {
            info!("Received Ctrl+C, shutting down cleanly");
        }
    }

    Ok(())
}
