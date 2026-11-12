// Every row of the Settings window: category, label, help, default, limits. No ImGui, no App (settings_logic.h), so the test can check it.
// The key is the stored name: for a field of App it is the key of app/state/prefs.cpp, so Settings and the old menus read and write the same value.
#pragma once
#include "settings_logic.h"
#ifndef N_
#define N_(s) s   // marks a text for the translation catalogs (app/i18n); tr() runs when it is shown
#endif

namespace settings {

// category names (the window translates them)
inline const char* const kGeneral = N_("General");
inline const char* const kAppearance = N_("Appearance");
inline const char* const kDisplay = N_("Spectrum and waterfall");
inline const char* const kRadio = N_("Radio");
inline const char* const kAudioC = N_("Audio");
inline const char* const kFiles = N_("Recording and files");
inline const char* const kDecoding = N_("Decoding");
inline const char* const kLocation = N_("Location");
inline const char* const kNetwork = N_("Network and updates");
inline const char* const kPrivacy = N_("Privacy");
inline const char* const kPlugins = N_("Plug-ins and scripts");
inline const char* const kAdvanced = N_("Advanced");
inline const char* const kUnits = N_("Units");                 // 4.0
inline const char* const kKeys = N_("Keyboard and mouse");     // 4.0
inline const char* const kNotify = N_("Notifications");        // 4.0
inline const char* const kPerf = N_("Performance");            // 4.0
inline const char* const kProfiles = N_("Profiles");           // 4.0

inline const std::vector<Info>& table() {
    using K = Kind;
    static const std::vector<Info> t = {
        // ---- General
        {"uiLang", kGeneral, N_("Language"), N_("The interface language. Same as the system follows the computer."), "translate locale", K::Custom},
        {"easyMode", kGeneral, N_("Easy mode"), N_("Every mode opens its Simple view: the spectrum and one screen with a Start button and the key controls. Off: the detailed tabs."), "simple advanced beginner one button start easy", K::Custom},
        {"regionPreset", kGeneral, N_("Region preset"), N_("One click sets the band plan, TV channels, FM band and step, default frequencies and units for a region (Lithuania, EU, ...)."), "region country lithuania eu europe units metric imperial band plan fm", K::Custom},
        {"setupRun", kGeneral, N_("First-run setup"), N_("Open the setup again: radio, region, antenna, interests, voice codecs."), "wizard first run setup codecs antenna", K::Link},
        {"modeVisLink", kGeneral, N_("Modes shown"), N_("Choose which modes the mode lists, picker and keys 1-9 show."), "hide favourites groups", K::Link},
        {"startMode", kGeneral, N_("Start-up mode"), N_("The mode OnAir opens with: the one you had last (default) or always the same one."), "startup launch begin mode open with", K::Custom},
        {"startFreqMode", kGeneral, N_("Start-up frequency"), N_("The frequency OnAir starts on: the last one (default) or a fixed frequency below."), "startup launch begin tune", K::Choice, 0, 0, 0, N_("The last frequency|A fixed frequency")},
        {"startFreqMhz", kGeneral, N_("Fixed start-up frequency (MHz)"), N_("Used when Start-up frequency is A fixed frequency."), "startup launch mhz", K::Float, 100, 0.001, 100000, nullptr, nullptr, "", false, false, "!%.6f"},
        {"autoStartRx", kGeneral, N_("Start the receiver when OnAir starts"), N_("Opens the radio you used last and starts receiving at once. Off (default): press Start."), "auto start launch radio open receive", K::Bool, 0},
        {"sessionRestore", kGeneral, N_("After a crash, restore the last session"), N_("When OnAir did not end normally: ask (default), restore it at once, or never offer it."), "crash restore session recover workspace", K::Choice, 0, 0, 0, N_("Ask|Always restore|Never offer")},
        {"confirmExit", kGeneral, N_("Confirm before quitting"), N_("Ask before the window closes: never (default), only while the radio receives or records, or always."), "quit exit close window ask", K::Choice, 0, 0, 0, N_("Never|While receiving or recording|Always")},
        {"discordOn", kGeneral, N_("Show in Discord"), N_("Rich Presence: tell Discord which mode you listen to. Off until you turn it on."), "presence", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"discordFreq", kGeneral, N_("Show the frequency in Discord"), N_("Include the tuned frequency in the Discord status."), "presence", K::Bool, 1, 0, 0, nullptr, nullptr, "", true},
        // ---- Appearance
        {"uiTheme", kAppearance, N_("Colour palette"), N_("Terminal, Instrument, Mono, Dev A (graphite) or Dev B (terminal green)."), "theme look colours", K::Choice, 3, 0, 0, "Terminal|Instrument|Mono|Dev A (graphite)|Dev B (terminal green)"},
        {"themeMode", kAppearance, N_("Light or high contrast"), N_("Dark (default), light, or black/white/yellow high contrast."), "theme light dark contrast accessibility", K::Custom},
        {"accent", kAppearance, N_("Own accent colour"), N_("Replace the accent colour of the interface."), "colour color theme", K::Custom},
        {"uiVariant", kAppearance, N_("Layout of the interface"), N_("Sidebar (default), Scope, Tiles, Faceplate and the Scope variants, Panel."), "layout variant", K::Choice, 0, 0, 0, "Sidebar|Scope|Tiles|Faceplate|Scope: Cinema|Scope: Meters|Scope: Dock|Panel"},
        {"layoutUnified", kAppearance, N_("Unified layout"), N_("Spectrum and waterfall stay above the decoded views instead of separate tabs."), "classic tabs", K::Bool, 0},
        {"uiTextScale", kAppearance, N_("Text size"), N_("Scale of all text and controls (also Ctrl+ and Ctrl-)."), "ui scale zoom font size", K::Float, 1, 0.8, 2, nullptr, nullptr, "", false, false, "%.2f"},
        {"uiTypography", kAppearance, N_("Font look"), N_("Current (default), Clean or Compact."), "typography fonts cjk", K::Custom, 0, 0, 0, nullptr, nullptr, "", false, true},
        {"restoreView", kAppearance, N_("Restore the default view"), N_("Back to the new interface, Sidebar layout, no touch mode, no docking. Also Ctrl+Shift+M and View > Restore the default view."), "reset layout menu bar hidden compact classic", K::Custom},
        {"touchUi", kAppearance, N_("Touch interface"), N_("Big buttons, an on-screen keypad and a full-screen mode picker for tablets."), "tablet android large rows", K::Bool, 0},
        {"touchScale", kAppearance, N_("Touch text size"), N_("Extra text scale of the touch interface."), "touch large", K::Float, 1.35, 1, 2, nullptr, nullptr, "", true},
        // ---- Units
        {"timeDisplay", kUnits, N_("Time display"), N_("Show times in UTC or in this computer's local time, with or without the zone label."), "utc local clock 24h 12h date time zone", K::Custom},
        {"adsbUnits", kUnits, N_("Aircraft units"), N_("ADS-B, Mode A/C and HFDL: aviation units (knots, feet, nautical miles) or metric (km/h or m/s, metres, km). Only what is shown and exported changes; the feeds stay in aviation units."), "distance altitude speed km mi nm nautical miles feet metres knots aviation metric", K::Choice, 0, 0, 0, N_("Aviation (kt, ft, nm)|Metric")},
        {"adsbSpeedUnit", kUnits, N_("Metric speed"), N_("km/h or m/s, when the aircraft units are metric."), "speed kmh ms", K::Choice, 0, 0, 0, "km/h|m/s"},
        // ---- Spectrum and waterfall
        {"wfMap3", kDisplay, N_("Waterfall colours"), N_("The colour map of the waterfall."), "palette colormap heat viridis", K::Choice, 15, 0, 0,
         "Classic|Viridis|Inferno|Grey|Turbo|Cividis|Magma|Deep heat|Ice|Fire|Night (red)|High contrast|Custom|Smooth heat|Deep blue|Navy heat|Blue green", "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"},
        {"wfAutoFloor", kDisplay, N_("Waterfall range follows the noise floor"), N_("The intensity range tracks the noise so weak signals show."), "auto level", K::Bool, 1},
        {"wfGamma", kDisplay, N_("Waterfall contrast curve"), N_("Below 1 lifts weak signals."), "gamma", K::Float, 1, 0.4, 2.5, nullptr, nullptr, "", true},
        {"wfSat", kDisplay, N_("Waterfall colour intensity"), N_("How vivid the waterfall colours are, apart from the level range: 0 is grey, 1 the map as it is, 2 twice as saturated."), "saturation colour vivid intensity", K::Float, 1, 0, 2, nullptr, nullptr, "", true},
        {"wfBright", kDisplay, N_("Waterfall brightness"), N_("Shifts every level."), "", K::Float, 0, -0.5, 0.5, nullptr, nullptr, "", true},
        {"wfContrast", kDisplay, N_("Waterfall contrast"), N_("Stretches the levels."), "", K::Float, 1, 0.25, 4, nullptr, nullptr, "", true},
        {"wfClock", kDisplay, N_("Clock time on the waterfall"), N_("Label rows with the clock time instead of seconds ago."), "time scale", K::Bool, 0},
        {"wfAvg", kDisplay, N_("Average slow rows"), N_("A slow waterfall shows the average of the frames in between, not the highest level."), "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"wfLowPass", kDisplay, N_("Waterfall noise reduction"), N_("A 2D smoothing of the waterfall."), "smooth", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"specAutoDb", kDisplay, N_("Spectrum range follows the noise floor"), N_("The dB range of the spectrum follows the view."), "auto db", K::Bool, 1},
        {"specPerHz", kDisplay, N_("Levels in dB per Hz"), N_("The noise floor stays put when the FFT size changes. Off: dB per bin."), "psd", K::Bool, 1, 0, 0, nullptr, nullptr, "", true},
        {"specDbm", kDisplay, N_("Show dBm instead of dBFS"), N_("Levels as dBm where the radio is calibrated."), "level units", K::Bool, 0},
        {"specFill", kDisplay, N_("Fill under the spectrum line"), N_("Colour the area under the spectrum line."), "", K::Bool, 1},
        {"specGrid", kDisplay, N_("Grid"), N_("Show the grid lines behind the spectrum."), "", K::Bool, 1},
        {"specPeaks", kDisplay, N_("Peak markers"), N_("Mark the strongest peaks on the spectrum."), "", K::Bool, 0},
        {"specReadout", kDisplay, N_("Level readout at the cursor"), N_("Show the level and frequency under the mouse pointer."), "", K::Bool, 1},
        {"specLine", kDisplay, N_("Spectrum line thickness"), N_("How thick the spectrum line is drawn."), "", K::Float, 1.2, 0.6, 3, nullptr, nullptr, "", true},
        {"specAvgSec", kDisplay, N_("Spectrum averaging (seconds)"), N_("Time constant of the averaged trace. 0 shows the raw frames."), "smoothing", K::Float, 0.08, 0, 5, nullptr, nullptr, "", true, false, "%.2f"},
        {"holdDecay", kDisplay, N_("Max-hold fall (dB per second)"), N_("How fast the max-hold line falls. 0 keeps it until reset."), "peak hold", K::Float, 4.5, 0, 60, nullptr, nullptr, "", true, false, "%.1f"},
        {"specFft", kDisplay, N_("FFT size"), N_("Bins of the spectrum. Automatic picks 8192 to 32768 from the sample rate; more bins show finer detail and cost more CPU."), "fft bins resolution rbw", K::Choice, 0, 0, 0, N_("Automatic|4096|8192|16384|32768|65536|131072|262144"), "0,4096,8192,16384,32768,65536,131072,262144"},
        {"wfRate", kDisplay, N_("Waterfall speed"), N_("Rows per second. Every frame (default) is about 30; slower rows each keep the highest level of the frames in between."), "waterfall speed rows scroll slow fast time", K::Choice, 0, 0, 0, N_("Every frame|60 rows/s|10 rows/s|5 rows/s|2 rows/s|1 row/s|1 row per 10 s|1 row per minute"), "0,60,10,5,2,1,0.1,0.0166667"},
        {"specHold", kDisplay, N_("Max-hold line"), N_("A second line that keeps the highest level, falling at the rate above."), "peak hold max", K::Bool, 0},
        {"minHold", kDisplay, N_("Min-hold line"), N_("A line under the signals at the lowest level (the noise floor)."), "min hold floor", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"specNfMarker", kDisplay, N_("Noise floor line"), N_("A line at the noise floor with its level."), "noise floor marker", K::Bool, 0},
        {"specLabels", kDisplay, N_("Station names on the spectrum"), N_("Names from the band plan, the frequency database, FM, DAB, DVB and the memories on the spectrum and the waterfall."), "labels names band plan", K::Bool, 1},
        {"traceColor", kDisplay, N_("Spectrum line colour"), N_("The colour of the spectrum line. The colour-blind safe ones read for everybody."), "trace colour color line cb colour-blind", K::Choice, 0, 0, 0, N_("Soft white|Amber|Green|Cyan|Yellow|Orange (CB safe)|Sky blue (CB safe)|Pink|Light blue|Pale cyan|Pale steel")},
        {"holdColor", kDisplay, N_("Max-hold line colour"), N_("The usual grey, or one of the line colours."), "hold colour color", K::Choice, -1, 0, 0, N_("Grey (default)|Soft white|Amber|Green|Cyan|Yellow|Orange (CB safe)|Sky blue (CB safe)|Pink|Light blue|Pale cyan|Pale steel"), "-1,0,1,2,3,4,5,6,7,8,9,10", "", true},
        {"specSmoothTrace", kDisplay, N_("Smooth the spectrum line"), N_("Mild smoothing passes over the line; As the look keeps the look's own choice."), "smooth trace line", K::Choice, -1, 0, 0, N_("As the look|Off|1|2|3|4"), "-1,0,1,2,3,4", "", true},
        {"specAxisLabels", kDisplay, N_("Frequency scale"), N_("Frequencies, or offsets from the tuned frequency."), "axis scale offset labels", K::Choice, -1, 0, 0, N_("As the look|Frequencies|Offsets from the tuned frequency"), "-1,0,1", "", true},
        {"specLabelSize", kDisplay, N_("Scale text size"), N_("Size of the scale numbers, times the look's own."), "axis font labels size", K::Float, 1, 0.7, 1.6, nullptr, nullptr, "", true},
        {"specSmoothZoom", kDisplay, N_("Smooth zoom"), N_("The wheel zoom eases in over a few frames. Off: it jumps."), "zoom animation ease", K::Bool, 1, 0, 0, nullptr, nullptr, "", true},
        {"wfFloorBelow", kDisplay, N_("Waterfall floor below the noise (dB)"), N_("With the range following the noise floor: how far under the noise the darkest colour sits."), "auto floor level", K::Float, 4, 0, 40, nullptr, nullptr, "", true, false, "%.1f"},
        {"wfFloorSpan", kDisplay, N_("Waterfall range (dB)"), N_("With the range following the noise floor: from the darkest to the brightest colour."), "auto range span", K::Float, 55, 20, 120, nullptr, nullptr, "", true, false, "%.0f"},
        {"specDefLo", kDisplay, N_("Default range: bottom (dB)"), N_("Where the spectrum's scale starts after Reset and the range key (Ctrl+Home by default)."), "default db range min bottom reset", K::Float, -110, -160, 0, nullptr, nullptr, "", true, false, "%.0f"},
        {"specDefHi", kDisplay, N_("Default range: top (dB)"), N_("Where the spectrum's scale ends after Reset and the range key."), "default db range max top reset", K::Float, -30, -150, 20, nullptr, nullptr, "", true, false, "%.0f"},
        {"dbKeyStep", kDisplay, N_("Range keys step (dB)"), N_("How far the range keys (Ctrl+Up and Ctrl+Down by default) move the scale."), "db step keys range", K::Float, 5, 1, 20, nullptr, nullptr, "", true, false, "%.0f"},
        // ---- Radio
        {"tuningMode", kRadio, N_("Spectrum click"), N_("Retune the radio or only move the decode offset (sticky LO)."), "tune lo", K::Choice, 0, 0, 0, "Retune the radio|Move the decode offset only"},
        {"centreTuning", kRadio, N_("Click centres the radio"), N_("A click on the spectrum puts the radio centre there."), "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"specDrag", kRadio, N_("Drag on spectrum"), N_("Tune: press on the spectrum or waterfall to tune there and drag to keep tuning, Shift or Ctrl for fine steps; drag the frequency scale, right- or middle-drag, or the wheel over the scale to pan. Pan: a drag moves the view and a click tunes (the old OnAir)."), "drag tune pan sdr++ vfo mouse", K::Choice, 0, 0, 0, "Tune (like SDR++)|Pan (old OnAir)"},
        {"dragMovesTuned", kRadio, N_("Dragging the band moves the tuned frequency"), N_("When you drag the view past the edge of the band, the tuned frequency moves with the radio. Off: only the radio centre moves."), "pan", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"specSnapHz", kRadio, N_("Click snaps to a multiple of (Hz)"), N_("0 uses the mode's own step."), "step snap", K::Int, 0, 0, 1000000, nullptr, nullptr, "", true},
        {"bwAuto", kRadio, N_("Measure the channel width by itself"), N_("The engine measures the width and switches (DVB)."), "bandwidth auto", K::Bool, 1},
        {"agcOn", kRadio, N_("Automatic gain"), N_("The radio's gain is set by itself (Auto in the gain control); off: the gains you set."), "agc auto gain lna vga", K::Bool, 0},
        {"dcRemove", kRadio, N_("Remove the DC spike"), N_("Takes the centre spike of the radio out of the samples."), "dc offset spike centre", K::Bool, 0},
        {"iqCorrect", kRadio, N_("Correct IQ imbalance"), N_("Removes the mirror image a radio's IQ imbalance makes."), "iq imbalance image mirror", K::Bool, 0},
        {"offsetTune", kRadio, N_("Tune off-centre by itself"), N_("Keeps the wanted signal away from the radio's centre spike."), "offset tuning dc lo", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"spurNotch", kRadio, N_("Notch fixed spurs"), N_("Finds carriers that never move (the radio's own) and removes them."), "spur birdie notch", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"nbOn", kRadio, N_("Noise blanker"), N_("Cuts short pulses (ignition, power lines)."), "noise blanker impulse nb", K::Bool, 0},
        {"nbThr", kRadio, N_("Noise blanker threshold"), N_("Times the mean level that counts as a pulse. Lower cuts more."), "noise blanker", K::Float, 8, 2, 40, nullptr, nullptr, "", true, false, "%.1f"},
        {"nbWidthUs", kRadio, N_("Noise blanker width (us)"), N_("How long each cut is."), "noise blanker", K::Float, 20, 2, 2000, nullptr, nullptr, "", true, false, "%.0f"},
        // ---- Audio
        {"popTop", kAudioC, N_("Video pop-out stays on top"), N_("The pop-out video window stays above other windows."), "window", K::Bool, 1},
        {"subsOn", kAudioC, N_("Show subtitles"), N_("Subtitles of the video modes, when the station sends them."), "teletext captions", K::Bool, 1},
        {"surroundLayout", kAudioC, N_("TV sound speakers"), N_("Automatic plays 5.1 or 7.1 TV sound (Dolby Digital, Dolby Digital Plus, the bed of Dolby Atmos) on as many speakers as the output device has, and mixes it to stereo otherwise. Or always stereo, stereo with Dolby Surround (Lt/Rt) matrix for a surround decoder, 5.1 or 7.1."), "dolby atmos surround 5.1 7.1 downmix ac-3 e-ac-3 eac3 channels speakers", K::Choice, 0, 0, 0, N_("Automatic (the device's speakers)|Stereo|Stereo, Dolby Surround (Lt/Rt)|5.1|7.1")},
        {"dolbyPassthrough", kAudioC, N_("Pass Dolby audio through to my receiver (bitstream, experimental)"), N_("Sends Dolby Digital and Dolby Digital Plus (with its Dolby Atmos) untouched over HDMI or optical to an AV receiver or soundbar, which decodes it: OnAir decodes nothing. Works only with devices that accept a bitstream; a laptop's own speakers cannot take it. Not tested on real hardware yet. The volume is then set on the receiver."), "dolby atmos passthrough bitstream hdmi spdif optical receiver soundbar iec 61937 e-ac-3 ac-3", K::Bool, 0},
        {"audioDevDefault", kAudioC, N_("Output device"), N_("The sound output for every mode that has not picked its own in its Audio pop-up. Default: the system's."), "sound card speakers headphones output device", K::Custom},
        {"aclDefAgc", kAudioC, N_("Audio AGC for new modes"), N_("The audio level control a mode starts with the first time you use it. Each mode keeps its own after that."), "agc level loudness default", K::Choice, 0, 0, 0, N_("Off|Fast|Slow")},
        {"aclDefSq", kAudioC, N_("Squelch for new modes"), N_("The squelch a mode starts with the first time you use it: off, by level, or by speech-to-noise."), "squelch default mute noise", K::Choice, 0, 0, 0, N_("Off|Level|Speech to noise")},
        {"aclDefSqDb", kAudioC, N_("Squelch opens at (dB)"), N_("Level squelch: dBFS. Speech to noise: dB."), "squelch threshold", K::Float, -50, -120, 40, nullptr, nullptr, "", true, false, "%.0f"},
        {"fmBass", kAudioC, N_("FM bass (dB)"), N_("Tone control of FM broadcast."), "tone eq bass", K::Int, 0, -6, 6},
        {"fmTreble", kAudioC, N_("FM treble (dB)"), N_("Tone control of FM broadcast."), "tone eq treble", K::Int, 0, -6, 6},
        {"fmWarm", kAudioC, N_("FM warm sound"), N_("A gentle -3 dB above 6 kHz."), "tone warm", K::Bool, 0},
        {"fmAudioBw", kAudioC, N_("FM audio bandwidth"), N_("Full is 15 kHz; narrower is quieter on a weak station."), "audio bandwidth filter", K::Choice, 0, 0, 0, N_("Full (15 kHz)|12 kHz|8 kHz|5 kHz"), "0,12000,8000,5000"},
        {"smoothGaps", kAudioC, N_("Hide short drop-outs"), N_("Fills short gaps in TV and DAB sound instead of a click."), "conceal gaps drop click", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"audioRate", kAudioC, N_("Sound card rate (Hz)"), N_("The sample rate of the sound card input and output."), "sample rate sound card", K::Choice, 48000, 0, 0, "8000|16000|22050|24000|32000|44100|48000|96000|192000", "8000,16000,22050,24000,32000,44100,48000,96000,192000", "", true, true},
        {"iecDevice", kAudioC, N_("Passthrough output"), N_("The output that goes to the receiver. Empty: the TV sound's output. Linux: an ALSA device such as hdmi:CARD=0,DEV=0 (the receiver's HDMI port)."), "passthrough bitstream device alsa hdmi", K::Text, 0, 0, 0, nullptr, nullptr, "", true},
        // ---- Recording and files
        {"recFormat", kFiles, N_("IQ recording format"), N_("8-bit (small), float (exact) or X-Midas BLUE .tmp with float samples."), "iq record file", K::Choice, 0, 0, 0, "8-bit|Float|X-Midas BLUE (.tmp)|16-bit"},
        {"recordingsFolder", kFiles, N_("Recordings folder"), N_("Where recordings (IQ, audio, images, logs) are saved. Choose your own folder; default is inside the data folder."), "folder directory path open recordings", K::Custom},
        {"dataFolder", kFiles, N_("Data folder"), N_("Where recordings, layouts and settings live."), "folder directory path open", K::Custom},
        {"recNamePattern", kFiles, N_("IQ file name"), N_("Tokens: {mode} {freq} (MHz) {khz} {rate} (Msps) {date} {time} {utcdate} {utctime}. Empty: onair_{mode}_{freq}MHz_{rate}Msps_{date}-{time}. Keep {freq}MHz and {rate}Msps so OnAir can read the file back."), "file name pattern tokens naming", K::Text, 0, 0, 0, nullptr, nullptr, ""},
        {"recSplitMb", kFiles, N_("Split IQ recordings every (MB)"), N_("Starts a new file when one reaches this size. 0: one file."), "split size chunk", K::Int, 0, 0, 1000000},
        {"recMinFreeMb", kFiles, N_("Stop recording below free space (MB)"), N_("An IQ recording stops when the disk has less than this left. 0: only when it is full."), "disk space guard free full", K::Int, 200, 0, 1000000},
        {"recSigmf", kFiles, N_("SigMF metadata beside IQ files"), N_("A .sigmf-meta file with the frequency, rate and mode next to each recording."), "sigmf meta", K::Bool, 1},
        {"audioRecFmt", kFiles, N_("Audio recording format"), N_("WAV, FLAC or MP3 (MP3 needs the LAME library)."), "audio record wav flac mp3", K::Choice, 0, 0, 0, "WAV|FLAC|MP3"},
        {"audioRecGate", kFiles, N_("Record audio only while there is sound"), N_("Leaves the silence out."), "gate vox silence", K::Bool, 0},
        {"audioRecSplit", kFiles, N_("One audio file per transmission"), N_("A new file each time the sound starts again."), "split vox", K::Bool, 0},
        {"audioRecKeep", kFiles, N_("Keep recording audio across retunes"), N_("Off: a retune ends the file."), "retune", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        // ---- Decoding
        {"fmMono", kDecoding, N_("FM: force mono"), N_("A weak station is quieter in mono."), "stereo", K::Bool, 0},
        {"fmStereo", kDecoding, N_("FM stereo"), N_("Auto, mono, or stereo whenever a pilot is there."), "", K::Choice, 0, 0, 0, "Auto|Mono|Stereo"},
        {"fmDeemph", kDecoding, N_("FM de-emphasis"), N_("50 us (Europe, most of the world) or 75 us (Americas); Off for a flat response."), "tone", K::Choice, 50, 0, 0, "Off|50 us|75 us", "0,50,75"},
        {"fmRegion", kDecoding, N_("RDS meanings"), N_("Auto picks RBDS for odd tenths of a MHz; or force RDS (Europe) or RBDS (US)."), "rbds", K::Choice, 0, 0, 0, "Auto|RDS (Europe)|RBDS (US)"},
        {"fmSquelch", kDecoding, N_("FM squelch (dB SNR)"), N_("0 is off."), "", K::Int, 0, 0, 40},
        {"staleVdl2", kDecoding, N_("Remove after: VDL2"), N_("How long after it was last heard an entry stays in the list and on the map. Default is the rule of the mode; Never keeps everything."), "age expire stale drop forget remove aircraft vessel station", K::Choice, -1, 0, 0, N_("Default|1 min|5 min|30 min|1 h|6 h|24 h|Never|Custom"), "-1,1,5,30,60,360,1440,0,-2"},
        {"staleAcars", kDecoding, N_("Remove after: ACARS"), N_("How long after it was last heard an entry stays in the list and on the map. Default is the rule of the mode; Never keeps everything."), "age expire stale drop forget remove aircraft vessel station", K::Choice, -1, 0, 0, N_("Default|1 min|5 min|30 min|1 h|6 h|24 h|Never|Custom"), "-1,1,5,30,60,360,1440,0,-2"},
        {"staleHfdl", kDecoding, N_("Remove after: HFDL"), N_("How long after it was last heard an entry stays in the list and on the map. Default is the rule of the mode; Never keeps everything."), "age expire stale drop forget remove aircraft vessel station", K::Choice, -1, 0, 0, N_("Default|1 min|5 min|30 min|1 h|6 h|24 h|Never|Custom"), "-1,1,5,30,60,360,1440,0,-2"},
        {"staleAdsb", kDecoding, N_("Remove after: ADS-B"), N_("How long after it was last heard an entry stays in the list and on the map. Default is the rule of the mode; Never keeps everything."), "age expire stale drop forget remove aircraft vessel station", K::Choice, -1, 0, 0, N_("Default|1 min|5 min|30 min|1 h|6 h|24 h|Never|Custom"), "-1,1,5,30,60,360,1440,0,-2"},
        {"staleAis", kDecoding, N_("Remove after: AIS"), N_("How long after it was last heard an entry stays in the list and on the map. Default is the rule of the mode; Never keeps everything."), "age expire stale drop forget remove aircraft vessel station", K::Choice, -1, 0, 0, N_("Default|1 min|5 min|30 min|1 h|6 h|24 h|Never|Custom"), "-1,1,5,30,60,360,1440,0,-2"},
        {"staleUat", kDecoding, N_("Remove after: UAT"), N_("How long after it was last heard an entry stays in the list and on the map. Default is the rule of the mode; Never keeps everything."), "age expire stale drop forget remove aircraft vessel station", K::Choice, -1, 0, 0, N_("Default|1 min|5 min|30 min|1 h|6 h|24 h|Never|Custom"), "-1,1,5,30,60,360,1440,0,-2"},
        {"staleInmsat", kDecoding, N_("Remove after: Inmarsat"), N_("How long after it was last heard an entry stays in the list and on the map. Default is the rule of the mode; Never keeps everything."), "age expire stale drop forget remove aircraft vessel station", K::Choice, -1, 0, 0, N_("Default|1 min|5 min|30 min|1 h|6 h|24 h|Never|Custom"), "-1,1,5,30,60,360,1440,0,-2"},
        {"staleCustomMin", kDecoding, N_("Custom remove-after time (minutes)"), N_("Used by every Remove after row set to Custom."), "age expire stale custom", K::Int, 120, 1, 100000},
        {"ttxPage", kDecoding, N_("Teletext start page"), N_("The page the teletext view opens on."), "ttx", K::Int, 100, 100, 899, nullptr, nullptr, "", true},
        {"tvLang", kDecoding, N_("Preferred TV language"), N_("Language code for audio and subtitle choice, e.g. eng."), "subtitle audio", K::Text, 0, 0, 0, nullptr, nullptr, "", true},
        {"tvBufMb", kDecoding, N_("TV pause buffer (MB)"), N_("How much of the TV service is kept so Pause can go on later."), "timeshift pause buffer live tv", K::Int, 256, 16, 4096, nullptr, nullptr, "", true},
        // ---- Location
        {"adsbLat", kLocation, N_("Station latitude"), N_("Degrees, north positive. Used by the maps, passes, ADS-B and distances."), "latitude position home", K::Float, 0, -90, 90, nullptr, nullptr, "", false, false, "!%.5f"},
        {"adsbLon", kLocation, N_("Station longitude"), N_("Degrees, east positive."), "longitude position home", K::Float, 0, -180, 180, nullptr, nullptr, "", false, false, "!%.5f"},
        // ---- Keyboard and mouse
        {"keysEditor", kKeys, N_("Keys, mouse buttons and wheel"), N_("Every action with its keys: Set records a key, Clear removes it, Default puts it back. Conflicts are marked; import and export the whole set."), "shortcut shortcuts hotkey hotkeys keybinding keybindings keyboard mouse wheel bind rebind", K::Custom},
        // ---- Notifications
        {"notifySystem", kNotify, N_("System notifications"), N_("Alerts (watch lists, automations, pass reminders) also go to the system's notification centre."), "notification alert desktop toast", K::Bool, 1},
        {"notifyBanner", kNotify, N_("Banner in OnAir"), N_("Alerts show a banner at the top of the OnAir window."), "notification banner", K::Bool, 1},
        {"notifyBannerSec", kNotify, N_("Banner time (seconds)"), N_("How long the banner stays."), "notification banner time", K::Int, 10, 2, 120, nullptr, nullptr, "", true},
        {"notifySound", kNotify, N_("Alert sounds"), N_("Alerts that play a sound play it. Off: silent."), "sound beep alert", K::Bool, 1},
        {"notifyQuiet", kNotify, N_("Quiet hours"), N_("No system notifications and no alert sounds between the two times below (the banner still shows)."), "quiet night do not disturb dnd", K::Bool, 0},
        {"notifyQuietFrom", kNotify, N_("Quiet from (minutes after midnight)"), N_("e.g. 1320 is 22:00. Local time."), "quiet night start", K::Int, 1320, 0, 1439, nullptr, nullptr, "", true},
        {"notifyQuietTo", kNotify, N_("Quiet until (minutes after midnight)"), N_("e.g. 420 is 07:00. Local time."), "quiet night end", K::Int, 420, 0, 1439, nullptr, nullptr, "", true},
        // ---- Network and updates
        {"updCheck", kNetwork, N_("Check for updates"), N_("Look for a new official version when the app starts."), "update version", K::Bool, 1},
        {"updBeta", kNetwork, N_("Include beta versions"), N_("Offer test versions too. Off: stable only."), "channel stable beta prerelease", K::Bool, 0},
        {"updAuto", kNetwork, N_("Download updates by itself"), N_("Download and install when a new version is found."), "automatic install", K::Bool, 0},
        {"updSrc", kNetwork, N_("Update source"), N_("Automatic uses GitHub; or GitHub only; or a mirror only."), "mirror github", K::Choice, 0, 0, 0, "Automatic|GitHub only|Mirror only", nullptr, "", true},
        {"updMirrorOn", kNetwork, N_("Use the mirror when GitHub cannot be reached"), N_("If GitHub cannot be reached, try the mirror address below for the update check."), "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"updMirror", kNetwork, N_("Mirror address"), N_("https://host/{url} or a base address. The mirror sees that you check for updates."), "", K::Text, 0, 0, 0, nullptr, nullptr, "", true},
        {"udpHost", kNetwork, N_("UDP output host"), N_("Where the transport stream output sends its packets."), "stream output rtp", K::Text, 0, 0, 0, nullptr, nullptr, "255.255.255.255", true},
        {"udpPort", kNetwork, N_("UDP output port"), N_("The UDP port that the transport stream is sent to."), "", K::Int, 0, 0, 65535, nullptr, nullptr, "", true},
        {"udpRtp", kNetwork, N_("UDP output as RTP"), N_("Wrap the transport stream in RTP packets. Off: plain UDP."), "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"mapTileUrl", kNetwork, N_("Map tile server"), N_("Address of the map tiles with {z} {x} {y}. Empty: OpenStreetMap (https://tile.openstreetmap.org/{z}/{x}/{y}.png). Tiles of another server are cached apart. The server sees which map area you look at."), "map tiles osm openstreetmap server url cache", K::Text, 0, 0, 0, nullptr, nullptr, "", true, true},
        {"mapCapMb", kNetwork, N_("Map tile cache size (MB)"), N_("When the tiles take more room, the ones used longest ago are deleted."), "map cache disk tiles", K::Int, 500, 20, 100000},
        {"mapKeepTiles", kNetwork, N_("Keep map tiles between runs"), N_("Off: the tiles of a session are deleted when OnAir ends."), "map cache keep", K::Bool, 1, 0, 0, nullptr, nullptr, "", true, true},
        // ---- Privacy
        {"privacyInfo", kPrivacy, N_("What leaves this computer"), N_("The update check (when on) sends one HTTPS request to api.github.com, or to your mirror, with the header User-Agent: OnAir/<version>. GitHub therefore sees your IP address and the app version, nothing else. The plug-in Store (View > Plugins > Store) contacts its registry only when you press Refresh, open a plug-in's page or install one: HTTPS requests to the registry address with the header User-Agent: OnAir-Store/<version>; that server sees your IP address and the app version, nothing else. Sending your station to the map server and the Discord status are opt-in and off until you turn them on."), "telemetry network data sent", K::Custom},
        {"adsbMap", kPrivacy, N_("Fetch online map tiles"), N_("The map screens load OpenStreetMap tiles (or your tile server's). Only tile numbers are sent; the server sees your IP address and the area. Off: offline maps only."), "map tiles online osm openstreetmap network sent", K::Bool, 1, 0, 0, nullptr, nullptr, "", false, true},
        // ---- Performance
        {"powerLowMode", kPerf, N_("Low power mode"), N_("Slower redraws and background work to save battery."), "battery energy laptop", K::Choice, 1, 0, 0, "Off|Automatic (on battery or hot)|Always"},
        {"powerHidden", kPerf, N_("Save power when hidden"), N_("Slower spectrum updates while the window is hidden or minimised."), "battery minimised", K::Choice, 1, 0, 0, "Off|On battery|Always"},
        {"reduceCpuHidden", kPerf, N_("Reduce CPU when hidden"), N_("No drawing while the window is minimised or hidden. Decoding goes on."), "minimised background", K::Bool, 1},
        {"gpuGlobal", kPerf, N_("Use GPU where available"), N_("Error correction (LDPC, Viterbi) runs on the graphics card for the modes that have a GPU stage. Off: the CPU does it all. The decoded result is the same."), "gpu graphics metal vulkan direct3d compute", K::Bool, 1},
        {"gpuModes", kPerf, N_("GPU per mode"), N_("Override the tick above for one mode. Modes without a GPU stage show a disabled tick and the reason."), "gpu graphics per mode override", K::Custom},
        {"compute", kPerf, N_("DVB error correction on"), N_("CPU, GPU or Automatic (default) for the DVB-T2 LDPC decoder."), "cpu gpu ldpc compute", K::Choice, 2, 0, 0, "CPU|GPU|Automatic", nullptr, "", true},
        {"perfOverlay", kPerf, N_("Performance overlay"), N_("Frame rate, CPU per thread and wake-ups in a corner."), "fps cpu debug", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        // ---- Advanced
        // ---- Plug-ins and scripts
        {"pluginsLink", kPlugins, N_("Plug-ins"), N_("Add your own decoders as plug-ins (a native library plus a manifest). Plug-ins are listed in the mode list under PLUGINS. They run native code with this program's rights: install only ones you trust. The Store tab installs signed plug-ins from a registry when you ask (nothing is fetched by itself)."), "plugin sdk decoder extension safe mode", K::Link},
        {"automationLink", kPlugins, N_("Scripts and rules"), N_("Run a program or script when something is decoded, with the decoded fields in its arguments. Off until you tick Allow running programs in Automations."), "automation script hook run command webhook rules", K::Link},
        // ---- Profiles
        {"profiles", kProfiles, N_("Settings profiles"), N_("Save every setting of this window and the keys under a name, switch between them (e.g. Home, Portable, Night), and export or import a profile as a file."), "profile profiles preset save load switch export import backup", K::Custom},
        // ---- Advanced
        {"verboseLog", kAdvanced, N_("Verbose logging"), N_("USB transfer counts, data rates, stall restarts and frame times in the log. For bug reports."), "debug log verbose diagnostics", K::Bool, 0},
        {"ffmpegLibs", kAdvanced, N_("FFmpeg libraries"), N_("Bundled (default): the FFmpeg inside OnAir. External folder: your own FFmpeg build (shared libavcodec, libavutil, e.g. an official build with software codecs) is used by the optional add-ons that load FFmpeg at run time (AC-4, AVS+). OnAir ships and downloads no extra codecs; the TV, DAB and DRM decoders keep using the bundled FFmpeg."), "ffmpeg libavcodec external h264 h265 x264 hevc codec folder", K::Custom},
        {"optLibs", kAdvanced, N_("Optional libraries and codecs"), N_("Every library OnAir loads at run time and does not ship (MP3, AAC, voice codecs, radio drivers), with found or missing, the folder to put it in and the official source."), "dll codec library lame faad fdk fobos rtlsdr missing found codecs folder", K::Custom},
        {"sdrconnectLog", kAdvanced, N_("Log SDRconnect websocket traffic"), N_("Write every message OnAir sends to and receives from SDRconnect (nRSP-ST) with a time stamp to sdrconnect-ws.log in the data folder. For bug reports. IQ data is not logged."), "sdrplay nrsp websocket debug headless", K::Custom},
        {"saveLog", kAdvanced, N_("Save the log"), N_("Write the log lines of this run to a text file in the data folder."), "verbose debug crash report", K::Custom},
        {"exportImport", kAdvanced, N_("Export or import settings"), N_("Write all these settings to a file, or read them back."), "backup config", K::Custom},
        {"resetAll", kAdvanced, N_("Reset every setting"), N_("Put every setting of this window back to its default."), "factory", K::Custom},
    };
    return t;
}

}   // namespace settings
