# RTL-SDR Blog V4 Lite (V4L)

Public facts (rtl-sdr.com launch post and users guide, 2026): USB manufacturer `RTLSDRBlog`, product `Blog V4L` (do not change them; the
drivers use them to identify the dongle). R828S tuner (one RF input fewer than the V4's R828D), diplexed input instead of the V4's
triplexer, **no notch filters**, same built-in HF upconverter (28.8 MHz LO) as the V4, 1 ppm TCXO, software-switched 4.5 V bias tee (up to
180 mA). No direct sampling is needed (it gives nothing). It needs the RTL-SDR Blog librtlsdr (V1.4.0 DLL or newer); older libraries give no
signal.

What OnAir does: lists it as "RTL-SDR Blog V4 Lite", treats it like a V4 (HF from 0.5 MHz through the library's upconverter, direct sampling
never asked for), normal RTL-SDR sample rates and gain table, bias tee through `rtlsdr_set_bias_tee`. No notch controls are shown.
Tested with the stub driver only, not with a real V4L.

Not public, so not assumed. A V4L owner please sends: the tuner id the library reports (`rtl_test -t` output), the descriptors
(`lsusb -v -d 0bda:2838`), the EEPROM (`rtl_eeprom -d 0`), whether HF below 28.8 MHz and the bias tee work in OnAir (log lines
"RTL-SDR: ... model V4"), and the highest sample rate without lost samples (`rtl_test -s 2.4e6`).

## Manual "Treat as RTL-SDR Blog V4 / V4L" (4.6, #663 / #337)

OnAir already recognises a V4 / V4L from the USB strings (`RTLSDRBlog` / `Blog V4`, `Blog V4L`), from the library's own model check, or from the
R828D tuner id, and then never asks for direct sampling: the Blog librtlsdr upconverts below 28.8 MHz when the frequency is set (relay, cable
input and the 28.8 MHz offset are inside the library, so the displayed frequency is the real one). A dongle that reports other strings (clone,
re-flashed EEPROM) was a generic one and got the Q-branch direct sampling, which a V4 does not have: HF stayed silent. Its radio settings now
offer **Treat as RTL-SDR Blog V4 / V4L** (key `v4`, takes effect on the next start). Test: `rtl_v4_manual` (tests/source/test_rtl_v4_manual.cpp).
If HF is still silent with this on, the library in use is not the Blog one (the log line "RTL-SDR: ... Blog HF upconverter in driver: no").
