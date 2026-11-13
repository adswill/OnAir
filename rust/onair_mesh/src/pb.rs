// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! A small Protocol Buffers reader (wire format: protobuf encoding guide) for the Meshtastic messages: varint, 64-bit, length-delimited and 32-bit fields.
//! Unknown fields are returned like any other and ignored by the caller; groups (wire types 3, 4) and the reserved types 6, 7 end the message as malformed.

/// One decoded field: number, wire type and the value (wire 0, 1, 5) or the bytes (wire 2; empty for the other types).
#[derive(Clone, Copy)]
pub struct Field<'a> {
    /// Field number (low 32 bits of the key shifted right by 3; 0 is malformed).
    pub num: u32,
    /// Wire type: 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32.
    pub wire: i32,
    /// Value of wire types 0, 1, 5 (0 for wire 2).
    pub value: u64,
    /// The bytes of a length-delimited field.
    pub data: &'a [u8],
}

impl<'a> Field<'a> {
    /// `int32` / `sfixed32`: the low 32 bits as signed (a negative int32 is a 10 byte varint).
    pub fn as_i32(&self) -> i32 {
        self.value as u32 as i32
    }
    /// `sint32` zig-zag.
    pub fn as_sint32(&self) -> i32 {
        let v = self.value as u32;
        ((v >> 1) as i32) ^ (-((v & 1) as i32))
    }
    /// `float` from the low 32 bits.
    pub fn as_f32(&self) -> f32 {
        f32::from_bits(self.value as u32)
    }
}

/// Walks the fields of one message.
pub struct Reader<'a> {
    buf: &'a [u8],
    pos: usize,
    ok: bool,
}

impl<'a> Reader<'a> {
    /// A reader over `buf`.
    pub fn new(buf: &'a [u8]) -> Self {
        Reader { buf, pos: 0, ok: true }
    }
    /// A reader that continues at byte offset `pos` of `buf` with the given error state (used by the C ABI to resume a walk).
    pub fn at(buf: &'a [u8], pos: usize, ok: bool) -> Self {
        Reader { buf, pos: pos.min(buf.len()), ok }
    }
    /// Current byte offset.
    pub fn pos(&self) -> usize {
        self.pos
    }
    /// False after a malformed field.
    pub fn ok(&self) -> bool {
        self.ok
    }
    /// A varint of at most 10 bytes.
    fn varint(&mut self) -> Option<u64> {
        let mut v = 0u64;
        for i in 0..10 {
            let b = *self.buf.get(self.pos)?;
            self.pos += 1;
            v |= ((b & 0x7f) as u64) << (7 * i);
            if b & 0x80 == 0 {
                return Some(v);
            }
        }
        None
    }
    /// The next field; `None` at the end of the message or on a malformed field (then `ok()` is false).
    #[allow(clippy::should_implement_trait)] // not an iterator: it also reports malformed input through `ok()`
    pub fn next(&mut self) -> Option<Field<'a>> {
        if !self.ok || self.pos >= self.buf.len() {
            return None;
        }
        let key = match self.varint() {
            Some(k) => k,
            None => {
                self.ok = false;
                return None;
            }
        };
        let num = (key >> 3) as u32;
        let wire = (key & 7) as i32;
        if num == 0 {
            self.ok = false;
            return None;
        }
        let rest = self.buf.len() - self.pos;
        let mut f = Field { num, wire, value: 0, data: &[] };
        match wire {
            0 => match self.varint() {
                Some(v) => f.value = v,
                None => {
                    self.ok = false;
                    return None;
                }
            },
            1 | 5 => {
                let n = if wire == 1 { 8 } else { 4 };
                if rest < n {
                    self.ok = false;
                    return None;
                }
                for i in 0..n {
                    f.value |= (self.buf[self.pos + i] as u64) << (8 * i);
                }
                self.pos += n;
            }
            2 => {
                let n = match self.varint() {
                    Some(n) if n <= (self.buf.len() - self.pos) as u64 => n as usize,
                    _ => {
                        self.ok = false;
                        return None;
                    }
                };
                f.data = &self.buf[self.pos..self.pos + n];
                self.pos += n;
            }
            _ => {
                self.ok = false;
                return None;
            }
        }
        Some(f)
    }
}

/// Calls `cb` for each value of a repeated `fixed32` field, packed or not (a partial trailing group of a packed field is ignored).
pub fn each_fixed32(f: &Field, mut cb: impl FnMut(u32)) {
    if f.wire == 5 {
        cb(f.value as u32);
    } else if f.wire == 2 {
        for c in f.data.chunks_exact(4) {
            cb(u32::from_le_bytes([c[0], c[1], c[2], c[3]]));
        }
    }
}

/// Calls `cb` for each value of a repeated varint field, packed or not (a packed varint cut short by the end of the field is still reported).
pub fn each_varint(f: &Field, mut cb: impl FnMut(u64)) {
    if f.wire == 0 {
        cb(f.value);
    } else if f.wire == 2 {
        let d = f.data;
        let mut p = 0;
        while p < d.len() {
            let mut v = 0u64;
            let mut i = 0;
            while i < 10 && p < d.len() {
                let b = d[p];
                p += 1;
                v |= ((b & 0x7f) as u64) << (7 * i);
                if b & 0x80 == 0 {
                    break;
                }
                i += 1;
            }
            cb(v);
        }
    }
}
