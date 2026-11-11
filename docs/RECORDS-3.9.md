# Records board (3.9)

Round-4 item 18 (scratchpad r4-per-mode list). Branch feat39-r4.

- A personal best distance from the receiver position per mode, and per band for the HF modes. Each record keeps the distance, who it was, the date and a few words. The board is in ADS-B > History and AIS > History (**Records board**), is saved in `<data folder>/stats/records.txt` and survives restarts. **Reset all records** asks first.
- Fed by:
  - **ADS-B**: airborne aircraft with a globally decoded position (local decodes can be wrong far out)
  - **AIS**: vessels and other stations (not base stations, not the coarse message 27 positions)
  - **APRS**: stations with a position, not objects
  - **Mesh**: nodes with a position (Meshtastic, MeshCore and the others in the mesh mode)
  - **FT8, FT4, FT2, WSPR** and the other WSJT modes: the locator's middle, per band. A 4-character locator is about ±50 km.
- A record needs at least 0.1 km more than the old one. Distances over 20 100 km are ignored.
- **Not included**: DAB and FM DX (the FM DX log has its own distances but no station positions here), sondes and LoRa satellites.

## Tests
`tests/adsb/test_adsb_records.cpp`: new records and ties, per band, nonsense distances, clearing a category, the saved text with tabs in the fields. Run locally: all passed. The feeds were syntax-checked only.
