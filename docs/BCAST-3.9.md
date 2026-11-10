# Broadcast package (3.9, branch feat39-bcast)

Mode-list family block 730-749. What was added, how it was checked and what was left out.

## ISDB-TSB (new mode, family 730, engine standard 732)

ARIB STD-B29 / ITU-R BS.1114 System F: the narrow-band ISDB of one or three segments (about 430 kHz or 1.29 MHz in a 6 MHz raster).

- Built on the ISDB-T code: the generator and the receiver take `Params::tsb` (1 or 3). The narrow band is laid out as the centre
  segment(s) of an ISDB-T band (segment 0 at the centre, 1 below, 2 above), so segment numbers, pilots, TMCC and AC carriers are those
  of ISDB-T. The FFT is 1/8 (one segment) or 1/4 (three segments) of the ISDB-T size; the pilot above the band continues the pilot
  sequence of the next segment place. The TMCC system identification is 01.
- `IsdbtReceiver::configureTsb()`: automatic by default. It starts with one segment, reads the TMCC and changes over to three
  segments when the TMCC says so. The one-segment receiver also takes the partial-reception segment (the one-seg service) out of a
  three-segment signal or a 13-segment ISDB-T signal.
- Its own mode-list entry "ISDB-TSB" (TV group). It uses the ISDB-T status bar and Receiver tab. The test signal has three segments:
  layer A QPSK 2/3 and a two-segment layer B that carries the test programme. Default rate 2.048 Msps.
- Tests (`tests/isdbt/test_isdbtsb.cpp`, ctest `isdbtsb`):
  - carrier counts against BS.1114 Table 5 for every mode
  - TMCC round trip
  - round trips with a carrier offset, sample clock error (+-40 ppm), another input rate, the channel 400-500 kHz beside the centre,
    noise and a slow drift
  - all modes x guard intervals
  - all modulations x code rates
  - three segments on automatic
  - one segment out of a three-segment band
  - the one-seg service of ISDB-T
  - a noise sweep for QPSK 1/2
  - the engine's own test signal through the receiver
- Simulated only: no recording of an ISDB-TSB broadcast was available.

## DVB-H (DVB-T extension, no new mode-list entry)

DVB-H is signalled in the DVB-T TPS and found automatically, so it stays in the DVB / DVB-T entry (one entry per protocol).

- Physical layer (EN 300 744 annex F):
  - the 4K mode (4096-point FFT, 3409 carriers, pilots of tables F.4 and F.5, the permutation of table F.1)
  - the in-depth inner interleaver of the 2K and 4K modes (6048-cell blocks with the 8K permutation over four or two symbols;
    the 2K blocks alternate across frames, so the receiver tracks the frame number)
  - the DVB-H TPS bits: s27 in-depth, s48 time slicing, s49 MPE-FEC, length indicator 0x21
  - Generator and receiver both handle these. The receiver searches 2K, 8K and 4K. The TPS flags appear in the telemetry and on the
    TPS page of the analysis tabs.
- Data link layer (EN 301 192 clauses 7 and 9, `core/src/tv/mpe_fec.cpp`):
  - MPE sections with IP datagrams and the real-time parameters (delta-t, table and frame boundaries, address)
  - MPE-FEC frames of 256-1024 rows, with RS(255,191) erasure decoding of every row; the section CRCs mark the erasures, and the
    padding and punctured columns are handled
  - The demultiplexer finds MPE streams by itself on any PID. The engine feeds it every transport stream. The DVB-T report carries
    the PID, the datagram count and the corrected and failed rows.
  - `MpeEncoder` is the transmit side, used by the tests.
- Tests:
  - `tests/dvbt/test_dvbh.cpp` (ctest `dvbh`):
    - the permutation and carrier tables
    - every TPS combination
    - round trips for 4K native, 4K in-depth, 2K in-depth (also hierarchical) and 8K with DVB-H bits, with offsets of whole and
      fractional carriers, +-30 ppm clock error, other input rates and noise
    - all modes x interleavers
  - `tests/tv/test_mpe_fec.cpp` (ctest `mpe_fec`):
    - RS(255,191) within and beyond 2e + s <= 64
    - every frame size with packet loss
    - punctured columns
    - a loss sweep
    - plain MPE and time slicing without FEC
    - the whole chain: MPE-FEC transport stream through 4K in-depth OFDM and the DVB-T receiver into the MPE demultiplexer
- Not done:
  - The datagrams are counted, not yet written out (no pcap or UDP output).
  - Damaged TS packets erase the whole section; packet-level erasure inside a section is not used.
  - The INT table (IP/MAC notification) is not read.
- Simulated only: no recording of a DVB-H transmission was available.

## T-DMB: already in OnAir

The DAB mode already decodes DMB video services: the TS 102 427 outer code (RS(204,188) and the Forney interleaver), the TS 102 428
MPEG-4 Systems remux, and a DMB video service in the DAB test generator (`core/src/dab/dab_dmb.cpp`, `dab_gen_dmb.cpp`,
`tests/dab/test_dab_dmb.cpp`). Nothing was added.

## Not done in this pass

- DVB-C2 (EN 302 769): the LDPC/BCH and baseband frames of DVB-T2 would carry over. The C2 OFDM framing is new and large: the
  preamble with L1 part 2 and its own time and frequency interleaving, data slices, notches, and the 1/64 and 1/128 guard
  intervals. Not started.
- DVB-SH (EN 302 583): the spec is public. It needs a 3GPP2 turbo decoder, the bit-wise and convolutional time interleavers with
  rate adaptation, the SH frame / CU mapping, its own TPS, and (for TDM) a single-carrier mode. Not started.
- CMMB / STiMi (GY/T 220.1): the Chinese standard text was not available here, and its LDPC tables and the byte interleaver cannot
  be checked without it. Skipped; no decoder was guessed.
