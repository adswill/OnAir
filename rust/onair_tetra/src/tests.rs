use crate::abi::{MacBlock, Neighbour, Nwrk, Sync, Sysinfo};
use crate::mac::{parse_block, Out};
use crate::sysinfo::*;

struct W(Vec<u8>);
impl W {
    fn put(&mut self, v: u64, n: usize) {
        for i in (0..n).rev() {
            self.0.push(((v >> i) & 1) as u8);
        }
    }
}

fn sync_bits() -> Vec<u8> {
    let mut w = W(vec![]);
    // system code 1, colour 33, tn 3 (field 2), fn 7, mn 12, sharing 1, reserved 2, dtx 1, ext 0, spare 0, mcc 262, mnc 1001, nbr 3, load 2, late 1
    for (v, n) in
        [(1, 4), (33, 6), (2, 2), (7, 5), (12, 6), (1, 2), (2, 3), (1, 1), (0, 1), (0, 1), (262, 10), (1001, 14), (3, 2), (2, 2), (1, 1)]
    {
        w.put(v, n);
    }
    w.0
}

#[test]
fn sync_known() {
    let mut s = Sync::default();
    assert_eq!(parse_sync(&sync_bits(), &mut s), Parsed::Ok);
    assert_eq!((s.colour_code, s.tn, s.fn_, s.mn, s.mcc, s.mnc, s.late_entry), (33, 3, 7, 12, 262, 1001, 1));
    assert_eq!(parse_sync(&sync_bits()[..30], &mut s), Parsed::Partial);
}

fn sysinfo_bits() -> Vec<u8> {
    let mut w = W(vec![]);
    for (v, n) in [
        (2, 2),
        (0, 2),
        (1000, 12),
        (4, 4),
        (1, 2),
        (1, 3),
        (0, 1),
        (0, 2),
        (3, 3),
        (5, 4),
        (6, 4),
        (7, 4),
        (0, 1),
        (0x1234, 16),
        (3, 2),
        (0x8F000, 20),
        (77, 14),
        (0xFFFF, 16),
        (0x0A0, 12),
    ] {
        w.put(v, n);
    }
    w.0
}

#[test]
fn security_needs_the_encryption_service_bit() {
    // security bits 0110 0000 of a real clear cell, no air interface encryption service bit: class 1 only, never class 2 / 3
    let mut s = Sysinfo::default();
    s.opt_flag = 3;
    s.opt_value = 0x60 << 12;
    s.bs_service = 0xd65;
    assert_eq!(security(&s), 1 | 1 << 2);
    s.bs_service = 0xd67;
    s.opt_value = 0xe0 << 12;
    assert_eq!(security(&s), 1 | 1 << 1 | 1 << 2 | 1 << 4);
    s.opt_value = 0x80 << 12;
    assert_eq!(security(&s), 1 | 1 << 1 | 1 << 3);
    s.bs_service = 0xd65;
    s.opt_value = 0x1f << 12;
    assert_eq!(security(&s), 1);
}

#[test]
fn sysinfo_known() {
    let b = sysinfo_bits();
    assert_eq!(b.len(), 124);
    let mut s = Sysinfo::default();
    assert_eq!(parse_sysinfo(&b, &mut s), Parsed::Ok);
    assert_eq!((s.main_carrier, s.band, s.la, s.bs_service), (1000, 4, 77, 0x0A0));
    assert_eq!(security(&s), 1 | 1 << 1);
    assert_eq!(dl_carrier_hz(4, 1000, 1), 4.0 * 100e6 + 25e6 + 6250.0);
    let mut o = [0u8; 256];
    let n = service_text(0x0A0, &mut o);
    assert_eq!(&o[..n], b"migration, voice".as_slice());
    let mut x = Sysinfo::default();
    assert_eq!(parse_sysinfo(&[0, 0, 0, 0], &mut x), Parsed::Rejected);
}

