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

inline const std::vector<Info>& table() {
    using K = Kind;
    static const std::vector<Info> t = {
        // ---- General
        {"uiLang", kGeneral, N_("Language"), N_("The interface language. Same as the system follows the computer."), "translate locale", K::Custom},
        {"easyMode", kGeneral, N_("Easy mode"), N_("Every mode opens its Simple view: the spectrum and one screen with a Start button and the key controls. Off: the detailed tabs."), "simple advanced beginner one button start easy", K::Custom},
        {"regionPreset", kGeneral, N_("Region preset"), N_("One click sets the band plan, TV channels, FM band and step, default frequencies and units for a region (Lithuania, EU, ...)."), "region country lithuania eu europe units metric imperial band plan fm", K::Custom},
        {"setupRun", kGeneral, N_("First-run setup"), N_("Open the setup again: radio, region, antenna, interests, voice codecs."), "wizard first run setup codecs antenna", K::Link},
        {"modeVisLink", kGeneral, N_("Modes shown"), N_("Choose which modes the mode lists, picker and keys 1-9 show."), "hide favourites groups", K::Link},
        {"powerLowMode", kGeneral, N_("Low power mode"), N_("Slower redraws and background work to save battery."), "battery energy laptop", K::Choice, 1, 0, 0, "Off|Automatic (on battery or hot)|Always"},
        {"powerHidden", kGeneral, N_("Save power when hidden"), N_("Slower spectrum updates while the window is hidden or minimised."), "battery minimised", K::Choice, 1, 0, 0, "Off|On battery|Always"},
        {"reduceCpuHidden", kGeneral, N_("Reduce CPU when hidden"), N_("No drawing while the window is minimised or hidden. Decoding goes on."), "minimised background", K::Bool, 1},
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
        {"timeDisplay", kAppearance, N_("Time display"), N_("Show times in UTC or in this computer's local time, with or without the zone label."), "utc local clock 24h date", K::Custom},
        {"touchUi", kAppearance, N_("Touch interface"), N_("Big buttons, an on-screen keypad and a full-screen mode picker for tablets."), "tablet android large rows", K::Bool, 0},
        {"touchScale", kAppearance, N_("Touch text size"), N_("Extra text scale of the touch interface."), "touch large", K::Float, 1.35, 1, 2, nullptr, nullptr, "", true},
        {"perfOverlay", kAppearance, N_("Performance overlay"), N_("Frame rate, CPU per thread and wake-ups in a corner."), "fps cpu debug", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        // ---- Spectrum and waterfall
        {"wfMap3", kDisplay, N_("Waterfall colours"), N_("The colour map of the waterfall."), "palette colormap heat viridis", K::Choice, 15, 0, 0,
         "Classic|Viridis|Inferno|Grey|Turbo|Cividis|Magma|Deep heat|Ice|Fire|Night (red)|High contrast|Custom|Smooth heat|Deep blue|Navy heat|Blue green", "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"},
        {"wfAutoFloor", kDisplay, N_("Waterfall range follows the noise floor"), N_("The intensity range tracks the noise so weak signals show."), "auto level", K::Bool, 1},
        {"wfGamma", kDisplay, N_("Waterfall contrast curve"), N_("Below 1 lifts weak signals."), "gamma", K::Float, 1, 0.4, 2.5, nullptr, nullptr, "", true},
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
        // ---- Radio
        {"tuningMode", kRadio, N_("Spectrum click"), N_("Retune the radio or only move the decode offset (sticky LO)."), "tune lo", K::Choice, 0, 0, 0, "Retune the radio|Move the decode offset only"},
        {"centreTuning", kRadio, N_("Click centres the radio"), N_("A click on the spectrum puts the radio centre there."), "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"dragMovesTuned", kRadio, N_("Dragging the band moves the tuned frequency"), N_("When you drag the view past the edge of the band, the tuned frequency moves with the radio. Off: only the radio centre moves."), "pan", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"specSnapHz", kRadio, N_("Click snaps to a multiple of (Hz)"), N_("0 uses the mode's own step."), "step snap", K::Int, 0, 0, 1000000, nullptr, nullptr, "", true},
        {"bwAuto", kRadio, N_("Measure the channel width by itself"), N_("The engine measures the width and switches (DVB)."), "bandwidth auto", K::Bool, 1},
        // ---- Audio
        {"popTop", kAudioC, N_("Video pop-out stays on top"), N_("The pop-out video window stays above other windows."), "window", K::Bool, 1},
        {"subsOn", kAudioC, N_("Show subtitles"), N_("Subtitles of the video modes, when the station sends them."), "teletext captions", K::Bool, 1},
        // ---- Recording and files
        {"recFormat", kFiles, N_("IQ recording format"), N_("8-bit (small), float (exact) or X-Midas BLUE .tmp with float samples."), "iq record file", K::Choice, 0, 0, 0, "8-bit|Float|X-Midas BLUE (.tmp)|16-bit"},
        {"recordingsFolder", kFiles, N_("Recordings folder"), N_("Where recordings (IQ, audio, images, logs) are saved. Choose your own folder; default is inside the data folder."), "folder directory path open recordings", K::Custom},
        {"dataFolder", kFiles, N_("Data folder"), N_("Where recordings, layouts and settings live."), "folder directory path open", K::Custom},
        // ---- Decoding
        {"gpuGlobal", kDecoding, N_("Use GPU where available"), N_("Error correction (LDPC, Viterbi) runs on the graphics card for the modes that have a GPU stage. Off: the CPU does it all. The decoded result is the same."), "gpu graphics metal vulkan direct3d compute", K::Bool, 1},
        {"gpuModes", kDecoding, N_("GPU per mode"), N_("Override the tick above for one mode. Modes without a GPU stage show a disabled tick and the reason."), "gpu graphics per mode override", K::Custom},
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
        // ---- Location
        {"adsbLat", kLocation, N_("Station latitude"), N_("Degrees, north positive. Used by the maps, passes, ADS-B and distances."), "latitude position home", K::Float, 0, -90, 90, nullptr, nullptr, "", false, false, "!%.5f"},
        {"adsbLon", kLocation, N_("Station longitude"), N_("Degrees, east positive."), "longitude position home", K::Float, 0, -180, 180, nullptr, nullptr, "", false, false, "!%.5f"},
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
        // ---- Privacy
        {"privacyInfo", kPrivacy, N_("What leaves this computer"), N_("The update check (when on) sends one HTTPS request to api.github.com, or to your mirror, with the header User-Agent: OnAir/<version>. GitHub therefore sees your IP address and the app version, nothing else. Sending your station to the map server and the Discord status are opt-in and off until you turn them on."), "telemetry network data sent", K::Custom},
        // ---- Advanced
        // ---- Plug-ins and scripts
        {"pluginsLink", kPlugins, N_("Plug-ins"), N_("Add your own decoders as plug-ins (a native library plus a manifest). Plug-ins are listed in the mode list under PLUGINS. They run native code with this program's rights: install only ones you trust."), "plugin sdk decoder extension safe mode", K::Link},
        {"automationLink", kPlugins, N_("Scripts and rules"), N_("Run a program or script when something is decoded, with the decoded fields in its arguments. Off until you tick Allow running programs in Automations."), "automation script hook run command webhook rules", K::Link},
        {"saveLog", kAdvanced, N_("Save the log"), N_("Write the log lines of this run to a text file in the data folder."), "verbose debug crash report", K::Custom},
        {"exportImport", kAdvanced, N_("Export or import settings"), N_("Write all these settings to a file, or read them back."), "backup config", K::Custom},
        {"resetAll", kAdvanced, N_("Reset every setting"), N_("Put every setting of this window back to its default."), "factory", K::Custom},
    };
    return t;
}

}   // namespace settings
