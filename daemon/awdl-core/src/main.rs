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
use airdrop::{AirDropBonjour, AirDropServer};
use std::path::PathBuf;

/// Shared raw-frame channel capacities
const RX_CHAN_DEPTH: usize = 512;
const TX_CHAN_DEPTH: usize = 256;

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

    // --- Open kernel driver device ---
    let radio = Arc::new(RadioDevice::open().expect("Failed to open \\\\.\\AwdlRadioDevice"));
    let status = radio.get_status()?;
    let chip_id = status.chip_id;
    let fw_ver = status.firmware_version;
    let fw_loaded = status.firmware_loaded;
    info!(
        chip_id   = format!("{:#010X}", chip_id),
        fw_ver    = format!("{:#010X}", fw_ver),
        fw_loaded = fw_loaded,
        "Radio status"
    );

    if fw_loaded == 0 {
        error!("Firmware not loaded — run Setup-WinDrop.ps1 first");
        return Err(anyhow::anyhow!("Firmware not loaded"));
    }

    // --- Switch to initial AWDL social channel (44) ---
    radio.set_channel(44, 1, 0)?;
    info!("Initial channel: 44 (5 GHz / 20 MHz)");

    // --- Inter-task channels ---
    let (rx_tx, rx_rx) = mpsc::channel::<Vec<u8>>(RX_CHAN_DEPTH);
    let (tx_tx, tx_rx) = mpsc::channel::<Vec<u8>>(TX_CHAN_DEPTH);
    let (tun_tx, tun_rx) = mpsc::channel::<Vec<u8>>(RX_CHAN_DEPTH);

    // --- Shared state machine ---
    let state = Arc::new(tokio::sync::Mutex::new(
        AwdlStateMachine::new(tx_tx.clone()),
    ));

    // --- Wintun bridge ---
    let tun = Arc::new(WintunBridge::open("awdl0")?);

    // --- Spawn tasks ---
    let radio_rx = Arc::clone(&radio);
    let t_rx = tokio::spawn(async move {
        driver::rx_loop(radio_rx, rx_tx).await;
    });

    let radio_tx = Arc::clone(&radio);
    let t_tx = tokio::spawn(async move {
        driver::tx_loop(radio_tx, tx_rx).await;
    });

    let sched_radio = Arc::clone(&radio);
    let sched_state = Arc::clone(&state);
    let t_sched = tokio::spawn(async move {
        ChannelScheduler::new(sched_radio, sched_state)
            .run()
            .await;
    });

    let state_rx  = Arc::clone(&state);
    let tun_sink  = tun_tx.clone();
    let t_state   = tokio::spawn(async move {
        state::awdl_state_loop(state_rx, rx_rx, tun_sink).await;
    });

    let tun_clone = Arc::clone(&tun);
    let tx_tun    = tx_tx.clone();
    let t_tun     = tokio::spawn(async move {
        tun::tun_bridge_loop(tun_clone, tun_rx, tx_tun).await;
    });

    // --- Phase 4: AirDrop Server & Bonjour Discovery ---
    let download_dir = PathBuf::from("downloads").join("AirDrop");
    let server = Arc::new(AirDropServer::new(download_dir));
    let t_server = tokio::spawn(async move {
        let addr = "[::]:8770".parse().expect("Valid IPv6 bind address");
        if let Err(e) = server.run(addr).await {
            error!("AirDrop server error: {:?}", e);
        }
    });

    let self_ip = {
        let st = state.lock().await;
        std::net::Ipv6Addr::from(st.self_ipv6)
    };

    if let Ok(bonjour) = AirDropBonjour::new("Win-Drop-PC") {
        let _ = bonjour.register_service("Win-Drop-PC", self_ip);
        let _ = bonjour.start_browsing();
        info!("AirDrop Bonjour service announced as 'Win-Drop-PC'");
    }

    info!("All tasks running — AWDL daemon and AirDrop services active");

    // Wait for any task to exit (they should run forever unless fatal error)
    tokio::select! {
        r = t_rx     => warn!("rx_loop exited: {:?}", r),
        r = t_tx     => warn!("tx_loop exited: {:?}", r),
        r = t_sched  => warn!("channel_scheduler exited: {:?}", r),
        r = t_state  => warn!("awdl_state_loop exited: {:?}", r),
        r = t_tun    => warn!("tun_bridge_loop exited: {:?}", r),
        r = t_server => warn!("airdrop server exited: {:?}", r),
    }

    error!("Daemon exiting unexpectedly");
    Ok(())
}
