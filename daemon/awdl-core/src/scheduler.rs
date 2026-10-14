//! scheduler.rs — AWDL 102.4 ms Channel Hopping & Availability Window Scheduler
//!
//! AWDL partitions time into 102.4 ms cycles (approx 100 TU):
//!   - [0 .. 16 ms]: Availability Window (AW) on Social Channel (CH 44 or 149)
//!     * Nodes transmit and listen for Periodic Synchronization Frames (PSF)
//!     * Master election status exchanges
//!   - [16 .. 102.4 ms]: Extension Window (EW) / Data Window
//!     * Nodes hop to negotiated unicast data channels for bulk AirDrop transfer,
//!       or remain on the social channel when idling.
//!
//! SPDX-License-Identifier: MIT

use std::sync::Arc;
use std::time::Duration;
use tokio::sync::Mutex;
use tokio::time::{interval, MissedTickBehavior};
use tracing::{debug, error, trace};

use crate::driver::RadioDevice;
use crate::state::AwdlStateMachine;

/// Standard AWDL cycle durations
pub const AWDL_CYCLE_DURATION_MS: u64 = 102; // 102.4 ms rounded
pub const AWDL_AW_DURATION_MS: u64    = 16;  // 16 ms availability window
pub const AWDL_DEFAULT_SOCIAL_CH: u8  = 44;  // 5 GHz social channel 44

pub struct ChannelScheduler {
    radio:        Arc<RadioDevice>,
    state:        Arc<Mutex<AwdlStateMachine>>,
    social_ch:    u8,
    data_ch:      Option<u8>,
}

impl ChannelScheduler {
    pub fn new(radio: Arc<RadioDevice>, state: Arc<Mutex<AwdlStateMachine>>) -> Self {
        Self {
            radio,
            state,
            social_ch: AWDL_DEFAULT_SOCIAL_CH,
            data_ch: None,
        }
    }

    /// Set an active data channel for bulk transmission during extension windows
    pub fn set_data_channel(&mut self, channel: Option<u8>) {
        self.data_ch = channel;
    }

    /// High-precision scheduling loop
    pub async fn run(&mut self) {
        let mut cycle_timer = interval(Duration::from_millis(AWDL_CYCLE_DURATION_MS));
        cycle_timer.set_missed_tick_behavior(MissedTickBehavior::Skip);

        debug!(
            social_ch = self.social_ch,
            cycle_ms = AWDL_CYCLE_DURATION_MS,
            "AWDL Channel Scheduler started"
        );

        loop {
            // Wait for start of next 102.4 ms cycle
            cycle_timer.tick().await;

            // --- Phase 1: Availability Window (AW) on Social Channel ---
            if let Err(e) = self.radio.set_channel(self.social_ch, 1, 0) {
                error!("Scheduler: failed to switch to social channel: {:?}", e);
            }

            // Emit Periodic Synchronization Frame (PSF)
            {
                let mut st = self.state.lock().await;
                st.emit_psf(self.social_ch).await;
            }

            // Sleep through the Availability Window (16 ms)
            tokio::time::sleep(Duration::from_millis(AWDL_AW_DURATION_MS)).await;

            // --- Phase 2: Data / Extension Window (EW) ---
            if let Some(target_data_ch) = self.data_ch {
                if target_data_ch != self.social_ch {
                    trace!("Scheduler: hopping to data channel {}", target_data_ch);
                    if let Err(e) = self.radio.set_channel(target_data_ch, 1, 0) {
                        error!("Scheduler: failed to hop to data channel {}: {:?}", target_data_ch, e);
                    }
                }
            }
        }
    }
}
