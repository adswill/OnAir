# BBC Red Button (UK MHEG-5 interactive TV) in OnAir

Status: design (stage 1 of 5). Nothing here has been run on a real UK broadcast: no public UK stream is available (see "What cannot be tested").

## What exists today

| Piece | File | State |
|---|---|---|
| DSM-CC object carousel (DSI, DII, DDB, BIOP, zlib modules) | `core/src/tv/dsmcc.cpp`, `include/dect2/tv/dsmcc.h` | works, files listable and savable, test carousel builder `buildObjectCarousel` |
| MHEG-5 BER structure parser, tree dump, BER encoder | `core/src/tv/mheg.cpp` | read-only, bounded |
| Static scene preview (Rectangle, Text, Bitmap) | `core/src/tv/mheg_render.cpp` | no links, no programs, no input |
| Carousel file viewer | `app/panels/carousel_view.cpp` | works |
| Interpreter | none | nothing executes |

The old branch `archive/2026-10-26/mheg5` holds an earlier `mheg5.cpp`, `biop.cpp`, `datacarousel.cpp` and `tests/tv/dsmcc_gen.h`. Those were folded into the current dsmcc code in main; the archive adds nothing needed for the interpreter and is not used.

## What UK Red Button needs (ETSI ES 202 184 UK DTT profile, ISO/IEC 13522-5 / ITU-T T.172)

The UK profile is "MHEG-5 with the ASN.1 BER interchange format" (not the textual notation). A BBC red button application is a set of files in a DSM-CC object carousel: a start-up `a` / `startup` Application object, Scenes, PNG/JPEG bitmaps, text files (UTF-8 or the UK subset of ISO 8859-1), CLUT-free true colour. Video is kept running behind the scene through a Stream/Video object (broadcast service video, `dvb://` URL), audio likewise.

### Object classes (implement in this order)
1. Required to show anything: Application, Scene, Rectangle (DynamicLineArt/LineArt), Text, Bitmap, Variable family (Boolean, Integer, OctetString, ObjectRef, ContentRef), Link, Timer, Stream with Video and Audio components, ResidentProgram.
2. Required for menus: Hotspot, PushButton, SwitchButton, HyperText (UK uses Hyperlink-style navigation heavily), Slider, ListGroup, TokenGroup, EntryField (used for text entry in some apps).
3. Skipped at first, flagged "unsupported" and ignored without failing the app: Cursor shape, InterchangedProgram, RemoteProgram, DynamicLineArt drawing actions beyond rectangle and line, Font class (the UK profile mandates the built-in Tiresias-like font; OnAir uses its own UI font, no font file is shipped).

### Actions and events
All 140+ ElementaryActions are in the ASN module; the interpreter implements the set the profile requires: Activate/Deactivate, Run/Stop (for Stream and Timer), SetData, SetVariable, Add/Subtract/Multiply/Divide/Modulo, Append, SetVisibility via SetPosition/MoveTo/BringToFront/SendToBack/PutBefore/PutBehind, SetFillColour/LineColour/TextColour/FontAttributes, SetBoxSize, SetTimer, SetVolume, SetVideoDecodeOffset, ScaleVideo, ScaleBitmap, Preload, TransitionTo, Launch, Spawn, Quit, CallActionSlot, SendEvent, Fork, GetEngineSupport, GetRunningStatus, GetTextData, GetListSize/GetItem..., Select/Deselect/Toggle, SetSliderValue, SetHighlightStatus, SetInteractionStatus, ReadPersistent/WritePersistent, plus Call (resident programs). Events: IsRunning, IsStopped, IsAvailable, ContentAvailable, TimerFired, UserInput (key codes), Anchor/HyperlinkFired, TestEvent, SelectionChanged, SliderValueChanged, StreamEvent, StreamPlaying/Stopped, CounterTrigger, EngineEvent (carousel updated), AsyncStopped, FocusGained/Lost.

Link semantics: a Link fires when its LinkEvent source raises the event and the optional EventData matches; its LinkEffect (ActionClass) runs to completion. Actions raised by actions are queued (asynchronous event queue), never recursive. This is the loop guard (below).

### Resident programs
`GetEngineSupport` (feature strings like `ICProfile=...`, `UKEngineProfile`, `DSMCC`, `MHEGProfile`), `Rnd`, `CTC` (clock to time), `CheckContentRef`, `CheckGroupIDRef`, `VideoToGraphics`, `SetWidescreenAlignment`, `SetSubtitleMode`, `GetCurrentDate`, `FormatDate`, `DayOfWeek`, `SI_GetServiceIndex`, `SI_TuneIndex`, `SI_TuneIndexInfo`, `BI_*`, `CI_*` (return failure: no interaction channel), `GetBootInfo`, `CheckTimerFired`. Persistent storage (`ReadPersistent` / `WritePersistent`, stores under `persistent/` files: each file limited and total capped; stored in the OnAir settings directory per service, never network).

