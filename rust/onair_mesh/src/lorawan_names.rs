// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! LoRaWAN name tables and lookups: MAC command names and payload lengths (TS001 1.0.4 / 1.1 chapter 5), MType names (4.2.1), DevAddr types and the
//! network of a DevAddr (TS002 6.1 / the LoRa Alliance NetID list), device makers by IEEE OUI, EUI and hex text helpers.
use onair_common::text::Buf;
/// A MAC command: id, name and payload length in each direction (-1: not sent that way).
pub struct MacDef {
    pub cid: u8,
    pub up: &'static str,
    pub up_len: i8,
    pub down: &'static str,
    pub down_len: i8,
}

const fn mac(cid: u8, up: &'static str, up_len: i8, down: &'static str, down_len: i8) -> MacDef {
    MacDef { cid, up, up_len, down, down_len }
}

/// MAC commands of TS001 1.0.4 / 1.1 chapter 5 (uplink name and length, downlink name and length).
pub const MACS: [MacDef; 20] = [
    mac(0x01, "ResetInd", 1, "ResetConf", 1),
    mac(0x02, "LinkCheckReq", 0, "LinkCheckAns", 2),
    mac(0x03, "LinkADRAns", 1, "LinkADRReq", 4),
    mac(0x04, "DutyCycleAns", 0, "DutyCycleReq", 1),
    mac(0x05, "RXParamSetupAns", 1, "RXParamSetupReq", 4),
    mac(0x06, "DevStatusAns", 2, "DevStatusReq", 0),
    mac(0x07, "NewChannelAns", 1, "NewChannelReq", 5),
    mac(0x08, "RXTimingSetupAns", 0, "RXTimingSetupReq", 1),
    mac(0x09, "TxParamSetupAns", 0, "TxParamSetupReq", 1),
    mac(0x0A, "DlChannelAns", 1, "DlChannelReq", 4),
    mac(0x0B, "RekeyInd", 1, "RekeyConf", 1),
    mac(0x0C, "ADRParamSetupAns", 0, "ADRParamSetupReq", 1),
    mac(0x0D, "DeviceTimeReq", 0, "DeviceTimeAns", 5),
    mac(0x0E, "", -1, "ForceRejoinReq", 2),
    mac(0x0F, "RejoinParamSetupAns", 1, "RejoinParamSetupReq", 1),
    mac(0x10, "PingSlotInfoReq", 1, "PingSlotInfoAns", 0),
    mac(0x11, "PingSlotChannelAns", 1, "PingSlotChannelReq", 4),
    mac(0x12, "BeaconTimingReq", 0, "BeaconTimingAns", 3),
    mac(0x13, "BeaconFreqAns", 1, "BeaconFreqReq", 3),
    mac(0x20, "DeviceModeInd", 1, "DeviceModeConf", 1),
];

pub fn mac_def(cid: u8) -> Option<&'static MacDef> {
    MACS.iter().find(|m| m.cid == cid)
}

/// NetIDs of a few big networks (LoRa Alliance NetID list): DevAddr type, NwkID, name.
const NETS: [(i32, u32, &str); 17] = [
    (0, 0x00, "Experimental / private"),
    (0, 0x01, "Experimental / private"),
    (0, 0x02, "Actility"),
    (0, 0x03, "Proximus"),
    (0, 0x04, "Swisscom"),
    (0, 0x07, "Bouygues Telecom"),
    (0, 0x08, "Orbiwise"),
    (0, 0x09, "Senet"),
    (0, 0x0A, "KPN"),
    (0, 0x0B, "Everynet"),
    (0, 0x0F, "SK Telecom"),
    (0, 0x10, "Sagemcom"),
    (0, 0x11, "Orange"),
    (0, 0x12, "A2A Smart City"),
    (0, 0x13, "The Things Network"),
    (0, 0x24, "Helium"),
    (6, 0x53, "Helium"), // NetID 0xC00053, DevAddr 0xFC01 4C00 - 0xFC01 4FFF
];
/// NwkID length in bits by DevAddr type 0..7 (TS002 table 1).
const NWK_BITS: [u32; 8] = [6, 6, 9, 11, 12, 13, 15, 17];

