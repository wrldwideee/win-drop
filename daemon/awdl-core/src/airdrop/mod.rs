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
pub mod cpio;
pub mod client;

pub use bonjour::AirDropBonjour;
pub use server::AirDropServer;
pub use ble::AirDropBleService;
#[allow(unused_imports)]
pub use cpio::{build_cpio_gz, extract_cpio_gz_or_raw};
pub use client::AirDropClient;
