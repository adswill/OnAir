# External HF / VHF modems for Winlink and peer to peer (3.9)

OnAir can carry a Winlink session, or a peer-to-peer session with another station, through a modem program that runs next to it: **ARDOP** (ardopcf), **VARA HF**, **VARA FM** and **Mercury**. OnAir connects to the program over TCP, the way Winlink Express and Pat do. It sends the B2F session through the program's data port.

**OnAir does not include, start, bundle, decode or reimplement ARDOP, VARA or Mercury.** The modem program owns the sound card and the radio's PTT. VARA is closed, proprietary, Windows-only software by EA5HVK. Its licence and any registration are between you and its author. A native ARDOP or Mercury decoder inside OnAir is not part of this work.

**Everything below was tested only against mock modems** (local TCP listeners in `tests/winlink/test_winlink_extmodem.cpp`) written from the published command descriptions listed under Sources. **No real ARDOP, VARA or Mercury program, no real gateway and no radio was used.** Differences between a real modem's behaviour and its documentation are possible and would only show up in real use.

## What OnAir already had (and where)

| Part | Where |
|---|---|
| Winlink client: B2F session, LZHUF, gzip, resume, P2P | `core/src/winlink/b2f.cpp`, `b2msg.cpp`, `lzhuf.cpp`, `wl_gzip.cpp`; `docs/WINLINK-3.9.md` |
| The transport layer: `ByteLink` (connect, listen, write, disconnect, abort, tick) with `Ax25Link`, `ArdopLink` (OnAir's own simplified ARQ over its ARDOP frame layer) | `core/include/dect2/winlink/wl_link.h`, `core/src/winlink/wl_link.cpp` |
| `TelnetLink` (CMS, off by default), mailbox, gateway list, `WinlinkStation` (one session over one link) | `core/src/winlink/wl_station.cpp` |
| Packet / ARDOP burst glue to the radio | `core/src/winlink/wl_modem.cpp`; frame taps in `packet_rx`, `ardop_rx` |
| Panels (APRS / Packet screen, ARDOP screen, view **Winlink**) | `app/winlink/winlink_ui.cpp` |
| ARDOP mode (monitor-only receive); WINMOR (monitor) | `core/src/ardop`, `core/include/dect2/xlink/winmor.h` |
| TNC interfaces | OnAir has a KISS TCP server that hands decoded packet frames to other programs (`core/src/net/feeds.cpp`); it is not used by Winlink and is not a client. No client for an external modem existed before this work |

The transport layer is the `ByteLink` interface. `WinlinkStation::makeLink()` builds the link for the chosen gateway's `Transport`. The new transport is one more `ByteLink`; the B2F code is unchanged.

## What was added

| Part | File |
|---|---|
| `ExtModemLink` (a `ByteLink`), modem profiles, BW rules, setup help, **Test connection** | `core/include/dect2/winlink/wl_extmodem.h`, `core/src/winlink/wl_extmodem.cpp` |
| `Transport::Modem`, `Gateway::modem` (profile name), `StationConfig::modem`, status line | `wl_station.h` / `wl_station.cpp`, `ByteLink::detail()` in `wl_link.h` |
| **Modems** view under Winlink (profile list and editor, Test connection, "?" setup help), the gateway type **External modem**, German strings | `app/winlink/winlink_ui.cpp`, `app/i18n/de.txt` |
| Mock modems and tests | `tests/winlink/test_winlink_extmodem.cpp` (ctest name `winlink_extmodem`) |

Settings are the profile list (`winlinkModems`) and the gateways (`winlinkGateways`, one more optional field). They are app preferences, like the other Winlink settings, so the engine's web schema and options generators (`tools/gen_web_schema.py`, `tools/gen_web_options.py`) have nothing to add: the schema check passes unchanged.

## The interfaces (all from public documents)

Commands end in a carriage return (CR). OnAir uses only commands that appear in these documents.

### ARDOP (ardopcf)

- Command port 8515 by default; the data port is the command port + 1 (8516). ardopcf listens on all interfaces by default: keep it behind a firewall.
- Commands used: `INITIALIZE`, `MYCALL <call>`, `PROTOCOLMODE ARQ`, `ARQBW <200|500|1000|2000><MAX|FORCE>`, `LISTEN TRUE|FALSE`, `ARQCALL <call> <repeats 2-15>`, `DISCONNECT`, `ABORT`, `VERSION` (only the Test connection).
- Messages handled: `CONNECTED <call> <bw>`, `DISCONNECTED`, `BUFFER <n>`, `BUSY TRUE|FALSE`, `PTT TRUE|FALSE`, `PENDING`, `CANCELPENDING`, `REJECTEDBW`, `REJECTEDBUSY`, `STATUS <text>` (a text with FAILED ends a call), `FAULT <text>`, `NEWSTATE`, `TARGET`, `VERSION <text>`.
- Data port frames, modem to host: 2-byte big-endian length (counts the tag and the bytes), a 3-letter tag, the bytes. `ARQ` frames are session data; `IDF` and `ERR` frames are logged and dropped. Host to modem: the 2-byte length, then the bytes, no tag. OnAir sends at most 1024 bytes per frame.
- Sources: ardopcf `docs/Host_Interface_Commands.md` (https://github.com/pflarue/ardop, MIT licence); the ARDOP specification and TNC interface documents in that repository's `docs/refs`; the data-port framing was read from wl2k-go `transport/ardop` (https://github.com/la5nta/wl2k-go, MIT licence).

### VARA HF and VARA FM

- Command port 8300, data port 8301 (VARA's defaults). The data port carries raw bytes. Commands and replies end in CR.
- Commands used: `MYCALL`, `WINLINK SESSION` or `P2P SESSION` (HF only), `CHAT OFF` (HF only), `COMPRESSION TEXT`, `BW500|BW2300|BW2750` (HF only), `LISTEN ON|OFF`, `CONNECT <mycall> <call>` (FM: `CONNECT <mycall> <call> via <digi1> <digi2>`), `DISCONNECT` (ends once the transmit buffer is empty), `ABORT` (dirty disconnect), `VERSION` (only the Test connection; VARA's document does not list it, so a `WRONG` answer is accepted).
- Each command is answered `OK` or `WRONG`, in order. `WRONG` to `MYCALL`, `LISTEN ON` or `CONNECT` ends the session with that message; for the others OnAir logs it and goes on.
- Messages handled: `CONNECTED <src> <dst> [via ...] [bw]`, `DISCONNECTED`, `BUFFER <n>`, `BUSY ON|OFF`, `PTT ON|OFF`, `PENDING`, `CANCELPENDING`, `IAMALIVE`, `MISSING SOUNDCARD`, `LINK REGISTERED|UNREGISTERED`, `REGISTERED <call>`, `SN <value>`.
- `IAMALIVE` is sent every 60 s according to VARA's document: when nothing has been heard for 150 s OnAir reports that the modem is gone.
- OnAir does not send `PUBLIC ON`, `CWID ON` or `CHAT ON`, and it does not read or change VARA's registration.
- Sources: "VARA Protocol Native TNC Commands" by EA5HVK, 13 Feb 2022 (the PDF published with https://github.com/n8jja/Pat-Vara, MIT licence); the connect, disconnect, abort, BUFFER, PTT and BUSY handling was cross-read in Pat-Vara (`vara/vara.go`, `vara/transport.go`) and Pat's VARA configuration (https://github.com/la5nta/pat, MIT licence). **No code from Pat or Pat-Vara was used**: the link was written from the documented behaviour.
- Running VARA on Linux or macOS under Wine: the Pat wiki page on VARA (https://github.com/la5nta/pat/wiki/VARA), the DF3FY guides "Winlink / VARA unter Linux" (DARC, in German), and https://github.com/WheezyE/Winelink. OnAir cannot check any of these setups.

### Mercury

- Control port 8300, data port 8301 (base port, base + 1, `-p <base_port>`), CR-terminated, raw data port. "VARA-compatible TCP TNC interface": the same commands as above, with `OK` / `WRONG` answers.
- Differences OnAir follows: `COMPRESSION` is a no-op, so it is not sent; `P2P` is a no-op, so it is not sent; `BW2750` is accepted and behaves as `BW2300` today; `DISCONNECT` answers `DISCONNECTED` at once and the air side ends afterwards; a call can last up to 300 s, so the default connect timeout is 330 s; `SN <value>` and `BITRATE (<code>) <bps> BPS` are reported by the modem and shown.
- Source: Mercury `docs/TNC.md` and the README (https://github.com/Rhizomatica/mercury, GPL-3.0-or-later). OnAir does not link or include any Mercury code. The `hermes-modem` repository and the HERMES documentation were not read for this work.

## Use

1. Start the modem program (see the "?" next to the profile editor; each modem has its own steps and licence note).
2. **Winlink > Modems**: add a profile (type, host, command port, data port, BW). The ports default to the type's usual values. The BW field stays editable: a value the modem does not document is not sent, and the editor says so.
3. **Test connection**: opens the command port and the data port, asks `VERSION`, and shows what answered. It sends nothing else and nothing over the air.
4. **Winlink > Gateways**: add a gateway with link type **External modem**, pick the profile, enter the call sign (VARA FM: digipeaters in Via). Mark "another station (peer to peer)" for a P2P call (VARA HF then gets `P2P SESSION`).
5. **Connect** runs the session. **Listen for a station** answers the next call through the modem (P2P). The status line shows the link state, BW, the peer, the modem's buffer, S/N (when reported), channel busy and transmitting.

OnAir does not tune a radio, start a receiver or transmit for an external modem. The radio belongs to the modem program. A "start the modem program" hint is the setup text and the download link (ardopcf: https://github.com/pflarue/ardop, VARA: https://rosmodem.wordpress.com/ (the author's site), Mercury: https://github.com/Rhizomatica/mercury); OnAir never launches a program.

## Errors and timers

| Situation | What OnAir does |
|---|---|
| Modem not running, a port closed | `cannot open the command port (or data port) of <modem> at host:port (...): is the modem program running, and are the host and port right?` after `tcpAttempts` tries (2) |
| Modem program stopped during a session | the link fails with "the modem closed the connection"; the unsent message stays in the Outbox |
| `DISCONNECTED` during a session | the session ends as a lost link |
| No `CONNECTED` after the connect timeout (120 s; Mercury 330 s; the profile can set it) | `DISCONNECT` (ARDOP: `ABORT`) is sent and the session fails with "no answer from <call>" |
| `WRONG` to `MYCALL`, `LISTEN ON`, `CONNECT` | the session fails naming the command |
| ARDOP `REJECTEDBW`, `REJECTEDBUSY`, `STATUS ... FAILED!` | the call fails with that text |
| `MISSING SOUNDCARD` | the session fails; restart the modem program |
| Stop | `DISCONNECT`; a second press (or no answer after 60 s, 10 s while still calling) sends `ABORT` |

A call sign, BW or digipeater with a character other than letters, digits, `-` and `/` is refused before anything is sent, so a setting cannot add a second command to the modem.

## Tests

`ctest -R winlink_extmodem` (run on GitHub with `quick.yml`). Mock ARDOP, VARA HF, VARA FM and Mercury modems, 127.0.0.1 only:

- a whole B2F session through each modem against a simulated gateway (proposals both ways over the data port, ARDOP framing with the ID frame ignored), the commands and their order, BW and `via` handling, buffer / busy / PTT / S/N reaching the link;
- listen and accept a peer-to-peer call (ARDOP, VARA HF, Mercury), only messages addressed to the caller offered;
- modem not running (each type), data port closed, a modem that drops both sockets mid-session, a `DISCONNECTED` mid-session, a call nobody answers (timeout), `WRONG` to `CONNECT`, a failed call, Stop while calling;
- Test connection against each mock (it sent only `VERSION`), a silent modem, a closed port;
- profile and gateway storage (old gateway lines still load), BW rules, help text, and refusal of settings that carry a CR or a space.

`test_winlink_telnet`, `test_winlink_p2p`, `test_winlink_modem` and `test_winlink_b2f` still pass (they cover the packet, OnAir-ARDOP and Telnet paths).

## Not verified

- Every flow above was verified **only against mocks built from the documents**, including the VARA command set, which is taken from a 2022 document, and Mercury's, taken from its repository documentation.
- Not verified with a real ardopcf, VARA HF, VARA FM or Mercury: the exact text of `VERSION` replies, how a real modem orders `CONNECTED` against the first data bytes, `BUFFER` semantics under load, behaviour when a second client holds the ports, and whether VARA accepts `CHAT OFF` and `COMPRESSION TEXT` on every version (a `WRONG` is logged and ignored).
- Not verified: any Wine setup, any registered or unregistered VARA, a real gateway, the CMS, any radio.
- OnAir's own ARDOP link (`ArdopLink`) is still a simplified ARQ that talks to another OnAir only; use ardopcf through this transport for real ARDOP.

## Follow-up (not in scope here)

A native ARDOP or Mercury decoder inside OnAir, so no separate modem program is needed.
