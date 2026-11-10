# Winlink email (3.9)

OnAir can now send and receive Winlink email. It runs a B2F session with a gateway over its own packet modem (AX.25, 1200 or 9600 baud), its own ARDOP modem, or Telnet to the Winlink CMS. Telnet is off unless you turn it on.

**Everything below was tested only against a local mock gateway** in the test programs: over a simulated channel, through OnAir's own generators and receivers, and over TCP on 127.0.0.1. Nothing has connected to a real gateway, a real RMS or the real CMS, and nothing has been sent over the air.

## Where it is

- **APRS / Packet screen**, view **Winlink**: packet gateways (1200 or 9600 baud) and Telnet gateways.
- **ARDOP screen**, view **Winlink**: ARDOP gateways and Telnet gateways.

The panel has these tabs:

- **Inbox, Outbox, Sent**: the message list and a preview. In the Inbox you can reply, archive a message, or save its attachments.
- **Compose**: To and Cc (call signs or email addresses, separated by commas), subject, body and attachments. **Put in the outbox** stores the message, and it goes out at the next connection.
- **Gateways**: a list you enter by hand. Each entry has a call sign, a link type, the dial frequency in MHz (or a host and port for Telnet), packet digipeaters and a note. **OnAir never looks gateways up on the internet.**
- **Settings**:
  - your call sign and locator
  - your Winlink password, which stays in memory for this run only and is never saved or logged
  - **Allow Telnet to the Winlink CMS (internet)**, off by default
  - the transmit gain

**Connect** tunes to the gateway's frequency, starts the receiver and runs the session. It ends when both sides have nothing more to send. **Stop** ends the session politely; pressing it a second time cuts the connection. The session log is shown below the tabs.

Messages are kept one per file as `<data folder>/winlink/{inbox,outbox,sent,archive}/<MID>.b2f`. Each file holds the B2 message bytes in the same form Pat uses.

## What was built

| Part | File | What it does |
|---|---|---|
| LZHUF | `core/src/winlink/lzhuf.cpp` | B2 framing: CRC-16 (XMODEM, two zero bytes appended), length, then LZSS with adaptive Huffman codes. A port of wl2k-go's lzhuf (from JNOS 2 lzhuf.c). Its output matches wl2k-go byte for byte. |
| B2 messages | `core/src/winlink/b2msg.cpp` | The header lines (Mid first, the others sorted), body and files; the MID (12 base32 characters of an MD5); the date form; Q-encoded titles (UTF-8, ISO-8859-1, base64); the secure-login answer (MD5 with the published salt). |
| B2F session | `core/src/winlink/b2f.cpp` | Client and gateway roles. Covers the handshake (SID, `;FW`, `;PQ` / `;PR`), turns of up to 5 proposals (`FC EM ...`, `F>` checksum), answers (`FS` with + - = and offset requests), and blocks (SOH title and offset, STX chunks of 125 bytes, EOT checksum). Also `FF` / `FQ`, `***` errors from the CMS, duplicate MIDs, and a corrupted block caught by its checksum. Byte driven: the input may arrive in any chunk size. |
| AX.25 link | `core/src/winlink/wl_link.cpp` | Connected mode, modulo 8. SABM / UA / DISC / DM, I frames with window k = 1..7, RR / REJ, and a T1 poll followed by go-back-N. The retry count resets when a poll is answered. A repeated SABM re-sends the data that was not acknowledged. Digipeater paths are supported. |
| ARDOP link | `core/src/winlink/wl_link.cpp` | A simplified ARQ made of ARDOP frame types: ConReq500M / ConAck500, 4FSK.500.100 data frames (64 bytes, with an even / odd toggle) answered by DataACK / DataNAK, plus IDLE, BREAK, and DISC / END. The session ID is the low byte of the CRC-16 of the two call signs. |
| Telnet | `core/src/winlink/wl_station.cpp` | TCP on a worker thread, the `Callsign :` / `Password :` login (`CMSTelnet`), then B2F. It refuses to start unless the setting is on. |
| Station, mailbox, gateways | `core/src/winlink/wl_station.cpp` | Runs one session over one link: outbox to proposals, inbox storage, outbox to Sent. |
| Modem glue | `core/src/winlink/wl_modem.cpp` | Renders frames as IQ with the packet and ARDOP generators. Adds the `winlinkpkt` and `winlinkardop` transmit modes, which send one burst and then end. |
| Receiver taps | `packet_rx`, `ardop_rx` | Small additions: `setFrameTap()` hands every good frame to the Winlink link. |
| Panel | `app/winlink/winlink_ui.cpp` | The panel described above. Transmission works the same way as on the MeshCore screen: the radio is claimed for each burst, and the receiver is paused while it sends (half duplex). It shows the transmit notice once and never blocks transmit. |

