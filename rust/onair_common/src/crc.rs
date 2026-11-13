// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Generic CRC in the Rocksoft model (width, poly, init, refin, refout, xorout), bitwise: small and fast enough for frame-sized data.

/// Parameters of a CRC algorithm. `poly`, `init` and `xorout` are in normal (non-reflected) form, as in published catalogues.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CrcParams {
    pub width: u8,
    pub poly: u32,
    pub init: u32,
    pub refin: bool,
    pub refout: bool,
    pub xorout: u32,
}

/// CRC-16/CCITT-FALSE (a.k.a. CRC-16/IBM-3740): poly 0x1021, init 0xFFFF, no reflection. Check value 0x29B1.
pub const CRC16_CCITT_FALSE: CrcParams = CrcParams { width: 16, poly: 0x1021, init: 0xFFFF, refin: false, refout: false, xorout: 0 };
/// CRC-16/XMODEM: poly 0x1021, init 0, no reflection. Check value 0x31C3.
pub const CRC16_XMODEM: CrcParams = CrcParams { width: 16, poly: 0x1021, init: 0, refin: false, refout: false, xorout: 0 };
/// CRC-16/X-25 (HDLC FCS, AX.25, AIS, DMR): poly 0x1021 reflected, init 0xFFFF, xorout 0xFFFF. Check value 0x906E.
pub const CRC16_X25: CrcParams = CrcParams { width: 16, poly: 0x1021, init: 0xFFFF, refin: true, refout: true, xorout: 0xFFFF };
/// CRC-32/ISO-HDLC (Ethernet, zip): check value 0xCBF43926.
pub const CRC32: CrcParams = CrcParams { width: 32, poly: 0x04C1_1DB7, init: 0xFFFF_FFFF, refin: true, refout: true, xorout: 0xFFFF_FFFF };
/// CRC-32/MPEG-2 (MPEG-TS PSI sections): check value 0x0376E6E7.
pub const CRC32_MPEG2: CrcParams = CrcParams { width: 32, poly: 0x04C1_1DB7, init: 0xFFFF_FFFF, refin: false, refout: false, xorout: 0 };

/// Reverses the lowest `width` bits of `v`.
pub fn reflect(v: u32, width: u8) -> u32 {
    let mut r = 0u32;
    for i in 0..width as u32 {
        if v & (1 << i) != 0 {
            r |= 1 << (width as u32 - 1 - i);
        }
    }
    r
}

/// Incremental CRC. The register is kept in normal form; `finish` applies output reflection and xorout.
#[derive(Clone, Copy, Debug)]
pub struct Crc {
    p: CrcParams,
    reg: u32,
}

impl Crc {
    fn mask(&self) -> u32 {
        if self.p.width >= 32 {
            u32::MAX
        } else {
            (1u32 << self.p.width) - 1
        }
    }

    /// Starts a CRC with the algorithm's init value. `width` must be 1..=32.
    pub fn new(p: CrcParams) -> Crc {
        let mut c = Crc { p, reg: 0 };
        c.reg = p.init & c.mask();
        c
    }

    /// Feeds one bit, in the order it is received (the caller applies `refin` by choosing the bit order).
    pub fn update_bit(&mut self, bit: bool) {
        let top = (self.reg >> (self.p.width as u32 - 1)) & 1 != 0;
        self.reg = (self.reg << 1) & self.mask();
        if top != bit {
            self.reg ^= self.p.poly & self.mask();
        }
    }

    /// Feeds whole bytes; each byte is read MSB first, or LSB first when `refin` is set.
    pub fn update(&mut self, data: &[u8]) {
        for &b in data {
            for i in 0..8 {
                let bit = if self.p.refin { (b >> i) & 1 } else { (b >> (7 - i)) & 1 };
                self.update_bit(bit != 0);
            }
        }
    }

    /// The register as it is, without output reflection or xorout (useful for residue checks).
    pub fn register(&self) -> u32 {
        self.reg
    }

    /// The final CRC value.
    pub fn finish(&self) -> u32 {
        let r = if self.p.refout { reflect(self.reg, self.p.width) } else { self.reg };
        (r ^ self.p.xorout) & self.mask()
    }
}

/// CRC of `data` in one call.
pub fn crc(p: CrcParams, data: &[u8]) -> u32 {
    let mut c = Crc::new(p);
    c.update(data);
    c.finish()
}

/// CRC-16/CCITT-FALSE of `data`.
pub fn crc16_ccitt(data: &[u8]) -> u16 {
    crc(CRC16_CCITT_FALSE, data) as u16
}
/// CRC-16/X-25 of `data` (HDLC frame check sequence value, before it is sent low byte first).
pub fn crc16_x25(data: &[u8]) -> u16 {
    crc(CRC16_X25, data) as u16
}
/// CRC-32 (Ethernet) of `data`.
pub fn crc32(data: &[u8]) -> u32 {
    crc(CRC32, data)
}

#[cfg(test)]
mod tests {
    use super::*;
    const CHECK: &[u8] = b"123456789";

    #[test]
    fn known_vectors() {
        assert_eq!(crc16_ccitt(CHECK), 0x29B1);
        assert_eq!(crc16_x25(CHECK), 0x906E);
        assert_eq!(crc32(CHECK), 0xCBF4_3926);
        assert_eq!(crc(CRC16_XMODEM, CHECK), 0x31C3);
        assert_eq!(crc(CRC32_MPEG2, CHECK), 0x0376_E6E7);
    }

    #[test]
    fn incremental_matches_oneshot() {
        let mut c = Crc::new(CRC32);
        c.update(&CHECK[..4]);
        c.update(&CHECK[4..]);
        assert_eq!(c.finish(), 0xCBF4_3926);
    }

    #[test]
    fn x25_residue() {
        // Appending the FCS (low byte first) to the data leaves the constant residue 0x0F47 after xorout (0xF0B8 before).
        let fcs = crc16_x25(CHECK);
        let mut v = [0u8; 11];
        v[..9].copy_from_slice(CHECK);
        v[9] = fcs as u8;
        v[10] = (fcs >> 8) as u8;
        let mut c = Crc::new(CRC16_X25);
        c.update(&v);
        assert_eq!(reflect(c.register(), 16), 0xF0B8);
    }

    #[test]
    fn reflect_works() {
        assert_eq!(reflect(0x0001, 16), 0x8000);
        assert_eq!(reflect(0b1101, 4), 0b1011);
    }
}
