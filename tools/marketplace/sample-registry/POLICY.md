# Listing policy

A plugin is listed only if all of this holds. The validator (`tools/onair_registry.py lint`) checks what a program can check; a maintainer decides the rest.

1. **Open licence.** The page and the package manifest carry an SPDX licence id of an open or freely redistributable licence (MIT, BSD, Apache-2.0, GPL, LGPL, MPL, CC0, ...). No "proprietary", "all rights reserved", non-commercial or no-derivatives terms. The source is public (`source` URL) unless the plugin is only listed, never "reviewed".
2. **No patented codecs.** No plugin ships or builds a patented voice codec (AMBE/IMBE and relatives, ACELP, G.729, ...). Users install codecs themselves (OnAir's `docs/CODECS.md`); the registry does not host codec builds or links to them.
3. **No encryption cracking.** No tool that breaks, brute-forces or bypasses encryption. Decoding with a key the user typed or imported is fine.
4. **Declared permissions.** The permissions on the page equal what the package manifest asks for and are the real needs (network, filesystem, audio-out, record, spawn-process). A plugin that reads files outside its own folder must declare `filesystem`; credentials, wallets, browser data and system files are never to be touched.
5. **Honest description.** What it does, what it needs, what it sends over the network (if it declares `network`). No hidden functions, no telemetry that is not described on the page.
6. **Artefacts match.** Every platform download is a `.onairplugin` package at a stable https URL (a release asset) with the sha256 and size on the page. The package's id, version, kind and ABI equal the page.
7. **Versions only go up.** A new version has a higher number than the listed one. A bad release is withdrawn through `revoked.json`, not by lowering the version.
8. **Names.** No impersonation of OnAir, of another author or of a company.

## Review levels

| Level | Meaning |
| --- | --- |
| `listed` | meets the rules above, checked by the validator. Nobody read the code. |
| `reviewed` | a maintainer read the source (public) and found nothing that breaks the rules. A person looked; it is **not** a guarantee. |
| `verified` | reviewed, and a maintainer built it from the tagged source and the build matches the package. Still not a guarantee. |

OnAir has no sandbox for untrusted plugins yet. Every plugin is native code that runs with the program's rights, whatever its level.

## Withdrawing

`revoked.json` is a list of `{"id": "...", "version": "*" | "1.2.0" | "<=1.2.0", "reason": "..."}`. At the next refresh every OnAir that has the plugin switches it off and shows the reason. The reason is shown to users: keep it factual.
