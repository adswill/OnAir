// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! C ABI of `onair_mesh` (core/include/dect2/rs/mesh.h). Every export is `onair_mesh_*`; pointers come as (pointer, length) pairs and the caller owns all
//! buffers. Text writers return the length written, or `usize::MAX` when the buffer is too small. A null pointer with a length is read as an empty input.
//! Decoders fill the flat records of `abi` / the protocol modules (payloads up to 255 bytes, as LoRa frames are; the arrays are sized for that).
//!
//! This is also the only module with `unsafe`: besides the C ABI it holds `cfn`, the C library `snprintf("%.Nf")` the text of numbers is made with, so the
//! digits match the C++ reference bit for bit.
#![allow(clippy::missing_safety_doc)]

use crate::abi::{Message, Node, S128, S2048, S64};
use crate::aprs::{self, LoraAprs, MeshCom};
use crate::lorawan::{self, Lorawan};
use crate::lorawan_names;
use crate::meshcore::{self, GroupOut, Out as McOut, Parsed};
use crate::meshtastic::{self, Ctx, Header};
use crate::{names, pb, tables};
use onair_common::text::Buf;

const FAIL: usize = usize::MAX;

/// C library `snprintf("%.Nf")` for the digits of numbers in text.
pub(crate) mod cfn {
    use core::ffi::{c_char, c_int};
    mod c {
        use core::ffi::{c_char, c_int};
        extern "C" {
            pub fn snprintf(s: *mut c_char, n: usize, fmt: *const c_char, ...) -> c_int;
        }
    }
    /// Writes `v` as C `%.{prec}f` (prec 0 to 5) into `out` (64 bytes); returns the length (at most 63).
    pub fn format_f(prec: u8, v: f64, out: &mut [u8; 64]) -> usize {
        const FMT: [&[u8]; 6] = [b"%.0f\0", b"%.1f\0", b"%.2f\0", b"%.3f\0", b"%.4f\0", b"%.5f\0"];
        let f = FMT[usize::from(prec.min(5))];
        // SAFETY: `out` is 64 writable bytes, `f` is NUL terminated with one double conversion, one double is passed.
        let n: c_int = unsafe { c::snprintf(out.as_mut_ptr() as *mut c_char, out.len(), f.as_ptr() as *const c_char, v) };
        if n < 0 {
            0
        } else {
            (n as usize).min(out.len() - 1)
        }
    }
}

unsafe fn rd<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}
unsafe fn wr<'a>(p: *mut u8, n: usize) -> &'a mut [u8] {
    if p.is_null() || n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(p, n)
    }
}
/// Writes text built by `f` into the caller buffer; the length, or `FAIL` when it does not fit.
unsafe fn put(out: *mut u8, cap: usize, f: impl FnOnce(&mut Buf)) -> usize {
    let mut w = Buf::new(wr(out, cap));
    f(&mut w);
    if w.overflow {
        FAIL
    } else {
        w.len()
    }
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_mesh_version() -> u32 {
    2
}

/// `sizeof` of the records in the order Node, Message, MtDecoded, Parsed, McOut, GroupOut, Lorawan, LoraAprs, MeshCom, PbField (7 more may follow): the
/// C++ side checks them at start-up. `out` must have room for 10 values.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_struct_sizes(out: *mut usize) {
    if out.is_null() {
        return;
    }
    let s = [
        core::mem::size_of::<Node>(),
        core::mem::size_of::<Message>(),
        core::mem::size_of::<MtDecoded>(),
        core::mem::size_of::<Parsed>(),
        core::mem::size_of::<McOut>(),
        core::mem::size_of::<GroupOut>(),
        core::mem::size_of::<Lorawan>(),
        core::mem::size_of::<LoraAprs>(),
        core::mem::size_of::<MeshCom>(),
        core::mem::size_of::<PbField>(),
    ];
    for (i, v) in s.iter().enumerate() {
        *out.add(i) = *v;
    }
}

// ---- protobuf reader ----

/// One protobuf field as the C side sees it.
#[repr(C)]
pub struct PbField {
    pub num: u32,
    pub wire: i32,
    pub value: u64,
    /// Bytes of a length-delimited field (null for the others) and their count.
    pub data: *const u8,
    pub len: usize,
}

/// Reads the next field of the message `buf[0..n]` starting at `*pos` (advanced); `*ok` goes false on a malformed field. Returns true when a field was read.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_pb_next(buf: *const u8, n: usize, pos: *mut usize, ok: *mut bool, f: *mut PbField) -> bool {
    if pos.is_null() || ok.is_null() || f.is_null() {
        return false;
    }
    let mut r = pb::Reader::at(rd(buf, n), *pos, *ok);
    let got = r.next();
    *pos = r.pos();
    *ok = r.ok();
    match got {
        Some(x) => {
            *f = PbField {
                num: x.num,
                wire: x.wire,
                value: x.value,
                data: if x.wire == 2 { x.data.as_ptr() } else { core::ptr::null() },
                len: x.data.len(),
            };
            true
        }
        None => false,
    }
}

