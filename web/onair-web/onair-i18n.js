/* OnAir Web client strings. English is the master list; a language only needs the keys it translates (the rest fall back to English).
 * Markup: data-i18n="key" (text), data-i18n-title, data-i18n-aria (aria-label), data-i18n-ph (placeholder). Code: t("key", {name: value}) with {name} placeholders.
 * To add a language: add a catalog below and its code to the `lang` list in onair-settings.js. Pure code: tested by tests/web/run.js. */
(function (root, factory) {
  const lib = factory();
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirI18n = lib;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";
  const CAT = {
    en: {
      "menu.view": "View", "menu.help": "Help", "menu.settings": "Settings...", "menu.theme": "Theme: {v}", "menu.wholeSpan": "Whole span", "menu.shortcuts": "Keyboard shortcuts", "menu.github": "OnAir on GitHub",
      "theme.dark": "Dark", "theme.light": "Light", "theme.auto": "Automatic",
      "conn.offline": "offline", "conn.connecting": "connecting", "conn.busy": "busy", "conn.error": "error", "conn.signin": "sign in", "conn.noreceiver": "no receiver",
      "state.offline": "Offline", "state.connecting": "Connecting", "state.idle": "Idle", "state.receiving": "Receiving", "state.muted": "Muted",
      "head.receiveOnly": "Receive only", "head.listeners": "{n} / {max} listeners",
      "load.title": "Connecting to the receiver", "load.text": "Opening a listening session.", "load.notconn": "Not connected", "load.why": "The notice above says why. This page retries by itself when it can.",
      "retry.in": "Retrying in {s} s.",
      "notice.ok": "Understood",
      "login.title": "Join this receiver", "login.text": "This receiver asks who is listening. Your name is shown to the host only.", "login.name": "Your name", "login.password": "Password", "login.invite": "Invite code", "login.go": "Connect",
      "audio.listen": "Listen", "audio.stop": "Stop", "audio.mute": "Mute", "audio.unmute": "Unmute", "audio.volume": "Volume", "audio.level": "Signal level",
      "sec.receiver": "Receiver", "sec.listen": "Listen", "sec.display": "Display",
      "mode": "Mode", "tuning": "Tuning", "step": "Step", "enterFreq": "Enter frequency...",
      "bandwidth": "Bandwidth", "squelch": "Squelch", "open": "open", "agc": "AGC", "agc.off": "Off", "agc.slow": "Slow", "agc.fast": "Fast", "gain": "Gain", "deemph": "De-emphasis", "deemph.off": "Off",
      "filter.narrow": "Narrow", "filter.normal": "Normal", "filter.wide": "Wide", "filter": "Filter",
      "muteAudio": "Mute audio", "record": "Record audio", "rec": "Rec", "buffer": "Audio buffer",
      "colours": "Colours", "range": "Range", "floor": "Floor", "autoRange": "Auto-range", "zoomOut": "Zoom out", "zoomIn": "Zoom in", "wholeSpan": "Whole span", "around": "Around",
      "tab.overview": "Overview", "tab.bands": "Bands", "tab.memory": "Memory", "tab.decoders": "Decoders", "tab.map": "Map",
      "waterfall": "Waterfall", "secondsAgo": "seconds ago", "zoom": "Zoom",
      "marks.name": "Name", "marks.save": "Save", "marks.none": "No memories yet. Tune somewhere and press Save.", "marks.delete": "Delete memory {n}", "marks.outside": "That memory is outside this server's capture.",
      "marks.export": "Export", "marks.import": "Import", "marks.bad": "That file is not a list of memories.",
      "dec.none": "This server offers no decoders.", "dec.pick": "Pick a decoder above. Some need the receiver tuned near their usual frequency.", "dec.wait": "Waiting for results.", "dec.at": "Decodes at the tuned frequency",
      "map.title": "Map", "map.wide": "Expand", "map.count": "{n} on the map", "map.none": "Nothing with a position yet. Start the ADS-B, AIS or APRS decoder.", "map.fit": "Fit", "map.aircraft": "Aircraft", "map.vessels": "Vessels", "map.stations": "Stations",
      "sb.frequency": "Frequency", "sb.mode": "Mode", "sb.listeners": "Listeners", "sb.state": "State", "sb.buffer": "Buffer",
      "foot.legal": "OnAir Web. Receive only: there is no transmit control.",
      "type.title": "Tune to a frequency", "type.hint": "MHz, or add kHz / Hz. Examples: 145.5, 7074 kHz, 121.5 MHz", "type.cancel": "Cancel", "type.go": "Tune", "type.bad": "That is not a frequency. Try 145.5, 7074 kHz or 121.5 MHz.",
      "help.title": "Keyboard shortcuts", "help.close": "Close",
      "set.title": "Settings", "set.reset": "Reset to defaults", "set.export": "Export settings", "set.import": "Import settings", "set.close": "Close", "set.saved": "Settings are kept in this browser only.", "set.imported": "Settings imported.", "set.badfile": "That file is not an OnAir Web settings export.",
      "set.tab.receiver": "Receiver", "set.tab.display": "Display", "set.tab.layout": "Layout and look",
      "set.agc": "AGC (automatic gain)", "set.gainDb": "Gain when AGC is off, dB", "set.deemphUs": "FM de-emphasis (wide FM)", "set.audioCodec": "Audio codec", "set.audioRate": "Audio sample rate", "set.step": "Tuning step", "set.autoMode": "Bands and memories also set the mode",
      "set.audioRate.0": "Automatic", "set.rangeMode": "Level range", "set.rangeMode.server": "Follow the noise floor", "set.rangeMode.adaptive": "Auto-range (adapts to the signals)", "set.rangeMode.manual": "Manual (minimum and maximum)",
      "set.palette": "Waterfall colours", "set.range": "Range, dB", "set.floorAdj": "Floor offset, dB", "set.minDb": "Minimum level, dBFS", "set.maxDb": "Maximum level, dBFS", "set.wfFps": "Waterfall speed, rows a second", "set.fftBins": "FFT size (bins)", "set.fftBins.0": "Fit the screen",
      "set.smooth": "Trace smoothing, %", "set.peakHold": "Hold peaks", "set.showBands": "Show the band plan", "set.showMarks": "Show memories on the spectrum", "set.showGrid": "Show grid lines",
      "set.theme": "Theme", "set.lang": "Language", "set.lang.auto": "Automatic (browser)", "set.layout": "Layout", "set.layout.auto": "Automatic", "set.layout.wide": "Wide (three columns)", "set.layout.compact": "Compact (one column)",
      "set.panelLeft": "Show the receiver panel", "set.panelRight": "Show the bands, memory and decoders panel", "set.scale": "Text size, %", "set.keys": "Keyboard shortcuts on", "set.reduceMotion": "Reduce motion",
      "pal.navy": "Classic", "pal.amber": "Amber", "pal.classic": "Blue-green", "pal.viridis": "Viridis", "pal.grey": "Grey", "pal.turbo": "Turbo", "pal.magma": "Magma", "pal.ice": "Ice",
      "rec.unsupported": "This browser cannot record audio.", "rec.na": "Recording is not available in this browser.", "audio.unsupported": "This browser cannot play audio.",
      "dir.title": "OnAir Web stations", "dir.sub": "Receivers shared by their owners. Open one to listen in your browser.",
    },
    lt: {
      "menu.view": "Rodinys", "menu.help": "Pagalba", "menu.settings": "Nustatymai...", "menu.theme": "Tema: {v}", "menu.wholeSpan": "Visas ruozas", "menu.shortcuts": "Klaviaturos nuorodos", "menu.github": "OnAir GitHub",
      "theme.dark": "Tamsi", "theme.light": "Šviesi", "theme.auto": "Automatinė",
      "conn.offline": "neprisijungta", "conn.connecting": "jungiamasi", "conn.busy": "užimta", "conn.error": "klaida", "conn.signin": "prisijunkite", "conn.noreceiver": "nėra imtuvo",
      "state.offline": "Neprisijungta", "state.connecting": "Jungiamasi", "state.idle": "Laukia", "state.receiving": "Klausoma", "state.muted": "Nutildyta",
      "head.receiveOnly": "Tik priėmimas", "head.listeners": "{n} / {max} klausytojų",
      "load.title": "Jungiamasi prie imtuvo", "load.text": "Atidaroma klausymo sesija.", "load.notconn": "Neprisijungta", "load.why": "Priežastis nurodyta pranešime aukščiau. Puslapis bando jungtis iš naujo.",
      "retry.in": "Bandoma iš naujo po {s} s.",
      "notice.ok": "Supratau",
      "login.title": "Prisijunkite prie imtuvo", "login.text": "Šis imtuvas klausia, kas klausosi. Jūsų vardą mato tik šeimininkas.", "login.name": "Jūsų vardas", "login.password": "Slaptažodis", "login.invite": "Kvietimo kodas", "login.go": "Prisijungti",
      "audio.listen": "Klausyti", "audio.stop": "Stop", "audio.mute": "Nutildyti", "audio.unmute": "Įjungti garsą", "audio.volume": "Garsumas", "audio.level": "Signalo lygis",
      "sec.receiver": "Imtuvas", "sec.listen": "Klausymas", "sec.display": "Vaizdas",
      "mode": "Režimas", "tuning": "Derinimas", "step": "Žingsnis", "enterFreq": "Įvesti dažnį...",
      "bandwidth": "Juosta", "squelch": "Triukšmo slopintuvas", "open": "atviras", "agc": "AGC", "agc.off": "Išjungta", "agc.slow": "Lėta", "agc.fast": "Greita", "gain": "Stiprinimas", "deemph": "Deakcentas", "deemph.off": "Išjungta",
      "filter.narrow": "Siaura", "filter.normal": "Įprasta", "filter.wide": "Plati", "filter": "Filtras",
      "muteAudio": "Nutildyti garsą", "record": "Įrašyti garsą", "rec": "Įr.", "buffer": "Garso buferis",
      "colours": "Spalvos", "range": "Ruožas", "floor": "Pagrindas", "autoRange": "Auto lygis", "zoomOut": "Tolinti", "zoomIn": "Artinti", "wholeSpan": "Visas ruožas", "around": "Aplink",
      "tab.overview": "Apžvalga", "tab.bands": "Juostos", "tab.memory": "Atmintis", "tab.decoders": "Dekoderiai", "tab.map": "Žemėlapis",
      "waterfall": "Krioklys", "secondsAgo": "sekundžių atgal", "zoom": "Mastelis",
      "marks.name": "Pavadinimas", "marks.save": "Įrašyti", "marks.none": "Atminties dar nėra. Suderinkite dažnį ir spauskite Įrašyti.", "marks.export": "Eksportuoti", "marks.import": "Importuoti",
      "dec.none": "Šis serveris nesiūlo dekoderių.", "dec.wait": "Laukiama rezultatų.",
      "map.title": "Žemėlapis", "map.none": "Dar nėra objektų su vieta. Paleiskite ADS-B, AIS ar APRS dekoderį.", "map.fit": "Tilpdyti", "map.aircraft": "Orlaiviai", "map.vessels": "Laivai", "map.stations": "Stotys",
      "sb.frequency": "Dažnis", "sb.mode": "Režimas", "sb.listeners": "Klausytojai", "sb.state": "Būsena", "sb.buffer": "Buferis",
      "foot.legal": "OnAir Web. Tik priėmimas: siųstuvo valdymo nėra.",
      "type.title": "Suderinti dažnį", "type.cancel": "Atšaukti", "type.go": "Derinti",
      "help.title": "Klaviaturos nuorodos", "help.close": "Uždaryti",
      "set.title": "Nustatymai", "set.reset": "Atkurti numatytuosius", "set.export": "Eksportuoti nustatymus", "set.import": "Importuoti nustatymus", "set.close": "Uždaryti", "set.saved": "Nustatymai saugomi tik šioje naršyklėje.",
      "set.tab.receiver": "Imtuvas", "set.tab.display": "Vaizdas", "set.tab.layout": "Išdėstymas ir išvaizda",
      "set.theme": "Tema", "set.lang": "Kalba", "set.lang.auto": "Automatinė (naršyklės)", "set.layout": "Išdėstymas", "set.scale": "Teksto dydis, %", "set.keys": "Klaviatūros nuorodos įjungtos", "set.palette": "Krioklio spalvos", "set.step": "Derinimo žingsnis",
    },
    de: {
      "menu.view": "Ansicht", "menu.help": "Hilfe", "menu.settings": "Einstellungen...", "menu.theme": "Design: {v}", "menu.wholeSpan": "Ganzer Bereich", "menu.shortcuts": "Tastenkürzel",
      "theme.dark": "Dunkel", "theme.light": "Hell", "theme.auto": "Automatisch",
      "conn.offline": "offline", "conn.connecting": "verbinde", "conn.busy": "belegt", "conn.error": "Fehler", "conn.signin": "anmelden", "conn.noreceiver": "kein Empfänger",
      "state.offline": "Offline", "state.connecting": "Verbinde", "state.idle": "Bereit", "state.receiving": "Empfang", "state.muted": "Stumm",
      "head.receiveOnly": "Nur Empfang", "head.listeners": "{n} / {max} Hörer",
      "load.title": "Verbindung zum Empfänger", "load.text": "Hörsitzung wird geöffnet.", "load.notconn": "Nicht verbunden",
      "notice.ok": "Verstanden",
      "login.title": "Empfänger beitreten", "login.name": "Ihr Name", "login.password": "Passwort", "login.invite": "Einladungscode", "login.go": "Verbinden",
      "audio.listen": "Hören", "audio.stop": "Stopp", "audio.mute": "Stumm", "audio.unmute": "Ton an", "audio.volume": "Lautstärke", "audio.level": "Signalpegel",
      "sec.receiver": "Empfänger", "sec.listen": "Hören", "sec.display": "Anzeige",
      "mode": "Modus", "tuning": "Abstimmung", "step": "Schritt", "enterFreq": "Frequenz eingeben...",
      "bandwidth": "Bandbreite", "squelch": "Rauschsperre", "open": "offen", "agc": "AGC", "gain": "Verstärkung", "deemph": "De-Emphase",
      "filter.narrow": "Schmal", "filter.normal": "Normal", "filter.wide": "Breit", "filter": "Filter",
      "muteAudio": "Ton stumm", "record": "Audio aufnehmen", "rec": "Aufn.", "buffer": "Audiopuffer",
      "colours": "Farben", "range": "Bereich", "floor": "Boden", "autoRange": "Auto-Pegel", "wholeSpan": "Ganzer Bereich", "around": "Umgebung",
      "tab.overview": "Übersicht", "tab.bands": "Bänder", "tab.memory": "Speicher", "tab.decoders": "Dekoder", "tab.map": "Karte",
      "waterfall": "Wasserfall", "secondsAgo": "Sekunden zuvor", "zoom": "Zoom",
      "marks.name": "Name", "marks.save": "Speichern", "marks.export": "Exportieren", "marks.import": "Importieren",
      "map.title": "Karte", "map.fit": "Einpassen", "map.aircraft": "Flugzeuge", "map.vessels": "Schiffe", "map.stations": "Stationen",
      "sb.frequency": "Frequenz", "sb.mode": "Modus", "sb.listeners": "Hörer", "sb.state": "Status", "sb.buffer": "Puffer",
      "foot.legal": "OnAir Web. Nur Empfang: es gibt keine Sendefunktion.",
      "type.title": "Frequenz einstellen", "type.cancel": "Abbrechen", "type.go": "Abstimmen",
      "help.title": "Tastenkürzel", "help.close": "Schließen",
      "set.title": "Einstellungen", "set.reset": "Standardwerte", "set.export": "Einstellungen exportieren", "set.import": "Einstellungen importieren", "set.close": "Schließen",
      "set.tab.receiver": "Empfänger", "set.tab.display": "Anzeige", "set.tab.layout": "Layout und Aussehen",
      "set.theme": "Design", "set.lang": "Sprache", "set.lang.auto": "Automatisch (Browser)", "set.layout": "Layout", "set.scale": "Textgröße, %", "set.keys": "Tastenkürzel aktiv", "set.palette": "Wasserfallfarben",
    },
  };
  const LANGS = Object.keys(CAT);
  let cur = "en";

  function pickLang(setting, navLangs) {
    if (setting && setting !== "auto" && CAT[setting]) return setting;
    for (const l of navLangs || []) { const c = String(l || "").slice(0, 2).toLowerCase(); if (CAT[c]) return c; }
    return "en";
  }
  function setLang(l) { cur = CAT[l] ? l : "en"; return cur; }
  function lang() { return cur; }
  function fmt(s, vars) { return vars ? s.replace(/\{(\w+)\}/g, (m, k) => (vars[k] != null ? String(vars[k]) : m)) : s; }
  function t(key, vars) {
    const s = (CAT[cur] && CAT[cur][key]) || CAT.en[key];
    return fmt(s == null ? key : s, vars);
  }
  // fill every data-i18n* element below root (a Document or Element)
  function apply(root) {
    const q = (a) => root.querySelectorAll("[" + a + "]");
    for (const e of q("data-i18n")) e.textContent = t(e.getAttribute("data-i18n"));
    for (const e of q("data-i18n-title")) e.title = t(e.getAttribute("data-i18n-title"));
    for (const e of q("data-i18n-aria")) e.setAttribute("aria-label", t(e.getAttribute("data-i18n-aria")));
    for (const e of q("data-i18n-ph")) e.setAttribute("placeholder", t(e.getAttribute("data-i18n-ph")));
  }
  // keys used by a language that English does not have (the tests keep this empty)
  function orphanKeys() { const out = []; for (const l of LANGS) for (const k of Object.keys(CAT[l])) if (!(k in CAT.en)) out.push(l + ":" + k); return out; }
  function coverage(l) { return Object.keys(CAT[l] || {}).length / Object.keys(CAT.en).length; }

  return { LANGS, CAT, pickLang, setLang, lang, t, apply, fmt, orphanKeys, coverage };
});
