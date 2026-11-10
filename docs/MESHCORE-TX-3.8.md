# MeshCore transmit and receive (3.8, branch feat38-meshcore-tx)

OnAir can act as a MeshCore companion node with a radio that transmits (HackRF, PlutoSDR, LimeSDR, USRP, bladeRF, the radios the Transmit
window already uses). Receive works as before.

**It has not been tested on the air against a real MeshCore node.** Interoperability was checked against the MeshCore firmware's own code
running on a computer (see Tests), not against a radio.

## What was added

- `core/include/dect2/mesh/meshcore_node.h`, `core/src/mesh/meshcore_node.cpp`: the node.
  - Identity: 32 random bytes from the system, turned into the orlp ed25519 64-byte private key that MeshCore uses. The firmware's
    rule applies: public keys starting with 00 or FF are refused and a new seed is drawn. Import and export use the MeshCore apps' format,
    the 64-byte private key in hex. An imported key goes through the firmware's `validatePrivateKey` check.
  - Adverts: flood or zero hop, signed, with the name and an optional position. Received adverts are checked against their signature,
    replayed timestamps are refused, and each one adds or updates a contact.
  - Direct messages: encrypted with the X25519 shared secret (orlp `ed25519_key_exchange`), with the ACK value the firmware computes.
    A message is tried up to 3 times. Each attempt has its own ACK because the attempt number is part of the message. The last attempt
    drops the learned route and floods.
  - Channel messages: Public, `#name` hashtag channels (key = first 16 bytes of SHA-256 of the name), and channels the user adds with a
    16- or 32-byte key in hex or base64.
  - Path learning: a flood message addressed to us is answered with a PATH packet that carries the ACK. A PATH addressed to us sets the
    contact's route and is answered with a PATH back, as `Mesh.cpp` does. Contacts with a route get direct messages.
  - Repeater mode, off by default. Floods get this node's hash appended and go out again after 0 to 4 half airtimes. Direct packets
    whose next hop is this node are passed on, and so are TRACE packets. A table of 160 packet hashes stops loops.
  - Path hash size 1 to 3 bytes, for regions whose preset uses 2 or 3.
  - LoRa airtime (Semtech formula). A duty-cycle table covers the ERC 70-03 sub-bands of 863-870 MHz and 433.05-434.79 MHz, with a
    sliding one-hour meter.
  - The node saves to a plain-text file holding the identity, contacts with their routes, channels and recent messages.
- `core/src/mesh/mesh_crypto_ed25519.cpp`: the orlp key format (expand seed, public key from the private key, signing with it) and the
  key exchange. The key exchange's Montgomery ladder does not branch on key bits. Signing is still variable-time.
- `core/include/dect2/mesh/meshcore_tx.h`, `core/src/mesh/meshcore_tx.cpp`: TX mode `meshcore`.
  - It sends the node's frames once each (sync word 0x12, CRC on) on the settings in `r.opt`, then ends.
  - It is on the Transmit window's list for the MeshCore entry. A line typed there goes to the Public channel.
- `MeshReceiver::setFrameCallback`: every good-CRC frame from a decoder that handles MeshCore goes to the node.
- `app/mesh/meshcore_ui.cpp`: a **Device** view, the first tab of the MeshCore entry.
  - **Join MeshCore** makes the identity (first time only) and sets the region's standard preset: EU/UK (Narrow) 869.618 MHz SF8
    62.5 kHz 4/8 in Europe, USA 910.525 MHz, Australia 915.8 MHz. It then starts the receiver and queues an advert.
  - Chat: a list of channels and contacts, messages with their state (queued / sent / delivered / no ACK), Send advert and Advert to
    neighbours, add or remove channels, reset a route, remove a contact.
  - Advanced: frequency, SF, bandwidth, CR, preamble, path hash size, repeater mode, position in adverts, and identity export
    (show / copy) and import.
  - The node runs while the MeshCore entry is selected, whichever of its views is open.
- Credits: the MeshCore row in THIRD_PARTY.md and in the in-app credits now names the node and transmitter. MeshCore is MIT
  (GPL-3 compatible). No firmware code is in OnAir.

### Notes on the owner's request

- "EU 869.525 MHz": that is MeshCore's old EU/UK preset (SF11, 250 kHz), now listed as "EU/UK (Deprecated)". The current standard is
  "EU/UK (Narrow)" at 869.618 MHz, so Join uses that. The old preset can still be chosen from the preset list in Settings, or set under
  Advanced.

## Transmit rules

