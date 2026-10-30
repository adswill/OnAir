# TV channels

Every TV mode (DVB-T/T2, ATSC 1.0 and 3.0, ISDB-T, DTMB, DVB-C and Analog TV) has a **Channel** selector next to the frequency. Pick a channel and OnAir
tunes its centre frequency and, where the mode has a width setting, sets the plan's channel width. The channel plan is data in
`core/src/channels/tvplan.cpp` (API: `core/include/dect2/channels/tvplan.h`); the selector is `app/bandplan/tvchan_ui.cpp`.

"Channel 2" is not one frequency worldwide. A channel number belongs to a plan, and a plan belongs to a standard and a region:

| Channel | US (ATSC) | Europe (DVB-T) | China (DTMB) | Japan (ISDB-T) |
|---|---|---|---|---|
| 2 | 54-60 MHz, centre 57 | no such channel (E2 is 47-54) | 56.5-64.5 MHz | none (1-3 are 90-108) |
| 21 | 512-518 MHz | 470-478 MHz, centre 474 | none in the VHF block, UHF starts at 13 | 518-524 MHz (+1/7 MHz) |

## Using it

* **Region**: the selector starts with the plan for the standard of the mode and your country (the setup wizard stores the country from your location).
  Change it under "Region" at the top of the list (or pick "Automatic (from your location)"): the choice is kept per computer, per kind of TV
  (DVB, ATSC, ISDB-T, DTMB, DVB-C, analog), and the wizard resets it when you set a new location.
* **Channel**: pick from the list, or type the number or label ("21", "E5", "S11", "9A") in the "type a channel" box at the top of the list and press Enter. The frequency box
  follows. The channel's name is shown on the selector whenever the frequency in the box is on a channel of the plan, however it got there
  (typed, scrolled, scanned or from a memory).
* **Previous / next**: the two small arrows step along the plan's channels from the current frequency; legacy channels are skipped unless you tick
  "Show reallocated channels".
* Nothing is locked: any frequency, bandwidth and sample rate can still be entered by hand. A channel changes the frequency and the width
  once, when picked; the width stays the mode's default if you never pick.
* Channels marked *legacy* in the list (greyed) were reallocated (for example the 700 MHz band) or exist only for analog. They are listed so they can be tuned.

## The plans

The ids are what `tvplan::find()` takes. "Default for" lists the countries that start on the plan; every other country starts on the first plan of the
standard (Europe for DVB, US for ATSC, Brazil for ISDB-T).

