//! state/mod.rs — AWDL State Machine & Master Election Engine
//!
//! Tracks:
//!   - Self MAC address and election metric
//!   - Current election master and synchronization time
//!   - Discovered peer table (Apple devices in proximity)
//!   - Ingress frame routing (PSF/MIF -> state update; Data -> Wintun TUN)
//!
//! SPDX-License-Identifier: MIT

use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::mpsc;
use tracing::{info, trace, warn};

use crate::frame::{
    build_psf_frame,
    mac_hdr::decap_awdl_data_frame,
    tlv::{AwdlTlv, ElectionParams, SyncParams},
    AwdlFrame,
};

/// Information about a discovered AWDL peer
#[derive(Debug, Clone)]
pub struct PeerInfo {
    pub mac:            [u8; 6],
    pub ipv6:           Option<[u8; 16]>,
    pub hostname:       Option<String>,
    pub rssi:           i8,
    pub last_seen:      Instant,
    pub master_metric:  u32,
    pub master_counter: u32,
}

/// AWDL node synchronization role
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NodeRole {
    Master,
    Slave,
    Electing,
}

pub struct AwdlStateMachine {
    /// Our AWDL MAC address (Apple OUI + random 3 bytes with local bit)
    pub self_mac:        [u8; 6],
    /// Our EUI-64 derived IPv6 link-local address
    pub self_ipv6:       [u8; 16],
    /// Our election metric (higher beats lower)
    pub self_metric:     u32,
    /// Our election monotonic counter
    pub self_counter:    u32,
    /// Current role
    pub role:            NodeRole,
    /// Current master MAC
    pub master_mac:      [u8; 6],
    pub master_metric:   u32,
    pub master_counter:  u32,
    /// Rolling sequence number for transmitted frames
    pub tx_seq:          u16,
    pub awdl_seq:        u16,
    /// Discovered peers
    pub peers:           HashMap<[u8; 6], PeerInfo>,
    /// Frame transmission sink to driver
    pub tx_sink:         mpsc::Sender<Vec<u8>>,
    /// Last PSF sent time
    pub last_psf_sent:   Instant,
    /// Daemon start time — used to compute a stable monotonic TX timestamp
    pub start_time:      Instant,
}

impl AwdlStateMachine {
    pub fn new(tx_sink: mpsc::Sender<Vec<u8>>) -> Self {
        // Generate random AWDL MAC: 00:17:F2:XX:YY:ZZ (with locally administered bit)
        let mut self_mac = [0x00, 0x17, 0xf2, 0x00, 0x00, 0x00];
        let rand_part = (std::process::id() ^ 0x4157) as u32;
        self_mac[3] = ((rand_part >> 16) & 0xFF) as u8 | 0x02; // locally administered
        self_mac[4] = ((rand_part >> 8) & 0xFF) as u8;
        self_mac[5] = (rand_part & 0xFF) as u8;

        // Derive EUI-64 IPv6: fe80::0217:f2ff:fexx:yyyy
        let mut self_ipv6 = [0u8; 16];
        self_ipv6[0] = 0xfe;
        self_ipv6[1] = 0x80;
        self_ipv6[8] = self_mac[0] ^ 0x02; // invert universal/local bit
        self_ipv6[9] = self_mac[1];
        self_ipv6[10] = self_mac[2];
        self_ipv6[11] = 0xff;
        self_ipv6[12] = 0xfe;
        self_ipv6[13] = self_mac[3];
        self_ipv6[14] = self_mac[4];
        self_ipv6[15] = self_mac[5];

        let self_metric = 0x0010_0000; // default baseline election metric

        Self {
            self_mac,
            self_ipv6,
            self_metric,
            self_counter: 1,
            role: NodeRole::Electing,
            master_mac: self_mac,
            master_metric: self_metric,
            master_counter: 1,
            tx_seq: 0,
            awdl_seq: 0,
            peers: HashMap::new(),
            tx_sink,
            last_psf_sent: Instant::now() - Duration::from_secs(10),
            start_time: Instant::now(),
        }
    }

    /// Process a received raw 802.11 frame from the driver
    pub async fn process_rx_frame(
        &mut self,
        raw_frame: &[u8],
        tun_sink: &mpsc::Sender<Vec<u8>>,
    ) {
        if raw_frame.is_empty() {
            return;
        }

        // 1. Try parsing as AWDL Action Frame (PSF / MIF)
        if let Some(frame) = AwdlFrame::parse(raw_frame) {
            self.handle_action_frame(&frame).await;
            return;
        }

        // 2. Try decapsulating as an AWDL Data Frame (IPv6 over AWDL SNAP)
        if let Some((ipv6_payload, da, sa)) = decap_awdl_data_frame(raw_frame) {
            trace!(
                "Decapsulated AWDL Data: {} bytes from {:02x?} to {:02x?}",
                ipv6_payload.len(),
                sa,
                da
            );
            // Forward IPv6 packet to Wintun TUN adapter
            let _ = tun_sink.send(ipv6_payload.to_vec()).await;
            return;
        }
    }

