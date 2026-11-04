# GPU audit for 3.7

Which decoder stages could run on the GPU, and which already do. Built from the sources under `core/src/<mode>/` (a stage is listed when the
mode's code contains it) and the GPU code in `core/src/dsp/gpu_*`. Nothing here is a speed claim: no stage below was newly measured in
phase 1.

## What exists today

| GPU code | Backends | Used by |
|---|---|---|
| `GpuLdpc::decode` (DVB-T2 / DVB-S2 / ATSC-style dense LDPC, flooding min-sum) | Metal, Direct3D 11, Vulkan | DVB-T2 (`t2plp.cpp`), DVB-S2 (`dvbs_s2rx.cpp`) |
| `GpuLdpc::decodeQc` (generic quasi-cyclic table kernel) | Metal, Vulkan | DTMB (`dtmb_chain.cpp`) |
| `GpuViterbi::decode` (K=7, rate 1/2 mother code) | Metal, Direct3D 11 | DVB-S (`dvbs_s1.cpp`), DVB-T (`dvbt_fec.cpp`, only when GPU is chosen) |
| `conceal_d3d11` (error concealment) | Direct3D 11 | TV video, not a decoder stage |

FFT, FIR / decimation, channelizer and correlators: **no GPU code anywhere** (phase 2).

## Per mode

Stages are what the mode's sources contain (grep of `ldpc`, `viterbi`/`trellis`, `turbo`, `fft`, `decimat`/`fir`). "GPU today" is what the
build really calls. "Tick" says what Settings shows: **active** = the tick works, **disabled** = a disabled tick with the reason.

| Mode (mode list) | FEC stages | Other heavy stages | GPU today | Could run on GPU | Tick |
|---|---|---|---|---|---|
| DVB-T2 (in "DVB") | LDPC (+BCH) | FFT 1k-32k, equaliser | LDPC | FFT / channelizer | active |
| DVB-T (in "DVB") | Viterbi, RS | FFT 2k/8k | Viterbi (only when GPU is picked, experimental) | FFT | active |
| DVB-S / S2 / S2X | LDPC (S2), Viterbi (S) | matched filter, decimation | LDPC, Viterbi | FIR decimation | active |
| DTMB | LDPC (QC) | FFT, FIR | LDPC (Metal only) | FFT | active |
| ATSC 3.0 | LDPC (long frames) | FFT 8k-32k | none | LDPC via the dense kernel (same family as T2), FFT | disabled: no GPU kernel wired yet |
| ATSC 1.0 | Viterbi (trellis 2/3), RS | FFT, FIR | none | Viterbi needs a 12-state trellis kernel | disabled |
| ISDB-T | Viterbi, RS | FFT 2k/4k/8k | none | Viterbi (same K=7 kernel), FFT | disabled |
| ISDB-S (BS) | Viterbi, LDPC (modern slot) | FIR | none | Viterbi (K=7), LDPC | disabled |
| DVB-C | RS only | FFT-free, FIR | none | nothing large | disabled |
| DAB / DAB+ | Viterbi (punctured), RS | FFT 2k | none | Viterbi (K=7), FFT | disabled |
| DRM / DRM+ | Viterbi, RS | FFT | none | Viterbi, FFT | disabled |
| HD Radio | Viterbi | FFT | none | Viterbi, FFT | disabled |
| CDR | LDPC, Viterbi | FFT | none | both | disabled |
| MAC, Analog TV, FPV video, TV auto, IPTV, TV sat beacon | none | FFT, FIR | none | FIR / FFT | disabled: nothing GPU-worthy in the decoder |
| FM, AM, AM stereo, Shortwave, General receiver | none | FFT, FIR | none | channelizer / FFT (wideband FM, spectrum) | disabled |
| Spectrum analyser / explorer, Signal workbench | none | FFT | none | FFT | disabled |
| Wi-Fi HaLow | LDPC, Viterbi | FFT | none | LDPC (QC kernel), Viterbi | disabled |
| Wi-Fi networks, 5G NR, Cell towers, NB-IoT | Viterbi / Turbo | FFT | none | Turbo needs a new kernel | disabled |
| DJI drones | Viterbi, Turbo | FFT | none | Turbo | disabled |
| JS8Call, FT8, FT4, FT2, WSPR, JT65/JT9/Q65, FST4, MSK144 | LDPC (174,91), Turbo-like convolutional (WSPR) | FFT | none | tiny codes (CPU is faster than a GPU round trip) | disabled |
| HF digital (hfdig: ALE, PACTOR, Olivia, MFSK, THOR, ...) | Viterbi, LDPC (few) | FFT | none | not worth it | disabled |
| TETRA, TETRAPOL, DMR, P25, NXDN, dPMR, D-STAR, Yaesu, M17 | Viterbi (short blocks) | FIR | none | blocks are a few hundred bits: CPU faster | disabled |
| GSM-R, DECT, Bluetooth LE, Zigbee, Z-Wave, LoRa, LoRaWAN, Meshtastic | Viterbi / none | FFT | none | LoRa dechirp FFT | disabled |
| Meteor-M LRPT, FY-3, HRPT/AHRPT, GOES, Elektro-L, GK-2A, FY-2/4, CubeSat | Viterbi, RS | FIR, FFT | none | Viterbi (K=7), FIR | disabled |
| GNSS, SBAS | Viterbi (SBAS) | FFT acquisition, FIR | none | acquisition FFT (large win possible) | disabled |
| Iridium, Inmarsat, Globalstar, Orbcomm, QO-100, Argos, SARSAT | Viterbi (some) | FFT, FIR | none | FFT / channelizer | disabled |
| ADS-B, Mode S, UAT, ACARS, VDL2/4, HFDL, AIS, VDES, DSC, Airband | Viterbi (HFDL, aero) or none | FIR | none | nothing large | disabled |
| SSDV, Radiosonde, Time signals, Radio astronomy | LDPC (SSDV) / none | FFT | none | not worth it | disabled |
| KAAL, KAFT, KAIT, KAPP, KAVB, KAWS, Kris modes, Mode Builder | LDPC, Viterbi (own codes) | FFT, FIR | none | LDPC (QC kernel) | disabled |
| Pagers (POCSAG, FLEX), tones, CW, RTTY, SSTV, PSK, APRS, trunking (EDACS, LTR, MPT1327) | none | FIR | none | nothing large | disabled |
| Plugins | n/a | n/a | none | n/a | disabled |

## What phase 1 changes

- Settings > Decoding: **Use GPU where available** (default on) and **GPU per mode** (override per mode list row). Stored as `gpuGlobal` and
  `gpuModes` (`DVB=0;DTMB=1`).
- Default changes nothing: the existing COMPUTE choice (CPU / GPU / Auto, default Auto) still decides; the tick only forces the CPU path of
  that decoder when it is off. The CPU path is the same code the GPU kernels are tested against (`gpu_ldpc_exact`, `viterbi_gpu`, `dtmb_gpu`).
- Modes without a GPU stage show the tick disabled with a one-line reason (list under "Modes with no GPU stage"): there is no fake tick.
- The same tick sits in the compute row of DVB (toolbar COMPUTE drop-down), DVB-S/S2 and DTMB.
- Diagnostics: each GPU mode shows `<mode> <stage>: GPU|CPU (reason)` in Settings, taken from the decoder's own report of where the last
  block ran.

## Phase 2 candidates (not started)

A generic FIR decimator / channelizer plus FFT kernel is the one piece that would reach most modes (DVB-T/T2, DAB, ATSC 3.0, ISDB-T, wideband
FM, spectrum). It only counts if it is bit-exact within float tolerance and faster than the NEON CPU path on this Mac, measured. The ATSC 3.0
LDPC could reuse the dense T2 kernel only if its tables map onto it, which needs checking.
