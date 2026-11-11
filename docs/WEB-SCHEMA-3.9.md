# OnAir Web 3.9: the panel schema (developers)

How every mode reaches the browser without a hand-written page per mode. docs/WEB-3.9.md is the user side.

## The idea

Every receiver in OnAir reports through one struct per mode inside `dect2::RxTelemetry` (core/include/dect2/t2/t2rx.h: `AdsbTelemetry adsb`,
`DmrTelemetry dmr`, ... 170 of them, plus the ones shared by families such as `WxsatTelemetry` for twelve weather satellites). The desktop app
draws each with its own ImGui panel. The web page cannot carry 290 panels, so the server exports the struct itself as JSON and a **schema**
tells the page what each field is, and the page renders every mode with the same handful of components.

`tools/gen_web_schema.py` reads the headers and generates `core/src/results/web_schema_gen.cpp` (1.4 MB of plain code, no templates):

```
headers (structs, nested rows, enums, trailing comments)
engine_telemetry.cpp  (case N: ... &RxTelemetry::member   -> standard N uses struct of member)
app/ui/mode_table.cpp (the 294 rows: family, name, group, blurb, sub id)
        |
        v
web_schema_gen.cpp:  ev_<Struct>(std::string&, const Struct&)   one writer per reachable struct (616)
                     kSchema_<Struct>[]                          the schema JSON of each struct
                     kStd[]      standard -> member, struct, schema, writer   (247 standards)
                     kRows[]     the mode rows with family, engine standard, telemetry standard, member
```

The quick gate runs `tools/gen_web_schema.py --check`, so the generated file cannot drift from the headers. Add a field to a telemetry struct,
rerun the script, and it is on the web.

## The panel object

`GET /app/v1/panel` and the WebSocket's `panel` message carry:

```json
{"seq": 1234, "data": {
  "standard": 12,
  "common": { "seq":.., "standard":12, "standardName":.., "state":2, "locked":true, "snrDb":14.2, "snrKind":"pulse", "cfoHz":0,
              "blocksOk":.., "blocksBad":.., "quality":{..}, "details":{..}, "sessions":{..} },
  "fields": { ...the mode's own struct, every public member... }
}}
```

`common` is `rxTelemetryJson()` (core/src/results/rx_json.cpp), the block every mode has; `details` holds the hand-written figures of the TV
and broadcast standards (DVB-T2, DVB-T, ATSC, DAB, FM, DVB-S/S2, DTMB, DVB-C). `fields` is the generated export. Standards 0 .. 6 (the TV
families) have no own struct: `fields` is `{}` and the page shows `details`.

### What the writer does with each member type

| C++ | JSON | note |
|---|---|---|
| `bool` | true / false | |
| integers, `float`, `double`, enums | number | NaN and infinity become `null`; integers print without a fraction |
| `uint64_t` | number | printed exactly (JavaScript reads up to 2^53 exactly) |
| `std::string` | string | valid UTF-8 only (a bad byte becomes `?`), control characters escaped, cut at 4096 bytes on a character boundary |
| `std::vector<number>`, C arrays, `std::array` | array | at most 2048 values |
| `std::vector<std::string>` | array of strings | at most 2048 |
| `std::vector<Struct>`, `deque`, `list` | array of objects (a table) | at most **200 rows**; the first 200 (the receivers keep the newest first) |
| `std::vector<cf32>` | flat `[i,q,i,q,...]` | subsampled to at most 512 points (a constellation) |
| nested struct | object | depth 6 at most |
| `std::pair<A,B>` | `[a,b]` | |
| `std::map<std::string,V>` | object | at most 200 keys; other key types become `[[k,v],...]` |
| `shared_ptr`, `unique_ptr`, `optional` | the value or `null` | |
| a struct with `w`/`h` (or width/height, cols/rows) and one `std::vector<uint8_t>` of pixels | an extra `"image"`: `data:image/png;base64,...` | grey when the vector has w*h bytes, RGB for 3*w*h; wider than 1024 px is box-downscaled; stored-deflate PNG (no zlib) |
| `std::function`, `std::mutex`, iterators, private members | skipped | the generator prints what it skipped |

The whole panel is capped at 256 KB: over it `fields` becomes `{"truncated":true}` (the common block always fits). The default
(all-zero) panel of the largest mode is under 64 KB (tests/web/test_web_schema.cpp measures it).

## The schema

`GET /app/v1/schema` returns `{"common":[...], "<standard>":[...], ...}`. Each entry is an array of field descriptors in declaration order:

```json
{"n":"msgsPerSec","k":"num","l":"Good messages per second, over the last second","u":""}
{"n":"aircraft","k":"table","l":"Most recently heard first","s":"dect2::AdsbAircraft","map":{"lat":"lat","lon":"lon","label":"callsign"}}
```

| key | meaning |
|---|---|
| `n` | the member name (the key in `fields`) |
| `k` | `num`, `bool`, `text`, `list` (numbers), `lines` (strings), `table`, `const` (I/Q points), `obj` (a nested block), `image` |
| `l` | the label: the member's trailing `//` comment up to its first `;`, `(` or ` - `, else the name split into words |
| `u` | the unit, from the name's suffix (`Db`, `Dbfs`, `Hz`, `Khz`, `Mhz`, `Ppm`, `Pct`, `Ms`, `Sec`, `Nm`, `Km`, `Kbps`, `Deg`, `Kts`, `Ft`) or a unit word in the comment |
| `s` | for `table` and `obj`: the C++ struct of the rows / block (for reading the code) |
| `map` | for a `table` whose rows have a latitude and a longitude member: which members, and which one labels the dot |

