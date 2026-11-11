# Smart features, round 5 (3.9): IoT, ISM and mesh

Branch `feat39-smart5c`. The list is in `scratchpad/smart37-backup/r5-avia-marine-iot.md`. The LoRa census, LoRaWAN
device health and wM-Bus leak check are on the feat39-smart5b list.

**Simulated data only.**

Privacy: the mesh coverage keeps grid cells, not nodes. The meter, tyre and home-network items follow only the IDs the
user marks as their own. Nothing is decrypted.

| # | Item | Where | What was measured |
|---|---|---|---|
| 11 | Mesh coverage map | `core/include/dect2/mesh/mesh_coverage.h`, LoRa mesh > Map (card under the node) | 4000 simulated receptions up to 14 km, log-distance loss, 2 / 6 dB fading, 30 % relayed. Near cells follow the path loss. The margin of far cells is more than 10 dB below that of near cells. The farthest cell is beyond 11 km. Relayed packets, 0/0, NaN and positions over 400 km away are not taken. SNR floors per spreading factor follow the SX127x data sheets. |
| 12 | My meter consumption | `core/include/dect2/sensors/meter_usage.h`, Sub-GHz sensors > Mine | 10 days at 1.5 / 0.5 kWh an hour (day / night, UTC+2), 0.1 kWh register steps, 0 / 50 % loss and repeats. At least 7 of 8 full days come out at 28 kWh +- 0.6; the hours split 24 / 4 kWh. A 5-digit wrap (99990 to 15) counts 25. A reset starts again. A 6 h gap is spread over its hours. |
| 13 | My tyres | `core/include/dect2/sensors/tpms_watch.h`, Sub-GHz sensors > Mine | Four tyres for 6 days with -7 to 17 C swings and 1 / 3 kPa noise. The leaking tyre (-5 kPa a day) is found at -5 +- 0.6 kPa a day and also flagged as below the others. The other three are clean. A stranger's sensor is never followed. The 20 C correction follows the gas law. |
| 14 | Home network health | `core/include/dect2/zwave/homenet_health.h`, Z-Wave > Health (pick your home ID) | Simulated network with retries using the same sequence number. The node behind a wall (40 % loss) is lossy, with retries and missing acks. A node that fades by 12 dB is "weaker". A node that stops is "silent" against its 5-minute interval. Healthy nodes are clean at 0 and 3 % loss. Frames without sequence numbers give silence only. |

## Notes

- **Zigbee:** the logic supports it, but the Zigbee frame list has no sequence number or ack request, so only level and
  silence would work. Since feat39-smartui the Zigbee receiver keeps the MAC sequence number and the ack request of each frame, so Zigbee > Health (and Thread) has all four: retries, missing acks (matched by sequence number within 1 s), level trend and silence (`zigbee_health.h`).
- **wM-Bus meters:** the Mine view covers rtl_433 meters (ERT SCM / IDM, energy and volume counters). The wM-Bus mode's
  own meters have their own Mine view since feat39-smartui (wM-Bus > Mine: the current Volume or Energy register; encrypted meters need the user's key).
- **Mesh positions:** a node's position is the last one it sent. A node that moves between position reports adds its
  SNR to its last position's cell. Only nodes heard directly (hop count 0) are counted.
- **Not kept between sessions:** the marks are kept in the settings (`sensMine`, `zwMyHome`). Usage, pressure history
  and coverage start again with each session.

## Tests

`mesh_coverage`, `sensors_mine`, `zwave_homenet`.
