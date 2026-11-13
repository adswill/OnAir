#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors
"""Sensitivity gap of every mode: the QA sweep's measured threshold (tests/sens/qa41/*.jsonl, docs/SENSITIVITY-4.1-ALL.md) against the
threshold an ideal receiver of the same modulation, rate and unit length needs, in the same band (the test signal's 99 % band, B99).

    python3 tools/sens/gap_table.py [--after DIR_OR_JSONL ...] > table.md

theory (dB in B99) = Eb/N0 needed + 10 log10(Rb / B99), Eb/N0 for 90 % of L-bit units: (1 - BER)^L = 0.9, BER from the detector named in
PHY below. "U" rows are the uncoded bound of a coded mode (its true limit is lower, so the gap shown is a lower bound); "C" rows use the
Eb/N0 of the code named in the note. Rows with no simple model keep "-" and say why (class)."""
import argparse, glob, json, math, os, sys

def qinv(p):
    lo, hi = 0.0, 40.0
    for _ in range(200):
        m = (lo + hi) / 2
        if 0.5 * math.erfc(m / math.sqrt(2)) > p: lo = m
        else: hi = m
    return (lo + hi) / 2

def ebn0_db(mod, L):
    p = 1 - 0.9 ** (1.0 / L)
    if mod == "ncfsk":     # orthogonal noncoherent binary FSK, PPM, limiter-discriminator FSK as its usual stand-in
        e = 2 * math.log(1 / (2 * p))
    elif mod == "dbpsk":
        e = math.log(1 / (2 * p))
    elif mod == "bpsk":    # also QPSK, MSK per bit
        e = qinv(p) ** 2 / 2
    elif mod == "gmsk":    # BT 0.3, coherent: Q(sqrt(1.36 Eb/N0))
        e = qinv(p) ** 2 / 1.36
    elif mod == "pi4dqpsk":  # differential detection: about 2.3 dB over coherent QPSK
        e = qinv(p) ** 2 / 2 * 10 ** 0.23
    elif mod == "d8psk":   # differential 8PSK: about 3 dB over coherent 8PSK (Gray, BER ~ 2/3 Q(sqrt(6 Eb/N0) sin(pi/8)))
        e = (qinv(1.5 * p) / (math.sqrt(6) * math.sin(math.pi / 8))) ** 2 * 2
    elif mod == "pcsk":    # e-Czas: two phase states 36 degrees apart, detected coherently (+-18 degrees about the mean), bits are state changes
        e = (qinv(p / 2) / math.sin(math.radians(18))) ** 2 / 2
    elif mod == "pcskdiff":  # the same, detected as the phase step over each bit against half the step (what the sp5wwp slicer does)
        e = (qinv(p / 2) / math.radians(18)) ** 2
    else:
        raise ValueError(mod)
    return 10 * math.log10(e)

# mode: (bit rate, detector, unit bits, basis, note[, Eb/N0 override dB])
PHY = {
    "ais":        (9600, "gmsk", 256, "U", "GMSK 9600, HDLC frame"),
    "cubesat":    (9600, "gmsk", 400, "U", "GMSK / G3RUH 9600 (mix of framings)"),
    "ax100m5":    (9600, "ncfsk", 1000, "C", "GMSK 9600, RS(255,223) hard: channel BER 1.5e-2", 8.5),
    "ax100m6":    (9600, "ncfsk", 900, "U", "G3RUH 9600, HDLC around the RS block: one bit error breaks the HDLC frame"),
    "usp":        (9600, "ncfsk", 1000, "C", "FSK 9600, RS(255,223) hard", 8.5),
    "ngham":      (4800, "ncfsk", 600, "C", "FSK 4800, RS hard", 8.5),
    "beesat":     (4800, "ncfsk", 300, "C", "GMSK 4800, Mobitex (12,8) Hamming + interleave: channel BER about 1e-2", 9.4),
    "dstar":      (4800, "gmsk", 96, "U", "GMSK 4800, 96-bit voice frame"),
    "tetrapol":   (8000, "gmsk", 160, "U", "GMSK 8000"),
    "dect":       (1152000, "gmsk", 80, "U", "GFSK 1.152 Mb/s, A-field + CRC"),
    "vdl4":       (19200, "ncfsk", 250, "U", "GFSK 19.2 kb/s (h 0.5, BT 0.28), HDLC burst; the orthogonal-FSK bound is optimistic for h 0.5 BT 0.28 (a 1-bit differential detector needs several dB more); the 'before' 20 dB counted neighbour-channel copies (18 frames for 11 bursts)"),
    "ble":        (1000000, "ncfsk", 300, "U", "GFSK 1 Mb/s (h 0.5) on a discriminator"),
    "ant":        (1000000, "ncfsk", 150, "U", "GFSK 1 Mb/s"),
    "nrf24":      (1000000, "ncfsk", 200, "U", "GFSK 1 Mb/s"),
    "uat":        (1041667, "ncfsk", 4400, "C", "CPFSK 1.04 Mb/s, RS(48,34) t=7: channel BER about 2e-2", 8.1),
    "dsc":        (100, "ncfsk", 10, "U", "FSK 100 Bd, 10-bit symbols"),
    "gtor":       (300, "ncfsk", 576, "U", "FSK 300 Bd, 72-byte frame, CRC only (Golay parity frames are separate); 170 Hz shift is not orthogonal at 300 Bd (about 2 dB more)"),
    "hfpacket300": (300, "ncfsk", 320, "U", "AFSK 300 Bd HDLC"),
    "sarsat":     (400, "bpsk", 144, "U", "biphase PSK +-1.1 rad, 400 b/s, BCH (the carrier takes 21 % of the power: +1 dB)", None, 1.0),
    "argos":      (400, "bpsk", 256, "U", "biphase PSK +-1.1 rad, 400 b/s (carrier +1 dB)", None, 1.0),
    "sigfox":     (100, "dbpsk", 208, "U", "DBPSK 100 b/s"),
    "tetra":      (36000, "pi4dqpsk", 268, "U", "pi/4-DQPSK 36 kb/s, coded blocks (RCPC 2/3 + interleaving): the true limit is lower"),
    "vdl3":       (31500, "d8psk", 200, "U", "D8PSK 10.5 kBd"),
    "eczas":      (50, "pcsk", 96, "U", "two phase states 36 degrees apart, 50 b/s, on a constant carrier (the carrier counts in the SNR); coherent bound (the step detector: +2.9 dB; 4.1 sens3 adds a coherent detector; the 'after' figure is from 60 s trials, the 30 dB before was the latency of 8 s ones: 18 dB with 60 s)"),
    "pocsag":     (1200, "ncfsk", 544, "U", "FSK 1200, BCH(31,21) t=2 per word: the true limit is lower"),
    "rtty":       (45.45, "ncfsk", 8, "U", "FSK 45.45 Bd, characters"),
    "sitor":      (100, "ncfsk", 7, "U", "FSK 100 Bd, 7-bit characters"),
}