/// The values of a repeated fixed32 field (wire 5: `value`; wire 2: `data[0..len]` packed) into `out`; returns how many there are (more than `cap` are counted, not stored).
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_pb_repeated_fixed32(
    wire: i32,
    value: u64,
    data: *const u8,
    len: usize,
    out: *mut u32,
    cap: usize,
) -> usize {
    let f = pb::Field { num: 0, wire, value, data: rd(data, len) };
    let o = if out.is_null() { &mut [][..] } else { core::slice::from_raw_parts_mut(out, cap) };
    let mut n = 0usize;
    pb::each_fixed32(&f, |v| {
        if n < o.len() {
            o[n] = v;
        }
        n += 1;
    });
    n
}

/// Like `onair_mesh_pb_repeated_fixed32` for a repeated varint field (wire 0: `value`; wire 2: packed varints).
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_pb_repeated_varint(
    wire: i32,
    value: u64,
    data: *const u8,
    len: usize,
    out: *mut u64,
    cap: usize,
) -> usize {
    let f = pb::Field { num: 0, wire, value, data: rd(data, len) };
    let o = if out.is_null() { &mut [][..] } else { core::slice::from_raw_parts_mut(out, cap) };
    let mut n = 0usize;
    pb::each_varint(&f, |v| {
        if n < o.len() {
            o[n] = v;
        }
        n += 1;
    });
    n
}

// ---- Meshtastic helpers ----

/// `!%08x` of a node number.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_node_id(num: u32, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| meshtastic::node_id(w, num))
}

/// Hardware model name or `HW_<n>`.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_hw_model_name(hw: i32, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| meshtastic::hw_model_name(w, hw))
}

/// Hardware model value of a name, -1 when unknown.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_hw_model_value(name: *const u8, n: usize) -> i32 {
    tables::hw_model_value(rd(name, n))
}

/// Channel hash of a name and key.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_channel_hash(name: *const u8, n: usize, key: *const u8, klen: usize) -> u8 {
    tables::channel_hash(rd(name, n), rd(key, klen))
}

/// Expands a pre-shared key into `out32`; returns its length (0: no encryption) or -1 for a key longer than 32 bytes.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_expand_psk(psk: *const u8, n: usize, out32: *mut u8) -> i32 {
    if out32.is_null() {
        return -1;
    }
    let mut k = [0u8; 32];
    let r = tables::expand_psk(rd(psk, n), &mut k);
    core::ptr::copy_nonoverlapping(k.as_ptr(), out32, 32);
    match r {
        Some(l) => l as i32,
        None => -1,
    }
}

/// The name of a port number or `PORT_<n>` (used by the C++ tests of the tables); same for the role and routing error tables through `table`
/// (0 hardware models, 1 port numbers, 2 roles, 3 routing errors): returns the length, 0 when the value is not in the table.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_enum_name(table: i32, value: i32, out: *mut u8, cap: usize) -> usize {
    let t: &[names::Entry] = match table {
        0 => &names::HW_MODELS,
        1 => &names::PORT_NUMS,
        2 => &names::ROLES,
        _ => &names::ROUTING_ERRORS,
    };
    match names::name(t, value) {
        Some(n) => put(out, cap, |w| w.str(n)),
        None => 0,
    }
}

// ---- Meshtastic packets ----

/// Reads the 16 byte header; false when `n < 16`.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_mt_header(p: *const u8, n: usize, out: *mut Header) -> bool {
    match meshtastic::parse_header(rd(p, n)) {
        Some(h) if !out.is_null() => {
            *out = h;
            true
        }
        _ => false,
    }
}

/// What decoding the plain Data of a Meshtastic packet gives.
#[repr(C)]
pub struct MtDecoded {
    pub portnum: i32,
    pub has_node: u8,
    pub has_message: u8,
    /// Port name (`PORT_<n>` when unnamed) and the detail line.
    pub type_name: S64,
    pub detail: S2048,
    pub node: Node,
    pub message: Message,
}

/// Parses decrypted bytes as a Data protobuf and decodes its port payload; false when they are not a plausible Data (wrong key). The caller supplies the
/// sender, destination, hop fields and packet id of the header.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_mt_decode(
    plain: *const u8,
    n: usize,
    from: u32,
    to: u32,
    hop_start: i32,
    hop_limit: i32,
    packet_id: u32,
    out: *mut MtDecoded,
) -> bool {
    if out.is_null() {
        return false;
    }
    let data = match meshtastic::parse_data(rd(plain, n)) {
        Some(d) => d,
        None => return false,
    };
    let c = Ctx { from, to, hop_start, hop_limit, packet_id };
    let d = meshtastic::decode_port(&data, &c);
    let o = &mut *out;
    o.portnum = data.portnum;
    o.type_name.write(|w| meshtastic::port_name(w, data.portnum));
    o.detail = d.detail;
    o.has_node = d.node.is_some() as u8;
    o.has_message = d.message.is_some() as u8;
    if let Some(x) = d.node {
        o.node = x;
    }
    if let Some(x) = d.message {
        o.message = x;
    }
    true
}