#[test]
fn time_known() {
    // 2025-01-01 00:00:10 UTC, local +01:00 (offs 4 x 15 min)
    let utc2 = 5u64;
    let t = utc2 << 24 | 4 << 17 | 25 << 11;
    let mut o = [0u8; 96];
    let n = network_time_text(t, &mut o);
    assert_eq!(&o[..n], b"2025-01-01 00:00:10 UTC, local +01:00");
    assert_eq!(network_time_text(0xFF_FFFF << 24, &mut o), 0);
}

#[test]
fn usage() {
    assert_eq!(aach_dl_usage(0, false), 2);
    assert_eq!(aach_dl_usage(1 << 12 | 5 << 6, false), 5);
    assert_eq!(aach_dl_usage(0, true), -1);
}

#[test]
fn block_sysinfo_and_garbage() {
    let mut b = sysinfo_bits();
    b.resize(124, 0);
    let mut nw = [Nwrk::default(); 4];
    let mut nb = [Neighbour::default(); 8];
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&b, &mut o);
    assert_eq!(o.blk.sysinfo, 1);
    // any bytes must not panic
    let mut x = 12345u32;
    for len in 0..300 {
        let v: Vec<u8> = (0..len)
            .map(|_| {
                x = x.wrapping_mul(1664525).wrapping_add(1013904223);
                ((x >> 20) & 1) as u8
            })
            .collect();
        let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
        parse_block(&v, &mut o);
    }
}

/// A MAC-RESOURCE (SSI address, channel allocation, fill bits) carrying an LLC BL-UDATA, MLE discriminator 2 (CMCE) and a D-SETUP.
fn setup_block(enc_flag: u64, slots: u64, carrier: u64) -> Vec<u8> {
    let mut w = W(vec![]);
    for (v, n) in [(0, 2), (1, 1), (0, 1), (0, 2), (0, 1), (14, 6), (1, 3), (0x123456, 24), (0, 1), (0, 1), (1, 1)] {
        w.put(v, n);
    }
    // channel allocation: type 0, timeslots, up/down 3, CLCH 0, cell change 0, carrier, no extended numbering, monitoring pattern 1
    for (v, n) in [(0, 2), (slots, 4), (3, 2), (0, 1), (0, 1), (carrier, 12), (0, 1), (1, 2)] {
        w.put(v, n);
    }
    // LLC BL-UDATA (0010), MLE protocol discriminator 2, D-SETUP: type 00111, call id 777, time-out 5, hook 0, simplex 1, basic service information
    for (v, n) in [(2, 4), (2, 3), (7, 5), (777, 14), (5, 4), (0, 1), (1, 1), (0, 3), (enc_flag, 1), (0, 2), (0, 2)] {
        w.put(v, n);
    }
    // fill bits: a 1 then 0s up to 14 octets
    w.put(1, 1);
    while w.0.len() < 112 {
        w.put(0, 1);
    }
    w.0
}

#[test]
fn call_setup_encryption_flag() {
    for (flag, want) in [(0u64, 0), (1, 1)] {
        let b = setup_block(flag, 0b0100, 1234);
        let mut nw = [Nwrk::default(); 4];
        let mut nb = [Neighbour::default(); 8];
        let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
        parse_block(&b, &mut o);
        assert_eq!(o.blk.resources, 1);
        assert_eq!(o.blk.n_call, 1);
        let c = o.blk.call[0];
        assert_eq!((c.kind, c.call_id, c.enc, c.slots, c.carrier), (1, 777, want, 0b0100, 1234));
    }
    // D-TX-GRANTED, D-TX-CONTINUE, D-TX-CEASED (types 11, 10, 9): the call id, kind 4 (the call goes on)
    for ty in [11u64, 10, 9] {
        let mut b = setup_block(0, 0b0100, 1234);
        // the CMCE PDU type sits after the MAC-RESOURCE header (43 bits), the channel allocation (25), LLC (4) and MLE discriminator (3)
        for k in 0..5 {
            b[75 + k] = ((ty >> (4 - k)) & 1) as u8;
        }
        let mut nw = [Nwrk::default(); 4];
        let mut nb = [Neighbour::default(); 8];
        let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
        parse_block(&b, &mut o);
        assert_eq!(o.blk.n_call, 1, "type {ty}");
        let c = o.blk.call[0];
        assert_eq!((c.kind, c.call_id, c.enc), (4, 777, -1), "type {ty}");
    }
    // an air interface encrypted MAC-RESOURCE: nothing of the call is read
    let mut b = setup_block(0, 0b0100, 1234);
    b[4] = 1; // encryption mode bits (after type 2, fill 1, grant 1): 2 bits at 4..6
    let mut nw = [Nwrk::default(); 4];
    let mut nb = [Neighbour::default(); 8];
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&b, &mut o);
    assert_eq!((o.blk.encrypted, o.blk.n_call), (1, 0));
}

