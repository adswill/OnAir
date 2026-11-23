# Bugs A, 4.6 (branch integ-46-bugsA, pushed to pub)

CI (quick.yml): 45122682300 (tx_660_analog_modes, map_scroll_zoom_652, tx_params, tx_param_model, mesh_custom pass; app target `dect2` builds; mesh_659 had a test bug), 45126728099 (mesh_659_custom_preset passes).
The test-first run (45120126450, tests only) was cancelled before it finished (cold build), so no recorded red run.

## #660 TX analog modes only send NFM
- Cause: the AM broadcast entry's generator modulation (SubMode "am", synth[0]) was 0 = NFM, and the General receiver transmitter had no visible Modulation choice (only inside "Test scene settings"), so its default stayed NFM. Analog voice also had no DSB/CW/RAW.
- Fix: `general.mod` row (Same as the receiver / NFM / WFM / AM / USB / LSB / CW / DSB / RAW / CW-L; the first follows the receiver's demodulator via generalApiDemod in tx_ui.cpp), "am" synth[0] = AM, DSB/CW/RAW in tx_voice.cpp.
- Test: tests/tx/test_tx_660_analog_modes.cpp (every modulation x tone / voice x 2, 2.4, 4 Msps, row mapping, C-QUAM envelope).
- Not done: hiding the FM rows (pre-emphasis, CTCSS) when the modulation is not FM; a C-QUAM-specific panel.
- Comment: "fixed in 4.6: AM broadcast now sends real AM, and the General receiver TX has a Modulation pick (follows the receiver, or NFM/WFM/AM/USB/LSB/CW/DSB/RAW). thanks for the report!"

## #659 Mesh Custom preset does not unlock bandwidth
- Cause: "Custom" in the MeshCore radio preset list only cleared the preset name; it never turned the custom LoRa setting on, so the Bandwidth field never appeared.
- Fix: core `meshChooseCustomPreset` (core/include/dect2/mesh/mesh_custom.h): Custom turns the setting on, on its own frequency, restarts the receiver; the screen uses it.
- Test: tests/mesh/test_mesh_659_custom_preset.cpp (the choice, 62.5 kHz SFNarrow decode, every bandwidth x SF7/SF10, every built-in MeshCore preset).
- Comment: "fixed in 4.6: picking Custom in the MeshCore preset list now opens the custom setting (bandwidth, SF, CR...), 62.5 kHz works. thanks!"

## #652 AIS map scroll zoom too sensitive (Magic Mouse / trackpad)
- Cause: every scroll event moved one zoom level (adsb_map.cpp, the shared map), and smooth scrolling sends dozens of events plus momentum.
- Fix: `ScrollZoom` (core/include/dect2/map/scroll_zoom.h): per-event clamp, accumulate to a notch, at most one level per 0.2 s, surplus and idle leftovers dropped; a wheel notch stays one level.
- Test: tests/map/test_map_scroll_zoom_652.cpp (delta sizes 0.01 - 0.3, swipe lengths, momentum tails, both directions, pauses, fling).
- Comment: "fixed in 4.6: the map zoom now follows the swipe (small swipe, one level; no more jumping to max). wheel mice work as before. thanks!"