| id | Standard | Region and channels | Default for |
|---|---|---|---|
| `eu` | DVB-T, DVB-T2 | CEPT: E2-E4 47-68 MHz, E5-E12 174-230 MHz (7 MHz); UHF 21-69 470-862 MHz (8 MHz); 49-69 legacy | Europe, Turkey, Israel, Gulf, Egypt, South Africa, Nigeria, Kenya, Morocco, India, Pakistan, Bangladesh, Thailand, Singapore, Malaysia, Indonesia, Vietnam, New Zealand |
| `ru` | DVB-T, DVB-T2 | OIRT 6-12 174-230 MHz (8 MHz); UHF 21-69 | Russia, Belarus, Ukraine |
| `au` | DVB-T, DVB-T2 | VHF 6-12 with 9A, UHF 28-69 (7 MHz); 52-69 legacy | Australia |
| `am6-dvb` | DVB-T, DVB-T2 | the Americas 6 MHz raster without the ISDB offset (unverified mapping) | Colombia, Panama |
| `us` | ATSC, ATSC 3.0 | 2-4, 5-6, 7-13, 14-69 (6 MHz); 37 and 52-69 legacy | US, Canada, Mexico |
| `kr` | ATSC, ATSC 3.0 | 7-13, 14-69 (6 MHz); 52-69 legacy (unverified) | South Korea |
| `br` | ISDB-T (SBTVD) | the 6 MHz raster plus 1/7 MHz: 2-6, 7-13, 14-69; 52-69 legacy | Brazil, Argentina, Chile, Peru, Ecuador, Bolivia, Venezuela, Uruguay, Paraguay, Costa Rica, Philippines |
| `jp` | ISDB-T | UHF 13-62 plus 1/7 MHz; VHF 1-12 legacy; 53-62 legacy | Japan |
| `cn` | DTMB | DS1-DS12 VHF, 13-24 (470-566), 25-68 (606-958) at 8 MHz; above 862 legacy | China, Hong Kong, Macau |
| `cable-eu` | DVB-C | E2-E4, S1-S10 (7 MHz), E5-E12, S11-S20 (7 MHz), S21-S41 (8 MHz), UHF as U21-U69 | Europe |
| `an-bg` | Analog | PAL/SECAM B (VHF 7 MHz) and G (UHF 8 MHz), sound +5.5 MHz | western, northern, southern Europe, Gulf, South Asia, South-East Asia, New Zealand |
| `an-i` | Analog | PAL I, UHF 21-69, sound +6.0 MHz | UK, Ireland, South Africa |
| `an-dk` | Analog | D/K, OIRT 1-12 and UHF 21-69, sound +6.5 MHz | Russia, Eastern Europe, Vietnam |
| `an-cn` | Analog | PAL D, DS1-DS68, sound +6.5 MHz | China |
| `an-l` | Analog | SECAM L, UHF 21-69, AM sound +6.5 MHz | France |
| `an-m` | Analog | NTSC M / PAL M / PAL N, 2-83 (6 MHz), sound +4.5 MHz | US, Canada, Mexico, Brazil, Argentina, Chile, Colombia, Peru, Korea, Philippines |
| `an-j` | Analog | NTSC-J, VHF 1-12 and UHF 13-62, sound +4.5 MHz | Japan |
| `an-au` | Analog | PAL B, VHF 0-12 (5A, 9A) and UHF 28-69 (7 MHz) | Australia |

Analog TV: the selector tunes the **centre** of the channel (the mode finds the vision carrier by itself). The plans also hold the vision carrier
(1.25 MHz above the lower edge) and the sound carrier, so a channel's carriers are known: for example PAL I channel 21 is 470-478 MHz, vision 471.25, sound 477.25.

DAB and DAB+ have had a block selector for a long time (5A to 13F, `app/dab/dab_ui.cpp`); it is unchanged. DVB-S/S2 has no channel plan (transponders are found in
the spectrum). Not in the list on purpose: US/Canadian cable (EIA/CEA-542, ITU-T J.83 annex B is not decoded), Italy's VHF letter channels, Hong Kong's and Cuba's
own DTMB plans: no primary source was checked for them. Type the frequency.

## How it was checked

The tables are built from a lower channel edge, a width and a step, so every number can be compared with the sources line by line. The test
(`tests/channels/test_tvplan.cpp`, ctest `tvplan`) checks, for every plan: ascending order, no overlapping channels (Japan's VHF 7 and 8 overlap
by 2 MHz in the standard itself), widths of 5 to 8 MHz, unique labels, the round trip label to frequency to label, both edges inside the channel,
and a list of published channels with their edges (E5, UHF 21, US 2/14/36, Australia 9A/28, Japan 13/52/62, China 13/25/68, cable S1/S10/S11/S20/S21/S41 ...).
A second test (`setup_logic`) checks that every country of the setup wizard has a plan and that the plan's UHF width is the country's TV width.

**Sources** (a secondary table was used to cross-check the edges; the primary standards define the plans):

