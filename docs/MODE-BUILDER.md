# Mode Builder

Tools > Mode Builder is a decoder you build without writing code. You describe a digital protocol (modulation, line code, sync word,
frame length, error correction, check value and fields) and OnAir runs a working receiver from that description: on a signal on the
spectrum (register the definition as a mode, it then appears in the mode list under **CUSTOM MODES**) or on an IQ recording
("Test on recording").

The description is one JSON document, the **mode definition**. The same JSON is written by the Signal Lab (it exports what it
worked out of a signal) and read by the Mode Builder, so a definition can go from one to the other without changes. This file is
the reference for that format. Everything is local: nothing is sent anywhere.

Code: `core/include/dect2/modebuilder/modebuilder.h`, `core/src/modebuilder` (library `dect2_modebuilder`), the window
`app/panels/modebuilder_ui.cpp`, the examples `data/modes/*.json` (compiled in), tests `tests/modebuilder/test_modebuilder.cpp`.

## Using it

1. Open Tools > Mode Builder. Pick one of the **Examples** (or *New*, *Saved*, *Open file*), or paste a definition. The Protocol lab's
   Export tab has **Open in Mode Builder**, which turns what the lab worked out of a recording into a definition to finish here.
2. Edit it in the **Form** tab or in the **JSON** tab. The JSON is checked as you type: every problem is listed with the line or the
   key it is about ("fields[2].width_bits: must be 1..64"). A definition with errors does not run.
3. **Test on recording**: pick an IQ file (cf32, cs16, cu8, cs8), its sample rate and the offset of the signal from the centre of the
   capture. The decoded frames fill the **Frames** table. **Generate test signal** writes (or decodes straight away) a recording made from
   the definition itself, with noise and a frequency offset you choose, so you can see that a definition works before you have a signal.
4. **Save** keeps the definition in the user's data folder (`<data folder>/modes/<id>.json`); **Load** reads one back.
5. **Register as mode** adds it to the mode list (group CUSTOM MODES). Selecting it tunes to `frequency_mhz` and decodes the live
   signal of receiver 1; the Frames table is the mode's main tab. Registered modes are remembered (`<data folder>/modes/registered.txt`).
6. **Export** the frames as CSV (one column per field) or JSON (one object per frame).

## Pipeline

Receive, in order (the generator does the same backwards):

```
IQ -> mix by modulation.offset_hz -> low-pass (filter.bandwidth_hz) and decimate
   -> demodulate (OOK/ASK envelope, FSK/GFSK/GMSK frequency discriminator, PSK carrier loop)
   -> symbol clock recovery (Gardner) -> symbol decisions -> bits (symbol_map / Gray)
   -> line decode (NRZ, NRZI, Manchester, ...) -> sync word search (max_errors, polarity)
   -> frame bits -> descramble -> deinterleave -> FEC decode -> bytes (bit_order)
   -> frame length (fixed / length field / end flag) -> CRC -> fields -> display template
```

Polarity and the phase ambiguity of PSK are resolved by the sync word: with `"polarity": "auto"` every inversion (and every rotation of
the PSK constellation) is tried and the one whose sync word matches is used.

## Format (version 1)

All keys are lower case with underscores. Numbers that are bit patterns (polynomials, seeds, sync words) may be written as a number or
as a string: `"0x2DD4"` (hex), `"0b1011"` (binary), `"1011"` with `"bits"` given (a string of only 0 and 1 is read as bits). Unknown
keys are reported as warnings and ignored, so newer files still load.

```json
{
  "onair_mode": 1,
  "id": "ook-433-sensor",
  "name": "OOK 433 MHz sensor",
  "description": "A simple temperature and humidity sensor",
  "frequency_mhz": 433.92,
  "sample_rate": 250000,
  "modulation": { "type": "OOK", "symbol_rate": 2000, "offset_hz": 0 },
  "filter": { "bandwidth_hz": 20000, "pulse": "none" },
  "line_code": { "type": "Manchester" },
  "polarity": "auto",
  "bit_order": "msb",
  "preamble": { "pattern": "0xAAAA", "bits": 16 },
  "sync": { "word": "0x2DD4", "bits": 16, "max_errors": 1 },
  "scrambler": { "type": "none" },
  "frame": { "length": { "mode": "fixed", "bytes": 7 } },
  "fec": { "type": "none" },
  "interleaver": { "rows": 0, "cols": 0 },
  "crc": { "preset": "CRC-8/SMBUS", "from_byte": 0 },
  "fields": [
    { "name": "id", "offset_bits": 0, "width_bits": 8, "type": "uint", "format": "hex" },
    { "name": "temperature", "offset_bits": 8, "width_bits": 16, "type": "int", "scale": 0.1, "units": "C" }
  ],
  "display": "Sensor {id}: {temperature} {temperature.units}"
}
```

