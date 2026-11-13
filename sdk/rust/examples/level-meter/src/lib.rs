// SPDX-License-Identifier: CC0-1.0
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Level meter: average channel power in dBFS, with a peak hold. The smallest useful Rust plugin.
use onair_plugin::{export_plugin, Host, IqMeta, Lamp, Plugin, CAP_IQ_SINK};

struct Meter { host: Host, peak: f32, blocks: u64 }

impl Plugin for Meter {
    fn create(host: Host, _mode_id: &str) -> Option<Self> {
        host.status("level", "waiting for samples", Lamp::Off);
        Some(Meter { host, peak: -200.0, blocks: 0 })
    }
    fn process_iq(&mut self, iq: &[f32], _meta: &IqMeta) {
        let n = iq.len() / 2;
        if n == 0 { return; }
        let power: f32 = iq.chunks_exact(2).map(|s| s[0] * s[0] + s[1] * s[1]).sum::<f32>() / n as f32;
        let db = 10.0 * (power + 1e-20).log10();
        self.peak = self.peak.max(db);
        self.blocks += 1;
        if self.blocks % 10 == 0 {
            self.host.status("level", &format!("{:.1} dBFS (peak {:.1})", db, self.peak), if db > -1.0 { Lamp::Warn } else { Lamp::Ok });
        }
    }
}

export_plugin!(Meter, "level-meter", CAP_IQ_SINK);
