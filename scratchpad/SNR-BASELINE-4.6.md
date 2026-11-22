# SNR threshold baseline (4.6 step 1)

Test: `snr_threshold` (tests/qa/test_snr_threshold.cpp, label `snr;slow`, not in fast ci.yml). It drives tools/sens/qa_sens.py over
test_qa_sweep --gen/--run and writes `snr-out/snr-threshold.csv` (mode,name,kind,threshold_dB,notes).

SNR = active signal power / white Gaussian noise power inside the signal's own 99 % band (channel bandwidth, not Eb/N0). Decoded = checked
block count >= half the clean run's, in every one of 2 fixed-seed trials. Threshold = lowest SNR reached from +20 dB down (4 dB steps, 2 dB near the edge, floor -20) with no failing point.
Empty = no decode. Wideband TV modes and a few others are skipped by qa_sens.py SKIP_DEFAULT (they have sens_sweep).

Run (20 modes first, then all):
    gh workflow run quick.yml -R adswill/OnAir --ref integ-46-snr -f tests=snr_threshold -f env="SNR_LIMIT=20" -f timeout=7200 -f minutes=120
    gh workflow run quick.yml -R adswill/OnAir --ref integ-46-snr -f tests=snr_threshold -f timeout=21600 -f minutes=350
Env (-f env=): SNR_LIMIT first N modes, SNR_RE mode-id regex, SNR_JOBS (3), SNR_CAP seconds per mode (240, then no more points), SNR_OUT.
Output: log lines `SNRTABLE mode,threshold_dB,notes` (worst first), `SNRWORST` (10 worst above 0 dB), artifact `snr-threshold` (CSV + jsonl).