/// IEEE OUIs of common LoRaWAN device makers (the first three bytes of the DevEUI).
const OUIS: [(u32, &str); 13] = [
    (0xA84041, "Dragino"),
    (0x0004A3, "Microchip"),
    (0x24E124, "Milesight"),
    (0xAC1F09, "RAKwireless"),
    (0x0018B2, "Adeunis"),
    (0x00137A, "Netvox"),
    (0x647FDA, "Tektelic"),
    (0x58A0CB, "The Things Industries"),
    (0x2CF7F1, "Seeed (SenseCAP)"),
    (0x0016C0, "Semtech"),
    (0x0080E1, "STMicroelectronics"),
    (0xA81758, "Elsys"),
    (0x70B3D5, "IEEE small-vendor block"),
];

/// Message type name of the MHDR (TS001 4.2.1); "?" outside 0..7.
pub fn mtype_name(t: i32) -> &'static str {
    const N: [&str; 8] = [
        "Join request",
        "Join accept",
        "Unconfirmed up",
        "Unconfirmed down",
        "Confirmed up",
        "Confirmed down",
        "Rejoin request",
        "Proprietary",
    ];
    if (0..8).contains(&t) {
        N[t as usize]
    } else {
        "?"
    }
}

/// Name of a MAC command (`CID 0x42` when unknown, `proprietary 0x80` from 0x80).
pub fn mac_name(w: &mut Buf, cid: u8, uplink: bool) {
    if cid >= 0x80 {
        w.str("proprietary 0x");
        w.hex(cid as u64, 2, true);
        return;
    }
    let nm = mac_def(cid).map(|d| if uplink { d.up } else { d.down }).filter(|n| !n.is_empty());
    match nm {
        Some(n) => w.str(n),
        None => {
            w.str("CID 0x");
            w.hex(cid as u64, 2, true);
        }
    }
}

/// What the DevAddr says: type 0..7 (the run of leading ones, -1 when all eight are ones), the NwkID and the NetID of types 0 to 2.
#[derive(Clone, Copy, Default)]
pub struct Network {
    pub kind: i32,
    pub nwk_id: u32,
    pub net_id: u32,
    /// True when `nwk_id` / `net_id` were computed (the reference leaves them untouched for the reserved type).
    pub have_ids: bool,
}

/// The network of a DevAddr as text ("The Things Network", "NetID 0x000005", ...) and its numbers.
pub fn network(w: &mut Buf, dev_addr: u32) -> Network {
    let mut kind = 0usize;
    while kind < 8 && (dev_addr >> (31 - kind)) & 1 != 0 {
        kind += 1;
    }
    let mut n = Network { kind: if kind < 8 { kind as i32 } else { -1 }, ..Network::default() };
    if kind >= 8 {
        w.str("reserved DevAddr");
        return n;
    }
    let bits = NWK_BITS[kind];
    let nwk = (dev_addr >> (32 - (kind as u32 + 1) - bits)) & ((1u32 << bits) - 1);
    let net = if kind <= 2 { (kind as u32) << 21 | nwk } else { 0 };
    n.nwk_id = nwk;
    n.net_id = net;
    n.have_ids = true;
    if let Some(e) = NETS.iter().find(|e| e.0 == kind as i32 && e.1 == nwk) {
        w.str(e.2);
    } else if kind <= 2 {
        w.str("NetID 0x");
        w.hex(net as u64, 6, true);
    } else {
        w.str("NetID type ");
        w.idec(kind as i64);
        w.str(", NwkID 0x");
        w.hex(nwk as u64, 1, true);
    }
    n
}

/// The device maker of an EUI-64 from the short OUI table; empty when not in it.
pub fn vendor(eui: u64) -> &'static str {
    if (eui >> 28) == 0x0007_0B3D_57ED {
        return "The Things Network block";
    }
    let oui = (eui >> 40) as u32;
    OUIS.iter().find(|o| o.0 == oui).map(|o| o.1).unwrap_or("")
}

/// `%016llX` of an EUI-64 (most significant byte first).
pub fn eui_text(w: &mut Buf, eui: u64) {
    w.hex(eui, 16, true);
}

/// Bytes as `40 2B 1A` (upper case hex pairs separated by blanks).
pub fn hex_string(w: &mut Buf, p: &[u8]) {
    for (i, &b) in p.iter().enumerate() {
        if i != 0 {
            w.byte(b' ');
        }
        w.hex(b as u64, 2, true);
    }
}
