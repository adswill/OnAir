// Every row of the Settings window: category, label, help, default, limits. No ImGui, no App (settings_logic.h), so the test can check it.
// The key is the stored name: for a field of App it is the key of app/state/prefs.cpp, so Settings and the old menus read and write the same value.
#pragma once
#include "settings_logic.h"
#ifndef N_
#define N_(s) s   // marks a text for the translation catalogs (app/i18n); tr() runs when it is shown
#endif

namespace settings {

// category names (the window translates them)
inline const char* const kGeneral = "General";
inline const char* const kAppearance = "Appearance";
inline const char* const kDisplay = "Spectrum and waterfall";
inline const char* const kRadio = "Radio";
inline const char* const kAudioC = "Audio";
inline const char* const kFiles = "Recording and files";
inline const char* const kDecoding = "Decoding";
inline const char* const kLocation = "Location";
inline const char* const kNetwork = "Network and updates";
inline const char* const kPrivacy = "Privacy";
inline const char* const kPlugins = N_("Plug-ins and scripts");
inline const char* const kAdvanced = "Advanced";

inline const std::vector<Info>& table() {
    using K = Kind;
    static const std::vector<Info> t = {
        // ---- General
        {"uiLang", kGeneral, "Language", "The interface language. Same as the system follows the computer.", "translate locale", K::Custom},
        {"easyMode", kGeneral, N_("Easy mode"), N_("Every mode opens its Simple view: the spectrum and one screen with a Start button and the key controls. Off: the detailed tabs."), "simple advanced beginner one button start easy", K::Custom},
        {"regionPreset", kGeneral, N_("Region preset"), N_("One click sets the band plan, TV channels, FM band and step, default frequencies and units for a region (Lithuania, EU, ...)."), "region country lithuania eu europe units metric imperial band plan fm", K::Custom},
        {"setupRun", kGeneral, N_("First-run setup"), N_("Open the setup again: radio, region, antenna, interests, voice codecs."), "wizard first run setup codecs antenna", K::Link},
        {"modeVisLink", kGeneral, "Modes shown", "Choose which modes the mode lists, picker and keys 1-9 show.", "hide favourites groups", K::Link},
        {"powerLowMode", kGeneral, "Low power mode", "Slower redraws and background work to save battery.", "battery energy laptop", K::Choice, 1, 0, 0, "Off|Automatic (on battery or hot)|Always"},
        {"powerHidden", kGeneral, "Save power when hidden", "Slower spectrum updates while the window is hidden or minimised.", "battery minimised", K::Choice, 1, 0, 0, "Off|On battery|Always"},
        {"reduceCpuHidden", kGeneral, "Reduce CPU when hidden", "No drawing while the window is minimised or hidden. Decoding goes on.", "minimised background", K::Bool, 1},
        {"discordOn", kGeneral, "Show in Discord", "Rich Presence: tell Discord which mode you listen to. Off until you turn it on.", "presence", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"discordFreq", kGeneral, "Show the frequency in Discord", "Include the tuned frequency in the Discord status.", "presence", K::Bool, 1, 0, 0, nullptr, nullptr, "", true},
        // ---- Appearance
        {"uiTheme", kAppearance, "Colour palette", "Terminal, Instrument, Mono, Dev A (graphite) or Dev B (terminal green).", "theme look colours", K::Choice, 3, 0, 0, "Terminal|Instrument|Mono|Dev A (graphite)|Dev B (terminal green)"},
        {"themeMode", kAppearance, "Light or high contrast", "Dark (default), light, or black/white/yellow high contrast.", "theme light dark contrast accessibility", K::Custom},
        {"accent", kAppearance, "Own accent colour", "Replace the accent colour of the interface.", "colour color theme", K::Custom},
        {"uiVariant", kAppearance, "Layout of the interface", "Sidebar (default), Scope, Tiles, Faceplate and the Scope variants, Panel.", "layout variant", K::Choice, 0, 0, 0, "Sidebar|Scope|Tiles|Faceplate|Scope: Cinema|Scope: Meters|Scope: Dock|Panel"},
        {"layoutUnified", kAppearance, "Unified layout", "Spectrum and waterfall stay above the decoded views instead of separate tabs.", "classic tabs", K::Bool, 0},
        {"uiTextScale", kAppearance, "Text size", "Scale of all text and controls (also Ctrl+ and Ctrl-).", "ui scale zoom font size", K::Float, 1, 0.8, 2, nullptr, nullptr, "", false, false, "%.2f"},
        {"uiTypography", kAppearance, "Font look", "Current (default), Clean or Compact.", "typography fonts cjk", K::Custom, 0, 0, 0, nullptr, nullptr, "", false, true},
        {"timeDisplay", kAppearance, "Time display", "Show times in UTC or in this computer's local time, with or without the zone label.", "utc local clock 24h date", K::Custom},
        {"touchUi", kAppearance, "Touch interface", "Big buttons, an on-screen keypad and a full-screen mode picker for tablets.", "tablet android large rows", K::Bool, 0},
        {"touchScale", kAppearance, "Touch text size", "Extra text scale of the touch interface.", "touch large", K::Float, 1.35, 1, 2, nullptr, nullptr, "", true},
        {"perfOverlay", kAppearance, "Performance overlay", "Frame rate, CPU per thread and wake-ups in a corner.", "fps cpu debug", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        // ---- Spectrum and waterfall
        {"wfMap3", kDisplay, "Waterfall colours", "The colour map of the waterfall.", "palette colormap heat viridis", K::Choice, 15, 0, 0,
         "Classic|Viridis|Inferno|Grey|Turbo|Cividis|Magma|Deep heat|Ice|Fire|Night (red)|High contrast|Custom|Smooth heat|Deep blue|Navy heat|Blue green", "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"},
        {"wfAutoFloor", kDisplay, "Waterfall range follows the noise floor", "The intensity range tracks the noise so weak signals show.", "auto level", K::Bool, 1},
        {"wfGamma", kDisplay, "Waterfall contrast curve", "Below 1 lifts weak signals.", "gamma", K::Float, 1, 0.4, 2.5, nullptr, nullptr, "", true},
        {"wfBright", kDisplay, "Waterfall brightness", "Shifts every level.", "", K::Float, 0, -0.5, 0.5, nullptr, nullptr, "", true},
        {"wfContrast", kDisplay, "Waterfall contrast", "Stretches the levels.", "", K::Float, 1, 0.25, 4, nullptr, nullptr, "", true},
        {"wfClock", kDisplay, "Clock time on the waterfall", "Label rows with the clock time instead of seconds ago.", "time scale", K::Bool, 0},
        {"wfAvg", kDisplay, "Average slow rows", "A slow waterfall shows the average of the frames in between, not the highest level.", "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"wfLowPass", kDisplay, "Waterfall noise reduction", "A 2D smoothing of the waterfall.", "smooth", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"specAutoDb", kDisplay, "Spectrum range follows the noise floor", "The dB range of the spectrum follows the view.", "auto db", K::Bool, 1},
        {"specPerHz", kDisplay, "Levels in dB per Hz", "The noise floor stays put when the FFT size changes. Off: dB per bin.", "psd", K::Bool, 1, 0, 0, nullptr, nullptr, "", true},
        {"specDbm", kDisplay, "Show dBm instead of dBFS", "Levels as dBm where the radio is calibrated.", "level units", K::Bool, 0},
        {"specFill", kDisplay, "Fill under the spectrum line", "", "", K::Bool, 1},
        {"specGrid", kDisplay, "Grid", "", "", K::Bool, 1},
        {"specPeaks", kDisplay, "Peak markers", "Mark the strongest peaks on the spectrum.", "", K::Bool, 0},
        {"specReadout", kDisplay, "Level readout at the cursor", "", "", K::Bool, 1},
        {"specLine", kDisplay, "Spectrum line thickness", "", "", K::Float, 1.2, 0.6, 3, nullptr, nullptr, "", true},
        {"specAvgSec", kDisplay, "Spectrum averaging (seconds)", "Time constant of the averaged trace. 0 shows the raw frames.", "smoothing", K::Float, 0.08, 0, 5, nullptr, nullptr, "", true, false, "%.2f"},
        {"holdDecay", kDisplay, "Max-hold fall (dB per second)", "How fast the max-hold line falls. 0 keeps it until reset.", "peak hold", K::Float, 4.5, 0, 60, nullptr, nullptr, "", true, false, "%.1f"},
        // ---- Radio
        {"tuningMode", kRadio, "Spectrum click", "Retune the radio or only move the decode offset (sticky LO).", "tune lo", K::Choice, 0, 0, 0, "Retune the radio|Move the decode offset only"},
        {"centreTuning", kRadio, "Click centres the radio", "A click on the spectrum puts the radio centre there.", "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"dragMovesTuned", kRadio, "Dragging the band moves the tuned frequency", "", "pan", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"specSnapHz", kRadio, "Click snaps to a multiple of (Hz)", "0 uses the mode's own step.", "step snap", K::Int, 0, 0, 1000000, nullptr, nullptr, "", true},
        {"bwAuto", kRadio, "Measure the channel width by itself", "The engine measures the width and switches (DVB).", "bandwidth auto", K::Bool, 1},
        // ---- Audio
        {"popTop", kAudioC, "Video pop-out stays on top", "The pop-out video window stays above other windows.", "window", K::Bool, 1},
        {"subsOn", kAudioC, "Show subtitles", "Subtitles of the video modes, when the station sends them.", "teletext captions", K::Bool, 1},
        // ---- Recording and files
        {"recFormat", kFiles, "IQ recording format", "8-bit (small), float (exact) or X-Midas BLUE .tmp with float samples.", "iq record file", K::Choice, 0, 0, 0, "8-bit|Float|X-Midas BLUE (.tmp)|16-bit"},
        {"recordingsFolder", kFiles, "Recordings folder", "Where recordings (IQ, audio, images, logs) are saved. Choose your own folder; default is inside the data folder.", "folder directory path open recordings", K::Custom},
        {"dataFolder", kFiles, "Data folder", "Where recordings, layouts and settings live.", "folder directory path open", K::Custom},
        // ---- Decoding
        {"fmMono", kDecoding, "FM: force mono", "A weak station is quieter in mono.", "stereo", K::Bool, 0},
        {"fmStereo", kDecoding, "FM stereo", "Auto, mono, or stereo whenever a pilot is there.", "", K::Choice, 0, 0, 0, "Auto|Mono|Stereo"},
        {"fmDeemph", kDecoding, "FM de-emphasis", "50 us (Europe, most of the world) or 75 us (Americas); Off for a flat response.", "tone", K::Choice, 50, 0, 0, "Off|50 us|75 us", "0,50,75"},
        {"fmRegion", kDecoding, "RDS meanings", "Auto picks RBDS for odd tenths of a MHz; or force RDS (Europe) or RBDS (US).", "rbds", K::Choice, 0, 0, 0, "Auto|RDS (Europe)|RBDS (US)"},
        {"fmSquelch", kDecoding, "FM squelch (dB SNR)", "0 is off.", "", K::Int, 0, 0, 40},
        {"ttxPage", kDecoding, "Teletext start page", "The page the teletext view opens on.", "ttx", K::Int, 100, 100, 899, nullptr, nullptr, "", true},
        {"tvLang", kDecoding, "Preferred TV language", "Language code for audio and subtitle choice, e.g. eng.", "subtitle audio", K::Text, 0, 0, 0, nullptr, nullptr, "", true},
        // ---- Location
        {"adsbLat", kLocation, "Station latitude", "Degrees, north positive. Used by the maps, passes, ADS-B and distances.", "latitude position home", K::Float, 0, -90, 90, nullptr, nullptr, "", false, false, "!%.5f"},
        {"adsbLon", kLocation, "Station longitude", "Degrees, east positive.", "longitude position home", K::Float, 0, -180, 180, nullptr, nullptr, "", false, false, "!%.5f"},
        // ---- Network and updates
        {"updCheck", kNetwork, "Check for updates", "Look for a new official version when the app starts.", "update version", K::Bool, 1},
        {"updBeta", kNetwork, "Include beta versions", "Offer test versions too. Off: stable only.", "channel stable beta prerelease", K::Bool, 0},
        {"updAuto", kNetwork, "Download updates by itself", "Download and install when a new version is found.", "automatic install", K::Bool, 0},
        {"updSrc", kNetwork, "Update source", "Automatic uses GitHub; or GitHub only; or a mirror only.", "mirror github", K::Choice, 0, 0, 0, "Automatic|GitHub only|Mirror only", nullptr, "", true},
        {"updMirrorOn", kNetwork, "Use the mirror when GitHub cannot be reached", "", "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        {"updMirror", kNetwork, "Mirror address", "https://host/{url} or a base address. The mirror sees that you check for updates.", "", K::Text, 0, 0, 0, nullptr, nullptr, "", true},
        {"udpHost", kNetwork, "UDP output host", "Where the transport stream output sends its packets.", "stream output rtp", K::Text, 0, 0, 0, nullptr, nullptr, "255.255.255.255", true},
        {"udpPort", kNetwork, "UDP output port", "", "", K::Int, 0, 0, 65535, nullptr, nullptr, "", true},
        {"udpRtp", kNetwork, "UDP output as RTP", "", "", K::Bool, 0, 0, 0, nullptr, nullptr, "", true},
        // ---- Privacy
        {"privacyInfo", kPrivacy, "What leaves this computer", "The update check (when on) sends one HTTPS request to api.github.com, or to your mirror, with the header User-Agent: OnAir/<version>. GitHub therefore sees your IP address and the app version, nothing else. Sending your station to the map server and the Discord status are opt-in and off until you turn them on.", "telemetry network data sent", K::Custom},
        // ---- Advanced
        // ---- Plug-ins and scripts
        {"pluginsLink", kPlugins, N_("Plug-ins"), N_("Add your own decoders as plug-ins (a native library plus a manifest). Plug-ins are listed in the mode list under PLUGINS. They run native code with this program's rights: install only ones you trust."), "plugin sdk decoder extension safe mode", K::Link},
        {"automationLink", kPlugins, N_("Scripts and rules"), N_("Run a program or script when something is decoded, with the decoded fields in its arguments. Off until you tick Allow running programs in Automations."), "automation script hook run command webhook rules", K::Link},
        {"ffmpegDir", kAdvanced, N_("FFmpeg libraries"), N_("Load the FFmpeg libraries of another FFmpeg installation of a similar version, after a restart. OnAir cannot guarantee stability when another FFmpeg library is used."), "ffmpeg codec h264 h265 hevc x264 x265 encoder decoder library dll so dylib", K::Custom},   // its (?) explains the restart (settings_window.cpp)
        {"saveLog", kAdvanced, "Save the log", "Write the log lines of this run to a text file in the data folder.", "verbose debug crash report", K::Custom},
        {"exportImport", kAdvanced, "Export or import settings", "Write all these settings to a file, or read them back.", "backup config", K::Custom},
        {"resetAll", kAdvanced, "Reset every setting", "Put every setting of this window back to its default.", "factory", K::Custom},
    };
    return t;
}

}   // namespace settings
