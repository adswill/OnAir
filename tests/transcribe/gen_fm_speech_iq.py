#!/usr/bin/env python3
"""Broadcast-FM test signal with real English speech, as SigMF (cf32). Used by e2e_fm_transcript.sh.
usage: gen_fm_speech_iq.py speech.wav out_base [snr_db=25] [freq_mhz=93.9]
75 kHz deviation, 50 us pre-emphasis, 15 kHz audio bandwidth, 1.2 Msps, white noise added at snr_db over the 200 kHz channel."""
import sys, json, wave
import numpy as np
from scipy.signal import resample_poly, butter, sosfilt

wav, base = sys.argv[1], sys.argv[2]
snr = float(sys.argv[3]) if len(sys.argv) > 3 else 25.0
fmhz = float(sys.argv[4]) if len(sys.argv) > 4 else 93.9
w = wave.open(wav)
sr, n = w.getframerate(), w.getnframes()
x = np.frombuffer(w.readframes(n), dtype='<i2').astype(np.float32) / 32768.0
if w.getnchannels() > 1: x = x.reshape(-1, w.getnchannels()).mean(axis=1)
fs = 1.2e6
x = resample_poly(x, 24000, sr) if sr != 24000 else x          # 24 kHz first
x = resample_poly(x, 50, 1)                                      # 1.2 MHz
sos = butter(6, 15000, fs=fs, output='sos'); x = sosfilt(sos, x)
tau = 50e-6
# pre-emphasis: y = x + tau * dx/dt
y = x + tau * fs * np.concatenate([[0], np.diff(x)])
y = y / (np.percentile(np.abs(y), 99.5) + 1e-9) * 0.8            # loud speech peaks at 80 % of the deviation
sil = np.zeros(int(fs * 1.0), dtype=np.float32)
m = np.concatenate([sil, y, sil]).astype(np.float64)
ph = 2 * np.pi * 75000.0 * np.cumsum(m) / fs
iq = np.exp(1j * ph)
# noise in the full 1.2 MHz band so that the SNR in 200 kHz is snr_db
sig_p = 1.0
npow = sig_p / (10 ** (snr / 10)) * (fs / 200e3)
noise = (np.random.default_rng(3).standard_normal(len(iq)) + 1j * np.random.default_rng(4).standard_normal(len(iq))) * np.sqrt(npow / 2)
iq = (np.exp(1j * ph) + noise).astype(np.complex64)
iq = iq / np.max(np.abs(iq)) * 0.9
iq.tofile(base + '.sigmf-data')
json.dump({"global": {"core:datatype": "cf32_le", "core:sample_rate": fs, "core:version": "1.0.0", "core:description": "FM speech test"},
           "captures": [{"core:sample_start": 0, "core:frequency": fmhz * 1e6}], "annotations": []}, open(base + '.sigmf-meta', 'w'))
print("wrote", base, len(iq) / fs, "s")
