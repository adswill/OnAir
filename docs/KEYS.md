# Voice decryption keys

OnAir can decrypt digital-voice calls **only with keys you import yourself** — for example to listen to friends on a private
encrypted channel you are authorised to monitor. There is no automatic decrypter.

> **Use only on channels you are authorised to listen to.** OnAir never recovers, guesses, searches for or cracks a key, never
> attacks a weak cipher, and never ships or downloads keys. It decrypts a call only when you have loaded a key that matches it.
> A first-use notice asks you to confirm this before any decryption is enabled.

## What is and is not supported

| Protocol | Ciphers OnAir can apply with your key | Notes |
|---|---|---|
| **P25** | ADP (RC4, 40-bit), DES-OFB, AES-256-OFB | Per TIA-102.AAAD. The message indicator (MI), algorithm id and key id come from the HDU / LDU2, so a key associated with that key id is selected automatically. Fully wired. |
| **NXDN** | scrambler (15-bit LFSR), DES, AES | The scrambler seed is your 15-bit key. The cipher type and key id come from the call. Fully wired. |
| **DMR** | AES-128 / AES-256 (ETSI TS 102 361-4 framing), ARC4, **Hytera-BP** (Hytera Basic Privacy: 5, 16 or 32 byte key), **Hytera-Enhanced** (Hytera Enhanced Privacy: 5 byte key + message indicator) | The Motorola "Basic / Enhanced Privacy" key and IV handling is proprietary and not implemented. The two Hytera types are not ETSI standards: the key stream follows the open-source DSD-FME (ISC licence, credited in THIRD_PARTY.md and the in-app Credits), which found it by analysing Hytera radios. You enter your own key in hex; OnAir never searches for or recovers a key. **Status:** the key stream code, the key types, the Keys window and the tests (known-answer vectors made with the DSD-FME algorithm, synthetic encrypt-then-decrypt) are in; the DMR receiver does **not** yet apply them to live audio, because that needs the AMBE+2 frame bits after error correction (the voice codec is a user-installed black box today). Not tested against a real Hytera call. |
| **dPMR** | scrambler | The dPMR cipher field is not yet parsed by the receiver; detection/skip is unchanged for now. |

OnAir implements **only ciphers with public specifications and standard primitives** (RC4, DES per FIPS 46-3, AES per FIPS 197,
OFB per SP 800-38A, the published 15-bit scrambler). It **never** implements restricted or secret algorithms such as TETRA
TEA1-4. Where a vendor's framing is not public, that is stated above rather than reverse-engineered.

The cipher code is validated against public known-answer vectors (RC4, DES) and, for the per-call path, against OnAir's own
**synthetic** encrypted frames — never against captured traffic. See `tests/voicekeys/test_voice_keys.cpp`.

## The Keys window

Open **Tools > Voice keys** (or press **Add key...** next to "encrypted: add your key" in a voice panel). The first time, a notice asks you to confirm you only use channels you are authorised to listen to; nothing works before that. The window shows *keys locked / unlocked*, asks for the passphrase when the passphrase fallback is in use, shows keys masked (press Reveal to see one), imports from CSV with a preview, exports (plain CSV with a warning, or locked with a passphrase) and has a Clear all button with confirmation. Use it to add, edit, delete, enable/disable, import and export keys. Each key has:

- a **name** (shown in the status line, e.g. *decrypted with key "club net"*),
- a **protocol** (DMR / P25 / NXDN / dPMR, or *any*),
- an **algorithm** (RC4, DES-OFB, AES-128-OFB, AES-256-OFB, scrambler),
- the **key** itself, entered as hex and validated for the algorithm's length (RC4 any length; DES 8 bytes; AES-128 16; AES-256 32; scrambler a 15-bit seed, so the first hex digit is 0–7),
- optional **associations** — key id, algorithm id, talkgroup, unit id, NAC (P25), colour code (DMR) — used to pick the key for a call automatically.

### How a key is chosen for a call

1. If the call carries a **key id** (P25/NXDN), only the enabled keys whose associations match it are used — normally exactly one.
2. If the call carries **no key id**, OnAir tries your **enabled list for that protocol, in the order you stored it**. This is the
   only "trying" OnAir ever does; it is your own small list, never the whole key space, and the window tells you it is happening.
3. Nothing is tried, and nothing is decrypted, until you accept the first-use legal notice.

**Wrong-key detection** is advisory only: if the decoded vocoder frames look like noise (frame-error / FEC / CRC sanity), the
status shows *key mismatch*. OnAir never responds to a mismatch by trying other keys against the traffic.

Per-call status text: **encrypted: add your key** / **decrypted with key *name*** / **key mismatch** / **clear**.

### Import / export (CSV)

Columns (a header row is optional; `#` lines are comments):

```
name,protocol,algorithm,key,keyid,algid,talkgroup,unit,nac,cc,enabled
club net,DMR,AES-256-OFB,00112233…ff,,,101,,,1,1
sar,P25,AES-256-OFB,00112233…ff,3,0x84,,,0x293,,1
```

## Storage and privacy

- Keys are stored **locally only**, encrypted at rest with AES-256-CTR and an HMAC-SHA256 integrity tag.
- The encryption (master) key comes from the **OS keychain** when a provider is installed — macOS Keychain, Windows DPAPI /
  Credential Manager, Linux libsecret — via `voicekeys::setSecretProvider()`. When no keychain is available, the store falls back
  to a file encrypted by a **passphrase you type** (PBKDF2-HMAC-SHA256, 200 000 rounds). A wrong passphrase or any tampering fails
  the integrity check with a clear error.
- Keys are **never** uploaded, and never written to logs, crash reports, bug-report zips, IQ metadata, the remote-control API or the
  web server. Web sessions never see or use keys. The key store is a process-global in `core`; the web and API layers do not read it.

## For developers

- `core/include/dect2/voicekeys/voice_keys.h` — the key model, the store (`voicekeys::store()`), matching and CSV.
- `core/include/dect2/voicekeys/voice_crypto.h` — RC4, DES, OFB, the 15-bit scrambler.
- `core/include/dect2/voicekeys/voice_decrypt.h` — the per-call `Decryptor` and `lookupForCall()`.
- `core/include/dect2/voicekeys/key_guard.h` — which file names count as key material (never zipped into a bug report).
- `app/keys/` — the Keys window; `app/platform/keychain_*` — the OS keychain providers (macOS Keychain, Windows Credential Manager, Linux libsecret).
- `core/include/dect2/voicekeys/key_persist.h` — encrypt-at-rest, the keychain provider hook, CSV-independent (de)serialization.

A receiver that has detected encryption builds a `Decryptor` from `lookupForCall()` and runs each voice frame's bits through
`decryptFrame()` before the vocoder; with no matching key it builds nothing and the call is reported and skipped, exactly as before.
See the P25 (`core/src/p25/p25_rx.cpp`) and NXDN (`core/src/nxdn/nxdn_rx.cpp`) receivers for the reference wiring.
