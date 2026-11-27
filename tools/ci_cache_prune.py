#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

"""Delete the older copies of each ccache family in the GitHub Actions cache (docs/CI-FAST.md (4.7)).

Every run saves its ccache under a new key (run id or commit hash at the end), so each family piled up copies until the repository passed
GitHub's 10 GB and the newest caches were the ones evicted. A family is the key without its run-id / commit-hash / file-hash parts, per
branch; the most recently used entry of each family stays. Only keys starting with "ccache-" are touched (the apt, MSYS2 and FFmpeg
caches are keyed by their contents). Needs gh with a token that has actions: write.
  tools/ci_cache_prune.py --repo adswill/OnAir [--dry-run]
"""
import argparse, json, re, subprocess, sys

def family(key):
    parts = key.split("-")
    keep = [p for p in parts if not (re.fullmatch(r"[0-9a-f]{12,}", p) or re.fullmatch(r"\d{6,}", p))]
    return "-".join(keep)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    out = subprocess.run(["gh", "api", "--paginate", "repos/%s/actions/caches?per_page=100" % a.repo, "--jq", ".actions_caches[]"],
                         capture_output=True, text=True, check=True).stdout
    caches = [json.loads(l) for l in out.splitlines() if l.strip()]
    groups = {}
    for c in caches:
        if not c["key"].startswith("ccache-"):
            continue
        groups.setdefault((family(c["key"]), c["ref"]), []).append(c)
    freed, n = 0, 0
    for (fam, ref), cs in sorted(groups.items()):
        cs.sort(key=lambda c: c["last_accessed_at"], reverse=True)
        for c in cs[1:]:
            print("%s %s %s (%d MB, last used %s; kept %s)" % ("would delete" if a.dry_run else "delete", ref, c["key"], c["size_in_bytes"] // 1000000,
                                                                c["last_accessed_at"][:16], cs[0]["key"]))
            if not a.dry_run:
                r = subprocess.run(["gh", "api", "-X", "DELETE", "repos/%s/actions/caches/%d" % (a.repo, c["id"])], capture_output=True, text=True)
                if r.returncode != 0:
                    print("  failed: " + (r.stderr or r.stdout).strip(), file=sys.stderr)
                    continue
            freed += c["size_in_bytes"]; n += 1
    print("%s %d entries, %d MB" % ("would free" if a.dry_run else "freed", n, freed // 1000000))

if __name__ == "__main__":
    main()
