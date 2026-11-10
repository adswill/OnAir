# Mesh node watch (3.9)

Round-4 item 27 (scratchpad r4-per-mode list; Meshtastic app notifications, MeshSense). Branch feat39-r4.

- In the Mesh node table, right-click a node and choose **Watch this node**. Watched nodes have a * before the name. The limits are in Mesh settings, **Node watch**:
  - **Silent after** N hours (6 by default)
  - **Battery below** X % (20 by default)
  - **New node within** N km of the receiver (off by default; needs the receiver position)
- New events, checked every 10 s:
  - `lora.node_silent`: a watched node has not been heard for N hours, once, and again with `back` when it is heard again. The time it was last heard is saved, so a node that stays silent across a restart is still noticed, also when it is no longer in the table.
  - `lora.node_battery`: a watched node's battery is below the limit. It warns again only after the battery has been 5 % above the limit.
  - `lora.node_new`: a node with a position heard for the first time this session, within the distance. The nodes of the first look after start do not count.
- Built-in recipes, off by default: **Mesh node silent**, **Mesh node battery low** and **New mesh node near me**.

## Tests
`tests/mesh/test_mesh_watch.cpp`:
- silence after 6 h, once, and back again
- the battery limit with its hysteresis
- new nodes: one near, not far, not without a position, not those of the first look
- silence across a restart
- the three recipes on the engine

Run locally: all passed. Simulated node tables only. The table menu and the settings were syntax-checked only.
