# OnAir and AI assistants (MCP)

OnAir speaks the **Model Context Protocol** (MCP, [modelcontextprotocol.io](https://modelcontextprotocol.io)), so an AI assistant of your choice (Claude Desktop, Claude Code, any MCP client, a local model behind one) can drive the radio. You say, for example:

> There is some DRM around 15 MHz. Find it and decode it.

and the assistant tunes, scans, identifies the signal, switches the decoder and tells you what locked, using the tools below. It is a thin layer over the same control surface as the [remote-control API](REMOTE-API.md) and the signal identifier ([IDENTIFIER.md](IDENTIFIER.md)); it adds nothing the app cannot already do by hand.

Status: built and tested with the synthetic test signal and with fake MCP clients (stdio and HTTP) only. **Not tried with a real radio, and not tried with a real Claude Desktop, Claude Code or ChatGPT-style client.** The protocol follows MCP 2025-06-18 (also answers 2025-03-26 and 2024-11-05).

## What it can and cannot do

| | |
|---|---|
| Can | list radios, set the radio (sample rate, gain, bandwidth, frequency), tune, list the modes and their usual frequencies, switch the decoder mode, scan a band and identify every signal in it, identify one signal, find-and-decode in one call, read the decoder's state and decoded messages, look at the spectrum, say why nothing decodes, record IQ or audio, search the frequency database and band plans, report which voice codecs are installed |
| Cannot | read or write files, run programs, change settings, read keys or credentials, install codecs, decrypt anything (there is no tool for it; encrypted voice is never decoded, and voice keys are only ever the ones **you** imported yourself), expose your file system (a recording is reported by file name, never by path) |
| Transmit | **No transmit tool exists** unless you tick **Allow the AI to transmit** (app) or start the server with `--mcp-tx`. It is off by default, and in the app it is forgotten at every restart. With it on, OnAir blocks no band or mode (the usual rule: warnings only, the decision is yours) |
| Limits | at most 120 tool calls a minute, one tool call at a time, results cut at 48 KB, request bodies at most 4 KB, 10 requests a second per address on the HTTP transport |
| Transparency | every call is written to an **activity log** (in the app: View > Data feeds > AI assistant (MCP) > What the AI did; in the server: stderr / the console). You can stop the assistant at any time by switching the feature off |

## Turning it on

### In the app

View > Data feeds > **AI assistant (MCP)**:

1. Tick **Let an AI assistant control this receiver (MCP)**. It listens on `127.0.0.1:8766` (this computer only) at `/mcp`.
2. Open **How to connect your AI**, pick your client, press **Copy**, paste where the text below says.
3. Optional: **Programs on this computer need the token too** (a token is made for you), and **Let other computers connect** (LAN; always needs the token). There is no TLS in the app: use a network you trust.

It is **off by default**.

### In the headless server (`onair-server`)

```
onair-server --device test --mcp-stdio            # an MCP client starts it and talks over stdin / stdout
onair-server --mcp-http                           # also http://127.0.0.1:8766/mcp (this computer only)
onair-server --mcp-http 9000 --mcp-token 0123456789abcdef --mcp-lan    # on the network, token required (16+ characters)
onair-server --mcp-http --mcp-tx                  # the transmit tools too (you decide)
```

`--mcp-stdio` serves nothing else (no web page, no API port): stdout carries only the protocol, the log goes to stderr. With TLS configured (`--tls-cert`, `--tls-key`) the HTTP transport is `https`. Settings-file keys: `mcp_http=1`, `mcp_port`, `mcp_token`, `mcp_lan`, `mcp_tx`.

## Connecting a client

**Claude Desktop** (speaks stdio only). Config file: Settings > Developer > Edit config.

* Without the OnAir window (the server owns the radio):

  ```json
  { "mcpServers": { "onair": { "command": "/Applications/OnAir.app/Contents/MacOS/onair-server", "args": ["--mcp-stdio"] } } }
  ```

  Add `"--device", "test"` to try it on the built-in test signal. On Windows the command is `C:\\Program Files\\OnAir\\onair-server.exe`.
* With the app running (the app owns the radio): bridge to its HTTP endpoint (needs Node.js):

  ```json
  { "mcpServers": { "onair": { "command": "npx", "args": ["-y", "mcp-remote", "http://127.0.0.1:8766/mcp"] } } }
  ```

  With a token add `"--header", "Authorization:Bearer <token>"` to `args`.

**Claude Code**:

```
claude mcp add --transport http onair http://127.0.0.1:8766/mcp
claude mcp add --transport http onair http://127.0.0.1:8766/mcp --header "Authorization: Bearer <token>"
claude mcp add onair -- /usr/bin/onair-server --mcp-stdio
```

**Any other client**: streamable HTTP endpoint `http://127.0.0.1:8766/mcp` (header `Authorization: Bearer <token>` when a token is set), or stdio with the command `onair-server --mcp-stdio`. Protocol details: JSON-RPC 2.0, `POST /mcp` with one message; a request is answered with `application/json`, a notification or a response with `202 Accepted`; `GET /mcp` answers 405 (no event stream); JSON-RPC batches are refused. No sessions. A browser page cannot call it: no CORS headers are sent, an `Origin` that is not the server's own is refused, and so is a `Host` that is not a loopback name on a loopback connection (DNS rebinding).

A local model: any MCP client for it works the same way (for example an MCP-capable chat front-end for Ollama or LM Studio); point it at the same endpoint or command.

## Example prompts

* "There is some DRM around 15 MHz. Find it and decode it."
* "What is on 118 to 137 MHz right now? Identify each signal."
* "Why is nothing decoding on this frequency? Fix what you can."
* "Tune to 162.55 MHz and read me the weather broadcast text." (needs a mode that decodes it)
* "Survey 430 to 440 MHz and list the digital voice systems."

The server also offers three **prompts** (`find_and_decode`, `survey_band`, `why_not_decoding`) that a client can show as ready-made starting points, and **resources**: `onair://guide` (how an assistant should use the tools), `onair://status`, `onair://modes`, `onair://activity`, and the templates `onair://mode/{id}` and `onair://bandplan/{mhz}`.

## Tools

All frequencies are in MHz. Every tool has a typed JSON schema (`tools/list`).

| Tool | What it does |
|---|---|
| `list_radios` | the radios and sources (index, name, kind, ranges); the built-in test signal is one of them |
| `get_status` | running, radio, mode, frequency, sample rate, gain, errors |
| `set_radio` | radio by index (restarts), sample rate, gain, auto gain, bandwidth, frequency |
| `tune` | tune to a frequency (starts the receiver if stopped) |
| `start_receiver`, `stop_receiver` | start (optionally on a radio index) and stop |
| `list_modes` | the several hundred decoder modes: number, id, name, usual band, known frequencies; `query` narrows |
| `set_mode` | choose the decoder by id, name or number (restarts the receiver); optional frequency |
| `scan_band` | signals in a range: centre, bandwidth, SNR, power, best guess and alternatives with confidence. Ranges wider than the capture are covered in hops (at most 40) |
| `identify_signal` | what the signal at a frequency is: modulation, bandwidth, symbol rate, ranked candidate modes with evidence |
| `auto_decode` | find and decode in one call: scan around a frequency or range, identify, pick the mode (a `mode` hint such as `drm` is preferred; tried blind when nothing is identified), switch the decoder, tune onto the signal, wait for the lock (30 s by default); returns what locked, the decoder state, and hints when it did not |
| `get_decoder_state` | the decoder's live state (lock, SNR, quality, the mode's own data); large tables are named, not sent |
| `get_messages` | decoded messages since a cursor: pager text, ADS-B aircraft, AIS vessels, ACARS, APRS ... (the app keeps a message stream; the headless server does not yet, it answers with the decoder state) |
| `get_spectrum` | centre, span, noise floor, strongest peaks with width, optionally the bins |
| `get_receiver_health` | SNR, lock, level, clipping, load and plain-language hints on why nothing decodes |
| `start_recording`, `stop_recording`, `get_recording_status` | IQ or audio recording (app host); file name only |
| `search_frequencies` | the frequency database by words, mode, region |
| `get_band_plan` | what the built-in band plans say about a frequency |
| `get_codec_status` | which user-installed voice codecs exist (installed or not; no path); OnAir ships none, see [CODECS.md](CODECS.md) |
| `tx_status`, `tx_start`, `tx_stop` | **only with the transmit switch on** |

### How `auto_decode` decides

1. Looks at the span around the frequency (default 1 MHz, hops if wider than the radio's capture), identifies every signal with the same identifier as the Signal identification window.
2. Scores each signal's top candidates that map to a mode the host has: calibrated confidence, a confirmed verify step, SNR. A `mode` hint wins over a better-scoring signal of another kind.
3. Stops the receiver, sets the mode, tunes onto the signal's measured centre, starts, and polls the decoder for **locked** (up to the timeout).
4. Answers `locked`, `result` (`locked`, `no_lock_yet` or `no_decodable_signal`), the mode, the exact frequency, what it identified, the decoder state, and on failure the hints of `get_receiver_health`.

An AI should say "ambiguous" or "unknown" when the identifier says so, not guess.

## Notes and limits

* One receiver at a time; the assistant shares the app's radio with you. Scanning retunes the radio and starts a capture of the signal identifier; if you use Identify in the window at the same moment the two can step on each other.
* On the test signal the radio produces the signal of the **currently selected mode**, whatever the frequency, so a test of "find DRM" needs the test radio in a mode that generates the signal (the end-to-end test uses DRM).
* The headless server keeps no message stream yet (`get_messages` falls back to the decoder state); recording tools need the app.
* HF (3 to 30 MHz) needs a radio that covers it.
* Voice codecs stay yours to install ([CODECS.md](CODECS.md)); encrypted traffic is never decoded.
* The assistant is a client of your radio, but its words come from a model that can be wrong: check what it decodes before relying on it, and read the activity log if something looks odd.

## Security notes

* Loopback only by default; a token (`Authorization: Bearer`) is required from other computers, constant-time compared, a wrong token is answered after 300 ms.
* No file system, shell, key, credential or settings tool exists; an unknown tool or resource name is refused; recordings are named, never located.
* Tool arguments are validated (numbers, enumerations, lengths); a model that sends `"15.2"` for a number is accepted, anything else is a tool error the model can read and correct.
* Request limits: 4 KB body, 8 KB headers, 5 s to send a request, 16 connections, 10 requests a second per address, 120 tool calls a minute.
* The transmit tools are not merely refused when off: they are not listed.
* Unreviewed: the tools return what the identifier and decoders measured; they were tested on synthetic signals, not on every real signal. See [IDENTIFIER.md](IDENTIFIER.md) for what the confidence means.

## Tests

`tests/mcp/test_mcp.cpp` (a fake client against the real engine on the test signal: protocol errors, schemas, every tool, the end-to-end "find DRM near 15 MHz", stdio, HTTP with its guards, the rate limit, the activity log) and `tests/mcp/test_mcp_stdio.py` (a real MCP client in a child process against `onair-server --mcp-stdio` and `--mcp-http`). ctest names: `mcp`, `mcp_stdio`.

## RDS tool
`rds_set` (transmit tools only) changes or reads the FM RDS data live: see [RDS-LIVE.md](RDS-LIVE.md).
