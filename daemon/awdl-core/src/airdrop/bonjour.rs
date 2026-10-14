//! airdrop/bonjour.rs — AirDrop mDNS / DNS-SD Service Discovery
//!
//! Advertises the Windows machine as an AirDrop receiver via `_airdrop._tcp.local`
//! and browses for nearby Apple devices (iPhones, iPads, Macs).
//!
//! TXT Record format:
//!   - `flags`: "1" (everyone / discoverable)
//!   - `name`: Display name of the PC (e.g. "Windows PC (WinDrop)")
//!   - `phash`: SHA256 hashes of contact identifiers (empty for everyone mode)
//!
//! SPDX-License-Identifier: MIT

use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};
use std::collections::HashMap;
use std::net::Ipv6Addr;
use std::sync::Arc;
use tokio::sync::Mutex;
use tracing::info;

pub const AIRDROP_SERVICE_TYPE: &str = "_airdrop._tcp.local.";
pub const AIRDROP_HTTP_PORT: u16     = 8770;

#[derive(Debug, Clone)]
pub struct DiscoveredAirDropPeer {
    pub instance_name: String,
    pub hostname:      String,
    pub port:          u16,
    pub ipv6_addr:     Option<Ipv6Addr>,
    pub display_name:  String,
}

pub struct AirDropBonjour {
    daemon:   ServiceDaemon,
    peers:    Arc<Mutex<HashMap<String, DiscoveredAirDropPeer>>>,
    hostname: String,
}

impl AirDropBonjour {
    pub fn new(display_name: &str) -> anyhow::Result<Self> {
        let daemon = ServiceDaemon::new()?;
        let peers = Arc::new(Mutex::new(HashMap::new()));
        let hostname = format!("{}.local.", display_name.to_lowercase().replace(' ', "-"));

        Ok(Self {
            daemon,
            peers,
            hostname,
        })
    }

    /// Register our AirDrop receiver service so Apple devices can discover us
    pub fn register_service(
        &self,
        display_name: &str,
        ipv6: Ipv6Addr,
    ) -> anyhow::Result<()> {
        let mut properties = HashMap::new();
        properties.insert("flags".to_string(), "1".to_string());
        properties.insert("name".to_string(), display_name.to_string());

        let host = &self.hostname;
        let service = ServiceInfo::new(
            AIRDROP_SERVICE_TYPE,
            display_name,
            host,
            ipv6.to_string().as_str(),
            AIRDROP_HTTP_PORT,
            properties,
        )?;

        self.daemon.register(service)?;
        info!(
            name = display_name,
            ipv6 = %ipv6,
            port = AIRDROP_HTTP_PORT,
            "AirDrop mDNS service registered"
        );
        Ok(())
    }

    /// Start listening for neighboring Apple AirDrop nodes
    pub fn start_browsing(&self) -> anyhow::Result<()> {
        let receiver = self.daemon.browse(AIRDROP_SERVICE_TYPE)?;
        let peers_map = Arc::clone(&self.peers);

        tokio::spawn(async move {
            while let Ok(event) = receiver.recv_async().await {
                match event {
                    ServiceEvent::ServiceResolved(info) => {
                        let name = info.get_fullname().to_string();
                        let peer_display = info
                            .get_property_val_str("name")
                            .unwrap_or_else(|| info.get_hostname())
                            .to_string();
                        let port = info.get_port();
                        let host = info.get_hostname().to_string();
                        let ip = info.get_addresses().iter().find_map(|addr| match addr {
                            std::net::IpAddr::V6(v6) => Some(*v6),
                            _ => None,
                        });

                        info!(
                            display = %peer_display,
                            host = %host,
                            port = port,
                            ip = ?ip,
                            "Discovered AirDrop device"
                        );

                        let mut map = peers_map.lock().await;
                        map.insert(
                            name.clone(),
                            DiscoveredAirDropPeer {
                                instance_name: name,
                                hostname: host,
                                port,
                                ipv6_addr: ip,
                                display_name: peer_display,
                            },
                        );
                    }
                    ServiceEvent::ServiceRemoved(_, fullname) => {
                        info!(name = %fullname, "AirDrop peer disappeared");
                        let mut map = peers_map.lock().await;
                        map.remove(&fullname);
                    }
                    _ => {}
                }
            }
        });

        Ok(())
    }

    pub fn get_peers(&self) -> Arc<Mutex<HashMap<String, DiscoveredAirDropPeer>>> {
        Arc::clone(&self.peers)
    }
}