## Tests (simulated only)

`tests/winlink/` holds four test programs.

### `winlink_lzhuf`

- Decodes all 5 published wl2k-go vectors, including the Gettysburg text (1548 to 861 bytes).
- **Encodes them byte for byte the same as wl2k-go.**
- Round trips of:
  - empty input (6 bytes)
  - a single byte
  - 5,000 spaces
  - 70,000 identical bytes (the frequency table is rebuilt)
  - 30 kB of random data
  - 200 kB of text (many window wraps)
- Refuses damaged data: bit flips, truncated data and short headers.

### `winlink_b2f`

- MD5 vectors, and the wl2k-go secure-login vectors (`23753528` with `FOOBAR` gives `72768415`; with `FooBar` it gives `95074758`).
- The message layout, MID, date, Q titles and binary attachments.
- Mock-gateway sessions with input delivered in chunks of 1, 7, 125 and 4096 bytes. Each session has:
  - 8 messages offered by the gateway, in two blocks of proposals
  - one message refused as a duplicate on each side
  - a 5 kB attachment
- A wrong password, a missing password, an empty exchange (`FF` / `FQ`), and a corrupted block.
- Whole station sessions over the AX.25 link with 0 to 30 % of frames lost, all windows and packet lengths from 32 to 256 bytes. There is also a sweep of 20 random channels at 25 % loss, and **all 20 complete**.
- Whole sessions over the ARDOP link with 0 to 30 % loss, plus 10 random channels at 25 % loss; **all complete**.
- A call nobody answers gives up after 11 SABM frames.
- The gateway list round trip.

### `winlink_modem`

Every frame of a whole session, in both directions, is rendered by the generator with noise and a tuning offset, read back by the real receiver through the new tap, and handed to the link.

| Case | Frames sent | Heard | Air time | Result |
|---|---|---|---|---|
| 1200 baud, clean | 22 | 22 | 21 s | ok |
| 1200 baud, 14 dB, offsets +1.5 / −2 kHz | 22 | 22 | 21 s | ok |
| 1200 baud, 6 dB | 22 | 22 | 21 s | ok |
| 9600 baud, offsets −0.8 / +0.6 kHz | 22 | 22 | 11 s | ok |
| 9600 baud, 18 dB, ±1.2 kHz | 22 | 22 | 11 s | ok |
| ARDOP, clean | 72 | 72 | 132 s | ok |
| ARDOP, 12 dB, +25 / −30 Hz | 72 | 72 | 132 s | ok |
| ARDOP, −1 dB | 104 | 83 | 207 s | ok (lost frames retried) |

At 4 dB, 1200 baud does not complete; this is expected for AFSK at that SNR. The SNR values are the generators' own measures: carrier to noise in 25 kHz for packet, and tone SNR in 2.5 kHz for ARDOP.

This test also covers:
- the transmit payload round trip
- the transmit generator ending after its burst
- both transmit modes sitting at the offset where the receivers listen

### `winlink_telnet`

- A mock CMS on 127.0.0.1 inside the test program. It checks the login lines and the secure login, then runs a session with one message each way.
- Telnet stays refused while the setting is off.

## Limits and what was not done

- **The ARDOP ARQ is OnAir's own simplification** built from the ARDOP frame types. It talks to another OnAir and to the mock gateway. Working with ardopcf, ARDOP_Win or a real ARDOP RMS gateway is **not verified**. The real ARDOP ARQ also has bandwidth negotiation, other data modes (PSK / QAM, 600 baud) and its own timing; none of that is here.
- **No real gateway, RMS or CMS has been contacted**, over the air or over the internet. B2F interoperability rests on following wl2k-go and the public notes, plus the published test vectors.
- **The panel has only been syntax-checked** (no app build, by the overnight rules). The transmit path follows the MeshCore screen, and an on-air key-up has not been tried.
- Not done:
  - resuming a partly received message (OnAir always answers `+`, never offsets; offsets asked for by the other side are honoured)
  - gzip proposals (`FD`, an experiment in Pat)
  - P2P sessions from the panel
  - the Winlink "pending messages" list
- The password is not stored, so you enter it once per run. OnAir has no general secret store yet.
