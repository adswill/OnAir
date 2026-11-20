# core/src/transcribe: Live transcripts

Live transcripts: speech to text for voice channels (segmenter, speech-engine queue and filters, callsign reader, transcript store and exports, model list and download, audio tap). The existing Whisper backend (third_party/whisper) and an optional Linux Moonshine Voice backend share the SpeechBackend interface; no models are shipped. See docs/MOONSHINE_BACKEND.md for the experimental Moonshine adapter. See docs/TRANSCRIBE.md.

Mode folders (core/src/<mode>/) hold one decoder each; this folder holds code shared by the voice modes. See ARCHITECTURE.md.
