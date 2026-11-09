//! MMSI plausibility (ITU-R M.585) and the class of the sender.

pub const CLASS_A: i32 = 0;
pub const CLASS_B: i32 = 1;
pub const CLASS_BASE: i32 = 2;
pub const CLASS_ATON: i32 = 3;
pub const CLASS_SAR: i32 = 4;
pub const CLASS_OTHER: i32 = 5;

pub fn class_of_mmsi(mmsi: u32, typ: i32) -> i32 {
    match typ {
        1 | 2 | 3 | 5 | 27 => return CLASS_A,
        18 | 19 | 24 => return CLASS_B,
        4 => return CLASS_BASE,
        21 => return CLASS_ATON,
        9 => return CLASS_SAR,
        _ => {}
    }
    if mmsi / 1_000_000 == 111 {
        return CLASS_SAR;
    }
    if mmsi / 10_000_000 == 99 {
        return CLASS_ATON;
    }
    if mmsi / 10_000_000 == 0 {
        return CLASS_BASE;
    }
    CLASS_OTHER
}

fn mid_ok(mid: u32) -> bool {
    let hi = mid / 100;
    let lo = mid % 100;
    (2..=7).contains(&hi) && (1..=79).contains(&lo)
}

pub fn mmsi_plausible(m: u32) -> bool {
    if m == 0 || m >= 1_000_000_000 {
        return false;
    }
    if (200_000_000..800_000_000).contains(&m) {
        return mid_ok(m / 1_000_000);
    }
    if (111_000_000..112_000_000).contains(&m) {
        return mid_ok((m / 1000) % 1000);
    }
    if (970_000_000..975_000_000).contains(&m) {
        return true;
    }
    if m >= 980_000_000 {
        return mid_ok((m / 10_000) % 1000);
    }
    if m < 100_000_000 {
        return mid_ok(m / 10_000) || mid_ok(m / 100_000);
    }
    false
}

pub fn acceptable_ais(typ: i32, valid: bool, mmsi: u32, decoded: bool) -> bool {
    if !(1..=27).contains(&typ) {
        return false;
    }
    if matches!(typ, 1..=27) && (!decoded || !valid) {
        return false;
    }
    mmsi_plausible(mmsi)
}