### Top level

| key | type | meaning |
|---|---|---|
| `onair_mode` | int, required | format version, `1`. A newer version is refused with a message. |
| `id` | string | file name and stable id: `a-z 0-9 - _`, at most 48 characters. Made from `name` when missing. |
| `name` | string, required | the name in the mode list. |
| `description` | string | one line under the mode. |
| `frequency_mhz` | number | where the mode tunes when picked from the mode list (default 433.92). |
| `sample_rate` | number | the radio rate the mode asks for (default 250000; at least 4 x the filter bandwidth is used). |
| `polarity` | `normal`, `inverted`, `auto` | inverted swaps 0 and 1 (FSK: the high tone is 0). auto tries both (default `auto`). |
| `bit_order` | `msb`, `lsb` | the order of the bits of each byte on the air (default `msb`). |
| `squelch_db` | number | a burst starts when the level is this far above the noise floor (default 6; `0`: always on, for a signal that never stops). |
| `show_bad_frames` | bool | also list frames whose check value is wrong (default false). |

### modulation

| key | meaning |
|---|---|
| `type` | `OOK`, `ASK`, `FSK`, `GFSK`, `GMSK`, `PSK` |
| `order` | FSK and PSK: 2, 4 or 8 (default 2). GMSK is always 2. OOK is 2; ASK 2 or 4. |
| `symbol_rate` | symbols per second on the air, required (> 0). With a line code this counts the chips: Manchester at 1000 bit/s is 2000 symbols per second. |
| `deviation_hz` | FSK / GFSK: the outer tones are at +- deviation from the centre; the M tones are evenly spaced between. GMSK: symbol_rate / 4, the key is ignored. Default: symbol_rate / 2. |
| `bt` | GFSK / GMSK: Gaussian filter bandwidth-time product (default 0.5; GMSK 0.3). |
| `differential` | PSK: the data is in the phase change from one symbol to the next (DBPSK, DQPSK, D8PSK) (default false). |
| `gray` | 4 and 8 levels: Gray-coded symbols (default true). |
| `symbol_map` | optional list of M integers: the bit value (MSB first, log2 M bits) of each level, from the lowest tone / amplitude (or phase 0, 1 x 360/M, ...) up. Overrides `gray`. |
| `offset_hz` | the signal's offset from the tuned frequency (default 0). |

### filter

| key | meaning |
|---|---|
| `bandwidth_hz` | width of the channel filter before demodulation. Default: FSK 2 x (deviation + symbol rate), other 2 x symbol rate. |
| `pulse` | pulse shape of the transmitter: `none` (rectangular), `rrc` or `rc` (raised cosine, PSK and ASK), `gaussian` (FSK). The receiver uses the matching filter. Default `none`, GFSK / GMSK `gaussian`. |
| `rolloff` | `rrc` / `rc` roll-off (default 0.35). |

### line_code

`type`: `NRZ` (default), `NRZI` (a 0 changes the level, as HDLC; `"toggle_on": 1` for the other way), `Differential` (a 1 changes
the level, NRZ-M), `Manchester` (IEEE 802.3: 0 = high-low... written as chips `10`, 1 = `01`), `Manchester-Thomas` (G.E. Thomas,
the opposite), `Differential-Manchester` (a transition at the start of the bit for 0), `Miller` (delay coding), `4b5b` (FDDI table),
`8b10b` (IEEE 802.3 table, both disparities accepted; the 6b/4b blocks are sent a first).

Line coding applies to everything on the air: preamble, sync word and frame.

### preamble and sync

| key | meaning |
|---|---|
| `preamble.pattern`, `preamble.bits` | what the transmitter sends before the sync word. The receiver does not need it (the sync word finds the frame); the generator sends it. Default 32 bits of `1010...`. |
| `sync.word`, `sync.bits` | the sync word (1..64 bits), required. |
| `sync.max_errors` | bits of the sync word that may be wrong (default 0, at most a quarter of the bits). |

### scrambler

| key | meaning |
|---|---|
| `type` | `none` (default), `additive` (a PN sequence XORed onto the frame bits, synchronised at the sync word), `multiplicative` (self-synchronising) |
| `taps` | the feedback taps, e.g. `[7, 4]`: the sequence is s[n] = s[n-7] xor s[n-4]. Or `poly` as a number: bit t set for tap t (`"0x90"` is the same as `[7, 4]`; bit 0 is ignored). |
| `seed` | the register at the start of each frame (additive; default all ones). Bit k of the seed is s[-1-k]. |

