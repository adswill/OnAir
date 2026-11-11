# "Works, but" follow-ups A (3.9)

Ten open issues where a fix landed and the reporter said "works, but ...": #243, #242, #338, #301, #344, #235, #239, #410, #432 and #422.

Fixed in 3.9:
- #410 DVB-S/S2 and QO-100: when "play a single service by itself" is on and the receiver moves to another carrier whose only service has the same id (most QO-100 carriers use 1), the service now restarts. Before, the old player session stayed on, and a GPU decoder fed the new stream mid-GOP returned no picture. `autoServicePick` has an optional restart flag (app/dvbs/dvbs_autoservice.h). Test: dvbs_autoservice.
- #301 General receiver: with the target button on, the step arrows and a typed frequency centre the radio, as a click and the wheel already did. At full span the signal sits 250 kHz left of centre. That is on purpose: the radio stays above the dial to keep its DC spike out of the channel.
- #235 ADS-B: when the tracker drops the selected aircraft, the selection is cleared after a 1 s grace, so its readout panel closes. `adsbd::selectionGone` is in dect2/adsb/adsb_display.h. Test: adsb_display.
- #239 Analog TV and FPV: the full-screen view now takes new frames itself. Before, the picture and the snow stood still there.

Already fixed earlier: #243 (aligned waterfall in 3.0, FFT up to 262144 in 3.7), #338 (a far-off saved frequency is ignored), #344 (select part of a text, 3.4), and #432 (FT8/FT4/WSPR export and a daily log, 3.8).

Feature requests, not built: separate OS windows for undocked panes (#242), brightness and contrast sliders for analog TV, analog TV on RTL-SDR (#239), and a UDP feed of FT8 decodes (#432).

Needs info: #422 (OnAir web greyed out; asked for a 3.8 check and a screenshot).

Validation: logic tests only. Nothing was checked against a live QO-100 signal or in the running GUI.
