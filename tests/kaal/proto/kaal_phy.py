# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""KAAL physical layer, shared constants and helpers (v0.2 draft).
Used by kaal_tx.py, kaal_rx.py and kaal_test.py. Do not change numbers here without updating all three.
"""
import numpy as np

FS = 48_000                 # baseband sample rate (samples/s)
SYM = 8_000                 # symbol rate (symbols/s), 16-QAM
SPS = FS // SYM            # samples per symbol (6)
BETA = 0.25                 # root-raised-cosine roll-off
RRC_SPAN = 8                # symbols, each side = RRC_SPAN/2

PILOT_EVERY = 16            # every 16th data symbol is a known pilot (1 in 16)
PILOT = (3 + 3j) / np.sqrt(10)
PREAMBLE_LEN = 32           # known symbols at the start of each frame

FEC_G = (0o171, 0o133)      # rate 1/2, constraint length 7 convolutional code
FEC_K = 7

FRAME_INFO_BITS = 320       # codec frame size
CODED_BITS = 2 * FRAME_INFO_BITS           # 640 coded bits per frame
QAM_SYMBOLS = CODED_BITS // 4              # 160 data symbols per frame
N_PILOTS = QAM_SYMBOLS // (PILOT_EVERY - 1)  # pilots inserted among data symbols (10)
FRAME_SYMBOLS = PREAMBLE_LEN + QAM_SYMBOLS + N_PILOTS

INTERLEAVE_FRAMES = 4       # block interleaver across this many frames

SWEEP_PERIOD_S = 0.5        # signature chirp repeats every 0.5 s
SWEEP_LOW_HZ = -5000.0
SWEEP_HIGH_HZ = 5000.0
SWEEP_POWER_DB = -10.0      # relative to the data power

PREAMBLE = np.array([((1 if (i * 7 + 3) % 5 < 2 else -1) + 1j * (1 if (i * 11 + 1) % 3 else -1)) / np.sqrt(2) for i in range(PREAMBLE_LEN)])

GRAY2 = {(0, 0): -3, (0, 1): -1, (1, 1): 1, (1, 0): 3}   # 2 bits -> level (Gray)

def qam16_map(bits):
    """bits: array of 4*n bits (I0 I1 Q0 Q1 per symbol) -> complex 16-QAM symbols, unit average power."""
    b = np.asarray(bits, dtype=int).reshape(-1, 4)
    i = np.array([GRAY2[(x[0], x[1])] for x in b])
    q = np.array([GRAY2[(x[2], x[3])] for x in b])
    return (i + 1j * q) / np.sqrt(10)

def qam16_demap_soft(sym, noise_var):
    """Max-log soft bits (positive = bit 0 more likely). Returns array of 4*n LLRs."""
    s = np.asarray(sym) * np.sqrt(10)
    levels = np.array([-3, -1, 1, 3])
    lv_bits = {(-3): (0, 0), (-1): (0, 1), (1): (1, 1), (3): (1, 0)}
    out = []
    for comp in (s.real, s.imag):
        for bit_pos in (0, 1):
            m0 = np.array([min(((c - l) ** 2 for l in levels if lv_bits[l][bit_pos] == 0)) for c in comp])
            m1 = np.array([min(((c - l) ** 2 for l in levels if lv_bits[l][bit_pos] == 1)) for c in comp])
            out.append((m1 - m0) / (noise_var * 10 + 1e-12))
    # interleave to I0 I1 Q0 Q1 order per symbol
    llr = np.stack([out[0], out[1], out[2], out[3]], axis=1)
    return llr.reshape(-1)

def conv_encode(bits):
    state = 0; out = np.empty(2 * len(bits), dtype=np.uint8)
    par = lambda x: bin(x).count('1') & 1
    for i, u in enumerate(bits):
        reg = (int(u) << 6) | state
        out[2*i] = par(reg & FEC_G[0]); out[2*i+1] = par(reg & FEC_G[1])
        state = reg >> 1
    return out

def sweep_template(n_samples, t0=0.0):
    """Known signature chirp, phase-continuous from time t0 (seconds). Complex baseband at FS."""
    t = t0 + np.arange(n_samples) / FS
    ph_t = t % SWEEP_PERIOD_S
    inst_f = SWEEP_LOW_HZ + (SWEEP_HIGH_HZ - SWEEP_LOW_HZ) * (ph_t / SWEEP_PERIOD_S)
    return np.exp(1j * 2 * np.pi * np.cumsum(inst_f) / FS)

def rrc_taps():
    beta, sps, span = BETA, SPS, RRC_SPAN
    n = np.arange(-span * sps // 2, span * sps // 2 + 1); t = n / sps
    h = np.where(np.abs(t) < 1e-9, 1 - beta + 4 * beta / np.pi, 0.0)
    m = np.abs(np.abs(t) - 1 / (4 * beta)) < 1e-9
    h[m] = beta / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
    o = ~(np.abs(t) < 1e-9) & ~m; tt = t[o]
    h[o] = (np.sin(np.pi * tt * (1 - beta)) + 4 * beta * tt * np.cos(np.pi * tt * (1 + beta))) / (np.pi * tt * (1 - (4 * beta * tt) ** 2))
    return h / np.sqrt(np.sum(h ** 2))

INTERLEAVE_SEED = 0x4B41414C   # "KAAL"

def _xorshift64(state):
    """Portable generator shared with core/src/kaal/kaal_phy.cpp: returns (new_state, value)."""
    M = (1 << 64) - 1
    x = state
    x ^= (x << 13) & M
    x ^= x >> 7
    x ^= (x << 17) & M
    return x, x

def interleave_perm(n):
    """Fixed pseudo-random permutation of n coded bits: out[i] = in[perm[i]].
    Fisher-Yates from the top with xorshift64 (13,7,17), j = draw % (i+1); identical in C++."""
    p = list(range(n)); st = INTERLEAVE_SEED
    for i in range(n - 1, 0, -1):
        st, v = _xorshift64(st)
        j = v % (i + 1)
        p[i], p[j] = p[j], p[i]
    return np.array(p, dtype=np.int64)

def interleave_perm_inv(n):
    p = interleave_perm(n)
    inv = np.empty(n, dtype=p.dtype); inv[p] = np.arange(n)
    return inv

def prand_interleave(bits):
    b = np.asarray(bits)
    return b[interleave_perm(len(b))]

def prand_deinterleave(x):
    x = np.asarray(x)
    return x[interleave_perm_inv(len(x))]

def block_interleave(bits, rows=INTERLEAVE_FRAMES):
    n = len(bits) // rows * rows
    return np.asarray(bits[:n]).reshape(rows, -1).T.reshape(-1)

def block_deinterleave(bits, rows=INTERLEAVE_FRAMES):
    n = len(bits) // rows * rows
    cols = n // rows
    return np.asarray(bits[:n]).reshape(cols, rows).T.reshape(-1)


import os
FEC_MODE = os.environ.get("KAAL_FEC", "ldpc")   # "ldpc" (default) or "conv" (old baseline)

def fec_encode(bits):
    """Frame-aligned info bits (multiple of 320) -> coded bits (2x)."""
    if FEC_MODE == "conv":
        return conv_encode(bits)
    import kaal_fec
    return kaal_fec.encode_frames(np.asarray(bits).reshape(-1, FRAME_INFO_BITS)).reshape(-1)
