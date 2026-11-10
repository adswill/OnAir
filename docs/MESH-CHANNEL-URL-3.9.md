# Meshtastic channel links (3.9)

Round-4 item 9 (scratchpad r4-per-mode list). Branch feat39-r4.

- **Mesh settings > Channel keys > Meshtastic channel link**: paste the link the Meshtastic app shares (`https://meshtastic.org/e/#...`, also with `?add=true`, or the bare text of its QR code) and press **Import**. In one step this:
  - adds every channel key in the link to the channel list. The built-in default channel (psk index 1 with the preset name) is skipped. Channels without encryption are added as such.
  - sets the region and the modem preset from the link's LoRa part. When the primary channel has a name of its own, a channel number or its own frequency, the frequency slot moves (the firmware hashes the primary channel name with djb2), so OnAir switches to the custom LoRa setting on that frequency, with the preset's bandwidth, spreading factor and coding rate. A link that does not use a preset sets the custom bandwidth, spreading factor and coding rate.
  - restarts the receiver when it is running
- A link without a region, or with a region or preset OnAir has no band for (the Lite, Narrow, Tiny and Medium Turbo presets, the ITU bands), still adds the keys. The message says why the radio setting was not used.
- **Copy link** puts a link on the clipboard: the default channel first, then your Meshtastic keys, with the region and preset in use.
- The link holds keys, so the input box hides its text. Nothing is sent anywhere.
- **Not done: scanning a QR code** with a camera or from an image. OnAir has no QR decoder. Paste the link instead (the phone app's Share button gives it as text).

## Layout
ChannelSet (apponly.proto), ChannelSettings and ModuleSettings (channel.proto), LoRaConfig with the ModemPreset and RegionCode enums (config.proto). These were checked against meshtastic/protobufs master on 2026-11-10 and are listed in `core/include/dect2/mesh/mesh_channel_url.h`. The protobuf reader and writer are OnAir's own.

## Tests
`tests/mesh/test_mesh_channelurl.cpp`:
- the factory default link (`#CgMSAQESBggBQANIAQ`): decoded, and re-encoded byte for byte
- the `?add=true` and bare base64 forms
- over 500 encode/decode round trips: every preset OnAir knows, 6 regions, keys of 0, 1, 16 and 32 bytes, names, uplink and downlink, position precision, mute, id, hop limit, TX power (including negative), frequency offset, channel number, a second channel
- the slot as OnAir computes it for empty names, the djb2 slot for a private name, channel_num, an override frequency with an offset, custom modem settings, refusals
- damaged links (no channel part, not base64, cut short, LoRa only, a 5-byte key, every truncation)
- a key imported from a link decrypting an AES-256 packet built for that channel

The test runs in CI, not locally (the mesh protocol code links the Rust parts). No link from a real phone app was used apart from the well-known default one. The UI was syntax-checked only.
