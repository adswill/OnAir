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

## Phase 2 (measured on this Mac: Apple M2 Pro, Metal)

Timings come from `test_gpu_atsc3`, `test_gpu_fft`, `test_viterbi_gpu` (ms per call on this machine, one run, not a benchmark suite). The CPU
column is one thread, the way that decoder runs inside its mode today.

### ATSC 3.0 LDPC (Type B codes, rates 6/15 to 13/15) on the GPU

`Bicm::decodeBatch` sends a batch of words to `GpuLdpc::decode` (the `atsc3::qcCode` graph, normalised min-sum with the two-minimum correction), and
every word the GPU does not converge on goes through the CPU decoder, so the output is never worse than the CPU path. Type A codes (rates 2/15 to
5/15) have no `LdpcCode` graph and stay on the CPU. Convergence matched the CPU on every code tested (the table counts words that decode to the sent word).

| code | CPU ms/word | GPU 1 word | GPU 4 words | GPU 16 words |
|---|---|---|---|---|
| 16200, 6/15 | 1.25 | 10.13 | 2.89 | 0.83 |
| 16200, 8/15 | 0.66 | 3.79 | 0.92 | 0.24 |
| 16200, 10/15 | 0.65 | 3.75 | 1.07 | 0.36 |
| 16200, 11/15 | 1.19 | 9.31 | 2.28 | 0.54 |
| 64800, 6/15 | 5.65 | 11.09 | 4.02 | 2.43 |
| 64800, 8/15 | 2.61 | 6.61 | 2.38 | 0.70 |
| 64800, 10/15 | 3.45 | 8.56 | 2.91 | 0.89 |
| 64800, 11/15 | 4.91 | 13.94 | 4.63 | 1.23 |

GPU wins only in batches of about 16 words (2 to 5 times faster per word here); at 1 to 4 words it is slower. So **Auto uses the GPU from 12 words
per batch, forced GPU always, CPU below that**. Rates 12/15 and 13/15 did not decode on either side at the noise level used (no comparison made for them).
Not measured: the whole ATSC 3.0 receiver with the GPU on, on a real capture.

### ISDB-T Viterbi

`isdbt_demod.cpp` can call `GpuViterbi` (same K=7 exact decoder as the CPU path: identical bits and path metric on every block, `viterbi_gpu` test),
and a failed GPU call falls back to the CPU decoder. Measured on a 387072-step block: CPU 1.8 to 2.7 ms (4 threads, NEON), GPU 12.5 to 18 ms; on 129919
steps CPU 0.78 ms, GPU 10.7 ms. **The GPU is 5 to 14 times slower here, so Auto stays on the CPU and the GPU runs only when COMPUTE is forced to GPU.**
The same holds for the DVB-T Viterbi (already experimental, GPU-only-when-chosen). Not measured: a full ISDB-T frame in the receiver.

### Generic FFT / FIR kernel (Metal only)

`GpuFft` (`core/src/dsp/gpu_fft.mm`): batched radix-2 Stockham FFT, windowed power accumulate, FIR decimator. Vulkan and Direct3D 11 versions are **not
written**; elsewhere `GpuFft::available()` is false and the Spectrum row shows "no usable GPU". Max error against the CPU `fftSplit`: below 1e-6 of the peak bin.

| power spectrum | CPU ms | GPU ms |
|---|---|---|
| 4096 x 2 blocks | 0.027 | 0.399 |
| 8192 x 2 | 0.048 | 0.438 |
| 32768 x 2 | 0.312 | 0.700 |
| 65536 x 2 | 0.391 | 0.964 |
| 65536 x 8 | 1.079 | 4.927 |

The CPU transform (NEON) is faster at every size, because a GPU round trip costs about 0.3 ms and the analyser averages only 2 blocks a frame. **Auto
stays on the CPU for the spectrum; the GPU is used only when COMPUTE is forced to GPU** (the "Spectrum" tick), and only for non-overlapped frames. No speedup
is claimed. The FIR decimator beat a scalar loop (decimate by 8, 120000 outputs: 8.7 ms vs 4.5 ms; by 50: 6.2 vs 3.8) but was not compared with the NEON
`GroupDecimator` and is **not wired** into the channelizer or any demodulator.

### Still on the CPU (no GPU stage), and why

ATSC 1.0 (12-state trellis kernel does not exist), ISDB-S (no LDPC code in the tree), DVB-C (RS only), DAB / DRM / HD Radio / Wi-Fi HaLow / hdr / meteor / HRPT
(different codes or punctured short blocks, not the K=7 rate 1/2 kernel contract; not attempted), Turbo modes (no kernel), WSJT-family and voice modes
(blocks of a few hundred bits), LoRa dechirp FFT, GNSS acquisition and wideband FM FFT (would need the FFT kernel wired in, and the table above says it loses at
these sizes). Their ticks stay disabled with the one-line reason.
