# core/src/ssbconv

Finds SSB voice on a wide capture and organises it into conversations (docs/SSB-CONVERSATIONS.md). `scanner.cpp` the spectrum, candidates, sideband, channel extraction and tracks; `voice.cpp` the voice analysis and voice activity; `book.cpp` the conversation state machine, filters, export, timeline, band labels, auto-listen, sweep plan, clip files; `synth.cpp` the speech-like test and demo signals. No radio, screen or network here.