// ---- MeshCore packets ----

/// Checks and splits a MeshCore packet. Returns true and fills `*h`, or false with the note in `*note`.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_mc_header(p: *const u8, n: usize, h: *mut Parsed, note: *mut S128) -> bool {
    if h.is_null() || note.is_null() {
        return false;
    }
    match meshcore::parse_header(rd(p, n)) {
        Ok(x) => {
            *h = x;
            true
        }
        Err(e) => {
            (*note).set(e);
            false
        }
    }
}

/// Decodes what needs no key of a packet already split by `onair_mesh_mc_header`; `sig_ok` is the Ed25519 verdict for adverts of 101 bytes or more.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_mc_decode(p: *const u8, n: usize, h: *const Parsed, sig_ok: u32, out: *mut McOut) {
    let sig_ok = sig_ok != 0;
    if h.is_null() || out.is_null() {
        return;
    }
    let h = &*h;
    let all = rd(p, n);
    let (po, pa, m) = (h.path_off as usize, h.pay_off as usize, h.m as usize);
    if po + h.hcnt as usize * h.hsz as usize > all.len() || pa + m > all.len() {
        *out = McOut::default();
        return;
    }
    *out = meshcore::decode(h, &all[po..po + h.hcnt as usize * h.hsz as usize], &all[pa..pa + m], sig_ok);
}

/// Interprets decrypted group bytes (`kind` 5 text, 6 data); `via` is the hop text from the header.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_mc_group_plain(
    kind: u32,
    hcnt: u32,
    via: *const u8,
    vlen: usize,
    d: *const u8,
    dlen: usize,
    out: *mut GroupOut,
) {
    let kind = kind as u8;
    let hcnt = hcnt as u8;
    if !out.is_null() {
        *out = meshcore::group_plain(kind, hcnt, rd(via, vlen), rd(d, dlen));
    }
}

/// The note when no channel decrypted a group packet.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_mc_group_fail(hash: u32, any: u32, out: *mut S128) {
    let hash = hash as u8;
    let any = any != 0;
    if !out.is_null() {
        *out = meshcore::group_fail_note(hash, any);
    }
}

// ---- LoRaWAN ----

/// Decodes a LoRaWAN PHY payload into `*out`.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_lorawan_decode(p: *const u8, n: usize, out: *mut Lorawan) {
    if !out.is_null() {
        *out = lorawan::decode(rd(p, n));
    }
}

/// MType name ("Join request", ...; "?" outside 0..7).
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_lorawan_mtype_name(t: i32, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| w.str(lorawan_names::mtype_name(t)))
}

/// MAC command name for a command id and direction.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_lorawan_mac_name(cid: u32, uplink: u32, out: *mut u8, cap: usize) -> usize {
    let cid = cid as u8;
    let uplink = uplink != 0;
    put(out, cap, |w| lorawan_names::mac_name(w, cid, uplink))
}

/// Network name of a DevAddr; `kind`, `nwk` and `net` (each may be null) receive the DevAddr type, NwkID and NetID (left untouched for the reserved type).
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_lorawan_network(
    dev_addr: u32,
    out: *mut u8,
    cap: usize,
    kind: *mut i32,
    nwk: *mut u32,
    net: *mut u32,
) -> usize {
    let mut info = lorawan_names::Network::default();
    let r = put(out, cap, |w| info = lorawan_names::network(w, dev_addr));
    if !kind.is_null() {
        *kind = info.kind;
    }
    if info.have_ids {
        if !nwk.is_null() {
            *nwk = info.nwk_id;
        }
        if !net.is_null() {
            *net = info.net_id;
        }
    }
    r
}

/// Device maker of an EUI-64 (empty text when not in the short table).
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_lorawan_vendor(eui: u64, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| w.str(lorawan_names::vendor(eui)))
}

/// `%016llX` of an EUI.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_lorawan_eui(eui: u64, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| lorawan_names::eui_text(w, eui))
}

/// Bytes as `40 2B 1A`.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_hex_string(p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| lorawan_names::hex_string(w, rd(p, n)))
}

// ---- LoRa APRS and MeshCom ----

/// Printable text of bytes (`\xNN` for the others).
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_printable(p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| aprs::printable(w, rd(p, n)))
}

/// Text with invalid UTF-8 and control characters replaced by '?'.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_clean_text(p: *const u8, n: usize, out: *mut u8, cap: usize) -> usize {
    put(out, cap, |w| aprs::clean_text(w, rd(p, n)))
}

/// Splits a LoRa APRS frame.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_lora_aprs(p: *const u8, n: usize, out: *mut LoraAprs) {
    if !out.is_null() {
        *out = aprs::lora_aprs(rd(p, n));
    }
}

/// Decodes a MeshCom frame.
#[no_mangle]
pub unsafe extern "C" fn onair_mesh_meshcom(p: *const u8, n: usize, out: *mut MeshCom) {
    if !out.is_null() {
        *out = aprs::meshcom(rd(p, n));
    }
}
