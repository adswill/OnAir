# Shortwave mode

Shortwave is the General receiver with a row of decoder buttons. Tune anywhere on HF, pick a demodulator (AM, USB, LSB, CW), and click a decoder button (RTTY, PSK31, ALE, JS8, ARDOP, PACTOR, MT63, FSQ, IFKP, marine, time signals, Codec2 voice) to start decoding at the current frequency. No window switch and no retune. Tune away and the decoder stops with the signal.

## Hook
`ShortwaveRx::setDetected(...)` lets the General detector hand a labelled signal to the Shortwave mode, so a detected RTTY signal gets a one-click decode button.

## Radios
Any HF-capable source works. Tested paths: Airspy HF+ (direct sampling, native), KiwiSDR (network, remote), RTL-SDR with an upconverter or direct-sampling mod, HackRF with an upconverter.

Mode numbers: see docs/MODE-NUMBERS.md (family 212, std 214, active 213, slot 188).
