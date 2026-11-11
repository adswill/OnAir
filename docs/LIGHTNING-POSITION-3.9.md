# Lightning position (network), 3.9

Family 790, engine standard 792, active standard 791, poll slot 790. Its own mode-list entry next to Sferics (lightning). Receive only.

**Simulated signals only.** No real multi-station recording exists yet. Every number below comes from the generator in
`core/include/dect2/sferics/ltpos_gen.h`, not from a real storm.

## What it does

Several OnAir stations with radios time the same sferic. One station acts as the server: it collects the time-stamped detections and finds
the strike by time difference of arrival (TDOA). Each station shows which timing source it uses and the error it estimates. The server
draws strikes on the map, coloured by age (white < 1 min, yellow < 5, orange < 15, red < 30), each with its 95 % error ellipse.

Roles (decoder options): *This station only* (timing only), *Client* (sends to a server) and *Server* (collects and locates; its own radio
counts as a station unless switched off).

## Timing (the core problem)

1 us is 300 m, so a millisecond-level clock is useless for a position fix. The sources, ranked:

| Rank | Source | How | Typical error |
|---|---|---|---|
| 1 | GPS (radio time) | a radio with a GPSDO or a PPS input: pick that time source in the radio's settings. SoapySDR and UHD zero the time register at the next PPS edge; every buffer's hardware timestamp becomes a mark (`core/include/dect2/source/hw_time.h`) | 0.03 to 0.1 us |
| 2 | GNSS receiver | OnAir's GNSS fix (UTC at its own signal time) through `ltpos::gnssMark` with a stream-alignment error | 0.3 us + alignment |
| 3 | Common view | the symbol clock of an MSK VLF / LF station that every station receives, in the same capture | about 1 us |
| 4 | PTP | the computer's clock | coarse: association only |
| 5 | NTP | the computer's clock | coarse: association only |
| 6 | system clock | not synchronised | coarse: association only |

Only sources 1 to 3 below 20 us (1 sigma) take part in a fix. PTP and NTP detections are only associated ("also heard"), and the UI says so.

Per station, `SampleClock` fits the sample clock against its source: offset and ppm drift by weighted least squares over a window, with a
+-50 ppm prior. It refuses outliers and restarts after three of them in a row, for example after lost samples. Each detection is stamped
to the sample. Measured: 23.7 ppm recovered as 23.700 ppm from GPS marks with 30 ns jitter. In the chain test every GPS, GNSS and
common-view station recovered its drift to within 0.2 ppm.

### Common view

- `MskRefTimer` mixes the reference down and decimates to 8 samples per symbol. It squares the signal (MSK squared has lines at +-baud/2),
  and the line phase difference gives the symbol boundary modulo T.
- That first estimate carries the data's self-noise, tens of microseconds. It is refined decision-directed: the bits are read, then the
  timing of the whole phase path (a boxcar-exact model) is fitted.
- The residual carrier is removed: 0.01 Hz would already move the timing by microseconds.
- Each station's clock is then the transmitter's time grid, anchored with the coarse clock.
- On the server, `CommonViewAligner` corrects each station by the propagation delay from the transmitter. GPS-timed stations that also
  hear the reference tie the grid to UTC. Without one, the common-view stations form their own clock group, and the solver gives each
  group its own offset.
- A reading off by whole symbol periods (NTP error over T/2) is folded by the solver. The chain test has one station 7.3 ms off.
- A double notch on the reference carrier keeps it from lifting the sferic detector's noise floor.

Measured, 200 Bd at +40 kHz, 1 s blocks:

| SNR in 200 Hz | Error | Scatter | Claimed |
|---|---|---|---|
| 45 dB | +0.5 us | 0.8 us | 0.9 us |
| 31 dB | +2.5 us | 3.8 us | 4.5 us |
| 19 dB | +10.7 us | 15.3 us | 19.0 us |

Carrier offsets of 0 and 1.2 Hz are both tracked. The tolerance is about +-2.5 Hz, so tune accurately.

The built-in reference list (`ltpos::builtinRefs`: NAA, NLK, NML, NPM, NAU, GBZ, GQD, DHO38, ICV, HWU, NSY, TBB, JXN, VTX, NWC, JJI) is
written from public listings **from memory**. Positions, frequencies and the 200 Bd rate must be checked before real use. Automatic
choice: the nearest listed station inside the capture.

Not implemented: DCF77 / MSF / WWVB second marks and broadcast-carrier references (only MSK symbol timing is there). GNSS time from a
second radio: the API and the test exist, but the app does not yet feed a second engine's fix into this mode.

## Detection

`PulseTimer` sets a median-of-blocks noise floor and a threshold (default 12 dB). It times the leading edge where the envelope first
reaches half of its peak, interpolated between samples. It also measures width, rise time and mean frequency. Pulses within 1 ms of a
detection are taken as its own ringing or sky wave.

Measured at 500 kHz: 40 of 40 strikes found at every level. The scatter is 0.77 / 0.28 / 0.07 / 0.02 us at 14 / 22 / 34 / 46 dB peak SNR.
The edge sits a constant 5.2 to 5.5 us after the onset of the simulated 12 us rise; the spread over 32 dB of amplitude is 0.28 us. It is
the same at every station, so it cancels in the differences.

## Network

