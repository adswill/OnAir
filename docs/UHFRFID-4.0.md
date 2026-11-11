# UHF RFID reader commands, EPC Gen2 (family 887)

Decodes what the READER sends in the EPC Class-1 Gen-2 / ISO 18000-63 air interface (860 - 960 MHz): the reader modulates its carrier with pulse-interval encoding and the mode lists every command.
**Simulated signals only.** No real recording was available; the checksum algorithms and field layouts are written from the EPC Gen2 specification as the author knows it and are checked only against the test generator (`tests/x40/x40_gen2.h`, not reachable from the application).
The tags' backscatter replies (FM0 / Miller, 40 - 640 kHz) are far weaker than the reader and are **not decoded**. Receive only.

- Framing: delimiter (12.5 us low), data-0 (Tari 6.25 - 25 us), RTcal (a 0 and a 1), for Query also TRcal; a symbol shorter than RTcal / 2 is a 0. Rising-edge times are interpolated between samples. Frames whose symbol lengths do not fit Tari and RTcal are dropped.
- Commands named: Query (slots, session, target, select, tag coding FM0 / Miller 2 / 4 / 8, pilot tone, DR; CRC-5), QueryRep, QueryAdjust, ACK (RN16), NAK, Select (bank, pointer, mask; CRC-16), Req_RN, Read, Write, Kill, Lock, Access (CRC-16). A frame that fails its CRC is counted as failed and not listed.
  QueryRep, QueryAdjust, ACK and NAK carry no CRC in the standard, so they are protected by the timing checks only.
- Receiver page: Tari, RTcal, TRcal (and the tag backscatter rate this implies: DR 8 or 64/3 over TRcal), count per command.
- Tested: every command type with its content; Tari 12.5 and 25 us at 1.0, 2.0 and 2.4 Msps, 6.25 us at 2.4 Msps (at 2.0 Msps the shortest Tari is not reliable); data-1 of 1.5 and 2.0 Tari; reader clock 1 % fast and slow; carrier offset 0 / +80 / -80 kHz with 20 kHz/s drift; a 300 kHz off-centre tuning; wrong CRCs.
- Noise (90 % depth, Tari 12.5 us, carrier to noise in 600 kHz, 32 commands per level): 32 of 32 at 30, 24, 20 and 17 dB; 23 of 32 at 14 dB; 0 at 11 dB. 225 s of noise (with and without a carrier): 0 false commands.
- Not covered: tag replies, DSB-ASK vs PR-ASK distinction, session-persistent inventory tracking, EPC memory content (comes from the tags).
