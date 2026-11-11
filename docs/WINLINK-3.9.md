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

## Resume, gzip and peer to peer

### Resuming a cut-off message

When the link drops while a message is arriving, the part received so far is kept as `<data folder>/winlink/partial/<MID>.part`. The file has a header line with the proposal code, the size and the compressed size, then the bytes.

At the next connection, when the same message is proposed again with the same sizes, OnAir answers `!<bytes held>` instead of `+`, and only the rest is sent.

- **Byte layout.** The resumed transfer may carry the bytes from the offset on, as wl2k-go sends them. Or it may first repeat the 6 header bytes (CRC and length), as the F6FBB description of compressed version 1 says. Both layouts are recognised.
- **Integrity.** The joined message must pass LZHUF's CRC (or gzip's CRC-32). If it does not, the held part is dropped, the session reports it, and the message comes whole the next time.
- **Safety.** A held part that belongs to a different proposal (another size) is deleted, not used. Offsets stay below 1,000,000, the protocol's limit.
- **Settings.** Settings > Advanced has "Resume a message that was cut off" (on by default) and lists the held messages, each with a Drop button.

### gzip proposals (FD)

Settings > Advanced > "Offer gzip compression" is off by default. When it is on, OnAir puts `G` in its SID, as Pat does with `GZIP_EXPERIMENT=1`. When the other side's SID has `G` too, messages are proposed as `FD EM ...` and sent gzip-compressed. Otherwise the usual LZHUF `FC` proposals are used. An `FD` proposal that arrives without gzip agreed is deferred (`=`).

The compressor is OnAir's own (`core/src/winlink/wl_gzip.cpp`): one deflate block with the fixed Huffman codes and LZ77 over a 32 KB window. Decompression is the existing `gunzip`.

Compression compared with LZHUF:

| input | gzip | LZHUF |
|---|---|---|
| 1548 bytes of text | 381 bytes | 354 bytes |
| 200 kB of text | 30416 bytes | 27235 bytes |
| 70000 identical bytes | 463 bytes | 1506 bytes |

Fixed Huffman codes are a little larger than LZHUF on text. The point of `FD` is compatibility with Pat's experiment, not size.

### Peer to peer (P2P)

A Gateways entry can be marked "another station (peer to peer)". Connect then runs a P2P session with that station over packet or ARDOP:

- no secure login, and the Winlink password is not sent
- only outbox messages addressed to that station are offered (To or Cc is its call sign, any SSID, or `CALL@winlink.org`)
- everything else stays in the outbox

**Listen for a station** is the answering side. It listens on the selected entry's link and frequency, or on 1200-baud packet (ARDOP on the ARDOP screen) when no entry is selected. Any caller is answered, with OnAir's own SID and no `;PQ`. After a session it listens again until Stop listening. P2P over Telnet is not offered.

The AX.25 link gained the idle poll of AX.25 2.2 (T3, 300 s by default). A connected link that hears nothing polls the other station and gives up after its retries. Without it, the answering side of a link whose caller had vanished stayed connected forever.

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

`tests/winlink/` holds five test programs.

### `winlink_p2p` (about 0.5 s)

**gzip**
- Round trips through `gunzip` for:
  - empty input
  - one byte
  - 5000 spaces
  - 70000 identical bytes
  - 30 kB of random data
  - 200 kB and 1548 bytes of text
- Damaged streams are refused.
- A 210 kB output was also checked once with Python's `gzip` module, outside the test.

**Mailbox and gateway list**
- held parts: round trip, a part for another proposal dropped, bad message IDs refused
- the P2P address matching
- the gateway list keeping the peer flag and still loading older lists

**Resume**
- 24 cut sessions: input chunks of 1, 7, 125 and 4096 bytes, each cut at 1/7 to 6/7 of a 20 kB attachment.
- **All 24 complete from the held part**: the answer is `FS !<n>`, the attachment is byte for byte the same, and the held file is removed.
- Example: cut after 1625 of 3176 bytes, the second session moved 1995 bytes against 3870 for an uncut session.
- A damaged held part is dropped, and the message then comes whole.
- A hand-written sender resumes both ways: data from the offset, and FBB v1 with the 6 header bytes again.

**gzip sessions**
- both sides gzip (`FD` both ways)
- only one side gzip (`FC`)
- an `FD` proposal without agreement is deferred

**P2P between two OnAir stations**
- **AX.25 1200, clean:** 53 s simulated.
- **AX.25 9600, 15 % frame loss, gzip on:** 66 s simulated.
- **ARDOP, 10 % frame loss:** 148 s simulated.
- In each case both directions are delivered, and the messages for other stations stay in the outboxes. There is no `;PQ` / `;PR`, the answering side logs who called and listens again, and Stop ends listening.

**P2P resume over AX.25 9600**
- The channel goes silent halfway through a 12 kB message. Both links give up (the receiving side through T3), and the receiving station keeps the part.
- On the second call it asks for the rest, and the message arrives whole.

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
- Resume, gzip and P2P were tested only between OnAir's own sessions and stations. That includes the hand-written FBB-style sender, but no real gateway, Pat or Winlink Express. Whether a real CMS resumes the way the F6FBB description says is not verified.
- Not done:
  - P2P over Telnet
  - the Winlink "pending messages" list
- The password is not stored, so you enter it once per run. OnAir has no general secret store yet.