/// A clear group-call D-SETUP (communication type 1) in a MAC-RESOURCE whose address is of type `at`: the SSI 0x123456, then for types 5 and 6
/// the event label (10) or usage marker (6); then the mandatory elements up to the call priority and an O-bit of 0.
fn group_setup_block(at: u64) -> Vec<u8> {
    let extra = [0usize, 0, 0, 0, 0, 10, 6, 0][at as usize];
    let mut w = W(vec![]);
    for (v, n) in [(0, 2), (1, 1), (0, 1), (0, 2), (0, 1), (17, 6), (at, 3), (0x123456, 24)] {
        w.put(v, n);
    }
    w.put(0, extra);
    for (v, n) in [(0, 1), (0, 1), (0, 1)] {
        w.put(v, n); // no power control, no slot granting, no channel allocation
    }
    // LLC BL-UDATA, MLE discriminator 2, D-SETUP: call id 777, time-out 5, hook 0, simplex 1, circuit mode 0, clear, point-to-multipoint,
    // slots / speech 0; transmission grant 0, request permission 0, priority 0, O-bit 0
    for (v, n) in
        [(2, 4), (2, 3), (7, 5), (777, 14), (5, 4), (0, 1), (1, 1), (0, 3), (0, 1), (1, 2), (0, 2), (0, 2), (0, 1), (0, 4), (0, 1)]
    {
        w.put(v, n);
    }
    w.put(1, 1);
    while w.0.len() < 136 {
        w.put(0, 1);
    }
    w.0
}

#[test]
fn group_setup_gssi_by_address_type() {
    // SSI (1), SSI + event label (5), SSI + usage marker (6): the group; USSI (3) and SMI (4) are not a group address
    for (at, want) in [(1u64, 0x123456), (5, 0x123456), (6, 0x123456), (3, -1), (4, -1)] {
        let b = group_setup_block(at);
        let mut nw = [Nwrk::default(); 4];
        let mut nb = [Neighbour::default(); 8];
        let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
        parse_block(&b, &mut o);
        assert_eq!(o.blk.n_call, 1, "address type {at}");
        let c = o.blk.call[0];
        assert_eq!((c.kind, c.call_id, c.enc, c.gssi), (1, 777, 0, want), "address type {at}");
    }
}

/// A clear MAC-RESOURCE around a CMCE PDU given as bits, with the SSI address 0x123456.
fn cmce_block(pdu: &[u8], fragmented: bool) -> Vec<u8> {
    let mut w = W(vec![]);
    w.put(0, 2); // MAC-RESOURCE
    w.put(1, 1); // fill bits
    w.put(0, 1); // position of grant
    w.put(0, 2); // no air interface encryption
    w.put(0, 1); // random access
    let total = 43 + 7 + pdu.len();
    let octets = (total + 8) / 8;
    w.put(if fragmented { 0x3F } else { octets as u64 }, 6);
    w.put(1, 3); // address type 1: SSI
    w.put(0x123456, 24);
    w.put(0, 1); // power control
    w.put(0, 1); // slot granting
    w.put(0, 1); // channel allocation
    while w.0.len() < 43 {
        w.put(0, 1);
    }
    w.put(0b0010, 4); // BL-UDATA
    w.put(2, 3); // CMCE
    for &b in pdu {
        w.put(b as u64, 1);
    }
    if !fragmented {
        w.put(1, 1);
        while w.0.len() % 8 != 0 {
            w.put(0, 1);
        }
    }
    while w.0.len() < 268 {
        w.put(0, 1);
    }
    w.0
}

