# SDRplay nRSP-ST (network)

Source: `core/src/source/source_nrspst.cpp`, WebSocket client `core/src/net/ws_client.h`, test `tests/source/test_nrspst.cpp` (a fake SDRconnect).
**Status: written from the public spec only; no real nRSP-ST or SDRconnect was available.**

## How the radio is reached

The nRSP-ST talks to a computer through SDRconnect's own transport (Full IQ / IQ Lite / Compact modes). That transport is not public. What SDRplay
publishes is the **SDRconnect WebSocket API**, served by SDRconnect (GUI, or the headless build with `--websocket_port=5454`). So OnAir connects to
SDRconnect, which has the nRSP-ST selected, not to the nRSP-ST itself. The radio list entry is "SDRplay nRSP-ST via SDRconnect host:port".

## Verified from public documents (read 2026-10-17)

- SDRconnect WebSocket API 1.0.3 (api.sdrplay.com/docs/SDRconnect_WebSocket_API.pdf; the sdrplay.com/wp-content/uploads/2026/08 copy is identical):
  text messages are JSON with `event_type`, `property`, `value` (all strings) and optional `device` ("primary"/"secondary", primary when absent), one JSON message per WebSocket message.
  Server pushes `property_changed` and `get_property_response`. Client sends `set_property`, `get_property`, `iq_stream_enable`, `audio_stream_enable`, `spectrum_enable`,
  `device_stream_enable`, `selected_device` (index), `selected_device_serial`, `selected_device_name` (both optionally `<x>:<Full IQ|IQ Lite|Compact>`), and others.
- Properties used: `device_center_frequency` (Hz), `device_sample_rate` (Hz, read-write since SDRconnect 1.0.7 per its release notes), `device_vfo_frequency`, `lna_state`, `lna_state_min`,
  `lna_state_max`, `valid_antennas` (comma list), `active_antenna`, `valid_devices`, `active_device`, `can_control`, `started`, `overload`, `api_version` (since 1.0.9).
- Binary messages: first 2 bytes little-endian type; 2 = signed 16-bit little-endian interleaved IQ of the primary device (5 = secondary; 1/4 audio, 3/6 spectrum, ignored).
- Port 5454 (SDRplay's page and its headless Docker image flag `--websocket_port=5454`; the Go client carlos-andres-osorio/sdrconnect, MIT, uses it as default). No code was taken from it.
- nRSP-ST datasheet / page: 1 kHz to 2 GHz, up to 10 MHz of spectrum, three antenna ports (two SMA, one BNC), Antenna B bias-T 4.7 V/100 mA, 14-bit, switchable AM/FM and DAB notches, HDR mode, firmware must match the SDRconnect version.

## Assumed (marked ASSUMED in the code)

- The WebSocket URL path is `/` (the spec names none).
- The IQ binary rate is `device_sample_rate`. In IQ Lite mode the stream may be narrower; the source does not know and takes `device_sample_rate`.
- `lna_state` 0 is the most gain (as the RSP API); OnAir's gain slider is counted down from `lna_state_max` (default range 0..9 until read).
- Sample rate limits 62.5 kHz to 10 MHz, and the default antenna names "Antenna A/B/C" when `valid_antennas` is not answered. Rates SDRconnect will not accept are answered by it with its own `device_sample_rate` push, which the source follows.
- Setting `device_vfo_frequency` equal to the centre does not hurt.

## Not in the API, so not offered

AGC, IF gain, bias-T, AM/FM/DAB notches, HDR mode, external-reference selection: no property for them in spec 1.0.3. Authentication and TLS: the spec has none (ws://, no user or password; the
nRSP-ST's own users and passwords are for SDRconnect/NRSPAdministrator). So OnAir stores no credentials. Device discovery: none documented (SDRconnect finds the unit itself); you type SDRconnect's address.

## What works against the fake server (`test_nrspst`)

Handshake with accept-key check, api_version probe, command sequence (device choice, rate, frequency, gain as LNA state, antenna, stream enables), IQ delivery and scaling, live retune,
rate follow-up, bad JSON, short/odd/unknown-type frames, fragmentation with a ping, a frame over the 16 MiB limit (dropped, reconnect; 1 MiB up to 3.9), a dropped connection (reconnect with the settings in use), a non-WebSocket
server, a silent server, a wrong accept key, JSON fuzzing.

## What the owner must provide

A capture of SDRconnect's WebSocket traffic with a real nRSP-ST (browser dev tools or `websocat`/Wireshark on port 5454): the URL path, the IQ rate in Full IQ vs IQ Lite, the lna_state direction and range, and
what `selected_device_name` really takes. Report any difference and it is a small fix.

## NOTICE

No third-party code is included. The message formats are from SDRplay's published specification.
