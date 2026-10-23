# OnAir Rust workspace

Everything here builds with `cargo build --offline`: **zero external crates**, no rustup targets, no network. Homebrew cargo 1.98 is the
reference; `rust-version` is 1.70. Parsers of untrusted over-the-air bytes are the first candidates, then servers/clients, the engine
threading core, forward error correction and the Android USB drivers.

## Layout

```
rust/Cargo.toml        workspace (profiles live here: release = lto + panic=abort, dev = panic=abort)
rust/onair_common/     shared building blocks (CRC, bits, ...), no C ABI
rust/onair_<module>/   one crate per protocol/area: src/lib.rs (safe code), src/ffi.rs (the only place with unsafe), README.md
rust/onair/            the ONE static library libonair_rs.a: depends on every crate, owns the panic handler
core/include/dect2/rs/<module>.h   hand-written C header of a crate's ffi.rs
cmake/OnairRust.cmake  builds the workspace once (target onair_rs) and sets the per-module switches
tools/rust_check.sh    fmt + clippy -D warnings + tests (+ --sanitize)
```

Why one staticlib: several per-module staticlibs would each carry their own copy of Rust `core`/`compiler_builtins`, giving duplicate symbols at
link time. Member crates are `no_std` rlibs; `rust/onair` re-exports them and, with the `bare` feature (what CMake builds), is `no_std` too and
defines the panic handler (abort) and `rust_eh_personality`. Without `bare` (plain `cargo test`/clippy) it is ordinary std code.

## CMake

Rust is **mandatory**: `cmake/OnairRust.cmake` stops configuring with an error message when cargo / rustc (>= 1.70) or the Rust target of this build is missing. There is
no C++ fallback: the converted modules exist only in Rust. For a cross build (Windows with MinGW, Android, ...) install the target first (`rustup target add <triple>`;
the error names it). The CI and release workflows install the toolchain (`mingw-w64-ucrt-x86_64-rust` in MSYS2, `rustup target add x86_64-pc-windows-gnu` for the
cross build on macOS); Linux and macOS runners have cargo.

Opt-in switches (default OFF, the C++ implementation is the one in use unless you turn them on):

* `-DDECT2_RUST_RING=ON`: the Rust lock-free `IqRing` instead of the C++ ring (`core/include/dect2/common/ring.h` keeps both; `tests/common/test_ring_rs.cpp` compares them).
* `-DDECT2_RUST_USB=ON`: the Rust Android USB drivers instead of the desktop librtlsdr driver (needs a real device test first).
* `-DDECT2_RUST_VITERBI=ON`: the Rust NEON Viterbi kernel on aarch64; it measures about 4% slower than the C++ NEON one, so the C++ `viterbiRange` in `core/src/fec/fec_proto.cpp`
  stays the implementation of that one function (`tests/fec/test_fec_proto_rs.cpp` cross-checks the two on aarch64).

Results: `ONAIR_RUST_ACTIVE`, `ONAIR_RUST_<MODULE>`, `ONAIR_RUST_MODULES`, imported target `onair_rs`. `onair_rust_use(<target>)` links `onair_rs`; call it on every target that links a
converted C++ file.

## How to add a module (5 steps)

Worked example: the AIS pilot (`onair_proto`).

1. **Write the Rust** in safe modules of `rust/onair_<module>/src/` (AIS: `frame.rs`, `msg.rs`, `nmea.rs`, `mmsi.rs`). Take slices in, write into caller buffers, return `Option`/`Result`; no panics on any input.
2. **extern "C" in `ffi.rs`**: `#[no_mangle] pub unsafe extern "C" fn onair_<module>_<name>(ptr, len, ...)`, converting (pointer, length) to slices and calling the safe code (AIS: `onair_ais_get_u`, `onair_ais_hdlc_frames`, ...). Prefix every name `onair_<module>_` (proto also owns `onair_ais_`, `onair_nmea_`).
   Bump `onair_<module>_version` when a signature changes. Exported functions must not take `u8` / `u16` / `bool` parameters (`python3 tools/check_ffi_widths.py`).
3. **Header** `core/include/dect2/rs/<module>.h`, by hand, every function documented (AIS: `rs/proto.h`). `rust/onair/tests/headers.rs` fails when the header and `ffi.rs` list different functions or break the prefix rule.
4. **The C++ wrapper**: implement the functions of the existing public C++ header (`dect2/<mode>/<x>_proto.h`) in `core/src/<mode>/<x>_proto_rs.cpp` by calling the C ABI (AIS: `core/src/ais/ais_proto_rs.cpp`
   implements the functions of `ais_proto.h` through `rs/proto.h` and checks struct layouts at start-up). The callers do not change. There is no C++ copy of the replaced code: delete it, do not leave it under an `#ifdef`.
