# Sub-GHz sensors (3.7)

Mode "Sub-GHz sensors" (family 241, engine standard 243): sensors on 433.92 MHz first, also 315, 868 and 915 MHz where the same protocols are used.
Default settings: 433.92 MHz, 250 kHz sample rate. Receive only. Works with an RTL-SDR or a HackRF (any input rate; up to 1.1 Msps the input is used
as it is, faster inputs are averaged down towards 1 Msps; 250 kHz is enough for most sensors, smart meters with a fast clock (ERT) want 1 Msps).

## What it is, honestly

The decoders are those of [rtl_433](https://github.com/merbanan/rtl_433) (GPL-2.0-or-later, state of f35183aaf), vendored in `third_party/rtl_433` with every
file header and its COPYING kept (credits: THIRD_PARTY.md, Settings > Credits). That is rtl_433's pulse detector (OOK / ASK by envelope, FSK by FM
discriminator), its pulse slicers (PWM, PPM, Manchester, DMC, PCM / NRZ / RZ, PIWM, ...) and its 377 decoder entries. OnAir adds the live list,
history, exports, the unknown signal analyser, custom decoders and the mode integration. **It does not decode "every sensor"**: it decodes the
protocols listed below, and only those; a sensor of another make shows up in "Unknown signals" at best.

## Screens