The scrambler works on the bits after the sync word, after FEC encoding and interleaving (it is the last step before the line code).

### frame

`frame.length.mode`:

* `fixed`: `bytes` = the frame length in bytes after the sync word (the decoded length, before FEC), check value included.
* `length_field`: the length is in a field of the frame: `offset_bits`, `width_bits` (big-endian, bit 0 = the first bit of the frame),
  frame bytes = value x `multiplier` (default 1) + `adjust` (default 0), clamped to `min_bytes`..`max_bytes` (required, at most 4096).
* `end_flag`: the frame ends before the bit pattern `flag` (`flag_bits` long), at most `max_bytes`. With `"bit_stuffing": true` a 0 after
  five 1s is removed (HDLC). FEC and the interleaver cannot be used with an end flag.

### fec

| `type` | keys |
|---|---|
| `none` | |
| `repetition` | `n` (2..15): every bit is sent n times, majority vote. |
| `hamming` | `code`: `"7,4"`, `"15,11"`, `"31,26"`, or the extended `"8,4"`, `"16,11"` (one more parity bit: two errors are detected, not corrected). Positional Hamming: parity bits at positions 1, 2, 4, 8 ... of each code word. |
| `convolutional` | `k` constraint length (3..9, default 7), `polys` the generators (2 for rate 1/2, 3 for rate 1/3; default `[0o171, 0o133]` written `["0x79", "0x5B"]`), `tail` (default true: k-1 zero bits end the frame). Hard-decision Viterbi. |
| `reed_solomon` | `n`, `k` (n <= 2^m - 1), `symbol_bits` m (3..8, default 8), `poly` the field polynomial (default 0x11D), `fcr` the first root (default 0), `prim` (default 1). Corrects (n-k)/2 symbols per code word. The frame is split into blocks of k bytes; a shorter last block is a shortened code word. Code words are sent one after the other, data then parity. |

### interleaver

`rows` x `cols` bits (both 0: none). The coded bits are written into the block row by row and sent column by column; a short last
block is sent as it is.

### crc

| key | meaning |
|---|---|
| `preset` | a named model: `CRC-8`, `CRC-8/SMBUS`, `CRC-8/MAXIM`, `CRC-16/CCITT-FALSE` (= `CRC-16/IBM-3740`), `CRC-16/CCITT` (= `CRC-16/KERMIT`, as in the CRC catalogue), `CRC-16/X-25`, `CRC-24/OPENPGP`, `CRC-32/BZIP2`, `CRC-8/AUTOSAR`, `CRC-16/XMODEM`, `CRC-16/IBM` (ARC), `CRC-16/MODBUS`, `CRC-16/DNP`, `CRC-32`, `CRC-32C`, or `SUM8`, `XOR8` (plain checksums). Keys given next to it override the preset's values. |
| `width` | 1..32 (bits) |
| `poly`, `init`, `xorout` | normal (not reflected) form |
| `reflect_in`, `reflect_out` | bool |
| `from_byte` | first byte covered (default 0, the first byte after the sync word) |
| `to_byte` | the byte after the last covered one; negative counts from the CRC (default: up to the CRC) |
| `position` | `end` (default: the last ceil(width/8) bytes of the frame) or a byte offset |
| `byte_order` | `big` (default) or `little` |

Without a `crc` key (or `"preset": "none"`) frames are not checked. Frames with a wrong check value are listed (marked bad) only when
the definition has `"show_bad_frames": true` at the top level.

### fields

A list of objects; bit offsets count from the first bit of the decoded frame (after the sync word, FEC and descrambling; bit 0 is the
most significant bit of byte 0).

