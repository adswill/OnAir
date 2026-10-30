# Signal Lab

Tools > Signal Lab works out an unknown digital signal without knowing anything about it in advance. It runs six stages, each one a
pure function with its own unit test (`core/src/signallab`, `tests/signallab`), and shows what every stage found and how sure it is.
Everything runs locally. Nothing is decrypted: encrypted or compressed data shows up as high-entropy bits.

It sits beside the older Protocol lab (Tools > Protocol lab), which tries a fixed set of demodulators on recordings. Signal Lab
starts from the spectrum selection, classifies the modulation with confidences, recovers symbols with loops, tests line codes and
scramblers, has a bit viewer and exports a description for Mode Builder.

## Input

- **Spectrum selection.** Select the signal with the Identify tool (key I) or Shift+drag across it on the spectrum. While the
  Signal Lab window is open, the Identify capture keeps its samples (`sigid::Capture::keepSamples`, at most 4 M samples), and the lab
  analyses them: "Analyse the last selection", or "Analyse each new selection" to follow along.
- **Recording.** An IQ file (cf32, cs16, cu8, cs8), its sample rate and, optionally, the signal's offset and width inside it.
- **Bits.** Frames or streams you already have, one per line, as 0/1 or hex: stages 4 to 6 only.
- **Test signal.** A made-up signal from the evaluation generator, with what was sent shown next to what was found.
- **Saved analysis.** A file written by Export > Save the analysis.

Options: force the modulation family, order or symbol rate, and the shortest pause that separates two bursts.

## The stages

1. **Channel** (`channel.cpp`). Bursts from the smoothed power (hysteresis between the noise floor and the strong level; short gaps
   between long ones are merged, so OOK zeros do not split frames). Welch spectrum of the active samples: the occupied region (wide
   smoothing, crossing the dip between FSK tones), its 99 % power bandwidth, -3 dB width, centre (coarse carrier offset), SNR, duty
   cycle and the burst period. The signal is then mixed to 0 Hz, filtered and decimated to about 4 times its width.
2. **Modulation** (`classify.cpp`). Envelope statistics (variation, kurtosis, the share of samples near zero, a two-level fit),
   power-law spectral lines of x^1, x^2, x^4, x^8 (a phase-only form for the high powers), k-means on the instantaneous frequency for
   2/4/8 levels (well populated, evenly spaced, and the split must pay off), a cyclic-prefix autocorrelation and the spectral flatness
   (OFDM). Families: OOK, ASK, FSK-2/4/8, MSK/GMSK (FSK with index about 0.5), PSK-2/4/8, 16-QAM, OFDM, with a spread-spectrum hint
   (noise-like and flat, or a chip stream that repeats every 11, 13, 15, 31, 63, ... chips). Each gets a score; the scores become
   confidences. The best few go on to stage 3 and the one whose frames make the most sense wins.
3. **Symbol rate and symbols** (`rate.cpp`, `recover.cpp`). Four estimators on the "symbol signal" (the FM discriminator for FSK,
   the envelope for OOK, the carrier-locked I and Q rails for PSK): cyclostationary line of |x|^2, spectral line of the squared
   derivative, a comb fit to the zero-crossing instants, and the peak spacing of the transition autocorrelation. Rates within 2 %
   vote together, harmonics support the fundamental. Symbols: a matched filter (moving average, or root-raised cosine for PSK/QAM),
   a Gardner timing loop with cubic interpolation set to the estimated rate, and for PSK/QAM an x^M carrier estimate plus a
   decision-directed second-order PLL. The slicer gives levels (k-means) or constellation points, with EVM and timing jitter. The
   amplitude rings tell PSK from QAM, the phase occupancy tells QPSK from 8PSK. Phase ambiguity: every burst is turned to agree
   with the first, then each overall rotation is tried.
4. **Line code** (`linecode.cpp`). NRZ, NRZI (differential), Manchester, differential Manchester, Miller, each with an optional
   self-synchronising descrambler (G3RUH x^17+x^12+1, V.22bis, V.29, 802.11b x^7+x^4+1). Two-chip codes need valid chip pairs (each
   burst picks its chip phase). Additive whitening (PN9 x^9+x^5+1, PN15, PN7, 802.11a, CCSDS) is tried per frame from the frame
   start with the register all ones. Polarity and bit order are settled by the checksum search.
5. **Frames** (`frame.cpp`, `checksum.cpp`). Bursts: the end of the preamble (alternating, a constant run after decoding, or any
   period up to 8), then the bits all bursts agree on are the sync word. Where the sync word really starts and ends is uncertain (a
   periodic preamble can take its first bits, a constant header can follow it), so variants are kept and the checksum search picks
   the one it confirms. Continuous streams: the commonest non-periodic 16-bit window, grown to what its occurrences share, plus the
   bit-stream autocorrelation for the period. The checksum search tries the catalogue (CRC-8 x6, CRC-16 x12, CRC-32 x5, Fletcher-16,
   Sum-8, XOR-8, Sum-16; each checked against its "123456789" value) over every coverage start, every checksum position, with the sync
   word in front or not, both bit orders, both byte orders, both polarities, and for variable lengths at the end of each frame. A hit
   needs most frames to agree, the covered data to vary in enough bits, and to hold at one position only; a hit that only exists
   because the decoder's polynomial is a multiple of the CRC's is dropped.
