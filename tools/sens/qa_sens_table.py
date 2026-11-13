#!/usr/bin/env python3
"""Merge the qa_sens.py shard results (qa-sens-*.jsonl) into the markdown table of docs/SENSITIVITY-4.1-ALL.md.

  tools/sens/qa_sens_table.py results/*/qa-sens-*.jsonl > table.md

Theory: for the modes listed in THEORY the SNR an ideal receiver needs in the measured 99 % band for 90 % of its units, from the bit error
rate of the modulation, the bit rate and the unit length: (1 - BER)^L = 0.9 (uncoded), Eb/N0 -> SNR = Eb/N0 + 10 log10(Rb / B99).
"""
import json, math, sys, glob, os, re

def q(x): return 0.5 * math.erfc(x / math.sqrt(2))

def ber(mod, ebn0):
    g = 10 ** (ebn0 / 10)
    if mod == "bpsk": return q(math.sqrt(2 * g))                 # coherent BPSK / QPSK per bit
    if mod == "dbpsk": return 0.5 * math.exp(-g)                 # differential BPSK
    if mod == "ncfsk": return 0.5 * math.exp(-g / 2)             # noncoherent orthogonal binary FSK
    if mod == "cfsk": return q(math.sqrt(g))                     # coherent orthogonal binary FSK
    if mod == "gmsk": return q(math.sqrt(2 * 0.68 * g))          # GMSK BT 0.3, coherent (Murota: 0.68)
    if mod == "ook": return 0.5 * math.exp(-g / 2)               # noncoherent OOK (average energy), approx
    if mod == "ppm": return 0.5 * math.exp(-g / 2)               # binary PPM (Mode S style), noncoherent
    m = re.match(r"mfsk(\d+)", mod)
    if m:                                                        # noncoherent M-FSK, symbol error -> bit error, union-free exact sum
        M = int(m.group(1)); k = math.log2(M); es = g * k
        ps = sum(((-1) ** (n + 1)) * math.comb(M - 1, n) / (n + 1) * math.exp(-n / (n + 1) * es) for n in range(1, M))
        return ps * (M / 2) / (M - 1)
    raise ValueError(mod)

def need_ebn0(mod, L, success=0.9):
    target = 1 - success ** (1.0 / L)
    lo, hi = -10.0, 30.0
    for _ in range(80):
        mid = (lo + hi) / 2
        if ber(mod, mid) > target: lo = mid
        else: hi = mid
    return hi

# mode id -> (modulation, bit rate b/s, bits per unit, note). Uncoded unless the note says so. Only modes whose physical layer is simple and
# known; the others have no theory entry.
THEORY = {
    "ais": ("gmsk", 9600, 256, "GMSK 9600, one 256-bit slot (HDLC, CRC)"),
    "cubesat": ("ncfsk", 9600, 1024, "GFSK 9600 (G3RUH / AX100), a 128-byte frame, noncoherent"),
    "usp": ("ncfsk", 9600, 1024, "USP 9600 FSK, uncoded bound (the code lowers the true limit)"),
    "ngham": ("ncfsk", 4800, 512, "NGHam 4800 GFSK, uncoded bound (RS lowers the true limit)"),
    "lbj": ("ncfsk", 1200, 544, "LBJ (POCSAG-style) 1200 FSK, one batch"),
    "hfpacket300": ("ncfsk", 300, 240, "300 Bd AFSK, 200 Hz shift (not orthogonal: about 1.5 dB more), 30 bytes"),
    "gtor": ("ncfsk", 300, 240, "G-TOR 300 Bd, 200 Hz shift (not orthogonal), 30-byte frame uncoded"),
    "sigfox": ("dbpsk", 100, 208, "Sigfox DBPSK 100 b/s, 26-byte frame"),
    "dsc": ("ncfsk", 100, 700, "HF DSC 100 Bd FSK, 170 Hz shift, a call (two copies merged: about 3 dB less)"),
    "atis": ("ncfsk", 1200, 140, "ATIS 1200 Bd tones on FM (FM gain not counted)"),
    "fskmodem": ("ncfsk", 300, 240, "Bell 103 300 Bd async, 24 bytes"),
    "fskmodem202": ("ncfsk", 1200, 240, "Bell 202 1200 Bd async, 24 bytes"),
}

def need0(ref):
    return 1 if ref["kind"] == "lock" else max(1, math.floor(0.5 * ref["m0"]))


def prior_measured():
    docs = glob.glob(os.path.join(os.path.dirname(__file__), "..", "..", "docs", "SENSITIVITY*.md"))
    txt = "\n".join(open(d, encoding="utf-8").read() for d in docs if "4.1-ALL" not in d)
    return txt.lower()

def main():
    rows = []
    for f in sys.argv[1:]:
        for line in open(f, encoding="utf-8"):
            if line.strip(): rows.append(json.loads(line))
    rows.sort(key=lambda r: r["mode"])
    prior = prior_measured()
    fmt = lambda v: "-" if v is None else "%g" % v
    print("| mode | name | measure | B99 (Hz) | awgn | +2 kHz +50 ppm | off-centre -50 ppm | false (units / noise trials) | theory | gap (dB) | had a figure before | note |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        mid = r["mode"]
        had = "yes" if re.search(r"(^|[^a-z0-9])" + re.escape(mid.lower()) + r"($|[^a-z0-9])", prior) else "no"
        if "error" in r and "ref" not in r:
            print("| %s | %s | - | - | - | - | - | - | - | - | %s | %s |" % (mid, r.get("name", ""), had, r["error"])); continue
        th, ref = r.get("thr", {}), r["ref"]
        theo, gap = "-", "-"
        if mid in THEORY:
            mod, rb, L, note = THEORY[mid]
            t = need_ebn0(mod, L) + 10 * math.log10(rb / max(ref["obw_hz"], 1.0))
            theo = "%.1f" % t
            if th.get("awgn") is not None: gap = "%+.1f" % (th["awgn"] - t)
            if th.get("awgn") is not None and th["awgn"] - t < -2: gap += " (theory assumption off: unit shorter or coded)"
        notes = []
        if r.get("note"): notes.append(r["note"])
        if th.get("awgn") is not None and th["awgn"] <= -24: notes.append("SNR band too wide (test signal holds several carriers or spurs): not comparable")
        if ref["kind"] == "lock": notes.append("lock flag only, no checked unit")
        if (r.get("false_units") or 0) > 0 and ref["kind"] != "lock": notes.append("FALSE DECODES on noise")
        if ref.get("first_s", 0) >= 0.9 * ref.get("secs", 8): notes.append("units only at the end of the trial: latency-limited")
        u = r.get("units", {})
        vals = [u[k] for k in sorted(u, key=float) if th.get("awgn") is not None and float(k) > th["awgn"]]
        if any(v < need0(ref) for v in vals): notes.append("a failing trial above the threshold (timing on a loaded runner)")
        print("| %s | %s | %s | %.0f | %s | %s | %s | %s / %s | %s | %s | %s | %s |" % (mid, r.get("name", ""), ref["kind"], ref["obw_hz"], fmt(th.get("awgn")),
              fmt(th.get("cfoclk")), fmt(th.get("offcentre")), r.get("false_units", "-"), r.get("false_trials", "-"), theo, gap, had, "; ".join(notes)))

if __name__ == "__main__":
    main()