| key | meaning |
|---|---|
| `name` | required, `a-z A-Z 0-9 _` (used by the display template and as the CSV column). |
| `offset_bits`, `width_bits` | position and size (1..64; `ascii` and `hex` any multiple of 8 up to the frame). |
| `type` | `uint`, `int` (two's complement), `bool`, `enum`, `bitflags`, `ascii`, `bcd`, `float32` (IEEE, width 32), `lat`, `lon` (signed, scaled; shown as degrees with N/S, E/W), `hex` |
| `endian` | `big` (default) or `little` (byte-aligned, whole bytes) |
| `scale`, `offset` | value = raw x scale + offset (numbers) |
| `units` | shown after the value |
| `enum` | `{ "0": "off", "1": "on" }` (type enum) |
| `flags` | `{ "0": "battery low", "3": "tamper" }` bit numbers from the least significant (type bitflags) |
| `format` | `dec` (default), `hex`, or a decimal count like `"%.2f"` |

### display

`display` is a template string: `{name}` is replaced by the field's shown value, `{name.raw}` the raw number, `{name.units}` its units,
and `{_crc}`, `{_len}`, `{_hex}` the check result, the length and the frame bytes in hex. Or a list of templates chosen by a field:
`[{"when": {"field": "type", "equals": 1}, "template": "..."}, {"template": "default ..."}]`; the first that matches is used.

### walsh, cycle, payload (optional)

- `"walsh": { "k": 3 }` (k = 2 to 6): after the scrambler, every k bits are sent as one of 2^k Walsh-Hadamard code words of 2^k chips; the receiver correlates (fast Walsh transform) and takes the largest. Not with an end flag.
- `"cycle": { "symbols": 400, "flywheel_misses": 3 }`: the frame repeats every N symbols, each cycle starts with the sync word (padded to N). After a good frame the receiver expects the next sync word N symbols later and accepts a damaged one there (up to a third of its bits wrong); up to `flywheel_misses` cycles may pass without a good frame before it searches again. Needs a plain line code (NRZ, NRZI, Manchester types).
- `"payload": { "text": "HELLO" }` or `{ "hex": "DE AD BE EF" }`, optional `"offset_bytes"`: the generator puts this payload in every frame (length field and check value filled in); the receiver shows it as `{_payload}` (the default display when there are no fields).

### OFDM (modulation.type "OFDM")

`"modulation": { "type": "OFDM", "order": 4, "symbol_rate": 1000, "fft_size": 64, "cyclic_prefix": 16, "used_carriers": 48, "pilot_every": 6 }`. `order` 2 = BPSK, 4 = QPSK per subcarrier; `symbol_rate` is the OFDM symbol rate, so the block runs at symbol_rate x (fft_size + cyclic_prefix) samples per second. A frame is two identical training symbols (timing, carrier offset, channel), then data symbols; every n-th used carrier is a pilot that tracks the common phase. No `sync` is needed. Fixed or length-field frames only.

## Examples (data/modes)

* `ook-433-sensor.json`: OOK, Manchester, 2000 chips/s, a temperature / humidity sensor with CRC-8.
* `fsk4-telemetry.json`: 4-FSK 4800 Bd, PN9 whitening, a length field, three frame types with their own display, lat / lon, CRC-16/CCITT-FALSE.
* `pocsag-like.json`: 2-FSK 1200 Bd, inverted, the POCSAG sync word, Hamming(15,11), a BCD message, CRC-16/IBM (not real POCSAG: no BCH code words).
* `gmsk-conv-beacon.json`: GMSK 9600 Bd, K=7 rate 1/2 convolutional code, an 8 x 16 interleaver, CRC-32.
* `bpsk-rs-telemetry.json`: DBPSK 2400 Bd with root raised cosine pulses, an additive scrambler, Reed-Solomon (255,223) shortened, CRC-16/IBM.
* `hdlc-fsk-packet.json`: 2-FSK 9600 Bd, NRZI, HDLC flags with bit stuffing, LSB first, CRC-16/X-25.

## Good preambles

The receiver finds the symbol clock and the levels in the first dozen symbols of a burst, so the preamble should change often:
alternating tones for 2-FSK (`0xAAAA`), the two outer tones for 4- and 8-FSK (`0x22` for Gray-coded 4-FSK, `0x104` for 8-FSK),
the lowest and highest level for 4-ASK (`0x22`), 180 degree steps for PSK (`0xAAAA` for BPSK, `0xFFFF` for DBPSK and DQPSK). A
preamble that keeps one tone or one phase step (a constant rotation) carries no timing. FSK levels are also tracked during the frame.
Frequency errors up to a few percent of the symbol rate (FSK: a fraction of the deviation) are followed.

## Errors

The definition is checked before it runs. Errors stop it, warnings do not:

* JSON syntax: `line 7: expected ',' or '}'`.
* keys: `modulation.symbol_rate: required, a number above 0`; `fec.reed_solomon: k must be less than n`; unknown key `modulaton`:
  warning `unknown key "modulaton"`.
* meaning: a field outside the longest frame, a CRC wider than the frame, a Hamming code name not in the list, FEC with an end flag,
  a sample rate under twice the filter bandwidth.
