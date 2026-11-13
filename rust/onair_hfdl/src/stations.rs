// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! The built-in ground station table (frequencies in kHz as published in recent HFDL system tables) and the frequency masks that squitters carry:
//! bit i of a mask is the i-th frequency of the station, as the system table (HFNPDU 0xD0, not reassembled) would define it.

/// One ground station; `name` ends with a NUL so C can use it as is.
pub struct Station {
    /// Ground station id (1 .. 127).
    pub id: i32,
    /// Name with a trailing NUL byte.
    pub name: &'static [u8],
    /// Position in degrees.
    pub lat: f64,
    /// Position in degrees.
    pub lon: f64,
    /// Frequencies, bit 0 first.
    pub khz: &'static [i32],
}

macro_rules! gs {
    ($id:expr, $n:expr, $la:expr, $lo:expr, [$($k:expr),*]) => {
        Station { id: $id, name: concat!($n, "\0").as_bytes(), lat: $la, lon: $lo, khz: &[$($k),*] }
    };
}

/// The table, in the order of the C++ original.
pub static STATIONS: [Station; 16] = [
    gs!(1, "San Francisco", 37.6, -122.4, [21934, 17919, 13276, 11327, 10081, 8927, 6559, 5508]),
    gs!(2, "Molokai", 21.2, -157.2, [21937, 17934, 13324, 13312, 10027, 8936, 8912, 5514]),
    gs!(3, "Reykjavik", 64.1, -21.9, [17985, 15025, 11184, 8977, 6712, 5720, 3900]),
    gs!(4, "Riverhead", 40.9, -72.6, [21931, 17919, 13276, 11387, 8912, 6661, 5652]),
    gs!(5, "Auckland", -37.0, 174.8, [17916, 13351, 10084, 8921, 6535, 5583]),
    gs!(6, "Hat Yai", 6.9, 100.4, [21949, 17928, 13270, 10066, 8825, 6535, 5655]),
    gs!(7, "Shannon", 52.7, -8.9, [11384, 10081, 8942, 6532, 5547, 3455]),
    gs!(8, "Johannesburg", -26.1, 28.2, [21949, 17922, 13321, 11321, 8834, 5529, 4681]),
    gs!(9, "Barrow", 71.3, -156.8, [21937, 17916, 11354, 10093, 8936, 6646, 5544, 4687]),
    gs!(10, "Muan", 34.9, 126.4, [21931, 17958, 13342, 11330, 8939, 6589, 5502]),
    gs!(11, "Albrook", 9.0, -79.6, [17901, 13264, 10063, 8894, 6589, 5589]),
    gs!(13, "Santa Cruz", -17.6, -63.1, [21997, 17916, 13315, 11318, 8957, 6628, 4660]),
    gs!(14, "Krasnoyarsk", 56.2, 92.5, [21990, 17912, 13321, 10087, 8886, 6596, 5559]),
    gs!(15, "Al Muharraq", 26.3, 50.6, [21982, 17967, 13354, 11348, 8885, 5544]),
    gs!(16, "Agana", 13.5, 144.8, [21928, 17919, 13312, 11306, 8927, 6652]),
    gs!(17, "Canarias", 27.9, -15.4, [21955, 17928, 13303, 11348, 8948, 6529, 5589]),
];

/// The station with this id.
pub fn station(id: i32) -> Option<&'static Station> {
    STATIONS.iter().find(|s| s.id == id)
}

/// The name without the NUL.
pub fn name_of(s: &Station) -> &'static [u8] {
    &s.name[..s.name.len() - 1]
}

/// The frequencies a 20-bit mask selects at station `gs_id` (up to 20 entries into `out`); a bit beyond the table (or an unknown station) gives `-1 - bit`.
/// Returns the count.
pub fn mask_khz(gs_id: i32, mask: u32, out: &mut [i32; 20]) -> usize {
    let s = station(gs_id);
    let mut n = 0;
    for b in 0..20usize {
        if (mask >> b) & 1 != 0 {
            out[n] = match s {
                Some(s) if b < s.khz.len() => s.khz[b],
                _ => -(b as i32) - 1,
            };
            n += 1;
        }
    }
    n
}

/// The first station that lists this frequency (0 none).
pub fn station_on_khz(khz: i32) -> i32 {
    for s in STATIONS.iter() {
        if s.khz.contains(&khz) {
            return s.id;
        }
    }
    0
}