6. **Fields** (`fields.cpp`). Per-bit-column statistics over the aligned frames (share of ones, entropy, how often it changes from
   one frame to the next). Suggested fields, byte by byte: constant runs, counters (8/16 bit, either bit order), ASCII text,
   few-valued enums, values, the checksum. Interpretations of any bit range as hex, unsigned / signed decimal (MSB or LSB first),
   ASCII and grouped bits, and the XOR of two frames.

## The window

Tabs: Summary (the stage log), 1 Channel (bursts on a time line), 2 Modulation (ranked families with confidences, the features, the
constellation or the slicer's soft values), 3 Symbol rate (each method), 4 Line code (every decoding tried; Use one, invert, or add a
descrambler), 5 Frames (sync word candidates, re-frame with your own sync word and length, the checksum hits), 6 Bit viewer, Export
and Evaluation.

The bit viewer shows the frames aligned in a grid. Above them: the suggested fields as a coloured band and a heat map of how often
each bit changes (or its entropy). Click or drag across bits of a frame to read them, see the same bits in every frame, and make
them a named field. "Diff two frames" marks the bits that differ. The field list can be renamed and retyped. Notes are saved with the
analysis and go into the Mode Builder description.

## Evaluation

`test_signallab evaluate N [seed]` (and the Evaluation tab) generates N random "unknown" signals: FSK-2/4, GMSK, OOK, BPSK, QPSK,
8PSK; 48 kHz to 1 MHz capture rates, 8 to 32 samples per symbol, carrier offsets, 14 to 28 dB SNR (in a symbol-rate bandwidth),
±50 ppm symbol clock error; NRZ, NRZI, Manchester, differential Manchester and Miller; G3RUH or PN9 scrambling; 24 to 64 bit
preambles, 16 or 32 bit sync words, 6 to 24 byte payloads with a counter and an ID byte, a random catalogue checksum (85 %), 6 to 14
frames in bursts or back to back. It reports, per stage, how often the truth came back: bandwidth, carrier offset, bursts,
modulation family and order, symbol rate (within 1 %), line code (and scrambler), sync word, frame length, frames recovered (80 % of
the payloads bit-exact), checksum algorithm, and no false checksum where none was sent. The ctest `signallab_eval` (label slow) runs
a fixed set of 36 and fails when a stage drops below its floor. On 80 signals (seed 500) the lab reached:

| stage | seed 500, 80 signals | gate set, 36 signals |
|---|---|---|
| bandwidth | 99 % | 97 % |
| carrier offset | 100 % | 97 % |
| bursts | 89 % | 92 % |
| modulation family | 90 % | 92 % |
| modulation order | 90 % | 92 % |
| symbol rate (1 %) | 96 % | 94 % |
| line code + scrambler | 73 % | 78 % |
| sync word | 61 % | 75 % |
| frame length | 74 % | 83 % |
| frames recovered | 60 % | 72 % |
| checksum algorithm | 79 % | 87 % |
| no false checksum | 85 % | 100 % |

The weak spots: without a checksum NRZ and NRZI (and Manchester and differential Manchester) cannot be told apart from the bits, and
where a sync word ends inside a constant header stays a guess; low-SNR wide FSK at 1 MHz capture rates; periodic 4-FSK preambles that
swallow sync bits.

## The exchange format (Mode Builder)

Export > "Mode Builder description" writes a JSON object. Mode Builder (branch mode-builder-030) reads it to make a decoder; any
other tool may as well. Fields not known are `null` or 0. Version 1:

