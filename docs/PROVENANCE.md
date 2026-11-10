# Provenance: who wrote what, under which licence

This page explains how OnAir's authorship and licensing can be shown, what the licence situation is, and how contributions are handled. It is the companion of `REUSE.toml` (the licence and holder of every file), `THIRD_PARTY.md` (every third-party project and what OnAir took from it) and `LICENSES/` (the licence texts).

## 1. Licence summary

| What | Licence | Where it is stated |
|---|---|---|
| OnAir's own source code, documents, data tables, tests | GPL-3.0-or-later. Holder line: `Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors` | `LICENSE`, `README.md` (License), `REUSE.toml` (default entry) |
| The OnAir name and logo (assets/branding, packaging/icons, the Windows installer bitmaps, the Android launcher drawables, web favicons) | All rights reserved, (c) 2026 Kristijonas Adomaitis; not covered by the GPL | `LICENSES/LicenseRef-OnAir-Logo.txt`, `assets/branding/README.md` |
| Plug-in SDK header, examples and Rust SDK (`sdk/`) | CC0-1.0 (the template: MIT), so a plug-in is not a derivative work of OnAir | `sdk/include/onair_plugin.h`, `docs/plugins.md` |
| Vendored libraries (`third_party/`) | Their own: Dear ImGui MIT, stb MIT or Unlicense, miniaudio MIT-0 (or public domain), SQLite blessing (public domain), Lua MIT, whisper.cpp and ggml MIT, RNNoise BSD-3-Clause, codec2 LGPL-2.1-only (kiss_fft and LPCNet files BSD-3-Clause), uat_fec GPL-2.0-or-later and Phil Karn's LGPL files, Inter and JetBrains Mono OFL-1.1 | `REUSE.toml`, `THIRD_PARTY.md`, each directory's own LICENSE/COPYING |
| Code ported or adapted from other projects | The original holders' lines are kept next to ours and the entry says which licence the upstream uses (DecSAT, etisnoop, nrsc5, e-Czas, railair-terminal, dump978, librtlsdr, welle.io, gr-dvbt2, WSJT-X, fldigi, dtmb-sdr, FFmpeg tables) | `REUSE.toml` (section "other people's code"), `THIRD_PARTY.md` |
| Libraries the user installs (mbelib, faad2, libfdk-aac, the DRA and AVS+ add-ons, SDRplay API, vendor radio libraries) | Not distributed and not linked at build time; OnAir loads them at run time | `docs/CODECS.md`, `THIRD_PARTY.md` |

OnAir ships no patented speech codec. Codec plug-ins (`onair-codec-<id>`) are installed by the user.

### How the licence is recorded (REUSE)

`REUSE.toml` (REUSE specification 3.x) gives every file a licence and a copyright holder by path pattern, so the files do not have to carry a header. The last matching entry wins; a notice inside a file wins over the entry (`precedence = "closest"`).

- `python3 tools/release/check_spdx.py` checks, offline, that every tracked file is covered, that every licence id used has its text in `LICENSES/`, and lists the files marked `LicenseRef-Unknown` (`--strict` fails on those too). It is safe for CI.
- `python3 tools/release/add_spdx.py --dry-run` shows what the code-freeze step will do; `--apply` writes the two SPDX comment lines at the top of OnAir-owned source files (idempotent; vendored, generated and other-holder files are skipped; `--selftest` compiles a sample before and after).
- If you have the `reuse` tool, `reuse lint` should agree; it was not available when this was written.

## 2. How authorship can be shown

1. **Git history.** The history starts on 2026-10-03 with the commit "OnAir 0.1.0" and every commit is by the author or a contributor under their own name. The public repository (adswill/OnAir) has its own history, made from this one by rebase, so commit hashes differ between the two; the commit messages, dates and authors are the same. Contributors' pull requests are merged with their authorship (see CONTRIBUTORS.md and the merge commits "Merge pull request #n from ...").
2. **Co-authors.** Work written with an AI assistant carries a `Co-Authored-By: Claude ... <noreply@anthropic.com>` line in the commit message. Third-party authors are credited in THIRD_PARTY.md, the in-app credits, CONTRIBUTORS.md and the notices in the files, not in commit trailers.
3. **Signed tags.** The existing release tags (for example `v0.2.9`) are lightweight, not signed. To make a release provable, create the release tag as a signed annotated tag with the owner's key (`git tag -s v3.0.0`), publish the public key in the repository and on the website, and verify with `git tag -v`. This is a decision for the owner; nothing was signed here.
4. **Release checksums.** Every release lists SHA-256 sums (`SHA256SUMS`); the updater verifies against GitHub's own digest (docs/UPDATE-MIRROR.md).
5. **Measured data that only the author's tools can reproduce.** Some tables in the repository are results of OnAir's own test programs, with the method and the seeds written down. They show that the numbers were measured with these tools, not copied:
   - per-MODCOD DVB-S2 LDPC correction factors (`s2LdpcAlpha`, measured for all 55 S2X MODCODs; branch dec-satTv-030, see docs/CHANGELOG-3.0.md),
   - the sensitivity and lock-time baseline from `tools/sens/sens_sweep` (docs/SENSITIVITY.md),
   - the DAB and DRM continuity audits (docs/DAB-AUDIT.md, docs/DRM-AUDIT.md), the performance tables (docs/PERFORMANCE*.md), the TX quality tables (docs/TX-OPTIMISATION.md),
   - golden digests in tests (for example `tests/kaal/kaal_golden*.h`) made by the generators next to them.
   These sit in the history with dates; re-running the tools gives the same figures (they use fixed seeds).