### Simple and Advanced

The page (web/onair-app/lib.js `buildPanel`) shows the common block first (SNR, lock, state, offset, block counts, the quality label), then
the mode's fields in declaration order. The first 10 plain fields (numbers, yes/no, text) are **Simple**; the rest need **Advanced**. Tables,
pictures, constellations, lists and text logs are always shown; an empty table is hidden in Simple and shown as "none yet" in Advanced.
Field order in the headers is therefore the one knob a mode author has: put the figures people want first.

## The page's components (web/onair-app/panels.js)

| kind | component |
|---|---|
| `num` | a tile with the value, unit and label; large numbers get thousands separators, Hz values switch to kHz / MHz |
| `bool` | a green / grey tile |
| `text` | a tile with wrapped text (logs up to 7 lines, scrollable) |
| `table` | a sortable table (click a heading), the newest 200 rows; **Map** button when the rows carry a position: a canvas with a degree grid and a dot per row (no map tiles: the page loads nothing from the internet) |
| `lines` | a monospace log that follows its end |
| `list` | a sparkline |
| `const` | an I/Q scatter plot |
| `image` | the picture |
| `obj` | a nested block rendered with the same rules |

The DOM is reused between updates: tiles, rows and cells keep their place and only changed text is touched, so a 2-updates-a-second panel
with a 200-row table stays smooth on a phone.

## Mode rows and numbers

`webModeRows()` (web_schema.h) is the mode table as the desktop app shows it, with three numbers per row:

* `family`: the mode table's number (app/ui/mode_table.cpp);
* `engineStd`: `Engine::setStandard()` and the headless server's mode number: `family + 2` from family 6 on; the TV families map to the
  server's own numbers (0 dvb, 3 atsc, 4 dab, 5 atsc3, 6 isdbt, 7 fm);
* `standard`: `RxTelemetry::standard`, the value the panel's `common.standard` carries: `family + 1` from family 6 on (the `case` values of
  `Engine::pollMode`), 0 .. 6 for the TV families.

`POST /app/v1/mode {"family":11,"sub":""}` selects by the table's numbers; `{"id":"adsb"}` by the engine's mode id.

### Rows without a panel block

290 of the 295 rows have a schema. The five without: Internet TV (IPTV: its state is not in RxTelemetry), ASTERIX (reports through the
sessions API, not telemetry), ISDB-TSB (new in 3.9, no telemetry block yet), Mode Builder and Plugins (not receivers). The seven TV / broadcast families show the common block and `details`.
tests/web/test_web_schema.cpp fails when a new row joins this list, so a new mode without telemetry is a decision, not an accident.

## Tests

* `tests/web/test_web_schema.cpp` (ctest `web_schema`): every mode row has a schema or is on the known list; for all 247 standards the export
  of a default report is valid JSON and every schema field appears in it; the writers (UTF-8, NaN, constellation subsampling, PNG round trip,
  the 200-row cap, the size cap, the map hint of the ADS-B aircraft table).
* `tests/server/test_webapp.cpp` (ctest `webapp`): the server side, including selecting every mode row through the API.
* `tests/web/app.test.js` (ctest `web_app_client`): the page's `buildPanel`, table columns, search, frames, the audio ring.

## Mode options

`tools/gen_web_options.py` (it imports the panel generator's header reader) writes `core/src/results/web_options_gen.cpp`:

```
engine_telemetry.cpp   standard -> RxTelemetry member (the same map as the panels)
engine.h               member name -> Engine accessor of that receiver (e.g. AdsbReceiver& adsb())
the receiver's header  public one-argument setters: void setX(bool | int | float | double | enum v);  // comment
        |
        v
web_options_gen.cpp:   kOpt_<standard>   the option schema (JSON)
                       set_<standard>    if (name == "setX") call(e.adsb(), &AdsbReceiver::setX, value)
```

| key | meaning |
|---|---|
| `n` | the setter's name |
| `k` | `bool`, `int`, `num`, `choice` (an enum, or an int whose comment reads "0 a, 1 b, 2 c"), `mask` (an int whose comment lists powers of two) |
| `l` | the setter's name split into words |
| `h` | the setter's comment (the help text) |
| `u` | the unit, from the name or the comment |
| `c` | for `choice` and `mask`: `[{"v":value,"l":label}]` |
| `t` | `s` Simple (the first four, declaration order) or `a` Advanced |

The schema leaves out:

* setters whose names say the engine, the radio settings or the app own them (offsets, centre, rates, gain, volume, mute, silent, sound
  devices, files and folders, keys and passwords, transmit, record, callbacks, taps, threads, sessions; the list is `EXCLUDE` in the
  script)
* overloaded setters
* static setters
* setters in a non-public section

`web_options.cpp` checks a value against its kind before the generated dispatch calls the setter (`applyOption`). The quick gate runs the
script with `--check`. A new setter in a receiver's header is on the web the next time the script runs. The way to keep an internal
setter off the page is a name the exclusion list matches.

## Adding to it

* A new telemetry field: nothing to do beyond the header (and `python3 tools/gen_web_schema.py`).
* A new widget kind: teach `field_schema()` in the generator to emit it, `buildPanel()` to pass it through and `panels.js` to draw it.
* A new mode: a `case` in `Engine::pollMode` and a row in the mode table are all the generator needs.