```json
{
  "format": "onair-signal-description",
  "version": 1,
  "name": "unknown signal",
  "source": "signal-lab",
  "modulation": {
    "family": "FSK",              // OOK, ASK, FSK, MSK, GMSK, PSK, QAM, OFDM, unknown
    "order": 2,                   // levels / phases / points
    "symbol_rate": 9600.0,        // Bd (symbols per second on the air: chips for Manchester / Miller)
    "bandwidth_hz": 20264.0,      // 99 % power bandwidth
    "carrier_offset_hz": 25009.0, // from the capture's centre
    "deviation_hz": 4246.0,       // FSK / MSK / GMSK only: peak deviation
    "tone_spacing_hz": 8491.0,    //   adjacent tone spacing
    "modulation_index": 0.85,     //   tone spacing / symbol rate
    "gray": true,                 // symbol index to bits: Gray (PSK/QAM always; FSK level 0 = lowest frequency)
    "confidence": 0.6,            // of the family, 0..1
    "rate_confidence": 0.92
  },
  "bursts": {"continuous": false, "duty_cycle": 0.57, "period_s": 0.0248, "mean_length_s": 0.014},
  "line_code": "nrz",             // nrz, nrzi, differential, manchester, manchester-inverted, differential-manchester, miller
  "polarity": "normal",           // or "inverted": invert the decoded bits
  "scrambler": null,              // or {"name", "taps", "degree", "type": "multiplicative"|"additive", "seed", "applies_to": "stream"|"frame"}
  "bit_order": "msb-first",       // inside bytes, for the fields and the checksum
  "preamble": {"bits": 32, "pattern": "10"},
  "sync_word": {"bits": 16, "hex": "2DD4", "binary": "0010110111010100", "max_errors": 1},
  "frame": {"length_bits": 96, "variable_length": false, "counts_from": "after-sync"},
  "fields": [
    {"name": "counter", "start_bit": 0, "length_bits": 8, "type": "counter", "example": "1B"},
    {"name": "CRC-16/IBM-3740 (CCITT-FALSE)", "start_bit": 80, "length_bits": 16, "type": "checksum", "example": "A3 0F"}
  ],
  "checksum": {
    "algorithm": "CRC-16/IBM-3740 (CCITT-FALSE)", "kind": "crc", "width": 16,
    "poly": "0x1021", "init": "0xFFFF", "refin": false, "refout": false, "xorout": "0x0000",
    "covers_sync": false, "covers_from_bit": 0, "from_end": false, "position_bit": 80,
    "byte_order": "big-endian", "bit_order": "msb-first", "inverted": false, "matched": 8, "tested": 8
  },
  "notes": "free text"
}
```

(The comments are for this page only; the file is plain JSON.)

Rules for a reader:

- Bit positions (`start_bit`, `covers_from_bit`, `position_bit`) count from the first bit after the sync word, except
  `covers_from_bit` when `covers_sync` is true: then it counts from the first bit of the sync word.
- Decoding order: slice symbols, symbols to bits (Gray, MSB of each symbol first), line code, a multiplicative descrambler over the
  stream, polarity, find the sync word (allowing `max_errors`), take `length_bits` (or up to the end of the burst when
  `variable_length`), an additive descrambler over the frame from its start with the register at `seed`, then the fields.
- The scrambler taps: bit k set means x^(k+1); the x^0 term is implied. Fibonacci form: the output bit is the parity of
  register & taps, the register shifts left taking the received bit (multiplicative) or the output bit (additive).
- `checksum.kind` is crc (Rocksoft model as above), fletcher16, sum8, xor8 or sum16; the stored value is `width` bits at
  `position_bit` (or `tail_bits` before the frame end when `from_end`), in `byte_order`, bytes packed in `bit_order`.
- `type` of a field: constant, counter, checksum, ascii, enum, uint, bytes.

The saved analysis (Export > Save the analysis) is the same object with an extra `"analysis"` member: `sample_rate`, the `frames`
and decoded `streams` as 0/1 strings, and the stage `log`. Signal Lab loads it back; Mode Builder can ignore it.

Other exports: frames as CSV (one row per frame: index, burst, start bit, length, hex, then one column per field) and a hex dump.

## Files

- `core/include/dect2/signallab/signallab.h`: the API, one section per stage.
- `core/src/signallab/`: dsp, channel, classify, rate, recover, linecode, frame, checksum, fields, pipeline, export, gen (generator and
  evaluation).
- `app/panels/signallab_ui.cpp`: the window.
- `tests/signallab/test_signallab.cpp`: unit tests per stage (`quick`), the gate (`gate`), the harness (`evaluate N seed`), one random
  case with its log (`case N`).

## LoRa demodulator

Tab "LoRa demodulator" (source: the Recording tab's file or the last spectrum selection). Settings: spreading factor 5 to 12, bandwidth 7.8 to 500 kHz
(or any value), coding rate 4/5 to 4/8 (implicit header only: an explicit header carries it), sync word, low data rate optimisation (automatic, off, on),
implicit or explicit header with payload length and CRC, inverted IQ (the samples are conjugated), preamble length and the signal offset. **Auto-detect
parameters** searches every bandwidth that fits the sample rate (narrowed by the measured signal width) and SF7 to SF12 with the usual sync words;
combinations that heard a preamble but no frame are searched again with all 256 sync words; if nothing is found the inverted IQ sense is tried. It reports
SF, bandwidth, coding rate, sync word, CRC, LDRO and IQ sense, fills the settings in and demodulates. Limits: it needs an explicit header (an implicit header
frame has no coding rate to read), only the first 12 s are searched, and SF5/SF6 are not searched automatically. Code: `core/src/loraraw/loraraw_lab.cpp`
(on the mesh receiver's Channelizer and Demod), test `test_loraraw_lab`.