6. **Clean-room notes.** Where code was written from a specification or from reading another project's documentation, `THIRD_PARTY.md` says "read / re-implemented" and the file header names the source. Where code or tables were taken, the row says "ported / adapted / generated" and REUSE.toml keeps the original holder.

## 3. Contributions from other people

CONTRIBUTING.md says: "OnAir is released under the GNU General Public License v3.0 or later. By sending a contribution you agree that it is released under the same licence." This is *inbound = outbound*: a pull request is accepted under GPL-3.0-or-later, the contributor keeps their copyright, and the holder line "OnAir contributors" covers them. There is no copyright assignment. There is no developer certificate of origin sign-off; the maintainer adds contributors to CONTRIBUTORS.md (`tools/contributors.tsv`), and the merge commit keeps their authorship. If a contribution comes from code with another licence, the contributor must say so in the pull request, and the file gets an entry in `REUSE.toml` and a row in `THIRD_PARTY.md` before it is merged.

Contributors with merged work since 0.2.9: @drfruct (TETRA voice, PRs #259, #260, #263, #272), @xeonliu (DTMB GBK service names, PR #276). Earlier: @ffishman (PR #217), @frank-1225 (PR #231, Chinese railway decoders), Arclamp (DecSAT).

## 4. Open points (decisions or facts the repository does not contain)

These are marked in `REUSE.toml` and counted by `check_spdx.py`.

| Point | What is known | Needed |
|---|---|---|
| `third_party/codec2/src/varicode.c` | The header says "GNU General Public License version 2.1" (there is no such version); the directory's COPYING is LGPL-2.1. Marked `LicenseRef-Unknown`. | Owner: confirm against the upstream codec2 tree that the file is LGPL-2.1. |
| `third_party/sddc_fx3/SDDC_FX3.img` | The SDDC projects are MIT; the image contains Cypress/Infineon FX3 SDK object code whose terms are not in the repository. Marked `LicenseRef-Unknown`. | Owner: keep the statement in `README-OnAir.md` or take the SDK terms from Infineon. |
| `voice_listen/*.wav` | A spoken test phrase and its codec round trips; who spoke it and under what terms is not recorded. Marked `LicenseRef-Unknown`. | Owner: who recorded it, or replace it. |
| uat_fec: Phil Karn's files | The headers say "LGPL" without a version. Marked `LicenseRef-Karn-LGPL`. | Check upstream libfec. |
| "Or later" of upstream projects | THIRD_PARTY.md and the file headers differ or are silent for e-Czas, gr-dvbt2, WSJT-X and the fldigi keyboard tables (plain "GPL-3.0"), and for nrsc5 (table says -or-later, headers say GPL-3.0). REUSE.toml uses the stricter `GPL-3.0-only` where only "GPL-3.0" is stated and `GPL-3.0-or-later` for nrsc5. | Owner: check each upstream's source headers, then relax or keep. |
| DecSAT-derived set | The Arclamp line is attached to whole directories (dvbs, ccsds, satbeacon and a few tv files, with their tests and UI). Some files in them are OnAir-written. | Owner or Arclamp: name the files that are really derived. |
| Holder names | dtmb-sdr (author handle ningzichun), railair-terminal (frank-1225), nrsc5 (Theori and contributors) are recorded as the projects name them. | Check real names if wanted. |
| `CC0-1.0` text | Downloaded from https://spdx.org/licenses/CC0-1.0.txt with the owner's OK (saved unchanged, including the SPDX template markers). | Done. |
| Texts not from the SPDX list | `Unlicense.txt` and `MIT-0.txt` are the texts as they appear in the vendored files (`Unlicense` without the "For more information" line; `MIT-0` with the placeholder copyright line); `blessing.txt` is SQLite's blessing. `GPL-3.0-or-later`, `GPL-3.0-only`, `GPL-2.0-or-later`, `LGPL-2.1-only` and `LGPL-2.1-or-later` are copies of the GPL-3.0, GPL-2.0 and LGPL-2.1 texts already in the repository. `CRRD-MIT.txt` is the MIT notice of Alex L Manstein (THIRD_PARTY.md). | Optionally replace by the spdx.org versions. |
| Logo copies | Other copies of the logo may be embedded in source (for example generated icon data) and are covered by the GPL entry. `assets/promo-thumb-0.2.7.jpg` and `docs/img/*.jpg` (screenshots, with map tiles) are under the default entry. | Owner: decide if any of these should be "all rights reserved" or carry the OpenStreetMap attribution. |
| Files with their own header | Only a handful of files have a copyright line today (for example `android/gradlew`, Apache-2.0). After the freeze run `add_spdx.py`; the rest stays covered by REUSE.toml. | Run at the code freeze. |

## 5. After the code freeze

1. Merge everything, then `python3 tools/release/add_spdx.py --dry-run`, read the counts and samples.
2. `python3 tools/release/add_spdx.py --selftest 50`, then `--apply`, build, run the tests, and `cargo check --workspace` in `rust/` (on the laptop worker).
3. `python3 tools/release/check_spdx.py` must exit 0.
4. Commit as one commit (a licence-header commit, no other changes) so it can be skipped in blame (`git blame --ignore-rev`).