fn pdu_bits(fields: &[(u64, u32)]) -> Vec<u8> {
    let mut w = W(vec![]);
    for &(v, n) in fields {
        w.put(v, n as usize);
    }
    w.0
}

#[test]
fn floor_holder_status_sds_and_fragments() {
    // D-TX-GRANTED with the transmitting party SSI 4321, clear call
    let p = pdu_bits(&[(11, 5), (777, 14), (0, 2), (0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (1, 1), (1, 2), (4321, 24)]);
    let mut nw = [Nwrk::default(); 4];
    let mut nb = [Neighbour::default(); 8];
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&cmce_block(&p, false), &mut o);
    assert_eq!(o.blk.n_call, 1);
    let c = o.blk.call[0];
    assert_eq!((c.kind, c.call_id, c.enc, c.issi), (4, 777, 0, 4321));
    // the same with the end-to-end encryption flag set: nothing about the party
    let p = pdu_bits(&[(11, 5), (777, 14), (0, 2), (0, 1), (1, 1), (0, 1), (1, 1), (0, 1), (1, 1), (1, 2), (4321, 24)]);
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&cmce_block(&p, false), &mut o);
    assert_eq!(o.blk.call[0].issi, -1);
    // D-STATUS: handed over as a bit range
    let p = pdu_bits(&[(8, 5), (1, 2), (4321, 24), (32769, 16)]);
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&cmce_block(&p, false), &mut o);
    assert_eq!(o.blk.n_cmce, 1);
    assert_eq!(o.blk.cmce_addr[0], 0x123456);
    let n = (o.blk.cmce_to[0] - o.blk.cmce_from[0]) as usize;
    assert!(n >= p.len() && n < p.len() + 12, "{n}");
    // a fragmented PDU: the first part is reported, nothing is parsed yet
    let long: Vec<u8> = vec![0u8; 400];
    let b = cmce_block(&long, true);
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&b, &mut o);
    assert_eq!((o.blk.frag_kind, o.blk.frag_addr, o.blk.n_call), (1, 0x123456, 0));
    assert_eq!(o.blk.frag_from as usize, 43);
    // MAC-FRAG without fill: TM-SDU after the 4-bit header
    let mut f = vec![0u8; 124];
    f[0] = 0;
    f[1] = 1; // MAC PDU type 01
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&f, &mut o);
    assert_eq!((o.blk.frag_kind, o.blk.frag_from, o.blk.frag_to), (2, 4, 124));
    // MAC-END with LI = 10 octets, no grants: TM-SDU 15 .. 80
    let mut w = W(vec![]);
    w.put(1, 2);
    w.put(1, 1);
    w.put(0, 1);
    w.put(0, 1);
    w.put(10, 6);
    w.put(0, 1);
    w.put(0, 1);
    while w.0.len() < 124 {
        w.put(0, 1);
    }
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&w.0, &mut o);
    assert_eq!((o.blk.frag_kind, o.blk.frag_from, o.blk.frag_to), (3, 13, 80));
    // an encrypted start is not reported as a fragment
    let mut b = cmce_block(&long, true);
    b[4] = 1;
    let mut o = Out { blk: MacBlock::default(), nwrk: &mut nw, nbr: &mut nb };
    parse_block(&b, &mut o);
    assert_eq!(o.blk.frag_kind, 0);
}
