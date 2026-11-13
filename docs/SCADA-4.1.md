# SCADA radio telemetry (4.1, family 862)

Receive only. Licensed utility and pipeline data radios (450 MHz, 900 MHz, the US MAS band 928 / 952 MHz) carry Modbus RTU and DNP3 between a
master station and its outstations (RTUs, PLCs) over simple FSK modems. This mode finds the modem by itself, reads the frames, pairs each
poll with its reply and lists the devices with the registers and points they reported. It was approved by the owner as a receive-only mode.

**Not verified on a live signal.** Simulated signals only (the generator in `tests/x40/x40_gen_scada.h`). test_x40_scada passes on Linux and Windows (quick.yml runs 43503179980, 43503184386).

Listening to non-broadcast utility traffic may be restricted in some countries; check local law. The mode has no transmit path and never
offers to send or replay a frame.

It is its own mode-list entry under IoT & Sensors. It uses the x40 receiver as variant 54 (`Scada`): family 862, active standard 863, engine
standard 864, poll slot 862 (`x40::kScadaFamily`, from the free range 857 - 867).

- Code: `core/src/x40/x40_scada.cpp` (physical layer), `core/include/dect2/x40/scada_proto.h` and `core/src/x40/scada_proto.cpp` (frames,
  meaning, pairing, devices, export), `app/x40/scada_ui.cpp` (Messages and Devices tabs, export).
- Tests: `tests/x40/test_x40_scada.cpp` (ctest `x40_scada`); generator `tests/x40/x40_gen_scada.h`, which exists only for the tests.

## Physical layer

| link | how it is read |
|---|---|
| Bell 202 (1200 / 2200 Hz) and V.23 (1300 / 2100 Hz), 1200 baud audio FSK on FM | narrow FM front end (+-8 kHz), the audio at 24 kHz mixed down by 1700 Hz (the centre of both) and put through a second discriminator: the mark / space decision does not depend on the audio level, the tone twist or the carrier offset. The steady tone frequencies name Bell 202 or V.23. |
| direct 2-FSK, 4800 / 9600 / 19200 bit/s | the FM discriminator (4800: the narrow front end; 9600 and 19200: +-16 kHz) |
| direct 4-FSK, 4800 / 9600 / 19200 bit/s (2400 / 4800 / 9600 symbols/s) | the same; dibits +3 = 01, +1 = 00, -1 = 10, -3 = 11 (the P25 / DMR order; an assumption, see below) |

The FM front end is `dect2::FmFront` (`dsp/fm_front.h`, the LTR / EDACS front end). All seven chains run side by side. Each has a boxcar of
3/4 of a symbol, the clock loop of the packet modem (`Afsk1200` in `packet/packet_demod.h`: the phase is pulled to the level crossings),
a decision-directed centre and deviation (follows the carrier offset, its drift and a wrong deviation; 4-FSK takes its levels from the eye's
envelope until it locks) and an eye-quality lock.

The bits go to async framers (8N1, 8E1, 8O1; direct FSK in both polarities) and HDLC framers (NRZ in both polarities, NRZI; flags,
zero deletion, the X.25 FCS). A burst of bytes ends at about three idle characters, when the chain loses lock, or at 1024 bytes. Which chain
and framer gave frames with valid CRCs is the link type, baud rate and framing; the chain's steady levels give the deviation (or the tones).
When an 8E1 frame is also read by the 8N1 framer (every parity bit happened to be 1), the stricter framing names the link.

## Protocols

- **Modbus RTU**: address, function (read coils / discrete inputs / holding / input registers, write single and multiple coils and
  registers, mask write, read/write multiple, read exception status, diagnostics with its sub-function, comm event counter and log, report
  server ID, FIFO, file records, MEI), exception responses with their names, register addresses with the 0x / 1x / 3x / 4x reference
  numbers, values, CRC-16. A frame needs a valid CRC, an address of 0 - 247 and the exact length its function gives; a function this reader
  does not know counts only when its CRC covers the whole burst. A burst of the exact length of a known request or response with a wrong
  CRC is shown as a Modbus frame with "CRC failed".
