# 4.2 changes (notes)

- SondeHub upload is switched off until SondeHub has validated OnAir's decoders (issue #526); the other radiosonde uploads (APRS-IS sites) are unchanged. Developers can use ONAIR_SONDEHUB=1 and ONAIR_SONDEHUB_LOG=<file> to produce validation logs. The RS41 time is now sent as the sonde's own GPS time. See docs/SONDE-UPLOAD-3.9.md.
