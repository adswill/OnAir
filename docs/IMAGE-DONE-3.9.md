# "Picture done" event (3.9)

Round-4 item 13 (scratchpad r4-per-mode list). Branch feat39-r4.

- New event **image.done** (family `image`). Fields: path, source, satellite, detail, key, width, height and picture_time. It fires once per picture when the picture is finished.
- It covers every picture the gallery (View > Images) receives: weather satellites (Meteor, HRPT family, NOAA recordings), GOES and the other geostationary satellites, SSTV, weather fax, SSDV and HamDRM.
- "Finished" means the receiver has not changed that picture for 20 s. A receiver republishes a growing picture under one key, so this gives one event per picture, not one per update. The detail carries the pass ("pass 09:41, max 62 deg") when the scheduler was receiving one.
- Built-in recipes, off by default:
  - **Post my Meteor pass to Discord**: a Discord webhook message with the satellite, pass and size. Paste your channel's webhook address.
  - **Picture finished**: a notification with the path.
- The webhook action sends text (JSON), so **the picture itself is not attached** to the Discord message. It says what was received, and the file stays in the gallery folder. Uploading the file needs a multipart web post, which the webhook action does not have.
- **Copying the picture to a folder by a rule is not built.** The existing "Save pictures to" setting and the gallery folder hold the files, and a rule can use the `run` action (with its confirmation) to call a copy command.

## Tests
`tests/meteor/test_meteor_imagedone.cpp`: the catalog and family; the Discord recipe only for Meteor pictures (also by satellite name, any case), once per picture, and the exact Discord message built from the event; "Picture finished" for any source with the path. The 20 s debounce in the gallery was syntax-checked only. CI only.