### Keys
UK key codes: 1 up, 2 down, 3 left, 4 right, 5-14 digits 0-9, 15 select/OK, 16 cancel/back, 100 red, 101 green, 102 yellow, 103 blue, 104 text; other codes (help, EPG, info) are profile-defined and checked against the spec when the engine is written. The engine keeps the profile's InputEventRegister rules: only keys in the current scene's `InputEventRegister` (and the application's) are delivered; Red (100) starts the red button. OnAir mapping: R/G/Y/B keys and an on-screen red/green/yellow/blue strip, arrows, Enter (OK), Backspace/Esc (back), 0-9, T (text).

### Carousel acquisition and caching
UK start-up: the PMT carries an Application Information / data broadcast ID (the exact `data_broadcast_id` and component tag values of UK MHEG are taken from ES 202 184 and ETSI TS 101 812 and are unverified on air) and a `carousel_id`; the DSI/service gateway gives the root. The engine opens `startup` (BBC uses `//startup` or `~//a`). Objects are fetched on demand by their path (`DSM:` / `~/` / `//` / `rec://` is not supported), cached in memory by module id and version, and invalidated when the DII module version changes (a version change raises EngineEvent so a running app can reload). Missing content: the object is "unavailable"; Preload and ContentAvailable events are used; a bounded retry wait (carousel cycle up to ~30 s) then the app is told the content failed. No file or network access outside the carousel.

## Architecture in OnAir

```
TS packets (live demux, already in TV engine)
   -> DsmccDemux/DsmccCarousel (exists)             core/src/tv/dsmcc.cpp
   -> MhegCarouselFs  (path -> bytes, version)        core/src/tv/mheg_fs.{h,cpp}   [stage 3]
   -> MhegEngine (object model, event queue, links,
       variables, timers, resident programs)         core/src/tv/mheg_engine.{h,cpp} [stage 3]
   -> MhegScene output: ordered draw list + focus     core/src/tv/mheg_render.cpp (extend)
   -> TV window: ImGui draw list over the video       app/panels/ + tv window         [stage 4]
   <- keys: R/G/Y/B, arrows, OK, back, digits, mouse hit-test on hotspots/buttons
```

- The engine is single-threaded, deterministic and driven by `step(nowMs, keys, carousel)`; the GUI calls it each frame. No threads inside, so tests are exact.
- Parsing reuses `MhegParse`; stage 3 adds a typed object model built from the tree (own data structures, written from the standard and the ASN module, not from any other engine).
- Rendering is an immediate-mode draw list (rectangles, text with the engine's own font, bitmaps decoded by the existing image decoders). The scene is 720x576 logical, scaled to the TV window; video is drawn first, MHEG over it with the UK transparency rules.
- Red button affordance: when the PMT/AIT of the tuned service signals MHEG (data_broadcast_id of the UK MHEG profile), the TV window shows a small red button; clicking it or pressing R launches `startup`. Background fetch of the carousel starts when the service is tuned, off the audio/video path.
- Video/audio inside MHEG: the Stream object maps to the TV window's existing video (decoded through the user's own FFmpeg as now). OnAir ships no codec. MHEG-5 Stream with an MPEG-2/H.264 stream is the broadcast service itself, so there is nothing extra to decode.

## Safety bounds (stage 3)
- Event queue length cap, actions per event cap, total actions per frame cap (app is "stalled" and stopped past it), nesting depth of Fork/CallActionSlot, object count cap, variable octet string cap (64 KB), text cap, bitmap pixel cap, persistent storage cap, carousel file size cap.
- Any malformed BER, unknown tag, or oversize object fails that object only (logged), never aborts the process.
- No file system access except the carousel and the per-service persistent store; no sockets (interaction channel resident programs report "no return path").

## Licences

- ETSI ES 202 184, ISO/IEC 13522-5 / ITU-T T.172: specifications; read, not copied. The ASN.1 module tag numbers and names are facts of the standard (OnAir's existing `mheg.cpp` already cites them).
- redbutton-browser (Simon Kilvington, GPL-2.0-or-later per the headers that carry one, e.g. `utils.h`; most files have no header, so the grant is the repository `COPYING`, GPL-2). GPL-2.0-or-later is compatible with OnAir's GPL-3. For safety this project reuses none of its code: the interpreter is written from the standard and the ASN module. The architecture was studied (class-per-object C structs generated from the ASN module, a per-frame loop, a backend that fetches carousel files). `THIRD_PARTY.md` credits it for the ASN tag-name reference already in `mheg.h`. If any code is later copied, notices and authors are added at that moment.
- MythTV's MHEG engine: not consulted, not copied (licence not checked and not needed).
- BBC red button applications are broadcast content: none is copied or bundled. Test content is synthetic.

## Stages

| # | Stage | Result | Estimate |
|---|---|---|---|
| 1 | Design | this document | done |
| 2 | Test material | synthetic carousel generator with a small MHEG-5 test app (scene, text, bitmap, link, key action), tests/tv | 1 h |
| 3 | Interpreter core | object model, event/action engine, links, variables, timers, resident programs, limits | 3 to 4 h |
| 4 | Rendering and input | draw list in the TV window, key and mouse handling, red button, background carousel fetch | 2 to 3 h |
| 5 | Quality | impairment tests, hidden GUI screenshot, help text, strings, THIRD_PARTY, PATHWAY | 1.5 h |

## What cannot be tested without real UK streams
- Real BBC application structure (their use of Scene transitions, Stream objects, preloading, the `GetEngineSupport` strings they query, quirks of broken BER in the wild).
- Carousel timing and updates on air (module version changes, load time over a 2 Mbit/s data PID).
- The PMT/AIT signalling (data_broadcast_id values, component tags) on actual UK muxes.
- Interaction with the live video position (SetVideoDecodeOffset, ScaleVideo alignment, widescreen), subtitles toggle, and the text-service handover.
- Fonts: the UK engine font metrics decide text wrapping; our output can differ.
- Freeview HD vs SD apps, Freesat's different profile (Freesat needs Interactible and Slider; also not testable here).

The user's recordings of UK DVB-T/T2 or Freesat transport streams, when available, are the missing evidence; the debug path (carousel panel "save", `dsmcc_dump`) already lets a recording be examined.
