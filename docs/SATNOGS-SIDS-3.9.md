# Smart CubeSat: live forwarding to SatNOGS DB (3.9)

Round-4 item 30 (scratchpad r4-per-mode list). Branch feat39-r4.

- **CubeSat > Smart > Forward to SatNOGS DB** (off by default), with your callsign: every new frame of a known satellite (one with a NORAD number) is posted at once to `https://db.satnogs.org/api/telemetry/` with the receiver position. The line beside it counts the frames sent and failed.
- The form is the one gr-satellites' telemetry forwarder posts (checked against `python/submit.py` of gr-satellites on 2026-11-10):
  - `noradID`, `source` (callsign)
  - `timestamp`: UTC ISO 8601 with milliseconds and Z
  - `frame`: upper-case hex
  - `locator=longLat`, `longitude` such as 25.2797E, `latitude` such as 54.6872N
  - `version`
  It is sent form-encoded in the body.
- Not sent: frames from a file, frames without a NORAD number, and anything without a callsign or receiver position. Each frame goes once.
- The SatNOGS-ready export file stays as it was.

## Tests
`tests/cubesat/test_cubesat_sids.cpp`: the exact form (URL encoding, hex case, milliseconds, suffixes), south and east, and the packets that must not go out. CI only. No frame was posted to SatNOGS DB. The checkbox and the upload thread were syntax-checked only.
