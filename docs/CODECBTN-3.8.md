# Codec install screens: Discord button (3.8)

Every codec install screen or notice has a **Join the Discord to get access to built codecs** button that opens https://discord.gg/ptxkzCuzR in the browser. It only opens a web page: OnAir still never downloads or bundles a codec, and the existing install-it-yourself steps stay next to it.

## One place
`appcodec::discordInviteUrl()`, `discordCodecLabel()` and `discordCodecButton(bool small)` in app/codec/speech_codec_ui.{h,cpp}. The label and tooltip go through tr(); the menu-bar Discord chip (ui2_shell.cpp) uses the same URL function. Catalog entries added to all 14 languages (machine translations, listed in app/i18n/unreviewed).

## Where it shows
- `openCodecFolderButtons()` (after the folder buttons, same line when it fits): so every screen that has "Open codecs folder": the ? help popups (mbelib topics, HDC, DRA+, AC-4), the install helper (DMR, P25, NXDN, D-STAR, dPMR, Yaesu, TETRA, TETRAPOL, Iridium, Inmarsat/Aero, ProVoice), Help > Voice codecs and its Codec setup section, the setup wizard codec screen, the DRM libfdk-aac (xHE-AAC) notice, the not-installed gate (`speechCodecGate`).
- `voiceCodecLine()`: the amber "Voice codec missing" line of every voice panel.
- ? help topics without folder buttons (AVS+, Aero AMBE, AVS, DRA TV): footer.
- Inline notices: Ease codec notice, CDR DRA+ notice, HD Radio HDC line.
- Not shown on the codec2 page (codec2 ships with OnAir).

## Not done
- Android: the app cannot load codec plug-ins and has no install screen, only the mode notes (strings.xml `mode_note_voice`, `mode_note_sound`). No button added; it would need a layout change that was not built or tested here.
- The MP3 (LAME) tooltip in the general receiver is a save-format hint, not an install screen: left alone.

## Checks
clang++ -std=c++20 -fsyntax-only on the six changed files: clean. Not run: full build, GUI, test_i18n (overnight limit); every new text is a literal tr() argument and the catalog lines keep tab format with no printf conversions. Button IDs: the helper pushes the ID "codec_discord" and the label has no ## suffix; sizes come from text and style (no fixed pixel sizes).