- **Sensors**: live list (model, ID, channel, temperature, humidity, pressure, wind, rain, battery, signal level, last heard). Click a row for the history
  graph of any of its measured values. "Remove after" (never, 1 / 5 / 15 min, 1 / 6 / 24 h) drops sensors not heard for that long.
  Export: CSV, JSON, and MQTT-style lines (`rtl_433/devices/<model>/<channel>/<id>/<value> <payload>`, as rtl_433's MQTT output names its topics).
- **Messages**: the decoded messages as rtl_433 JSON lines.
- **Unknown signals**: every pulse train that no decoder took (and that is clear enough): pulse / gap widths, modulation guess (PWM, PPM, Manchester, DMC,
  PCM), bit rows (binary and hex), and a flex decoder definition for it. The definition is editable, can be tested and saved ("Save decoder"); a saved
  decoder runs on every package from then on and reports its rows as a sensor under the name given (`n=`). Syntax: rtl_433's flex decoder (`-X`).
  The analyser slices the bits with rtl_433's own slicers and picks the candidate that gives the longest consistent row, length codes (PWM / PPM) first.
- **Decoders**: switch single decoders on or off (rtl_433's default set is on; the noisy ones it keeps off start unticked) and manage your own.

## Test result (rtl_433_tests, merbanan/rtl_433_tests, 2026-11-05)

`tools/sensors_vectors.py` runs this receiver over the recordings of that repository with the same rules as its `make test` (field `time` ignored, output of
other models counted as false positives and dropped, first output line compared with the first expected line, numbers equal to the 3 decimals rtl_433
prints). `tools/sensorstool` is the command line front end.

| | passing | of |
|---|---|---|
| IQ recordings (.cu8 / .cs8 / .cs16) | **2946** | 3031 (the 11 .cs16 files are not supported: 16-bit input is not wired) |
| bit-code tests (`codes_test.txt`) | 2299 | 2520 (71 directories) |
| protocols (decoded models) with a test | 318 pass all their tests, 15 pass some, 20 pass none | 353 |

For comparison, rtl_433 25.12 (Homebrew) on the same recordings passes 2545: the repository's recordings are newer than that release. The pulse detector
runs in rtl_433's "classic" FSK mode: with the "auto" mode of the vendored version only 2147 recordings passed (rtl_433's `-Y classic`).

Not passing at all (models): ADLM FPRF, AradMsMeter-Dialog3G, BMW-GEN5, Blueline-PowerCost, Curv, DY-SQ100B-WaterLeak-EV1527, Doorbell-Generic-B01M1KB4UM,
EN2058, Fanimation-Fan, Flowis, Hanwell-ML4000, Hosmart-HS002, JNStar-Beam, LandisGyr-GS, Lynx-Doorbell, Mercator, Olympia-9571, Swann-Doorbell,
TyreGuard400, Vivint-Security. Passing some: AmbientWeather-WH31E 16/17, Bresser-Gateway 5/9, Chamberlain-CWPIRC 78/88, Fineoffset-WH0290 3/5,
Generic-Remote 20/21, Honeywell-CM921 2/4, IDM 1/2, KNX-RF 5/11, NETIDM 3/4, Schrader-MRXBC5A4 3/4, SilverSpring-Mesh 210/238, Solight-TE44 1/6,
Typhur-SyncGold 19/21, Wireless-MBus 39/44, iMars-T240 39/40. These need a closer look (some recordings want rtl_433's other detector settings or
sample rates that are not wired here); not investigated yet.

What this does **not** show: how well it works on air. The recordings are rtl_433's own; no live signal and no recording of ours were tested. The
unit tests (`ctest -R sensors`) add synthetic OOK / FSK signals (Nexus-type messages at 250 kHz to 2 Msps, off the centre, in noise, FSK through a custom
decoder, the analyser, exports, "Remove after") and one real recording per family rebuilt as IQ from its pulse trains: Acurite, LaCrosse, Oregon
Scientific, Fine Offset / Ecowitt, Bresser, TFA, Nexus / Prologue, Hideki, Ambient Weather, DSC security (the Honeywell-CM921 FSK recording did not decode when rebuilt as IQ, so Honeywell is not
in the family test), tyre pressure (Schrader), smart meters (ERT), X10, Alecto / Esperanza. rtl_433 has no Davis decoder; it is not covered.

Receive only: nothing is transmitted. Encrypted or rolling-code remotes are shown as the codes they send, not decrypted.

## Decoders

Protocol numbers are rtl_433's. Needs the radio tuned to the sensor's band (433.92, 315, 868.3, 915 MHz and others; the decoder list says nothing about it).

- [1] Silvercrest Remote Control
- [2] Rubicson, TFA 30.3197 or InFactory PT-310 Temperature Sensor
- [3] Prologue, FreeTec NC-7104, NC-7159-675 temperature sensor
- [4] Waveman Switch Transmitter
- [6] ELV EM 1000 (off by default)
- [7] ELV WS 2000 (off by default)
- [8] LaCrosse TX Temperature / Humidity Sensor, TX5U rain gauge
- [10] Acurite 896 Rain Gauge
- [11] Acurite 609TXC Temperature and Humidity Sensor
- [12] Oregon Scientific Weather Sensor
- [13] Mebus 433 (off by default)
- [14] Intertechno 433 (off by default)
- [15] KlikAanKlikUit Wireless Switch
- [16] AlectoV1 Weather Sensor (Alecto WS3500 WS4500 Ventus W155/W044 Oregon)
- [17] Cardin S466-TX2
- [18] Fine Offset Electronics, WH2, WH5, Telldus Temperature/Humidity/Rain Sensor
- [19] Nexus, FreeTec NC-7345, NX-3980, Solight TE82S, TFA 30.3209 temperature/humidity sensor
- [20] Ambient Weather F007TH, TFA 30.3208.02, SwitchDocLabs F016TH temperature sensor
- [21] Calibeur RF-104 Sensor
- [22] X10 RF
- [23] DSC Security Contact
- [24] Brennenstuhl RCS 2044 (off by default)
- [25] Globaltronics GT-WT-02 Sensor
- [26] Danfoss CFR Thermostat
- [29] Chuango Security Technology (off by default)
- [30] Generic Remote SC226x EV1527
- [31] TFA-Twin-Plus-30.3049, Conrad KW9010, Ea2 BL999
- [32] Fine Offset Electronics WH1080/WH3080 Weather Station
- [33] WT450, WT260H, WT405H
- [34] LaCrosse WS-2310 / WS-3600 Weather Station
- [35] Esperanza EWS
- [36] Efergy e2 classic
- [37] Inovalley kw9015b, TFA Dostmann 30.3161 (Rain and temperature sensor) (off by default)
- [38] Generic temperature sensor 1
- [39] WG-PB12V1 Temperature Sensor
- [40] Acurite 592TXR temp/humidity, 592TX temp, 5n1, 3n1, Atlas weather station, 515 fridge/freezer, 6045 lightning, 899 rain, 1190/1192 leak
- [41] Acurite 986 Refrigerator / Freezer Thermometer
- [42] HIDEKI TS04 Temperature, Humidity, Wind and Rain Sensor
- [43] Watchman Sonic / Apollo Ultrasonic / Beckett Rocket oil tank monitor
- [44] CurrentCost Current Sensor
- [45] emonTx OpenEnergyMonitor
- [46] HT680 Remote control
- [47] Conrad S3318P, FreeTec NC-5849-913 temperature humidity sensor, ORIA WA50 ST389 temperature sensor
- [48] Akhan 100F14 remote keyless entry (off by default)
- [49] Quhwa
- [50] OSv1 Temperature Sensor
- [51] Proove / Nexa / KlikAanKlikUit Wireless Switch
- [52] Bresser Thermo-/Hygro-Sensor 3CH
- [53] Springfield Temperature and Soil Moisture
- [54] Oregon Scientific SL109H Remote Thermal Hygro Sensor
- [55] Acurite 606TX / Technoline TX960 Temperature Sensor
- [56] TFA pool temperature sensor
- [57] Kedsum Temperature & Humidity Sensor, Pearl NC-7415
- [58] Blyss DC5-UK-WH
- [59] Steelmate TPMS
- [60] Schrader TPMS
- [61] LightwaveRF (off by default)
- [62] Elro DB286A Doorbell (off by default)
- [63] Efergy Optical
- [64] Honda Car Key (off by default)
- [67] Radiohead ASK
- [68] Kerui PIR / Contact Sensor
- [69] Fine Offset WH1050 Weather Station
- [70] Honeywell Door/Window Sensor, 2Gig DW10/DW11, RE208 repeater
- [71] Maverick ET-732/733 BBQ Sensor
- [72] RF-tech (off by default)
- [73] LaCrosse TX141-Bv2, TX141TH-Bv2, TX141-Bv3, TX141W, TX145wsdth, (TFA, ORIA) sensor
- [74] Acurite 00275rm,00276rm Temp/Humidity with optional probe
- [75] LaCrosse TX35DTH-IT, TFA Dostmann 30.3155 Temperature/Humidity sensor
- [76] LaCrosse TX29IT, TFA Dostmann 30.3159.IT Temperature sensor
- [77] Vaillant calorMatic VRT340f Central Heating Control
- [78] Fine Offset Electronics, WH25, WH32, WH32B, WN32B, WH24, WH65, WS69, HP1000, Misol WS2320 Temperature/Humidity/Pressure Sensor
- [79] Fine Offset Electronics, WH0530 Temperature/Rain Sensor
- [80] IBIS beacon
- [81] Oil Ultrasonic STANDARD FSK
- [82] Citroen TPMS
- [83] Oil Ultrasonic STANDARD ASK
- [84] Thermopro TP11 Thermometer
- [85] Solight TE44/TE66, EMOS E0107T, NX-6876-917
- [86] Wireless Smoke and Heat Detector GS 558 (off by default)
- [87] Generic wireless motion sensor
- [88] Toyota TPMS
- [89] Ford TPMS
- [90] Renault TPMS
- [91] inFactory, nor-tec, FreeTec NC-3982-913 temperature humidity sensor
- [92] FT-004-B Temperature Sensor
- [93] Ford Car Key (off by default)
- [94] Philips outdoor temperature sensor (type AJ3650)
- [95] Schrader TPMS EG53MA4, Saab, Opel, Vauxhall, Chevrolet
- [96] Nexa
- [97] ThermoPro TP08/TP12/TP20 thermometer
- [98] GE Color Effects
- [99] X10 Security
- [100] Interlogix GE UTC Security Devices
- [101] Dish remote 6.3 (off by default)
- [102] SimpliSafe Home Security System (May require disabling automatic gain for KeyPad decodes)
- [103] Sensible Living Mini-Plant Moisture Sensor
- [104] Wireless M-Bus, Mode C&T, 100kbps (-f 868.95M -s 1200k)
- [105] Wireless M-Bus, Mode S, 32.768kbps (-f 868.3M -s 1000k)
- [106] Wireless M-Bus, Mode R, 4.8kbps (-f 868.33M) (off by default)
- [107] Wireless M-Bus, Mode F, 2.4kbps (off by default)
- [108] Hyundai WS SENZOR Remote Temperature Sensor
- [109] WT0124 Pool Thermometer
- [110] PMV-107J (Toyota) TPMS
- [111] Emos TTX201 Temperature Sensor
- [112] Ambient Weather TX-8300 Temperature/Humidity Sensor
- [113] Ambient Weather WH31E Thermo-Hygrometer Sensor, EcoWitt WH40/WN20 rain gauge, WS68 weather station
- [114] Maverick ET73
- [115] Honeywell ActivLink, Wireless Doorbell
- [116] Honeywell ActivLink, Wireless Doorbell (FSK)
- [117] ESA1000 / ESA2000 Energy Monitor, GIRA Wetterstation (off by default)
- [118] Biltema rain gauge (off by default)
- [119] Bresser Weather Center 5-in-1
- [120] Digitech XC-0324 / AmbientWeather FT005TH temp/hum sensor
- [121] Opus/Imagintronix XT300 Soil Moisture
- [122] FS20 / FHT (off by default)
- [123] Jansite TPMS Model TY02S (off by default)
- [124] LaCrosse/ELV/Conrad WS7000/WS2500 weather sensors
- [125] TS-FT002 Wireless Ultrasonic Tank Liquid Level Meter With Temperature Sensor
- [126] Companion WTR001 Temperature Sensor
- [127] Ecowitt Wireless Outdoor Thermometer WH53/WH0280/WH0281A
- [128] DirecTV RC66RX Remote Control
- [129] Eurochron temperature and humidity sensor (off by default)
- [130] IKEA Sparsnas Energy Meter Monitor
- [131] Microchip HCS200/HCS300 KeeLoq Hopping Encoder based remotes
- [132] TFA Dostmann 30.3196 T/H outdoor sensor
- [133] Rubicson 48659 Thermometer
- [134] AOK Weather Station rebrand Holman Industries iWeather WS5029, Conrad AOK-5056, Optex 990018
- [135] Philips outdoor temperature sensor (type AJ7010)
- [136] ESIC EMT7110 power meter
- [137] Globaltronics QUIGG GT-TMBBQ-05
- [138] Globaltronics GT-WT-03 Sensor
- [139] Norgo NGE101
- [140] Elantra2012 TPMS
- [141] Auriol HG02832, HG05124A-DCF, Rubicson 48957 temperature/humidity sensor
- [142] Fine Offset Electronics/Ecowitt WH51, WN31, SwitchDoc Labs SM23 Soil Moisture Sensor
- [143] Holman Industries iWeather WS5029 weather station (older PWM)
- [144] TBH weather sensor
- [145] WS2032 weather station
- [146] Auriol AFW2A1 temperature/humidity sensor
- [147] TFA Drop Rain Gauge 30.3233.01
- [148] DSC Security Contact (WS4945)
- [149] ERT Standard Consumption Message (SCM)
- [150] Klimalogg (off by default)
- [151] Visonic powercode
- [152] Eurochron EFTH-800 temperature and humidity sensor
- [153] Cotech 36-7959, SwitchDocLabs FT020T wireless weather station with USB
- [154] Standard Consumption Message Plus (SCMplus)
- [155] Fine Offset Electronics WH1080/WH3080 Weather Station (FSK)
- [156] Abarth 124 Spider and Shenzhen EGQ Q85 TPMS
- [157] Missil ML0757 weather station
- [158] Sharp SPC775 weather station
- [159] Insteon
- [160] ERT Interval Data Message (IDM)
- [161] ERT Interval Data Message (IDM) for Net Meters
- [162] ThermoPro-TX2 temperature sensor (off by default)
- [163] Acurite 590TX Temperature with optional Humidity
- [164] Security+ 2.0 (Keyfob)
- [165] TFA Dostmann 30.3221.02 T/H Outdoor Sensor (also 30.3249.02)
- [166] LaCrosse Technology View LTV-WSDTH01 Breeze Pro Wind Sensor
- [167] Somfy RTS
- [168] Schrader TPMS SMD3MA4 (Subaru)
- [169] Nice Flor-s remote control for gates (off by default)
- [170] LaCrosse Technology View LTV-WR1 Multi Sensor
- [171] LaCrosse Technology View LTV-TH Thermo/Hygro Sensor
- [172] Bresser Weather Center 6-in-1, 7-in-1 indoor, soil, new 5-in-1, 3-in-1 wind gauge, Froggit WH6000, Ventus C8488A
- [173] Bresser Weather Center 7-in-1, Air Quality PM2.5/PM10 7009970, CO2 7009977, HCHO/VOC 7009978 sensors
- [174] EcoDHOME Smart Socket and MCEE Solar monitor
- [175] LaCrosse Technology View LTV-R1, LTV-R3 Rainfall Gauge, LTV-W1/W2 Wind Sensor
- [177] Burnhard BBQ thermometer
- [178] Security+ (Keyfob)
- [179] Cavius smoke, heat and water detector
- [180] Jansite TPMS Model Solar
- [181] Amazon Basics Meat Thermometer
- [182] TFA Marbella Pool Thermometer
- [183] Auriol AHFL temperature/humidity sensor
- [184] Auriol AFT 77 B2 temperature sensor
- [185] Honeywell CM921 Wireless Programmable Room Thermostat
- [186] Hyundai TPMS (VDO)
- [187] RojaFlex shutter and remote devices
- [188] Marlec Solar iBoost+ sensors
- [189] Somfy io-homecontrol
- [190] Ambient Weather WH31L (FineOffset WH57) Lightning-Strike sensor
- [191] Markisol, E-Motion, BOFU, Rollerhouse, BF-30x, BF-415 curtain remote (off by default)
- [192] Govee Water Leak Detector H5054, Door Contact Sensor B5023
- [193] Clipsal CMR113 Cent-a-meter power meter
- [194] Inkbird ITH-20R temperature humidity sensor
- [195] RainPoint soil temperature and moisture sensor
- [196] Atech-WS308 temperature sensor
- [197] Acurite Grill/Meat Thermometer 01185M
- [198] EnOcean ERP1 (off by default)
- [199] Linear Megacode Garage/Gate Remotes (off by default)
- [200] Auriol 4-LD5661/4-LD5972/4-LD6313, Sempre 4-AH0423-4 temperature/rain sensors (off by default)
- [201] Unbranded SolarTPMS for trucks
- [202] Funkbus / Instafunk (Berker, Gira, Jung)
- [203] Porsche Boxster/Cayman TPMS
- [204] Jasco/GE Choice Alert Security Devices
- [205] Telldus weather station FT0385R sensors
- [206] LaCrosse TX34-IT rain gauge
- [207] SmartFire Proflame 2 remote control
- [208] AVE TPMS
- [209] SimpliSafe Gen 3 Home Security System
- [210] Yale HSA (Home Security Alarm), YES-Alarmkit
- [211] Regency Ceiling Fan Remote (-f 303.75M to 303.96M)
- [212] Renault 0435R TPMS
- [213] Fine Offset Electronics WS80 weather station
- [214] EMOS E6016 weatherstation with DCF77, EMOS E6018 temperature/humidity sensor
- [215] Emax W6, rebrand Altronics x7063/4/x7064A, Optex 990040/50/51, Orium 13093/13123, Infactory FWS-1200, Newentor Q9, Otio 810025, Protmex PT3390A, Jula Marquant 014331/32, TechniSat IMETEO X6 76-4924-00, Weather Station or temperature/humidity sensor
- [216] ANT and ANT+ devices (off by default)
- [217] EMOS E6016 rain gauge
- [218] Microchip HCS200/HCS300 KeeLoq Hopping Encoder based remotes (FSK)
- [219] Fine Offset Electronics WH45 air quality sensor
- [220] Maverick XR-30 BBQ Sensor
- [221] Fine Offset Electronics WN34S/L/D, WN38 and Froggit DP150/D35 temperature sensor
- [222] Rubicson Pool Thermometer 48942
- [223] Badger ORION water meter, 100kbps (-f 916.45M -s 1200k)
- [224] GEO minim+ energy monitor
- [225] TyreGuard 400 TPMS
- [226] Kia TPMS (-s 1000k)
- [227] SRSmith Pool Light Remote Control SRS-2C-TX (-f 915M)
- [228] Neptune R900 flow meters
- [229] WEC-2103 temperature/humidity sensor
- [230] Vauno EN8822C
- [231] Govee Water Leak Detector H5054
- [232] TFA Dostmann 14.1504.V2 Radio-controlled grill and meat thermometer
- [233] CED7000 Shot Timer (off by default)
- [234] Watchman Sonic Advanced / Plus, Tekelek
- [235] Oil Ultrasonic SMART FSK
- [236] Gasmate BA1008 meat thermometer
- [237] Flowis flow meters
- [238] Wireless M-Bus, Mode T, 32.768kbps (-f 868.3M -s 1000k)
- [239] Revolt NC-5642 Energy Meter
- [240] LaCrosse TX31U-IT, The Weather Channel WS-1910TWC-IT
- [241] EezTire E618, Carchet TPMS, TST-507 TPMS
- [242] Baldr / RainPoint rain gauge. (off by default)
- [243] Celsia CZC1 Thermostat
- [244] Fine Offset Electronics WS90 weather station
- [245] ThermoPro TX-2C, TX-2B Thermometer and Humidity sensor (off by default)
- [246] TFA 30.3151 Weather Station
- [247] Bresser water leakage
- [248] Nissan TPMS
- [249] Bresser lightning
- [250] Schou 72543 Day Rain Gauge, Motonet MTX Rain, MarQuant Rain Gauge, TFA Dostmann 30.3252.01/47.3006.01 Rain Gauge and Thermometer, ADE WS1907
- [251] Fine Offset / Ecowitt WH55 water leak sensor
- [252] BMW Gen4-Gen5 TPMS and Audi TPMS Pressure Alert, multi-brand HUF/Beru, Continental, Schrader/Sensata, Audi
- [253] Watts WFHT-RF Thermostat
- [254] Thermor DG950 weather station
- [255] Mueller Hot Rod water meter
- [256] ThermoPro TP28b Super Long Range Wireless Meat Thermometer for Smoker BBQ Grill
- [257] BMW Gen2 and Gen3 TPMS
- [258] Chamberlain CWPIRC PIR Sensor
- [259] ThermoPro Meat Thermometers, TP829B 4 probes with temp only
- [261] Geevon TX16-3 outdoor sensor
- [262] Fine Offset Electronics WH46 air quality sensor
- [263] Vevor Wireless Weather Station 7-in-1
- [264] Arexx Multilogger IP-HA90, IP-TH78EXT, TSN-70E
- [265] Rosstech Digital Control Unit DCU-706/Sundance/Jacuzzi
- [266] Risco 2 Way Agility protocol, Risco PIR/PET Sensor RWX95P
- [267] ThermoPro Meat Thermometers, TP828B 2 probes with Temp, BBQ Target LO and HI
- [268] Bresser Thermo-/Hygro-Sensor Explore Scientific ST1005H
- [269] DeltaDore X3D devices
- [270] Quinetic (off by default)
- [271] Landis & Gyr Gridstream Power Meters 9.6k
- [272] Landis & Gyr Gridstream Power Meters 19.2k
- [273] Landis & Gyr Gridstream Power Meters 38.4k
- [274] Revolt ZX-7717 power meter
- [275] GM-Aftermarket TPMS
- [276] RainPoint HCS012ARF Rain Gauge sensor
- [277] Apator Metra E-RM 30 water meter
- [278] ThermoPro TX-7B Outdoor Thermometer Hygrometer
- [279] Nexus, CRX, Prego sauna temperature sensor
- [280] Homelead HG9901 (Geevon, Dr.Meter, Royal Gardineer) soil moisture/temp/light level sensor
- [281] Maverick XR-50 BBQ Sensor
- [282] Orion Endpoint from Badger Meter, GIF2014W-OSE, water meter, hopping from 904.4 Mhz to 924.6Mhz (-s 1600k)
- [283] Fine Offset Electronics WH43 air quality sensor
- [284] Baldr E0666TH Thermo-Hygrometer
- [285] bm5-v2 12V Battery Monitor
- [286] Universal (Reverseable) 24V Fan Controller
- [287] Fine Offset Electronics WS85 weather station
- [288] Oria WA150KM freezer and fridge thermometer
- [289] Voltcraft EnergyCount 3000 (ec3k)
- [290] Orion Endpoint from Badger Meter, GIF2020OCECNA, water meter, hopping from 904.4 Mhz to 924.6Mhz (-s 1600k)
- [291] Geevon TX19-1 outdoor sensor
- [292] WallarGe CLTX001 Outdoor Temperature Sensor
- [293] Sainlogic SA8, Gevanti SA8 Weather Station
- [294] ThermoPro TempSpike XR TP862b / TP863b Wireless Dual-Probe Meat Thermometer
- [295] Airpuxem TPMS TYH11_EU6_ZQ
- [296] Apator Metra E-ITN 30 heat cost allocator
- [297] ThermoPro TP211B Thermometer
- [298] TRW TPMS OOK OEM and Clone models
- [299] TRW TPMS FSK OEM and Clone models
- [300] Govee Water Leak Detector H5059
- [301] Astrostart 2000 Car Remote (-f 372.4M)
- [302] Compustar 1WG3R Car Remote
- [303] Chrysler Car Remote (-f 315.1M -s 920k)
- [304] Nidec Car Remote (-f 313.8M -s 1024k) (off by default)
- [305] Audiovox PRO-OE3B Car Remote (-f 303.4M)
- [306] Microchip HCS361 KeeLoq Hopping Encoder based remotes (-f 315.1M) (Sync, 833 bit/s)
- [307] Microchip HCS361 KeeLoq Hopping Encoder based remotes (-f 315.1M) (Sync, 1667 bit/s)
- [308] Microchip HCS361 KeeLoq Hopping Encoder based remotes (-f 315.1M) (No Sync, 833 bit/s)
- [309] Microchip HCS361 KeeLoq Hopping Encoder based remotes (-f 315.1M) (No Sync, 1667 bit/s)
- [310] Microchip HCS361 KeeLoq Hopping Encoder based remotes (-f 315.1M) (PIWM, 2500 bit/s)
- [311] Microchip HCS361 KeeLoq Hopping Encoder based remotes (-f 315.1M) (PIWM, 5000 bit/s)
- [312] MIC 6SC2 Car Remote (-f 315.1M)
- [313] GM ABO1502T Car Remote (-f 314.9M)
- [314] Siemens 5WY72XX Car Remote (-f 315.1M)
- [315] Alps FWB1U545 Car Remote
- [316] Continental KR5V2X Car Remote (-f 313.8M -s 1024k)
- [317] Code Alarm FRDPC2002 Car Remote
- [318] RFM69 LowPowerLab Moteino board (-s 1000k)
- [319] Shenzhen Wale WL-TH6R Temperature & Humidity Sensor
- [320] Cellular Tracking Technologies LifeTag/PowerTag/HybridTag
- [321] Schrader Motorcycle TPMS sensor
- [322] Gear Hive TPMS sensor
- [323] LaCrosse Technology TX22U-IT
- [324] LaCrosse Technology TX22U-IT (OOK)
- [325] Thermor A6N 132TX temperature sensor
- [326] Acurite/Chaney 985 Refrigerator / Freezer Thermometer
- [327] Honda Keyfob KR5V2X/1X (-f 433.6M -s 1024k)
- [328] Schrader TPMS MRXBC5A4 (BMW)
- [329] Microchip HCS362 KeeLoq PWM
- [330] Microchip HCS362 KeeLoq MC
- [331] RF-T0912 Grill Thermometer
- [332] TR-502MSV remote smart socket controller
- [333] Opel Mokka Car Key
- [334] RfxMeter, RFXPwr
- [335] Martec MPLCD Ceiling Fan Remote
- [336] NetAtmo temp/hum and wind sensors
- [337] Omni Multisensor
- [338] Elster/Honeywell R2S/REXU power meter
- [339] Florabest FB-TH-1 BBQ Thermometer
- [340] Holman Industries iWeather WS5029 weather station (older PWM, OOK), BIOWIN 270208
- [341] Esun EN2053 two-channel BBQ thermometer
- [343] SmarTire TPMS sensor, Aston Martin/Vantage DB9 protocol
- [344] Dickert MAHS433-01 garage door remote control (off by default)
- [345] FSL Cricket Scoreboard Controller
- [346] EN2058 four probe temperature sensor
- [347] TFA Dostmann 30.390X T/H sensors series
- [348] Watts WFHT-RF / WFHC-MASTERH&C-RF underfloor heating thermostat
- [349] Govee Pool/Spa Thermometer H5310
- [350] Govee H5112 Dual-Probe Thermometer
- [351] Eco-Eye solar PV/grid current monitor
- [352] Schrader TPMS MRXNIS315G3, 3039 (Infiniti, Nissan, Renault), aka Redi-Sensor SE10001HP/SE10001HPR
- [353] Fine Offset Electronics / Ecowitt WH52 Soil Moisture/Temperature/EC Sensor
- [354] iMars T240 TPMS
- [355] Jansite TPMS TY-468-eu2 / KKMOON TPMS
- [356] Agimex Rosenborg 66796 (collides with Fine Offset Electronics WH5) Temperature/Humidity Sensor (off by default)
- [357] Auriol HG04641A temperature station
- [358] Eberle Instat 868r1 floor heating thermostat remote
- [359] Kingspan/Watchman Plus (Niveau) oil tank monitor
- [360] Baldr HCS528ARF Pool Thermometer sensor
- [361] Kidde RF-SM-DC wireless-interconnect smoke alarm (off by default)
- [362] Jansite TPMS TY588-EU2
- [363] Oregon Scientific WMR500 weather station
- [364] Elsner Solexa 230V
- [365] Mercedes Benz Sprinter 4500 TPMS sensor
- [366] Watts Vision thermostat (-f 868.3M)
- [367] TFA 30.3307.02 Wind sensor
- [368] Cavius Door/Window sensor (-f 868.7M)
- [369] Cotech FT0203/18-3676 anemometer (off by default)
- [370] 2GIG-KEY2E-345 encrypted keyfob (off by default)
- [371] Hanwell ML/RL4000-series Radiologger temperature/humidity sensor (off by default)
- [372] Elero bidirectional blinds/awning remote (Silent Gliss and others)
- [373] Elster/Honeywell R2S/REXU power meter, type-2 frames
- [374] LaCrosse WS6868 TX232TH-LCD temperature/humidity sensor
- [375] LaCrosse WS6868 TX231RW wind/rain sensor
- [376] McPower Kinetic battery-less wall switch
- [377] Typhur Sync Gold meat thermometer probe
- [378] Sefis M3 / Careud / Sykik SRTP300 TPMS
- [379] RADIAN/RADIAN0 meter
- [380] Jeep TPMS (off by default)
- [381] Honda (TRW PPA-GF33) TPMS
- [382] Cotech 36-7900 rain gauge (off by default)
- [383] Silver Spring Networks mesh endpoint (-s 1600k)
- [384] Bresser SmartHome Garden soil moisture and water timer valve (Baldr Homgar, RainPoint)
- [385] Neptune R900 BCD flow meters (off by default)
- [386] Olympia Protect 9571 alarm system sensors