* Europe, VHF/UHF raster (CCIR/ITU system B, G): ITU-R Recommendation BT.470-6 "Conventional analogue television systems" and ITU-R BT.1306; CEPT/ITU Geneva 2006 (GE06) plan; ETSI EN 300 744 and EN 302 755 (the 7 and 8 MHz channels). Edges E2-E12, 21-69 confirmed against the Wikipedia table "Television channel frequencies" (CCIR system B rows E2-E12: DVB-T centres 50.5 ... 226.5).
* UK: Ofcom, "UK digital terrestrial television channel plan" (8 MHz UHF 21-69 on the same raster, 474 MHz centre for channel 21); analog system I: ITU-R BT.470-6.
* Russia/OIRT, system D/K: ITU-R BT.470-6; GOST 7845; edges confirmed against the same Wikipedia table (video carriers 49.75, 59.25, 77.25, 85.25, 93.25, 175.25 ... 223.25).
* Australia: ACMA, Australian Radiofrequency Spectrum Plan and AS/NZS 1367; channel table (centres 177.5, 184.5 ... 226.5, UHF 28 centre 529.5) confirmed against Wikipedia. The earlier built-in band plan had UHF 28 at 530 MHz, 0.5 MHz too high; it is corrected.
* USA/Canada/Mexico: ATSC A/65 and 47 CFR 73.603(a) (channels 2-69, 6 MHz; channel 37 radio astronomy, 52-69 reallocated by the 600 MHz and 700 MHz auctions); FCC repack. Edges confirmed (2: 54-60, 5: 76-82, 7: 174-180, 14: 470-476, 51: 692-698).
* South Korea: KCC and TTA ATSC channel plan (same 6 MHz raster); marked unverified in the data (`verified = false`) because only the raster is confirmed, not Korea's own list of used channels.
* Japan: ARIB STD-B31 (ISDB-T transmission, the +1/7 MHz centre offset), MIC channel plan: VHF 1-3 (90-108), 4-12 (170-222, channel 8 starting at 192 MHz, so 7 and 8 overlap), UHF 13-62; edges confirmed against Wikipedia.
* Brazil and ISDB-Tb countries: ABNT NBR 15601 (SBTVD), ANATEL channel plan 14-51 after the 700 MHz clearing; the same offset as Japan.
* China: GB 20600-2006 (DTMB); GB 4943 / GY/T 106 channel plan DS1-DS68 (DS1 48.5-56.5 ... DS12 215-223, DS13-24 470-566, DS25-68 606-958); edges confirmed against Wikipedia (DS-1 48.5-56.5 ... DS-68 video 951.25).
* Cable Europe: CENELEC EN 50083-1 / ITU-T J.83 annex A channel plan; S-channel video carriers (S1 105.25 ... S10 168.25, S11 231.25 ... S20 294.25, S21 303.25 ... S41 463.25) from the German Wikipedia "Kabelfernsehen" table, converted to channel edges with the 1.25 MHz vision offset (7 MHz up to S20, 8 MHz from S21). Many digital operators use their own raster (centres 114, 122, 130 ... MHz): the selector sets the plan's channel, the DVB-C panel keeps its raster list, and the frequency box tunes anything else.
* Analog sound carriers: ITU-R BT.470-6 Table 1 (B/G 5.5, I 6.0, D/K and L 6.5, M/N 4.5 MHz above the vision carrier).

Not verifiable from a primary text in this pass (they are marked or noted as such): the list of countries per plan is a default, not a legal statement. Several countries
mix standards (Eastern Europe used D/K and B/G, Cuba and the Philippines have their own details). Pick another plan in the list header whenever the default is wrong.

## Where it hooks in

* `app/bandplan/tvchan_ui.cpp`: `tvChannelCombo(App&)` (the selector for every TV mode; the toolbar calls it for DVB, ATSC, ISDB-T; the DTMB, DVB-C and Analog TV tuner rows call it too).
* `app/ui/setup_logic.h`: `Derived::tvPlan`, the plan the wizard's location gives (tested for every wizard country); `wizard.cpp` stores the country (pref `locCountry`) and `tvPlanFollowLocation()` clears any region you picked, so the selector starts from the new country (prefs `tvPlan.<kind>` hold your own pick).
* Band plans (`data/bandplans/builtin.json`) keep their own TV bands for the spectrum labels and the scan ranges; the selector uses its own, finer tables.
