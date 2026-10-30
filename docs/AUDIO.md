# Audio control

One "Audio" pop-up (the wave button next to the volume, in every mode) holds everything that shapes the sound you hear. The decoders always get
the untouched sound; only the speaker (and the recorder, see below) hears the chain. Everything is off or transparent by default: a mode you
have not touched sounds as before. Code: `core/include/dect2/audio/audio_control.h`, `core/src/audio/audio_control.cpp`, UI
`app/panels/audio_control_ui.cpp`, tests `tests/audio/test_audio_control.cpp`.

## What is in it

| Area | Controls |
|---|---|
| Volume | per mode (remembered under the mode, so FM and airband keep their own level), mute, `Alt+Up` / `Alt+Down` volume, `Alt+M` mute |
| Squelch | off / level (dBFS) / SNR (speech band over the high-band noise, so it follows noise, not level) / CTCSS (50 standard tones) / DCS (octal code); open threshold, hysteresis (closes lower than it opens), hang time, tail elimination (the sound runs behind by up to 300 ms so the noise burst at the end of a transmission is cut before it is heard) |
| Filters | high-pass and low-pass, cut-off sliders, 12 or 24 dB/octave; the right audio bandwidth is pre-set per kind of mode (AM 100-5000, NFM 300-3000, SSB 300-2700, CW 400-900, digital voice 300-3400 Hz) |
| EQ | 5 bands (120, 400, 1k, 2.5k, 6k Hz; low and high shelf), presets Flat / Voice / Music / CW / Custom |
| Notch | manual (centre and Q), automatic (an adaptive line enhancer that learns a steady whistle and cuts it, following drift); click the AF spectrum to put the manual notch there |
| AGC | off / fast / slow, target level; it never boosts the noise between transmissions |
| De-emphasis | off / 50 us / 75 us (only for modes that do not de-emphasise themselves) |
| Stereo | stereo / mono / swap / left only / right only |
| Clarifier | -1000..+1000 Hz shift (Hilbert-transform single-sideband shifter); CW tone pitch 300-1200 Hz (receivers make 700 Hz) |
| Meters | peak + RMS bar of what is played, CLIP light (red for 2 s after a sample at full scale); AF spectrum and waterfall strip |
| Output | device per mode (falls back to the system default when the device has gone), latency slider |
| Recorder | "Record audio (WAV)" next to the volume; "only while the squelch is open" writes nothing while it is closed |
| Receivers | with several receivers: solo (that receiver's sound plays, the others are muted) and mute |
| Reset | "Reset audio to defaults" puts the mode's chain back (volume and device are kept) |

Noise reduction (spectral gating, off by default) is the existing Audio cleanup chain (`audio_cleanup.h`, the button beside this one); it runs
before this chain. The noise blanker of the spectrum work acts on the IQ; its audio-side switch has a slot at the start of the chain
(`kSlotBlanker` in the header comment) and is not wired here.

## Signal path

`AudioOut::write()`: voice inversion -> Audio cleanup -> **Audio control** -> ring -> device (the volume slider is applied by the device
callback). Order inside Audio control: stereo -> high/low-pass -> notch -> auto notch -> clarifier -> EQ -> de-emphasis -> AGC -> squelch gate
(+ tail delay) -> meters. The squelch decision is measured on the sound before the filters. The chain is cross-faded in and out over 10 ms (no
clicks), and returns the input pointer itself while off.

## Design notes

- All blocks are plain functions or small structs (`BiquadCoef`/`bqRun`, `eqDesign`, `SquelchCore`, `ToneDetector`, `DcsDetector`, `Agc`, `Meter`,
  `spectrumDb`) with no allocation or locking in `process()`. Float, per-channel loops, no virtual calls: they vectorise and cost well under 1 %
  of a core at 48 kHz.
- Settings are one string per mode in the preferences (`acl.<family>`, `audioControlToString` / `audioControlFromString`; unknown keys are
  ignored and values are clamped, so an old or damaged setting cannot break the chain). The audio thread reads a copy when the generation
  counter changes.
- CTCSS: Goertzel at the tone against the tones 12 Hz either side, 0.2 s blocks. When tone squelch is chosen a 260 Hz high-pass removes the
  tone from the sound you hear.
- DCS: the 23-bit Golay(23,12) word (g(x) = x^11+x^10+x^6+x^5+x^4+x^2+1, 9 code bits, marker 1,0,0) at 134.4 bit/s, sliced at four bit-clock
  phases, any rotation and either polarity, 24 consecutive matching bits. It is tested against our own encoder and noise only; no on-air DCS
  recording was available, so treat DCS as unverified until tried on a real repeater.
- Auto notch: NLMS adaptive line enhancer (32 taps, 24 samples decorrelation delay) on the mid signal. Steady tones are predictable and
  removed (over 20 dB in the test); speech changes faster than it adapts and passes, voiced vowels lose a little.

## Known limits

- The output sample rate is the mode's own (48 kHz); the system converts it. There is no rate picker.
- Latency applies to outputs that do not set their own start threshold after `start()`; the device applies to the next start of a mode's sound.
- Mute/solo is per receiver (the heard receiver plays, the others are muted), not a free mixer.
- The AF waterfall/spectrum is computed on the audio thread only while the pop-up is open.

## Tests

`ctest -R audio_control`: filter corner and slope, notch depth (< -60 dB) and width, EQ band gains and presets, squelch hysteresis and hang,
tail elimination, SNR and level squelch in the chain, CTCSS and DCS detection (and rejection of other tones/codes and noise), AGC level and
noise-floor hold, auto notch depth and noise kept, clarifier and CW pitch moving a tone, stereo modes, settings round trip and clamping,
device fallback, spectrum peak, meter and clip count, pass-through when off.

## Several channels at once (multi-VFO)

Extra channels on one capture, each with its own frequency, mode, squelch, volume and pan; the open ones are mixed to the sound output, pager and APRS channels decode in parallel. See docs/MULTI-VFO.md.
