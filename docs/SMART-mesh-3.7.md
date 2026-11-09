# Smart mesh package 3.7 (hamvoice 16, 17; aviosensors 10)

Checked first: no topology or multi-signal LoRa survey existed (relay was a hex byte, the Signal Lab search handled one signal, Z-Wave routes were "not decoded").

## Shared topology (items hamvoice 16 + aviosensors 10)
One model, `core/include/dect2/map/mesh_topology.h`, with front ends in `mesh_topology_feed.h`:
- Evidence kinds: Heard (receiver to node, not part of the graph), Link (a header proves the hop), Route (named by a route, not seen), Inferred (guess, dashed, off by default).
- Meshtastic: relay byte matched to known nodes; collisions share the weight, preferring nodes the receiver hears directly and nodes that already relay (edge flagged ambiguous, shown "?"); one-hop packets prove origin-relay links; MQTT packets ignored; flood amplification factor from copies per packet id. MeshCore: path hashes chain.
- Zigbee: MAC hop links, NWK originator vs transmitter (relay), source-route relay list (now parsed); Thread: MAC links (clear even when encrypted) and the 6LoWPAN mesh header originator. Z-Wave: route header decoded (type, repeaters, hop), acks prove links.
- Analysis per network: busiest relays, single points of failure with the nodes cut off from the root (coordinator/controller or best connected), one advice line. UI: Topology tab in the mesh, Zigbee/Thread and Z-Wave screens.
Measured (synthetic 30-node flooding mesh, 600 packets, 8 networks): relay attribution 0.94 / 0.91 / 0.88 for last bytes drawn from 30 / 16 / 10 values, uniform guessing 0.64 / 0.43 / 0.34; 30 % receiver loss changes it by under 0.015. Z-Wave routes and Zigbee MAC links: every link found at up to 60 % / 50 % frame loss, no invented links, the right single point reported.
Limits: Z-Wave route header layout from public descriptions, loopback only; nothing real captured. Zigbee/Thread nodes seen only by extended address are not merged with their short address.

## Wideband LoRa survey (hamvoice 17)
`lora::loraSurvey` (Signal Lab LoRa tab, "Survey all channels"): matched spectral detection at every LoRa bandwidth and two time scales, one dechirp bank per candidate for all SFs, sync-word search for heard-not-decoded combinations, classification (Meshtastic preset, MeshCore preset, LoRaWAN plan channel, LoRa APRS, private). Measured: 4 signals in one 2 MHz capture all found and classified at 0.36x real time; SF7 found down to -6 dB (same as the lab told the channel); SF12 to -12 dB (lab -18 dB, the gate's limit); no entries on 8 noise/carrier captures. Offline on a capture only, not yet a live panel.
