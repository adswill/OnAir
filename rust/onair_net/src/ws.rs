// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! WebSocket framing (RFC 6455 section 5) as the remote-control API uses it: server frames are never masked; client frames must be (a single frame, at most 4096 bytes
//! of payload, no fragmentation, text and control opcodes only). The parser works on whatever has arrived and says what to do.

/// Largest client frame payload the server accepts.
pub const MAX_CLIENT_FRAME: usize = 4096;

/// Length of the frame `frame` writes for a payload of `n` bytes.
pub const fn frame_len(n: usize) -> usize {
    n + if n < 126 {
        2
    } else if n < 65536 {
        4
    } else {
        10
    }
}

/// Writes a server to client frame (FIN set, not masked) with `opcode` and `payload` into `out`; returns its length or `usize::MAX` when `out` is too short.
pub fn frame(opcode: u8, payload: &[u8], out: &mut [u8]) -> usize {
    let n = payload.len();
    let total = frame_len(n);
    if out.len() < total {
        return usize::MAX;
    }
    out[0] = 0x80 | (opcode & 0x0F);
    let mut k = 1;
    if n < 126 {
        out[k] = n as u8;
        k += 1;
    } else if n < 65536 {
        out[k] = 126;
        out[k + 1] = (n >> 8) as u8;
        out[k + 2] = n as u8;
        k += 3;
    } else {
        out[k] = 127;
        for i in 0..8 {
            out[k + 1 + i] = (((n as u64) >> (8 * (7 - i))) & 0xFF) as u8;
        }
        k += 9;
    }
    out[k..k + n].copy_from_slice(payload);
    total
}

/// What a client frame at the start of the received bytes says.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Client {
    /// Not all of the frame is here yet.
    Need,
    /// Close the connection with this status code (1002 unmasked, 1003 unsupported opcode or fragmentation, 1009 too big).
    Close(u16),
    /// A whole frame: `used` bytes of the input, opcode, FIN flag; the payload (unmasked) was written to the caller's buffer, `len` bytes.
    Frame {
        /// Bytes taken from the input.
        used: usize,
        /// Opcode (1 text, 8 close, 9 ping, 10 pong after the checks below).
        op: u8,
        /// FIN bit (always true for the frames that get through).
        fin: bool,
        /// Payload bytes written to `payload`.
        len: usize,
    },
}

/// Parses the first client frame of `input`. The unmasked payload goes to `payload` (at least `MAX_CLIENT_FRAME` bytes). Frames that fragment, are binary or reserved,
/// or exceed the size limit are answered with `Close`; a ping, pong, close or text frame comes back as `Frame`.
pub fn parse_client(input: &[u8], payload: &mut [u8]) -> Client {
    if input.len() < 2 {
        return Client::Need;
    }
    let (b0, b1) = (input[0], input[1]);
    let op = b0 & 0x0F;
    let fin = b0 & 0x80 != 0;
    let masked = b1 & 0x80 != 0;
    let mut len = (b1 & 0x7F) as u64;
    let mut hl = 2usize;
    if len == 126 {
        if input.len() < 4 {
            return Client::Need;
        }
        len = ((input[2] as u64) << 8) | input[3] as u64;
        hl = 4;
    } else if len == 127 {
        return Client::Close(1009); // 64-bit lengths are never this small
    }
    if !masked {
        return Client::Close(1002); // RFC 6455 5.1: clients mask
    }
    if len > MAX_CLIENT_FRAME as u64 {
        return Client::Close(1009);
    }
    let len = len as usize;
    if input.len() < hl + 4 + len {
        return Client::Need;
    }
    if payload.len() < len {
        return Client::Close(1009);
    }
    let mk = &input[hl..hl + 4];
    for i in 0..len {
        payload[i] = input[hl + 4 + i] ^ mk[i & 3];
    }
    if !fin || op == 0 || op == 2 || (op > 2 && op < 8) || op > 10 {
        return Client::Close(1003); // no fragmentation, no binary
    }
    Client::Frame { used: hl + 4 + len, op, fin, len }
}
