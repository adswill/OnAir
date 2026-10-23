# PACTOR-I monitor (experimental)

A receive-only monitor for **PACTOR-I**, the 1991 amateur HF data mode of Helfert (DL6MAA) and Strate (DF4KV). It lists the packets and control
signals of both stations of an ARQ link and the repeated packets of unproto (FEC) broadcasts, and shows the text.
Mode list: **HF UTILITY > PACTOR-I (experimental)**.

**Experimental.** It was written from the public descriptions only (listed below). No real PACTOR recording was available, so it has been checked
against its own test signal and nothing else. If you hear PACTOR-I, a short IQ or audio recording would help a lot: please send one.

## PACTOR-II, -III and -IV are not supported

PACTOR-II, -III and -IV are proprietary protocols of SCS (Special Communications Systems). Their modulation, framing and compression are not
published, and OnAir does not reverse engineer proprietary protocols. The only way to read them is the SCS modems; the HF waveform identifier
(mode **HF waveform ID**) can still tell you that a signal looks like PACTOR, and then names this mode for PACTOR-I-like FSK.

## What it decodes

- FSK with 200 Hz shift at 100 and 200 baud. Found automatically: the tone pair (anywhere between 150 and 3600 Hz of the sideband, so a tuning
  error of up to some hundred Hz does not matter), the speed and the tone polarity (the real system swaps the polarity every cycle) of every packet, and the
  timing (+-30 ms of the burst start).
- The 1.25 s ARQ cycle: a packet of 0.96 s, 0.17 s of silence and the 12-bit control signal (0.12 s) of the receiving station. The control signal and
  the next packet of the sending station follow each other without a gap, so the receiver walks each burst: packet, control signal, packet.
- Packet: header (55 or AA hex), 8 data bytes at 100 baud or 20 at 200 baud, status byte, CRC-16 (CCITT) over data and status. A packet is
  accepted only with a good CRC and a header that is at most one bit away from 55 / AA.
- Control signals **CS1** (4D5 hex, acknowledge), **CS2** (AB2, reject / busy), **CS3** (34B, changeover: the side that was receiving sends next, the
  list marks the side 1 / 2) and **CS4** (D2C, speed change), one bit error allowed.
- Connect (sync) packets: the callsign of the called station, padded with 0F hex; QRT packets; break-in and QRT bits of the status byte.
- Text: plain 8-bit ASCII and Huffman compressed text (status bit), the text of both stations in one view, the callsigns named in it in the station list.
- Repeats: the same packet again is marked "repeat" and not added to the text twice. A repeat without a control signal in between is marked "unproto / FEC
  repeat", and after three such packets in a row the receiver reports FEC mode.
- **Memory ARQ**: a packet that fails its CRC is kept as soft bits; the next copy of it is added up with it and the CRC is checked again. The counter
  "memory ARQ saves" shows how often that rescued a packet.

## The Huffman table (you add it)

The public ARRL description of PACTOR (Annex, section 9) prints the Huffman table of PACTOR-I, but its terms do not allow copying it into other
software, so OnAir ships only the handful of entries the page shows as examples (space, e, t, s, a, CR, LF). Text that uses other characters shows `~`
at the first code that is not in the table. To get the full text, save the table from that page in a file, one entry per line (the ASCII number or the
character, a tab, the code bits in sending order, as the table prints them), and give its path under the **Text** view (**Load table**). The path is
remembered. ASCII-mode packets need no table.

## What is an assumption

The public descriptions leave these open, so they are guesses that may be wrong for real traffic (the first thing a real recording would settle):

- the bit positions of the status byte (here: bits 0-1 packet number, 2 Huffman, 3 break-in, 4 QRT);
- the CRC start value (FFFF), the byte order of the CRC (high byte first), and whether it is sent inverted (both are accepted);
- which bit of a byte goes first (here: least significant bit first, as the Huffman description says);
- the exact timing of the control signal and of the first packet after a connect (connect packet = a 100 baud packet with the callsign in its data field);
- how short Huffman texts are padded.

## Test signal

The generator (`core/src/pactor/pactor_gen.cpp`, mode option 0 ARQ script, 1 unproto) makes the packets above with the polarity swap, both speeds,
both headers, noise, a tuning error and a sender clock error. Tests: `tests/pactor` (packet layer, receiver with +-100 Hz tuning error, +-150 ppm clock,
noise, FEC repeats, memory ARQ, noise only; engine run) and `tests/app/test_pactor_mode.cpp` (wiring and the honesty strings).

## Sources

ARRL, "PACTOR" (arrl.org/pactor); Wavecom "PACTOR" decoder notes and the SCS protocol pages for the cycle, packet sizes, control signals and CRC; the
sigidwiki page for PACTOR I. Facts only, no code taken. See THIRD_PARTY.md.
