# Neural noise remover (Listen)

Removes steady background noise from speech with [RNNoise](https://github.com/xiph/rnnoise) (Xiph.Org, BSD-3-Clause; vendored in
`third_party/rnnoise`, tag v0.1.1, small built-in model). Off by default.

Where: **every mode's sound** (since 3.5): the shared audio cleanup chain (`audio_cleanup.h`, one per audio output, so analog and digital voice
alike: NFM, AM, SSB, DMR, P25, NXDN, D-STAR, dPMR, TETRA, M17, Iridium and the rest) has a **Voice noise remover (neural)** switch with its own
strength, kept per kind of mode (Settings > Audio cleanup; in the simple view for the speech kinds, always in Advanced). It sits after the
spectral noise reduction and before the voice EQ, adds about 20 ms, and is off by default. Test: `tests/audio/test_cleanup_neural.cpp` (noise
between bursts 20 to 27 dB down at 8, 16, 24 and 48 kHz, speech level kept, any chunking, toggling, unsupported rates pass through). Do not turn it on
together with the General receiver's own switch below (it would clean twice). The original places:

**General receiver** (checkbox "Noise remover (neural)" under the audio filters, shown for NFM, WFM and AM; the choice and the strength are
remembered per demodulator, so switching to USB, LSB or CW never has it on) and **Airband** (the mixed audio). It is not offered for
SSB, CW, music or data modes: it treats anything that is not speech as noise.

Strength 0 to 100 % blends the original (0) and the cleaned audio (100); RNNoise itself has no strength control.

How it works (`core/src/audio/neural_nr.{h,cpp}`): the demodulated audio (8, 12, 16, 24 or 48 kHz; any rate that divides 48000) is brought to
48 kHz with a Kaiser low-pass, cut into 10 ms frames of 480 samples, run through RNNoise and brought back. Output has as many samples as the input,
delayed by `latency()` samples (about 20 ms: 176 at 8 kHz, 959 at 48 kHz); the original is delayed the same, so the blend stays aligned.
The squelch and the recordings are on the cleaned audio, so a recording has the same 20 ms of delay. All memory is allocated when the receiver is
made; `process()` never allocates. Off, the samples are not touched at all (bit-identical, no CPU). Strength changes are ramped across a block.
`setEnabled()`, `setStrength()` and `vad()` are safe from any thread.

Tests: `tests/audio/test_neural_nr.cpp` (off = bit-identical, strength 0 = delayed input exactly, noise between speech-like bursts 25 dB or more
down, SNR improvement, latency, block-size independence, 8 / 12 / 16 / 24 / 48 kHz) and a block in `tests/general/test_general_rx.cpp`.
