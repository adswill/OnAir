# Radiosonde upload to APRS-IS aggregators (3.9)

Request: a German user (DG9BFC) set wettersonde.net as the target of the APRS-IS iGate, it connected, and it showed 0 packets forwarded with perfect sonde reception; his receiver was not on the map.

## Root cause

The APRS-IS iGate (`Feeds::aprs`, core/src/net/feeds.cpp) is fed from one place only: `packet_rx.cpp`, the AX.25 / AFSK APRS decoder. The sonde receiver (core/src/sonde) never called any feed, and no code produced APRS text for a sonde. So the iGate counters stayed at 0 by design, and no receiver station beacon was ever sent, which is why the receiver was not listed. Nothing was gated away, no login or version string was wrong: the data was never wired.

## What was added

- `core/src/net/sonde_upload.cpp` / `dect2/net/sonde_upload.h`: `SondeUpload`, a list of targets (name, host, port, callsign-SSID, passcode, enable, beacon interval). One thread, a TCP socket per enabled target, reconnect with a growing wait (2 s doubling to 120 s).
- Hook: `sonde_rx.cpp`, after each CRC-good frame with a position (`SondeUpload::get().frame()`); one cheap atomic check when no target is on.
- UI: View > Data feeds > "Radiosonde upload (APRS-IS)", with "Add a target..." presets (wettersonde.net 14580, radiosondy.info 14580, custom). Per target: connected state, packets forwarded, beacons, dropped, reconnects, last error, last line sent (passcode shown as ****). German strings in app/i18n/de.txt.
- Settings keys `sondeUp<N>...`; the passcode key ends in `AprsPass`, so backups leave it out.
- Opt-in: nothing connects or sends until a target is ticked.

## Wire format (dxlAPRS / radiosonde_auto_rx)

- Login: `user CALL-SSID pass N vers OnAir <version>\r\n` (empty passcode is sent as -1; the iGate does the same). The `# logresp ... verified|unverified` answer is read; on `unverified` the sonde lines are not sent and are counted as dropped.
- Beacon on connect and every N minutes (default 10), only when a location is set (View > Map): `CALL>APRS,TCPIP*:!DDMM.MMN/DDDMM.MME&OnAir Radiosonde RX`.
- Object per sonde: `CALL>APRS,TCPIP*:;NAME9*HHMMSShDDMM.MMN/DDDMM.MMEOCCC/SSS/A=AAAAAA Clb=x.xm/s t=x.xC h=x.x% p=x.xhPa 403.500MHz Type=T`.
  - Position in hundredths of a minute (carry handled), `N`/`E` for zero. Time: the sonde's UTC time, else the PC's.
  - Course 001 to 360 (APRS: 0 is unknown), speed in KNOTS (APRS data extension, as auto_rx does: m/s x 1.944), altitude in feet (`/A=` six digits, `-00328` when negative). The form in the request left the speed unit open; knots is the APRS definition.
  - Fields the sonde does not report are left out (no velocity: no course/speed block and no `Clb`; no temperature, humidity, pressure, frequency: no field).
  - Rate limit: one object per sonde per 5 s. Frames without a valid position (or 0,0) are skipped.
- Object names follow the sonde-decoder agreement used by auto_rx (utils.py `generate_aprs_id`), padded to 9 characters: RS41 / RS92 serial as is; DFM `D` + decimal number; LMS6 `LMS6` + 5 hex digits; iMS-100 `IMS` + 6 hex; MRZ `MRZ` + low 6 hex of the two serial numbers joined; iMet-54 `IMET` + last 5 digits. `Type=` is the subtype for RS41/RS92 (RS41-SG), else the type (iMS-100 is written IMS100).
- Not sent: M10 and M20 (their APRS ID needs raw ID bytes that OnAir does not keep), iMet-1/4 (no serial in its frames), DFM-06 sondes with a hex serial (digits only are converted). Skipped honestly, shown in the UI hint.

Checked against: radiosonde_auto_rx `aprs.py` / `utils.py` (format, units, object names). The public pages of wettersonde.net and radiosondy.info do not state the upload host and port: 14580 is the usual APRS-IS client port and the user's own value for wettersonde.net. radiosondy.info host and port are NOT confirmed: the field is editable and the preset note says so. auto_rx itself defaults to rotate.aprs2.net, which radiosondy.info reads from.

## Passcode

The repo has no passcode generator (the iGate takes the passcode as typed, empty = -1), so none was added; this follows the iGate. Without the real passcode the server answers `unverified` and discards the packets: the UI says so.

## Tests (tests/net/test_sonde_upload.cpp, ctest `sonde_upload`)

All against generated frames and a localhost mock server; no real APRS-IS server was ever contacted. Golden line for RS41, RS92, DFM, LMS6, iMS-100, MRZ, iMet-54; padding; unit conversions and rounding carry; login line and -1; beacon; objects on two targets at once with their own callsigns; rate limit; M10 / M20 never sent; counters; passcode not in the last line; beacon repeat; server closes then reconnect then login again; `unverified` drops; off sends nothing.

Verified only against the mock: the real servers' acceptance, the map listing on either site, and a real sonde frame end to end (the receiver hook is a few lines and was not exercised with a recorded sonde here). The app UI was syntax-checked only, not run.

## Shared APRS-IS client

The new code has its own sockets and shares nothing with the APRS iGate (`Feeds` / `AprsGate`) or the ADS-B / AIS targets (`FeedTargets`); `Feeds::configure` / `stop` only forward the new list. The iGate and those targets behave as before (test_feeds, test_aprs_gate and test_feed_targets are run with this change).

## Reply for the owner (to the user)

hey, found it: the igate only ever got the 2 m afsk packets, the sonde decoder was never connected to it. 3.9 has a new "radiosonde upload" in data feeds, you can add wettersonde.net and radiosondy.info (or more) at the same time, with your own call-ssid and passcode.
it sends a station beacon with your map location first, then one line per sonde (rs41, rs92, dfm, lms6, mrz, imet-54, ims-100). m10/m20 are not sent yet.
tried it only against a fake local server so far, so tell me if the sites show you after you ticked it.
