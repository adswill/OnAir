# The video generator link, version 1

How the Digital video generator (`onair-videogen`, `onair_videogen.exe` on Windows) and OnAir talk to each other (#458). The generator
is its own program; OnAir's transmitter takes its multiplex over this link. Everything is plain RTP and RTCP (RFC 3550), over a local
pipe or over UDP: no connection, no handshake, no buffering in the link itself.

- Code: `core/include/dect2/tv/vglink.h`, `core/src/tv/vglink.cpp` (both sides), `core/src/tv/vglink_transport.cpp` (pipe and UDP), `app/panels/vg_launch.cpp` (OnAir starts the
  generator), `app/videogen/` (the generator program)
- Tests: `tests/tv/test_videogen_link.cpp` (packets, server and client, over the pipe and over UDP), `tests/tv/test_videogen_mux.cpp` (the whole chain: generator,
  link, DVB-T transmitter, OnAir's own DVB-T receiver)

## Transports

### The local pipe (always on, no ports)

OnAir always listens on a pipe that only this user on this computer can reach. OnAir starts its generator on it (`--onair local`), and
a generator started by hand uses it too. So the usual setup needs no network port and cannot clash with another program's port.

| | Name |
|---|---|
| Windows | the named pipe `\\.\pipe\onair-videogen-<user>`, message mode, local clients only (`PIPE_REJECT_REMOTE_CLIENTS`) |
| Linux, macOS | the Unix datagram socket `onair-videogen.sock` in `$XDG_RUNTIME_DIR` (macOS: `$TMPDIR`; else `/tmp/onair-<uid>/`), mode 0600 |

On Linux and macOS a named pipe (FIFO) is a byte stream, so the link uses the same idea with whole datagrams there: a Unix-domain
datagram socket. One pipe message or datagram is one RTP or RTCP packet. RTP and RTCP share the pipe and are told apart by the packet
type, as in RFC 5761 (RTCP types 200-204 sit where RTP has its marker bit and payload type 33). Each generator has its own pipe instance
(Windows) or its own address (Linux: an automatic abstract address; macOS: a socket file next to OnAir's, removed when it ends).
Nothing waits for a slow reader: a packet that does not fit in the pipe is dropped, as with UDP (Windows: a 1 MB buffer per direction
and generator, and a write waits at most 20 ms). Only one OnAir can have the pipe. A socket file left behind by a crashed OnAir is
taken over.

### UDP (the "Listen on the network for generators" box)

In the Transmit panel, under the **Video generator** row, **Listen on the network for generators** (`videoGenLinkNet=1`) makes OnAir
also listen on UDP, for generators on other computers or started with `--onair host:port`:

| | Default | Setting |
|---|---|---|
| RTP, the transport stream (generator -> OnAir) | UDP **5460** | OnAir: `videoGenLinkPort` (must be even) |
| RTCP, the control (both ways) | UDP **5461** (the RTP port + 1) | follows the RTP port |

OnAir then listens on every network card (`videoGenLinkAll=0`: on `127.0.0.1` only). The generator sends from any local port and
gets its replies on that port, so it needs no port of its own and works through NAT. 5454 is not used: SDRconnect has it.

The generator uses the local pipe unless it is started with `--onair host:port`. OnAir's Tools menu passes `--onair local --token <t>`
(`127.0.0.1:<port>` only when the pipe could not be made).

## The stream (RTP)

- Payload type **33** (MPEG-2 TS, RFC 2250 / RFC 3551), version 2, no CSRC, no extension.
- Up to **7 TS packets** (1316 bytes) per datagram, so a datagram stays under a 1500-byte MTU.
- Time stamp: 90 kHz, from the generator's mux clock. Sequence numbers count up by one per datagram.
- The generator's mux paces the packets at its mux rate. OnAir does not pace or reorder them: it puts each packet in the slot's queue,
  which the transmitter reads as `generator:N`.
- OnAir counts **lost** packets (sequence gaps) and **late** packets (behind the last one), and drops the late ones: the transmitter's
  queue only goes forward.
- An RTP packet is ignored when its SSRC has sent no State, when it comes from another IP address than that State, or when the
  generator's State says it is not sending (`rate=0`, so a stale datagram does not refill the queue).

## The control (RTCP)

Each control message is a compound RTCP packet: **RR** (no report blocks) + **SDES** (CNAME, and NAME when there is one) + **APP**
with name `OAVG`. The APP subtype is the message. Its data is UTF-8 text, `key=value` lines, padded with zeros to 32 bits. Unknown keys
are ignored, so either side can add keys without a new version. Unknown RTCP packets in a compound packet are skipped. A packet that is
cut off or not version 2 is dropped as a whole.

| Subtype | Message | Direction |
|---|---|---|
| 1 | State | generator -> OnAir |
| 2 | Reply | OnAir -> generator |
| 3 | Refused | OnAir -> generator |
| (RTCP BYE) | Bye | generator -> OnAir |

Every message describes a **state**, not an event, so a lost datagram only delays it:

- The generator sends its State about **once a second**, and at once when something changes.
- OnAir answers each State with a Reply to the address the State came from. It also repeats every Reply each second.
- Raise and Quit are **counters** in the Reply: the generator acts when one goes up.
- A new stream is a counter in the State: when it goes up, OnAir empties the slot's queue. This is the same as "a starting generator
  empties the queue" of #494.

### State (generator -> OnAir)

| Key | Value |
|---|---|
| `proto` | link version, `1` |
| `name` | the generator's name, shown in the transmitter (at most 60 characters) |
| `slot` | the slot it asks for (1-8). OnAir gives the next free one when that one is taken. |
| `token` | the token OnAir started it with (empty when started by hand), at most 80 characters |
| `rate` | its mux rate in bit/s, 0 when not sending |
| `stream` | counts its starts (see above) |
| `easy` | `1` in Easy mode, `0` in Advanced (for the user: the transmitter takes the stream as it is in both) |
| `programmes`, `p0`, `p1` ... | the programmes of its multiplex, "number name" each (at most 16) |

### Reply (OnAir -> generator)

| Key | Value |
|---|---|
| `proto` | link version, `1` |
| `slot` | the slot it got: the transmitter reads it as `generator:<slot>` |
| `raise`, `quit` | counters: come to the front (Tools > Digital video generator once more); quit (OnAir is closing the generator it started) |
| `running` | `1` while the transmitter is on air |
| `tv` | `1` when the transmitter's mode carries a transport stream (DVB-T/T2, DVB-S/S2, DVB-C, ATSC, ISDB-T, DTMB) |
| `channel` | the channel's transport-stream rate in bit/s (0: not known): worked out from the mode and its Advanced settings (`txTsRateFor`), on the air the rate of the signal made (`txTsBitrate`) |
| `usable` | what this generator may use: 97 % of the channel, shared equally by the generators in the multiplex |
| `system`, `params` | the transmitter's system and parameters, as text for the user ("DVB-T2", "8 MHz, 32K, 1/128, 256-QAM, 3/4": the width and the Modulation and Framing choices shown in the Transmit panel) |
| `generators`, `inmux` | how many generators the multiplex uses; `1` when this one is among them |

Only a generator started by OnAir (one with a token) acts on `usable`; one started by hand shows the link but keeps its own rates.
In Easy mode it then encodes CBR to fit `usable`, lowering the frame rate when the channel is narrow (`fitEasyToMux`), and shows
`usable` as **Allowed bitrate** in its header. In Advanced mode, `usable` is the capacity on its Muxing tab (unless a capacity is typed
in there).

### Refused (OnAir -> generator)

`error`: why. For example, eight generators are linked already.

### Bye

An RTCP **BYE**, sent three times. The reason `closed` means the user closed the generator window: OnAir then clears "the generator
runs" (`videoGenOpen`), so it does not start the generator again next time. Any other reason (`stop`) is a plain stop.

## Timeouts

- OnAir counts a generator as gone after **5 s** without a State from it (the RTP stream alone does not keep it linked). Its slot's
  rate goes to 0 and the slot is free again.
- The generator counts OnAir as gone after **3 s** without a Reply. It keeps sending its State, so it links again by itself once OnAir is
  back.

## Versions

| Version | Change |
|---|---|
| 1 | first version (#458) |

A generator that sends `proto` < 1 is ignored. A later version keeps the keys above and adds new ones.