- UDP datagrams between stations: a 22-byte header, a station header (position, timing source, clock error), then 32 bytes per detection
  (time, error, peak, amplitude, width, rise, frequency). One packet holds up to 32 detections; 5 detections take 215 bytes.
- Every packet carries an HMAC-SHA256 (truncated to 16 bytes) made with the shared network key (12 characters or more). The server
  drops anything that does not authenticate.
- Replays are blocked by a 64-packet sequence window per station plus a send-time window. A restarted station is accepted by its newer
  signed send time.
- The server applies a per-address rate limit before checking the MAC.
- Private by default: off until a role and a key are set. The client sends only to the server address the user enters. There is no relay,
  directory or public service.
- The network key is stored in the preferences only and is never sent; it only signs packets.
- The remote-control API token was not reused: it grants full control of the receiving instance. The mode's telemetry still reaches the
  API and the web through the usual schema. `web_policy` classifies the mode as restricted.
- `server/web` is unchanged. The web UI does not expose any of the key or role settings.

## Solving

- WGS-84 geodesics (Vincenty) in both directions.
- Propagation by ground wave at 0.9993 c (an assumption; Advanced). An optional ionosphere-hop model (reflection height, hops of at most
  3000 km) applies to paths over 1500 km.
- Robust least squares (Gauss-Newton with Huber weights) for latitude, longitude and one time offset per clock group, from a start grid
  reaching 4000 km.
- With two or more degrees of freedom left, the worst residual over 4 sigma is dropped and the fit is done again.
- The final fit must pass a chi-square test at 99.9 %, which rejects false matches.
- Three stations give an exact fix flagged "unchecked" (and "ambiguous" when a second position fits).
- The 95 % error ellipse comes from the covariance, scaled by chi2/dof.
- Matching (server): detections wait 2 s for the others. Candidates must fit the light time of each baseline plus the timing errors, and
  pulse widths must agree within a factor of 8. On a chi-square failure the worst station's next candidate is tried.

## Measured (`tests/sferics/test_sferics_ltpos.cpp`, simulated)

Position error, median / 95 %, in km, against the timing error per station (60 strikes per cell; `ONAIR_TEST_LONG=1` runs 400):

| Network | Strikes within | 0.1 us | 0.3 us | 1 us | 3 us | 10 us |
|---|---|---|---|---|---|---|
| 4 stations, 150 km | 300 km | 0.06 / 0.94 | 0.17 / 2.82 | 0.56 / 9.12 | 1.75 / 30.3 | 5.9 / 123 |
| 6 stations, 150 km | 300 km | 0.04 / 0.28 | 0.12 / 0.83 | 0.40 / 2.71 | 1.21 / 7.68 | 4.1 / 26 |
| 6 stations, 400 km | 600 km | 0.03 / 0.17 | 0.08 / 0.51 | 0.26 / 1.69 | 0.77 / 5.06 | 2.6 / 17 |
| 8 stations, 800 km | 1000 km | 0.02 / 0.11 | 0.07 / 0.32 | 0.25 / 1.08 | 0.74 / 3.22 | 2.5 / 10.7 |
| 6 stations, 150 km | 1500 km (outside) | 1.1 / 6.9 | 3.4 / 20.8 | 11.2 / 69 | 33 / 205 | 119 / 522 |

- The 95 % ellipse covers 98 % of the errors (6 stations, 1 us).
- A 0.1 % wrong velocity gives 0.30 / 1.32 km (6 stations over 400 km, 0.3 us). The ellipse does not show this bias.
- Hops, 6 stations over 1500 km with strikes within 2500 km at 1 us: the hop model gives 0.34 / 0.93 km. The ground model on the same hop
  data gives 10.8 / 108 km, and half of those fits fail the chi-square test.
- Solver cases: an 80 us outlier is dropped; two clock groups solve; a 5 ms ambiguity is folded; three stations are flagged unchecked;
  a group mixing two strikes is rejected.
- Matcher, 150 strikes (some 2 ms apart), 84 false alarms, 6 stations each missing 10 %: 147 located (median 0.14 km), 1 wrong. The
  false alarms are left unmatched.
- Packets: the round trip works; a wrong key, a changed byte, 2000 random packets (0 accepted), replays and stale packets are all refused.
- Whole chain from IQ at 250 kHz: six stations (2 GPS, 1 GNSS, 2 common view, one of them 7.3 ms off by NTP, and 1 NTP), DHO38 as the
  reference. The packets go through encoding, light reordering and duplicates into the server: 17 of 17 strikes located, 16 within 3 km,
  median 158 m, 95 % 850 m. The NTP station is associated in every strike and never used.
- The mode's test signal goes through one station: the reference locks and gives the drift.
- The receiver over UDP on the same computer: a server and four clients with GPS marks, plus one client with the wrong key. The test
  requires both strikes located within 3 km, the wrong key's packets refused and only the four keyed stations listed. It passed on
  quick.yml run 42956431741 (Linux); the run did not keep the printed figures.

## Not done / limits

- Real sferics disperse with distance, and at long range the sky wave can be stronger than the ground wave, so stations may time
  different parts of the waveform. Only identical simulated waveforms were tested.
- Strong carriers other than the reference are not notched.
- A resampler or decimation in the receive chain adds a constant delay. It is the same on stations with the same settings, and is
  carried as a bias in the hardware-time error.
- Hardware timestamps were never tried on a real GPSDO radio. Both the Soapy and UHD paths are untested.