5. **Golden-vector test + fuzz run**: `tests/<mode>/test_<mode>_proto_rs.cpp` feeds a seeded run (random, mutated, truncated, real vectors) into a digest (`tests/common/golden.h`) and compares it with the recorded
   value; record the value from the old implementation (or an independent one) the first time and check that the Rust output gives the same. `fuzz_<mode>_proto` is the seeded mutation run for the
   sanitizer build. Register both with `onair_rust_golden_tests(<mode>)` in `CMakeLists.txt`. Add Rust unit tests with known vectors in the crate too.

Then update the status table below and run `tools/rust_check.sh`.

| Module | Crate | Switch | Header | Holds | Status |
|---|---|---|---|---|---|
| common | `onair_common` | - | - | shared CRC/bits/endian/hex/parity/gray/NRZI/Manchester/ring | done (no C ABI) |
| proto | `onair_proto` | always | `rs/proto.h` | AIS frame layer (HDLC, NMEA, messages, MMSI) | done |
| adsb | `onair_adsb` | always | `rs/adsb.h` | ADS-B / Mode S message layer (CRC-24, altitude/identity codes, CPR, Comm-B inference, field extraction, geometry) | done (generator, tracker and register in C++) |
| acars | `onair_acars` | always | `rs/acars.h` | ACARS block layer (CRC-16, parity repair, block parsing, label names, OOOI / media advisory text) | done (frame builder in C++) |
| same | `onair_same` | always | `rs/same.h` | SAME / EAS header parser, tables, location text, time text, county CSV reader | done (county table state in C++) |
| pager | `onair_pager` | always | `rs/pager.h` | POCSAG / FLEX code word (BCH, repair), word classification, text readers and looks-like-text heuristic, FLEX speed table, FIW, phase decoding | done (generators, POCSAG message assembler and receiver in C++) |
| ax25 | `onair_ax25` | always | `rs/ax25.h` | AX.25 frame parser, FCS, address / path / TNC-2 text, HDLC receiver, APRS parser (positions, Mic-E, objects, items, messages, weather, telemetry...) | done (frame builder, HDLC / NRZI / G3RUH transmit helpers and receiver in C++) |
| dmr | `onair_dmr` | always | `rs/dmr.h` | DMR frame layer: Golay / QR / Hamming / RS (12,9), CRCs, BPTC (196,96), embedded LC, short LC, CACH, trellis 3/4, rate 1, sync patterns, bursts, slot type / EMB, link control, CSBK, data headers and blocks, text formats, talker alias | done (link / receiver, generator, bit helpers and alias PDU builder in C++) |
| p25 | `onair_p25` | always | `rs/p25.h` | P25 Phase 1 frame layer: NID (BCH), Reed-Solomon GF(64), Hamming / Golay, CRC-CCITT, trellis 1/2, TSBK, link control, encryption sync, header word, IMBE interleave | done (receiver, generator and vocoder glue in C++) |
| nxdn | `onair_nxdn` | always | `rs/nxdn.h` | NXDN frame layer: scrambler, LICH, CRC-6/12/16, SACCH / FACCH1 / CAC coding (convolutional code, interleaver, soft Viterbi), layer 3 messages, AMBE+2 bit order | done (receiver, generator and vocoder glue in C++) |
| dstar | `onair_dstar` | always | `rs/dstar.h` | D-STAR DV frame layer: CRC, radio header, flag text, header FEC (convolutional code, interleaver, scrambler, soft Viterbi), AMBE bit order, NMEA / D-PRS slow data GPS text | done (receiver, generator, sync tables and vocoder glue in C++) |
| tetra | `onair_tetra` | always | `rs/tetra.h` | TETRA network information only (broadcasts to all terminals): SYNC and SYSINFO PDUs, BS service / security text, carrier frequencies, D-NWRK-BROADCAST neighbours and network time, signalling block walk (counters, call set-up id / encryption flag / group GSSI), ACCESS-ASSIGN usage | done (burst layers, receiver, generator and the encoders in C++) |
| vdl2 | `onair_vdl2` | always | `rs/vdl2.h` | VDL Mode 2 link layers: scrambler, burst header code, Reed-Solomon (255,249), burst gathering, HDLC deframer, FCS, AVLC addresses and frames, X.25 / CLNP / COTP and XID text | done (demodulator, receiver, tracker, test signal builders and the ACARS / ADS-C reader in C++) |
| hfdl | `onair_hfdl` | always | `rs/hfdl.h` | HFDL bit layers (rates, known symbols, scrambler, interleaver, convolutional code with soft Viterbi decoder, FCS), ground station table and frequency masks, PDU parser (SPDU, MPDU up / down, LPDU, HFNPDU performance / frequency data, system table, positions) | done (PSK maps, burst / PDU builders, ACARS reader, demodulator and receiver in C++) |
| mesh | `onair_mesh` | always | `rs/mesh.h` | LoRa mesh packet layers: protobuf reader, Meshtastic header / Data / port payloads, MeshCore header and key-free payloads, LoRaWAN MAC frames (MHDR, FHDR, MAC commands, join / rejoin), LoRa APRS and MeshCom frames, name and key tables | done (AES / HMAC / Ed25519 and channel tables, LoRa PHY, builders and settings tables in C++) |
| wisun | `onair_wisun` | always | `rs/wisun.h` | Wi-SUN FAN frame layers: PHR, PN9 whitening, FCS, 802.15.4 MAC header, header / payload IEs (schedules, PAN, network name, POM), MPX / EAPOL summary, names | done (demodulator, receiver, generator, channel plan tables and the MPDU builder in C++) |
| uat | `onair_uat` | always | `rs/uat.h` | UAT 978 message layer: downlink (header, state vector, mode status, aux SV), uplink frame, information frames, FIS-B APDU headers | done (FEC, demodulator, tracker, display text and UI mapping in C / C++) |
| ts | `onair_ts` | always | `rs/ts.h` | MPEG-TS packet header, section CRC-32, PAT / PMT / SDT / NIT / EIT / TDT / ATSC TVCT section parsers, PAT packet | done (demux state, section reassembly, EPG store, dvbText / iconv in C++) |
| dvbsi | `onair_dvbsi` | always | `rs/dvbsi.h` | broadcast character sets (EBU Latin, ISO 6937, UCS-2, DAB charset, DVB text field, clean), teletext (Hamming, G0 subsets, data units, renderer), DVB subtitles (segments, colours, run-length pixels, blending) | done (iconv wrapper, teletext page store, subtitle state and PES reassembly in C++) |
| dab | `onair_dab` | always | `rs/dab.h` | DAB FIB walk, FIG 0/0 0/1 0/2 0/3 0/10 0/13 0/17 0/18 0/19, FIG 1 / 2 labels, MOT time / header / data group / directory (shared with DRM), PAD split, dynamic label framing, DL Plus, Journaline framing, packet mode headers | done (ensemble tables, label segments, MOT / PAD assembly, FIG analyser text in C++) |
| drm | `onair_drm` | always | `rs/drm.h` | DRM FAC / SDC parsing and checks, text message segment check, audio super frame split, descriptive tables, MJD conversion, data service helpers (MOT and packet mode headers are in `onair_dab`) | done (demodulator, channel coding, generators, text message and data service state in C++) |
| files | `onair_files` | always | `rs/files.h` | CSV (channel lists), SigMF meta reader / writers (with their JSON reader), WAV `fmt ` and names, TLE parser and text scan, SQLite page / record / cell decoding of the MBTiles reader, results container header | done (file access, WAV chunk walk, b-tree walks, results encoder templates, channel rows in C++) |
| net | `onair_net` | always | `rs/net.h` | remote API text (SHA-1 / base64, WebSocket frames, HTTP request head, URL / query text, flat JSON), rtl_tcp greeting / commands / address, SpyServer words / commands / headers / rates, SDR++ server packets and menu draw list, network radio list | done (sockets, threads, sample conversion, share server, discovery in C++) |
| engine | `onair_engine` | always | `rs/engine.h` | ring buffer, threading core | not started |
| fec | `onair_fec` | always | `rs/fec.h` | Reed-Solomon (DVB-T 204/188, ATSC 207/187, DAB 120/110), DVB-T2 BCH (t = 1..12), K = 7 Viterbi (NEON kernel, opt-in `DECT2_RUST_VITERBI`: 4% slower than the C++), LDPC parity-check table builder | done (LDPC decoders, SSE2/AVX2/NEON C++ Viterbi default, other modes' small Viterbi/RS copies in C++) |
| ring | `onair_ring` | opt-in `DECT2_RUST_RING` | `rs/ring.h` | lock-free SPSC sample ring (`IqRing`) behind `common/ring.h`; the generic mutex queues / pool stay in C++ | done, opt-in (see crate README) |
| usb | `onair_usb` | opt-in `DECT2_RUST_USB` | `rs/usb.h` | RTL-SDR protocol layer over a transfer trait (RTL2832U, R820T/R820T2/R828D, E4000), differential-tested against librtlsdr; Airspy / HackRF to follow | RTL-SDR done, untested on hardware |

## Where the converted code lives now

The C++ originals of the converted modules are deleted. The Rust-backed implementation of each public C++ header is the `<file>_rs.cpp` next to it (AIS: `core/src/ais/ais_proto_rs.cpp`; ADS-B: `adsb_msg_rs.cpp`;
ACARS: `acars_proto_rs.cpp`; SAME: `same_proto_rs.cpp`; pager: `pager_proto_rs.cpp`; AX.25 / APRS: `packet_ax25_rs.cpp`, `packet_aprs_rs.cpp`; NXDN: `nxdn_proto_rs.cpp`; DMR: `dmr_fec_rs.cpp`, `dmr_proto_rs.cpp`;
P25: `p25_proto_rs.cpp`; D-STAR: `dstar_proto_rs.cpp`; TETRA: `tetra_pdu_rs.cpp`; UAT: `uat_decode_rs.cpp` (the display functions of dump978's `uat_decode.c` stay C); Wi-SUN: `wisun_proto_rs.cpp`;
VDL2: `vdl2_proto_rs.cpp`; HFDL: `hfdl_proto_rs.cpp`; mesh: `mesh_proto_rs.cpp`, `mesh_proto_lora_rs.cpp`, `mesh_pb_rs.cpp`; MPEG-TS: `ts_proto_rs.cpp`; DVB service information: `text_charset_rs.cpp`, `dvb_text_rs.cpp`,
`teletext_rs.cpp`, `dvbsub_rs.cpp`; DAB: `dab_proto_rs.cpp`; DRM: `drm_proto_rs.cpp`; FEC: `fec_proto_rs.cpp`; files: `files_proto_rs.cpp`; network: `net_proto_rs.cpp`). The `<file>.cpp` next to it, where
there still is one, holds only what stays C++ (below). Still two implementations, on purpose: the ring buffer (`-DDECT2_RUST_RING`), the Android USB drivers (`-DDECT2_RUST_USB`) and the K = 7 Viterbi
kernel (`-DDECT2_RUST_VITERBI`), all opt-in with the C++ one the default.

What deliberately stays C++ in the three first converted protocol modules: ADS-B `getBits` / `setBits` / `toHex` (helpers without a length, called in loops by the
generator and trackers), the encoder and generator (`adsb_encode`, `adsb_gen`), the tracker and register, the demodulator; ACARS `acarsBuildFrame` (transmitter of the
test signal) and the receiver; SAME's county table (user state with a mutex) and file loader, and the receiver. Pager: `pocsagBits` / `flexPhaseBits` (generators) and the
`PocsagParser` message assembler (unbounded bit list). AX.25: `buildFrame`, `appendFcs`, `hdlcBits`, `nrziEncode`, `scramble`. `HdlcRx` and the pager `PocsagParser` are thin C++ classes
over Rust steps (`hdlcStep`, `pocsagClassify`).

## Golden-vector tests

`tests/<mode>/test_<mode>_proto_rs.cpp` runs a seeded set of inputs (the real vectors and 100000+ random / mutated / truncated inputs per function family, `ONAIR_DIFF_ITERS` overrides the count)
through the public C++ functions, which are the Rust code, and hashes every result into a 64 bit digest (`tests/common/golden.h`; doubles are rounded to 1e-6 first). The digest is compared with the
value recorded from the C++ implementation the Rust code replaced (and checked equal to the Rust output when it was recorded), so a changed result anywhere in the run shows as a different digest.
`ONAIR_GOLDEN_DUMP=1` prints the digest without comparing. `fuzz_<mode>_proto` is the seeded mutation run for the sanitizer build (also a libFuzzer target with `-DONAIR_LIBFUZZER`). Both are
registered by `onair_rust_golden_tests()` in `CMakeLists.txt`. The test programs are compiled with `-ffp-contract=off` so the digest does not depend on fused multiply-add in the test's own arithmetic.

When you change a Rust function on purpose, the digest changes: run the test, read the new value from the output and put it into the test, and say in the commit message why the results changed.

Floating point: Rust never fuses `a * b + c` and calls the C math library for `sin`, `cos`, `floor`, `fmod`... (`no_std` has no methods for them; they are declared in `ffi.rs`, `math`).
