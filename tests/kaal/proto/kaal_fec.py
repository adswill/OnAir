# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""KAAL FEC: rate-1/2 quasi-cyclic LDPC, n=640, k=320 (Z=32, 10x20 base matrix),
systematic, IRA-style dual-diagonal parity part, normalised min-sum decoder vectorised across frames.
Own construction (random QC shifts, 4-cycle free in the base graph), not a standard code."""
import numpy as np

Z, MB, NB = 32, 10, 20
N, K, M = Z * NB, Z * (NB - MB), Z * MB
ALPHA = 0.8        # min-sum normalisation
ITERS = 40


def _base(seed, weights):
    rng = np.random.default_rng(seed)
    for _ in range(2000):
        E = -np.ones((MB, NB), dtype=int)
        load = np.zeros(MB)
        for c, w in enumerate(weights):
            rows = rng.choice(MB, w, replace=False, p=None)
            # prefer low-load rows
            order = np.argsort(load + rng.random(MB) * 0.9)[:w]
            for r in order:
                E[r, c] = rng.integers(Z); load[r] += 1
        # parity: first column rows 0, MB//2, MB-1 ; then dual diagonal
        E[0, MB] = 1; E[MB // 2, MB] = 0; E[MB - 1, MB] = 1
        for j in range(1, MB):
            E[j - 1, MB + j] = 0; E[j, MB + j] = 0
        if _no4(E):
            return E
    raise RuntimeError("no base")


def _no4(E):
    for a in range(MB):
        for b in range(a + 1, MB):
            cols = [c for c in range(NB) if E[a, c] >= 0 and E[b, c] >= 0]
            seen = set()
            for c in cols:
                d = (E[a, c] - E[b, c]) % Z
                if d in seen:
                    return False
                seen.add(d)
    return True


def _expand(E):
    H = np.zeros((M, N), dtype=np.uint8)
    I = np.arange(Z)
    for r in range(MB):
        for c in range(NB):
            if E[r, c] >= 0:
                H[r * Z + I, c * Z + (I + E[r, c]) % Z] = 1
    return H


def _gf2_inv(A):
    n = len(A); A = np.hstack([A.copy(), np.eye(n, dtype=np.uint8)])
    for i in range(n):
        p = i + np.argmax(A[i:, i])
        if A[p, i] == 0:
            return None
        A[[i, p]] = A[[p, i]]
        rows = np.nonzero(A[:, i])[0]; rows = rows[rows != i]
        A[rows] ^= A[i]
    return A[:, n:]


def build(seed=0, weights=(3,) * 10):
    E = _base(seed, weights)
    H = _expand(E)
    Hinv = _gf2_inv(H[:, K:])
    if Hinv is None:
        return None
    Pm = (Hinv.astype(np.int32) @ H[:, :K].astype(np.int32) % 2).astype(np.uint8)  # parity = Pm @ info
    return H, Pm


# defaults chosen by sim (see __main__ search); filled below
SEED, WEIGHTS = 0, (6,) + (3,) * 9
_cache = {}


def _get():
    if "H" not in _cache:
        r = build(SEED, WEIGHTS)
        assert r is not None
        H, Pm = r
        dc = H.sum(1).max()
        chk = np.zeros((M, dc), dtype=np.int32); msk = np.zeros((M, dc), dtype=bool)
        for i in range(M):
            idx = np.nonzero(H[i])[0]; chk[i, :len(idx)] = idx; msk[i, :len(idx)] = True
        _cache.update(H=H, Pm=Pm, chk=chk, msk=msk, PmT=Pm.T.astype(np.int32))
    return _cache


def encode_frames(info_bits):
    """info_bits: (F,320) -> (F,640) systematic codewords [info | parity]."""
    c = _get()
    u = np.asarray(info_bits, dtype=np.int32).reshape(-1, K)
    par = (u @ c["PmT"]) & 1
    return np.hstack([u, par]).astype(np.uint8)


def decode_frames(llr, iters=None, alpha=ALPHA):
    """llr: (F,640), positive = bit 0 more likely. Returns (bits (F,320), syndrome_ok (F,))."""
    c = _get(); chk, msk = c["chk"], c["msk"]
    iters = iters or ITERS
    L = np.asarray(llr, dtype=np.float32)
    F = len(L)
    R = np.zeros((F, M, chk.shape[1]), dtype=np.float32)
    big = np.float32(1e9)
    for it in range(iters):
        post = L + 0  # recomputed below
        # variable->check: Q = L + sum R - R(this edge)
        tot = L.copy()
        np.add.at(tot, (slice(None), chk[msk]), R[:, msk]) if False else None
        S = np.zeros_like(L)
        for k in range(chk.shape[1]):
            m = msk[:, k]
            np.add.at(S.T, chk[m, k], R[:, m, k].T)
        post = L + S
        Q = post[:, chk] - R
        Q = np.where(msk[None], Q, 0)
        hard = post < 0
        syn = (hard[:, chk] & msk[None]).sum(2) & 1
        ok = ~syn.any(1)
        if ok.all():
            break
        A = np.where(msk[None], np.abs(Q), big)
        sgn = np.where(Q < 0, -1.0, 1.0).astype(np.float32)
        sgn = np.where(msk[None], sgn, 1.0)
        tsign = sgn.prod(2, keepdims=True)
        i1 = A.argmin(2)[..., None]
        m1 = np.take_along_axis(A, i1, 2)
        A2 = A.copy(); np.put_along_axis(A2, i1, big, 2)
        m2 = A2.min(2, keepdims=True)
        pos = np.arange(chk.shape[1])[None, None]
        mag = np.where(pos == i1, m2, m1)
        R = (alpha * tsign * sgn * mag).astype(np.float32)
        R = np.where(msk[None], R, 0)
    S = np.zeros_like(L)
    for k in range(chk.shape[1]):
        m = msk[:, k]
        np.add.at(S.T, chk[m, k], R[:, m, k].T)
    post = L + S
    hard = (post < 0)
    syn = (hard[:, chk] & msk[None]).sum(2) & 1
    return hard[:, :K].astype(np.uint8), ~syn.any(1)


def syndrome_score(llr):
    """Fraction of satisfied parity checks on hard decisions, averaged (alignment metric)."""
    c = _get()
    hard = np.asarray(llr) < 0
    syn = (hard[:, c["chk"]] & c["msk"][None]).sum(2) & 1
    return float(1 - syn.mean())