    async fn handle_action_frame(&mut self, frame: &AwdlFrame) {
        let sa = frame.mac.addr2;
        if sa == self.self_mac {
            return; // Ignore own echoes
        }

        let tlvs = frame.tlvs();
        let mut master_update = None;
        let mut peer_ip = None;
        let mut peer_host = None;

        for tlv in &tlvs {
            match tlv {
                AwdlTlv::Election(e) => {
                    master_update = Some(e.clone());
                }
                AwdlTlv::PeerAddr(p) => {
                    peer_ip = Some(p.ipv6_addr);
                    peer_host = Some(p.hostname.clone());
                }
                _ => {}
            }
        }

        // Update peer table
        let entry = self.peers.entry(sa).or_insert_with(|| {
            info!(
                mac = format!("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                    sa[0], sa[1], sa[2], sa[3], sa[4], sa[5]),
                "Discovered new AWDL peer"
            );
            PeerInfo {
                mac: sa,
                ipv6: peer_ip,
                hostname: peer_host.clone(),
                rssi: 0,
                last_seen: Instant::now(),
                master_metric: 0,
                master_counter: 0,
            }
        });

        entry.last_seen = Instant::now();
        if let Some(ip) = peer_ip {
            entry.ipv6 = Some(ip);
        }
        if let Some(host) = peer_host {
            entry.hostname = Some(host);
        }

        // Process election parameters
        if let Some(e) = master_update {
            entry.master_metric = e.master_metric;
            entry.master_counter = e.master_counter;

            // Master Election rule: higher metric wins. If equal, higher MAC wins.
            let dominates = e.master_metric > self.master_metric
                || (e.master_metric == self.master_metric && e.master_id > self.master_mac);

            if dominates {
                if self.master_mac != e.master_id {
                    info!(
                        old = format!("{:02X?}", self.master_mac),
                        new = format!("{:02X?}", e.master_id),
                        metric = e.master_metric,
                        "Yielding to higher AWDL Master"
                    );
                }
                self.master_mac = e.master_id;
                self.master_metric = e.master_metric;
                self.master_counter = e.master_counter;
                self.role = NodeRole::Slave;
            }
        }
    }

    /// Emits a Periodic Synchronisation Frame (PSF)
    pub async fn emit_psf(&mut self, current_channel: u8) {
        self.tx_seq = (self.tx_seq + 1) & 0x0FFF;
        self.awdl_seq = (self.awdl_seq + 1) & 0xFFFF;
        self.self_counter += 1;

        let sync_tlv = AwdlTlv::Sync(SyncParams {
            next_channel: current_channel,
            tx_counter: 1,
            master_chan: 44, // social channel
            guard_time: 0,
            aw_period: 16, // 16 ms availability window
            aw_seq: self.awdl_seq,
        });

        let election_tlv = AwdlTlv::Election(ElectionParams {
            flags: 0x00,
            master_id: self.master_mac,
            master_metric: self.master_metric,
            master_counter: self.master_counter,
            self_metric: self.self_metric,
            self_counter: self.self_counter,
        });

        let tlvs = vec![sync_tlv, election_tlv];

        // Compute target_tx as microseconds elapsed since daemon start,
        // truncated to 32 bits — this gives a stable, monotonically increasing
        // PHY-TSF-like counter that Apple peers can sanity-check.
        let target_tx = (self.start_time.elapsed().as_micros() & 0xFFFF_FFFF) as u32;
        let psf = build_psf_frame(self.self_mac, self.tx_seq, self.awdl_seq, target_tx, &tlvs);

        if let Err(e) = self.tx_sink.send(psf).await {
            warn!("Failed to enqueue outgoing PSF: {:?}", e);
        } else {
            self.last_psf_sent = Instant::now();
        }
    }

    /// Evict stale peers older than timeout
    pub fn prune_stale_peers(&mut self, timeout: Duration) {
        let now = Instant::now();
        self.peers.retain(|mac, peer| {
            let keep = now.duration_since(peer.last_seen) < timeout;
            if !keep {
                info!(
                    mac = format!("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]),
                    "Pruned stale AWDL peer"
                );
            }
            keep
        });
    }

    /// Register or update a peer discovered via BLE or AWDL frames
    pub fn upsert_peer(&mut self, mac: [u8; 6], hostname: Option<String>, rssi: i8) {
        let entry = self.peers.entry(mac).or_insert_with(|| {
            info!(
                mac = format!("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]),
                host = ?hostname,
                rssi = rssi,
                "Discovered Apple peer via Continuity BLE"
            );
            PeerInfo {
                mac,
                ipv6: None,
                hostname: hostname.clone(),
                rssi,
                last_seen: Instant::now(),
                master_metric: 0,
                master_counter: 0,
            }
        });
        entry.last_seen = Instant::now();
        entry.rssi = rssi;
        if let Some(h) = hostname {
            entry.hostname = Some(h);
        }
    }

    /// Retrieve the top active peers seen within the last 30s with valid RSSI, sorted descending by signal strength
    pub fn get_top_peers(&self, limit: usize) -> Vec<PeerInfo> {
        let now = Instant::now();
        let mut list: Vec<PeerInfo> = self
            .peers
            .values()
            .filter(|p| now.duration_since(p.last_seen).as_secs() <= 30 && p.rssi > -100)
            .cloned()
            .collect();
        list.sort_by(|a, b| b.rssi.cmp(&a.rssi));
        list.truncate(limit);
        list
    }
}

/// Long-running task for processing incoming frames into the state machine
pub async fn awdl_state_loop(
    state: Arc<tokio::sync::Mutex<AwdlStateMachine>>,
    mut rx_source: mpsc::Receiver<Vec<u8>>,
    tun_sink: mpsc::Sender<Vec<u8>>,
) {
    let mut prune_interval = tokio::time::interval(Duration::from_secs(10));
    prune_interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    loop {
        tokio::select! {
            frame = rx_source.recv() => {
                match frame {
                    Some(raw) => {
                        let mut st = state.lock().await;
                        st.process_rx_frame(&raw, &tun_sink).await;
                    }
                    None => {
                        warn!("RX frame channel closed, exiting awdl_state_loop");
                        break;
                    }
                }
            }
            _ = prune_interval.tick() => {
                let mut st = state.lock().await;
                st.prune_stale_peers(Duration::from_secs(30));
            }
        }
    }
}
