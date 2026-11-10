# LoRaWAN receive-only packet forwarder (3.9)

Round-4 item 36 (scratchpad r4-per-mode list; ChirpStack / TTN community gateways). Branch feat39-r4.

- **Mesh settings > LoRaWAN packet forwarder** (off by default): the network server host and port (ChirpStack's gateway bridge, or The Things Network's `eu1.cloud.thethings.network` 1700) and a gateway EUI you registered there. Every LoRaWAN uplink the mesh mode hears is sent there: join requests and data up, with a good CRC. OnAir becomes a receive-only gateway and a coverage probe for the community network.
- Protocol: the Semtech UDP packet forwarder protocol, version 2:
  - **PUSH_DATA** with an `rxpk` entry: time (µs, UTC), tmst, freq (MHz, 6 decimals), modu LORA, datr (SF7BW125), codr (4/5), rssi (OnAir's level in dB, not a calibrated dBm), lsnr, size, data (base64 PHYPayload)
  - **PULL_DATA** every 30 s as the keep-alive
  - PUSH_ACK and PULL_ACK are counted
- **Downlinks are never transmitted.** A PULL_RESP from the server is read and dropped. Packets the radio heard going down (MType 1, 3, 5) are not forwarded.
- The status line shows the uplinks sent, acknowledged and the keep-alive answers.
- **From memory**: the packet layout follows Semtech's packet_forwarder PROTOCOL.TXT as known here (header byte 2, a 2-byte token, identifier, 8-byte EUI, JSON). The test checks it against itself and a loopback server, not against a real ChirpStack or TTN.

## Tests
`tests/mesh/test_mesh_lorawanfwd.cpp`:
- the exact rxpk JSON
- the PUSH_DATA and PULL_DATA headers and the EUI parser
- uplinks only: not downlinks, CRC failures or other mesh protocols
- a loopback network server that receives one PUSH_DATA and one PULL_DATA, acks both and sends a PULL_RESP that is ignored

Run locally: all passed. The settings were syntax-checked only.