- **DNP3**: the data link layer (0x0564, length, control with direction, primary, FCB / FCV and the function, destination and source, the
  header CRC and a CRC every 16 octets), the transport layer (FIR, FIN, sequence; fragments of several segments are put together), and an
  application summary: function, objects as gNvM with their ranges and qualifiers, IIN flags by name, class polls and integrity polls,
  unsolicited responses, application confirms, time sync (write of g50v1 shown as a UTC time, delay measurement, record current time), and
  the values of binary, double-bit, counter, analog input and output points.
- **Pairing**: a Modbus request with the next reply from the same slave with the same function (or its exception); a DNP3 request with the
  response of the same application sequence between the same master and outstation; a confirm with the response or unsolicited response
  that asked for it; link-layer requests (request link status, reset, test, confirmed data) with their ack or link status. The reply shows
  the response time.
- **Devices**: every Modbus slave and DNP3 master and outstation, with first and last heard, last poll, polls, replies, unanswered polls,
  exceptions, unsolicited responses, CRC failures, mean response time, the last IIN, and the register map / points seen so far.
- **Not decoded, shown raw**: DNP3 Secure Authentication (functions 32, 33, 131, object group 120), HDLC frames whose content is neither
  protocol, and bursts a chain locked on that held no frame (the bits as sent, packed MSB first): unknown, proprietary or encrypted
  framings such as MDS or MOSCAD. Nothing is guessed about them.

Export: CSV (one row per message: time, wall clock, link, protocol, from, to, function, summary, CRC, direction, pair, response time,
encrypted, raw hex) and JSON (the messages and the devices with their points).

## Certain, assumed, not known

- Certain (public specifications): the Modbus RTU frame and CRC, function codes and exception codes; the DNP3 link frame, both CRCs, the
  transport header, the application header, IIN bits, qualifier codes and the object sizes in the table of `scada_proto.cpp`.
- Assumed: how a radio modem carries async bytes on direct 2-FSK / 4-FSK (transparently, idle = mark) and the 4-FSK dibit order. Real
  radios of this kind often use their own synchronous framing; such bursts come out as "not recognised" with their raw bits.
- Not known: any proprietary radio framing (MDS, MOSCAD and others). They are not decoded.

## Tests

`test_x40_scada` (simulated signals): the CRC check values; the frame finder with garbage around frames, damaged frames, unknown functions
and random bytes; Modbus meaning and pairing (reads, writes, coils, exceptions, diagnostics echo, an 8-byte coil response, unanswered polls);
DNP3 meaning (link status pairing, integrity and class polls, IIN, points, time sync, event response with confirm, unsolicited with confirm,
a two-segment response, Secure Authentication shown raw, a damaged block, a lost first segment) and the export; then through the air every
link and framing, impairments (offset +-1.5 kHz, drift 400 Hz/s, deviation -30 % / +30 %, level x0.001 / x30, bit clock +-300 ppm, 192 ksps
and 1 Msps, continuous carrier, tone twist +-6 dB), noise alone and an idle carrier (no message), unknown framings, CRC failures and pairing
with the response time through the air, and an AWGN sweep printing the C/N0 where 9 of 10 frames come through.

AWGN sweep (192 ksps, C/N0 in dB-Hz where 9 of 10 Modbus frames come through; quick.yml runs 43503179980 Linux and 43503184386 Windows, both passed):

| link | 90 % point |
|---|---|
| Bell 202 AFSK 1200 Bd, 8E1 (3 kHz FM deviation) | about 51 |
| 2-FSK 4800 bit/s (+-2 kHz) | about 51 |
| 2-FSK 9600 bit/s (+-3 kHz) | about 54 |
| 2-FSK 19200 bit/s (+-4.8 kHz) | about 57 |
| 4-FSK 4800 bit/s (+-1.8 kHz outer) | about 57 |
| 4-FSK 9600 bit/s (+-2.4 kHz outer) | about 66 (63: 8 of 10) |
| 4-FSK 19200 bit/s (+-4 kHz outer) | about 66 |

No frame with a valid CRC that was not sent, at any level; noise alone and an idle carrier give no message.

Lessons from the CI runs: noise in the gap between bursts reached an eye error near 0.42 against the extremes of its own symbol window and
locked the chain now and then, and a burst that began during such a false lock kept the noise's levels and a clock sampling on the
transitions (lost frames on clean signals). The lock now needs an eye error under 0.3 and is lost above 0.5 (checked first with a small
Python model of the 2-FSK chain). Measurements (tones, deviation) use only clean symbols.
