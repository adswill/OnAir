// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Parity-check tables of the DVB-T2 / DVB-S2 style IRA LDPC codes (EN 302 755 section 6.1.2, EN 302 307 section 5.3): the address table of the 360 bit
//! groups of information bits is expanded into everything the encoder and the decoders walk (the decoders themselves stay in C++, where the NEON / AVX2 /
//! GPU paths are). Address `a` of group `g`, information bit `c` of the group (0..360) is connected to parity check `(a + c * q) mod m`, with
//! `m = n - k` and `q = m / 360`; the parity bits form the usual zig-zag (check j involves parity bits j and j-1).
//!
//! The output is CSR: for every check the list of its variables (information edges first in group / bit / address order, then the zig-zag pair), the edge
//! list (parity index, information bit) for the encoder, and for every layer `a mod q` the list of (group, shift = a / q) the layered decoders use.
//! Nothing is allocated: the caller sizes the arrays with `LdpcDims`.

/// Bits per group of the quasi-cyclic structure.
pub const GROUP: usize = 360;

/// Sizes of the output arrays for a code.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct LdpcDims {
    /// Parity bits, n - k.
    pub m: usize,
    /// m / 360: the number of layers and the step of the address pattern.
    pub q: usize,
    /// Total number of addresses of the table (all groups).
    pub addrs: usize,
    /// Information edges, `addrs * 360` (length of `enc_p`, `enc_d`).
    pub edges: usize,
    /// Length of `chk_var`: the information edges plus 2 m - 1 zig-zag edges.
    pub chk_var: usize,
}

/// The dimensions for a code with `k` information bits and `n` bits, or `None` when `n <= k`, `m < 360` or the table has more addresses than fit.
pub fn dims(k: usize, n: usize, addrs: usize) -> Option<LdpcDims> {
    let m = n.checked_sub(k)?;
    let q = m / GROUP;
    if m == 0 || q == 0 {
        return None;
    }
    let edges = addrs.checked_mul(GROUP)?;
    let chk_var = edges.checked_add(2 * m - 1)?;
    if chk_var > i32::MAX as usize {
        return None;
    }
    Some(LdpcDims { m, q, addrs, edges, chk_var })
}

/// Output arrays of `build` (all `i32`, sized by `LdpcDims`).
pub struct LdpcOut<'a> {
    /// `m + 1` offsets into `chk_var`.
    pub chk_start: &'a mut [i32],
    /// Variables of every check.
    pub chk_var: &'a mut [i32],
    /// Parity index of every information edge (group-major, then bit, then address).
    pub enc_p: &'a mut [i32],
    /// Information bit index of every information edge.
    pub enc_d: &'a mut [i32],
    /// `q + 1` offsets into `layer_group` / `layer_shift`.
    pub layer_start: &'a mut [i32],
    /// Group of every layer connection (ordered by layer, then table order).
    pub layer_group: &'a mut [i32],
    /// Shift (a / q) of every layer connection.
    pub layer_shift: &'a mut [i32],
}

/// Builds the tables. `row_len[g]` is the number of addresses of group g and `addrs` all addresses in order. Returns false for bad dimensions, a negative address,
/// `row_len` not adding up to `addrs.len()`, or output arrays of the wrong length.
pub fn build(k: usize, n: usize, row_len: &[u32], addrs: &[i32], out: &mut LdpcOut) -> bool {
    let total: usize = row_len.iter().map(|&x| x as usize).sum();
    if total != addrs.len() || addrs.iter().any(|&a| a < 0) {
        return false;
    }
    let d = match dims(k, n, total) {
        Some(d) => d,
        None => return false,
    };
    if out.chk_start.len() != d.m + 1
        || out.chk_var.len() != d.chk_var
        || out.enc_p.len() != d.edges
        || out.enc_d.len() != d.edges
        || out.layer_start.len() != d.q + 1
        || out.layer_group.len() != total
        || out.layer_shift.len() != total
    {
        return false;
    }
    let (m, q) = (d.m, d.q);
    // ---- layers: counts, prefix sums, fill (stable: group order, then address order)
    out.layer_start.fill(0);
    for &a in addrs {
        out.layer_start[a as usize % q + 1] += 1;
    }
    for l in 0..q {
        out.layer_start[l + 1] += out.layer_start[l];
    }
    {
        // the fill cursor of layer l is layer_start[l]; afterwards layer_start[l] has moved to the start of layer l + 1, which is undone below
        let mut idx = 0usize;
        for (g, &len) in row_len.iter().enumerate() {
            for &a in &addrs[idx..idx + len as usize] {
                let l = a as usize % q;
                let pos = out.layer_start[l] as usize;
                out.layer_group[pos] = g as i32;
                out.layer_shift[pos] = (a as usize / q) as i32;
                out.layer_start[l] += 1;
            }
            idx += len as usize;
        }
        for l in (1..=q).rev() {
            out.layer_start[l] = out.layer_start[l - 1];
        }
        out.layer_start[0] = 0;
    }
    // ---- edges, in the order group, bit, address; the check of an edge is (a + c q) mod m
    let mut e = 0usize;
    let mut im = 0i32;
    let mut idx = 0usize;
    for &len in row_len {
        let row = &addrs[idx..idx + len as usize];
        idx += len as usize;
        for c in 0..GROUP {
            for &a in row {
                let p = (a as usize + c * q) % m;
                out.enc_p[e] = p as i32;
                out.enc_d[e] = im;
                e += 1;
            }
            im += 1;
        }
    }
    // ---- checks: counts (information edges plus the zig-zag pair), prefix sums, fill in the same order as the reference (information edges first)
    out.chk_start.fill(0);
    for &p in out.enc_p.iter() {
        out.chk_start[p as usize + 1] += 1;
    }
    for j in 0..m {
        out.chk_start[j + 1] += if j > 0 { 2 } else { 1 };
    }
    for j in 0..m {
        out.chk_start[j + 1] += out.chk_start[j];
    }
    // cursors: chk_start[j] is the fill position of check j; afterwards it is the end of check j (= the start of j + 1)
    for (ed, &p) in out.enc_d.iter().zip(out.enc_p.iter()) {
        let pos = out.chk_start[p as usize] as usize;
        out.chk_var[pos] = *ed;
        out.chk_start[p as usize] += 1;
    }
    for j in 0..m {
        let pos = out.chk_start[j] as usize;
        out.chk_var[pos] = (k + j) as i32;
        out.chk_start[j] += 1;
        if j > 0 {
            out.chk_var[pos + 1] = (k + j - 1) as i32;
            out.chk_start[j] += 1;
        }
    }
    // chk_start[j] is now the end of check j: shift to the offsets of the starts (chk_start[m] already holds the total from the prefix sums)
    for j in (1..m).rev() {
        out.chk_start[j] = out.chk_start[j - 1];
    }
    out.chk_start[0] = 0;
    true
}
