# RF fingerprint (View > RF fingerprint)

Recognise an individual NFM, WFM or AM transmitter by its RF fingerprint, also when it sends only a carrier (a repeater keying up with no audio).

## Use
1. Tune to the station and start the receiver. Type is chosen from the mode (FM radio: WFM, airband: AM, everything else: NFM); change it in the window if needed.
2. Press **Watch this channel**. Every transmission on the channel is logged with time, length, carrier-only or modulated, the carrier offset and the best match in your library.
3. Press **Capture now** while a signal is on (or **Use** on a log row) to see its fingerprint, type a name and **Save fingerprint**. Saved prints live in `rf_fingerprints.txt` in the OnAir data folder.
4. New captures and log rows are matched against the library (0..100 %). Hover a match to see which features agreed.
5. **Export CSV** writes the log to `rf_fingerprint_log.csv`.

## What is measured
- Carrier offset from the tuned frequency (lag-1 phase estimator; a few mHz on a clean unmodulated carrier) and its uncertainty; warm-up drift while the carrier is up.
- Key-up transient (only when the carrier turns on inside the capture): rise time, overshoot, start-up frequency chirp, settling time.
- FM: audio deviation, pre-emphasis tilt, mains hum (50/60/100/120 Hz). Sub-audible tones (CTCSS) are ignored when deciding "carrier only". AM: depth.
- WFM: 19 kHz pilot level, its exact frequency error (the exciter's own clock, independent of your radio) and its phase jitter.
- Spurs and the I/Q image from a wideband spectrum (needs a transmission longer than 0.6 s). The image also includes your receiver's imbalance, so it only matches captures taken with the same radio at a similar offset from the centre.

## Limits (honest)
- Your radio's clock error adds to every offset. Calibrate the radio (Tools > Calibrate frequency) or set the clock error under Advanced; with an uncalibrated radio widen the offset allowance and rely on the transient, pilot and spurs.
- Tested on synthetic IQ only (transmitters differing in offset, transient, I/Q imbalance, spurs, pilot, hum; noise; off-centre carriers; other sample rates). Not yet validated on real repeaters. Temperature changes the transient and drift, so one fingerprint is not proof of identity.
- RDS-to-pilot phase and turn-off transients are not measured yet.
- The window uses the engine's IQ tap, shared with the Reception Doctor: do not run both at once.
