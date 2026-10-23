//! C ABI of `onair_tetra` (core/include/dect2/rs/tetra.h). Every export is `onair_tetra_*`; pointers come as (pointer, length) pairs and the caller owns all
//! buffers. Bits are one per byte. A null pointer where a buffer is needed makes the function fail (nothing is written). Text goes into a caller buffer and
//! the length written is returned; names are static NUL-terminated strings.
#![allow(clippy::missing_safety_doc)]

use crate::abi::{MacBlock, Neighbour, Nwrk, Sync, Sysinfo};
use crate::mac::{parse_block, Out};
use crate::sysinfo::{self, Parsed};

unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}
unsafe fn wr<'a, T>(p: *mut T, n: usize) -> &'a mut [T] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_tetra_version() -> u32 {
    4
}

/// Sizes of the `repr(C)` records (Sync, Sysinfo, Neighbour, Nwrk, MacBlock) so C++ can check its own layout at start-up.
#[no_mangle]
pub unsafe extern "C" fn onair_tetra_struct_sizes(out: *mut usize) {
    if !out.is_null() {
        *(out as *mut [usize; 5]) = [
            core::mem::size_of::<Sync>(),
            core::mem::size_of::<Sysinfo>(),
            core::mem::size_of::<Neighbour>(),
            core::mem::size_of::<Nwrk>(),
            core::mem::size_of::<MacBlock>(),
        ];
    }
}

fn code(p: Parsed) -> i32 {
    match p {
        Parsed::Rejected => 0,
        Parsed::Partial => 1,
        Parsed::Ok => 2,
    }
}

/// Parse a 60-bit SYNC PDU into `*out` (always written). Returns 2 when valid, 1 when too short or the frame / multiframe number is out of range.
#[no_mangle]
pub unsafe extern "C" fn onair_tetra_parse_sync(bits: *const u8, len: usize, out: *mut Sync) -> i32 {
    if out.is_null() {
        return 0;
    }
    code(sysinfo::parse_sync(rd(bits, len), &mut *out))
}

/// Parse a 124-bit SYSINFO block starting at the MAC PDU type. Returns 2 valid (written), 1 too short (written, zeros past the end), 0 not a SYSINFO (not written).
#[no_mangle]
pub unsafe extern "C" fn onair_tetra_parse_sysinfo(bits: *const u8, len: usize, out: *mut Sysinfo) -> i32 {
    if out.is_null() {
        return 0;
    }
    code(sysinfo::parse_sysinfo(rd(bits, len), &mut *out))
}

/// Comma separated BS service names of a 12-bit details word, or "none"; writes at most `cap` bytes (no NUL) and returns their count.
#[no_mangle]
pub unsafe extern "C" fn onair_tetra_service_text(v: i32, out: *mut u8, cap: usize) -> usize {
    sysinfo::service_text(v, wr(out, cap))
}

/// Security class of the extended services broadcast: 0 not present, else bit 0 valid, 1 authentication required, 2 class 1, 3 class 2, 4 class 3.
#[no_mangle]
pub unsafe extern "C" fn onair_tetra_security(si: *const Sysinfo) -> i32 {
    if si.is_null() {
        return 0;
    }
    sysinfo::security(&*si)
}

/// Downlink carrier in Hz.
#[no_mangle]
pub extern "C" fn onair_tetra_dl_carrier_hz(band: i32, carrier: i32, offset: i32) -> f64 {
    sysinfo::dl_carrier_hz(band, carrier, offset)
}
/// Duplex spacing in Hz, negative when reserved.
#[no_mangle]
pub extern "C" fn onair_tetra_duplex_spacing_hz(band: i32, duplex: i32) -> f64 {
    sysinfo::duplex_spacing_hz(band, duplex)
}
/// Uplink carrier in Hz, 0 when the duplex spacing is reserved.
#[no_mangle]
pub extern "C" fn onair_tetra_ul_carrier_hz(band: i32, carrier: i32, offset: i32, duplex: i32, reverse: i32) -> f64 {
    sysinfo::ul_carrier_hz(band, carrier, offset, duplex, reverse != 0)
}

/// Network time text ("YYYY-MM-DD HH:MM:SS UTC, local +HH:MM", at most 96 bytes, no NUL) of the 48-bit time; returns the length, 0 when not valid.
#[no_mangle]
pub unsafe extern "C" fn onair_tetra_network_time_text(t: u64, out: *mut u8, cap: usize) -> usize {
    sysinfo::network_time_text(t, wr(out, cap))
}

/// Walk one signalling block of `len` bits. `blk` receives the counters, the SYSINFO and the number of broadcasts found; `nwrk` (capacity `nwrk_cap`) and
/// `nbr` (capacity `nbr_cap`) receive the D-NWRK-BROADCASTs and their neighbours; broadcasts beyond a capacity are counted in `blk` but not stored. A block
/// of `len` bits holds at most `len / 8 + 1` broadcasts of at most 7 neighbours each.
#[no_mangle]
pub unsafe extern "C" fn onair_tetra_parse_block(
    bits: *const u8,
    len: usize,
    blk: *mut MacBlock,
    nwrk: *mut Nwrk,
    nwrk_cap: usize,
    nbr: *mut Neighbour,
    nbr_cap: usize,
) -> i32 {
    if blk.is_null() {
        return 0;
    }
    let mut o = Out { blk: MacBlock::default(), nwrk: wr(nwrk, nwrk_cap), nbr: wr(nbr, nbr_cap) };
    parse_block(rd(bits, len), &mut o);
    *blk = o.blk;
    1
}

/// ACCESS-ASSIGN downlink usage (see header).
#[no_mangle]
pub extern "C" fn onair_tetra_aach_dl_usage(aa: u32, frame18: i32) -> i32 {
    sysinfo::aach_dl_usage(aa as u16, frame18 != 0)
}
/// Name of a downlink usage value.
#[no_mangle]
pub extern "C" fn onair_tetra_dl_usage_text(u: i32) -> *const u8 {
    sysinfo::dl_usage_text(u).as_ptr()
}