# no simple model: the class of each, by mode id
CLASS = {
    "analog or squelch count (calls, tones, audio lines): no bit error theory": "am cb fm frs pmr446 hamfm hfvoice mdc1200 multitone tones selcal ils vor vorils ndb cw general airband cquam elt121 voiceinv wmic sstv wefax hell noaaapt codar sferics whistler ism esubghz sensors",
    "data on an FM or AM audio channel: limited by the FM threshold, not a bit-error formula": "packet klw tdcs lbj same atis u482c acars flex pager efr",
    "4FSK on a discriminator with voice FEC (no closed form here; DMR at 6 dB is the reference)": "dmr dpmr nxdn p25 m17",
    "OFDM / concatenated broadcast and satellite links: see SENSITIVITY-4.1-TV.md and the X-band rows of SENSITIVITY-4.1-ALL.md": "drm hdr cdr isdbtsb dab freedv hamdrm kaal kapp kaft kait kavb kaws",
}
SCENE = "B99 holds several signals, levels or a wide scene: not comparable"

def load(paths):
    rows = {}
    for p in paths:
        files = sorted(glob.glob(os.path.join(p, "*.jsonl"))) if os.path.isdir(p) else [p]
        for f in files:
            for line in open(f):
                line = line.strip()
                if not line: continue
                d = json.loads(line)
                rows[d["mode"]] = d
    return rows

def theory(mode, b99):
    e = PHY.get(mode)
    if not e: return None
    rb, mod, L, basis, note = e[:5]
    eb = e[5] if len(e) > 5 and e[5] is not None else ebn0_db(mod, L)
    pen = e[6] if len(e) > 6 else 0.0
    return eb + pen + 10 * math.log10(rb / b99)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--before", default="tests/sens/qa41")
    ap.add_argument("--after", nargs="*", default=[])
    a = ap.parse_args()
    before, after = load([a.before]), load(a.after)
    cls = {m: k for k, v in CLASS.items() for m in v.split()}
    out = []
    for m, d in before.items():
        ref, thr = d.get("ref") or {}, d.get("thr") or {}
        b99 = ref.get("obw_hz")
        meas = thr.get("awgn") if ref.get("kind") == "blocks" else None
        th = theory(m, b99) if (b99 and meas is not None) else None
        gap = meas - th if th is not None else None
        aft = after.get(m, {}).get("thr", {}).get("awgn") if m in after else None
        cause = PHY[m][4] if m in PHY else cls.get(m, "")
        if not cause:
            if d.get("error"): cause = "not measured: " + d["error"]
            elif ref.get("kind") == "lock": cause = "lock flag only: no checked unit"
            elif ref.get("kind") == "saved": cause = "counts saved output, not checked units"
            elif meas is None: cause = "no threshold: " + d.get("note", "no point passed")
            else: cause = "no closed-form model here (coded, multi-carrier or PHY not modelled)"
        flags = []
        if ref.get("m0") is not None and ref.get("m0") <= 2: flags.append("1-2 units a trial: coarse")
        if ref.get("first_s") is not None and ref.get("secs") and ref["first_s"] >= 0.95 * ref["secs"]: flags.append("first unit at the end of the trial: latency-limited")
        if gap is not None and gap < -2: flags.append("negative gap: the model does not fit the test signal (shorter unit, code, 50 % pass)")
        if flags: cause += "; " + "; ".join(flags)
        out.append((m, d.get("name", m), meas, aft, th, gap, PHY[m][3] if m in PHY else "", cause, ref.get("m0"), d.get("false_units"), d.get("false_trials")))
    out.sort(key=lambda r: (-(r[5] if r[5] is not None else -99), r[0]))
    print("| mode | measured (dB in B99) | after | theory | gap (dB) | gap after | basis | units per trial | false (units / trials) | model / class |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    f = lambda v: "-" if v is None else ("%+.1f" % v if isinstance(v, float) and v < 0 else "%.1f" % v)
    for m, name, meas, aft, th, gap, basis, cause, m0, fu, ft in out:
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (m, f(meas), f(aft), f(th), "-" if gap is None else "%+.1f" % gap,
              "-" if (aft is None or th is None) else "%+.1f" % (aft - th), basis or "-",
              "-" if m0 is None else int(m0), "-" if ft is None else "%d / %d" % (fu, ft), cause))

if __name__ == "__main__":
    main()
