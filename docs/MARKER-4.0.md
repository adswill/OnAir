# ILS marker beacons (4.0, family 779)

Receiver for the 75 MHz marker beacons of an instrument landing system (ICAO Annex 10): the AM carrier is keyed with a tone. Outer marker 400 Hz dashes (2 per second), middle marker 1300 Hz alternate dots and dashes (95 dot-dash pairs per minute), inner marker 3000 Hz dots (6 per second).
Shares the x40 receiver (`core/src/x40/x40_marker.cpp`, test `tests/x40/test_x40_marker.cpp`, generator `tests/x40/x40_gen4.h`). Variant 42, family 779, active standard 780, engine standard 781.

How it works: the envelope of the 12 kHz channel goes through a Goertzel filter at each of the three tones in 10 ms blocks; a block is keyed when one tone has 30 % depth (of the carrier) and three times the depth of each other tone. A passage runs from the first keyed block until 1.5 s without one.
The marker is named from the tone; the keying (elements per second, dot / dash mix) is measured and compared with the standard. A tone with the wrong keying is listed as "Tone only". It does not give a position.

Tested (simulated signals, 48 ksps): each marker with the carrier at 0 / +2.5 kHz / -3.5 kHz and fading of 0 and 30 % at 0.3 Hz, 30 dB carrier to noise in 12 kHz: always one passage, named right, keying inside the limits.
Carrier to noise in 12 kHz, passages named right of 4 runs (wrong names: none at any level): 30 to 8 dB 4 of 4; 4 dB 3 of 4 (the fourth as "Tone only"); 0 dB and -4 dB none named (all "Tone only"). 400 Hz with dots keying: "Tone only"; two tones at once, a 1020 Hz Morse identification, 60 s of noise and 60 s of a fading carrier: no marker row.
The element lengths of the generator are an assumption (a dash is three dots); **no recording of a marker beacon was available**.
