# Live RDS control (FM transmitter)

The FM transmitter's RDS data can change **while it transmits**: station name (PS), radio text (RT), PI, programme type (PTY), TP, TA, music/speech and the clock time (CT). A change is sent with the next RDS group (a cycle is five groups, about half a second); nothing restarts. Each new radio text flips the A/B flag, so receivers clear the old text.

Policy: warnings only. OnAir refuses nothing here (the usual transmit rule); you are responsible for what you send.

Four ways in, all ending at the same encoder state:

## 1. The panel
Transmit view, mode FM broadcast, step 2: **Station name (PS)**, **Radio text (RT)** and **RDS PI** change live while on the air. **Live RDS control** has PTY, TP, TA, music, **Send the clock time**, the command port and the text file.

## 2. Text commands (UDP and TCP)
Tick **Listen for RDS commands** (default port 9101, this computer only). One command per line, `KEY=value`:

| Command | Meaning |
|---|---|
| `PS=NAME` | station name, cut or padded to 8 characters |
| `RT=text` (or `TEXT=`) | radio text, up to 64 characters |
| `PI=4A21` | PI code, hex |
| `PTY=10` | programme type 0 to 31 |
| `TP=1`, `TA=0` | traffic flags |
| `MS=M` / `MS=S` | music / speech |
| `CT=1`, `CTOFF=4` | send the clock group; local offset in half hours |
| `?` | the state as JSON |

Replies `OK` or `ERR reason`. UDP: `echo "PS=MYRADIO" | nc -u -w1 127.0.0.1 9101`. TCP: `nc 127.0.0.1 9101`. With a token set, every UDP datagram starts with `<token>;` and a TCP session begins with `AUTH=<token>`. Listening on the network needs a token of 16+ characters (the headless server / API; the app panel stays on this computer).

## 3. REST and MCP
Same token rules as the rest of the remote API ([REMOTE-API.md](REMOTE-API.md)).

```
GET  /api/v1/x/rds
POST /api/v1/x/rds   {"ps":"MYRADIO","rt":"Now: A - B","pi":"4A21","pty":10,"tp":true,"ta":false,"ms":"M","ct":true}
```
The MCP tool `rds_set` takes the same fields (it exists only when AI transmit control is on, see [MCP.md](MCP.md)).

## 4. Now-playing file
Type a file name in **Radio text from a file**: its first line is the radio text, re-read twice a second. A script that writes the current track into the file is enough.

## Not done / not tested
Tested with the OnAir FM receiver decoding the generator's output (loopback), not with a real FM radio. PS scrolling effects, RT+ tagging and dynamic PTY names are not generated.