- Nothing goes on the air until **Arm transmit** is ticked. It is not saved, so every start of OnAir begins disarmed.
- The Transmit window's one-time notice is shown first.
- The radio is claimed and given back, the map connection is paused, and the HackRF amplifier stays off.
- Nothing is blocked.
  - Airtime used in the last hour is shown against the band's duty-cycle limit (869.4-869.65 MHz: 10 %, 868.0-868.6 MHz: 1 %, and so
    on). A red warning appears above the limit.
  - The gain tooltip gives the EU power limits (500 mW ERP on 869.4-869.65 MHz, 25 mW on most other 868 MHz sub-bands).
  - A line says the user is responsible for the local rules.
- While the device is disarmed, ACKs, paths and repeats older than 30 s are dropped so they don't go out late. The user's own messages
  and adverts stay queued.
- Half-duplex radios (HackRF) stop the receiver for each burst. The receiver restarts within about a second. An ACK that arrives in that
  window is missed, so a message can show "no ACK" although it was delivered; the retries make this less likely. A full-duplex radio
  avoids it. The UI says so.

## Tests

`tests/mesh/test_mesh_meshcore_node.cpp` (new) all passes locally, along with test_mesh_tx, test_mesh_rx, test_mesh_impair,
test_mesh_basic, test_mesh_custom, test_mesh_proto_meshcore, test_mesh_crypto_kat, test_tx_loraaprs, test_tx_quality_a/b (meshcore row),
test_gui_rules and test_credits.

1. **Firmware frames.**
   - Setup:
     - MeshCore firmware at 81dbae5 (2026-10-15) was built on this Mac as a test peer behind a fake radio: `src/Mesh.cpp`,
       `Dispatcher.cpp`, `Packet.cpp`, `Utils.cpp`, `Identity.cpp`, `helpers/BaseChatMesh.cpp` and `lib/ed25519`.
     - SHA-256, HMAC and AES came from Apple CommonCrypto, not OnAir's code.
     - A harness relayed frames between it and OnAir's node over chosen topologies.
   - Interop runs, all passed:
     - shared secret, adverts both ways, direct messages both ways with ACK and route learning
     - the second message direct, channel messages on Public, `#test` and a 32-byte-key channel
     - through a firmware repeater with 1- and 2-byte path hashes
     - OnAir as the repeater between two firmware nodes, flood and direct with ACKs
     - a lost frame and a lost ACK recovered on the second attempt, three failed attempts reported as failed
   - The frames the firmware sent were saved as test vectors. The test checks that OnAir:
     - matches the firmware's public and private key for the same seed, and the shared secret
     - builds a byte-identical advert and message from the same inputs
     - finds its expected ACK inside the firmware's PATH packet and its direct ACK
     - answers the firmware's message with exactly the ACK value the firmware expects (`3d8e67c5`)
     - reads the three channel messages
     - repeats a flood byte for byte as the firmware repeater did
2. **Loop-back with impairments** (simulated air): node → TX generator → noise and carrier offset → OnAir's LoRa receiver on the
   MeshCore setting → second node. Each run carries adverts, a direct message, its PATH + ACK and a channel message.
   - All 60 settings got through at 5 dB SNR with an offset of 4 % of the bandwidth: SF7-12 × 62.5/125/250 kHz × CR 4/5-4/8 (only 4/5
     and 4/8 at SF11-12).
   - EU Narrow got through at +10, 0 and -5 dB SNR with offsets 0, +2.5 and -5 kHz.
   - SF8/62.5 kHz and SF11/250 kHz got through with ±20 ppm clock error.
3. Packet layer: transport codes, 2-byte hashes, bad encodings, 2000 random frames (no crash), airtime against the Semtech formula,
   the duty-cycle table, save/load.

Not run locally: the full test_tx and the app build. The prebuilt libraries from 3.7 don't match this branch's `source.h`, so test_tx
crashes inside the old library whether or not this branch's code is linked. The integrator owns the app build. ci.yml was dispatched for
the branch.

## What is left

- An on-air test against a real MeshCore node (a companion radio with the phone app, and a repeater), on a full-duplex radio and on a
  HackRF. Measure how often ACKs are missed during the HackRF turnaround.
- Not implemented:
  - room-server login and posts (ANON_REQ, signed messages are read but not sent)
  - repeater CLI and status requests (REQ/RESPONSE)
  - trace requests from this node
  - multipart ACKs (sent)
  - transport codes / region scopes (sent)
  - telemetry
- The duty-cycle meter counts only since OnAir started.
