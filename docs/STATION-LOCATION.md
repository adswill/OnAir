# One station location (issue #453)

Settings > Location holds the one station position: latitude, longitude, height, locator and name. Every screen uses it as its default.

- Latitude, longitude, locator: edit any one, the others follow. The locator box accepts a locator or coordinates.
- Height (m): the pass planner's look angles and Doppler. The ADS-B screen shows the same value.
- Name: one label for the place.
- Screens that keep an optional own locator (PSK Reporter, WSPRnet, Winlink) show "using the global location" with `change` (opens Settings > Location) and `use another`. With their own locator they show `use the global location` to go back.
- Older saved values are folded in once at start-up: if no station position is set, the first non-empty one wins (UAT, RFI, set-up wizard, ham grid, Winlink, PSK Reporter, WSPRnet, in that order). A saved locator that differs from the station's square stays that screen's own, so nothing is lost.

Code: `core/include/dect2/geo/station_location.h` (pure helpers), `app/station/station_loc.{h,cpp}`. Test `station_location` (text to position, first-non-empty, own-locator rule, round trip): written, not yet run on CI.
