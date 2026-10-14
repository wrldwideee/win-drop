//! airdrop/mod.rs — AirDrop Service Integration Layer
//!
//! Submodules:
//!   - bonjour : mDNS discovery and advertising (_airdrop._tcp.local)
//!   - server  : AirDrop HTTPS endpoint receiver (/Discover, /Ask, /Upload)
//!
//! SPDX-License-Identifier: MIT

pub mod bonjour;
pub mod server;
pub mod ble;

pub use bonjour::AirDropBonjour;
pub use server::AirDropServer;
pub use ble::AirDropBleService;
