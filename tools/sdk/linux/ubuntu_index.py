# SPDX-License-Identifier: GPL-3.0-or-later
"""linux/ubuntu_index.py - used by linux/fetch_sysroot.sh: resolve Ubuntu packages and their dependencies.

    python3 -I ubuntu_index.py <asked> <exclude> <minimum> <Packages file>...

<asked>, <exclude> and <minimum> are files with one name (or glob) per line: the packages wanted; runtime-only
dependencies that linking does not need; and packages whose version is only recorded (the SDK's minimum versions). The
Packages files are read in order; for a name in several of them the highest version wins (updates and security over
the release). Prints "minimum name version" for each of <minimum>, then the closure, one
"name version filename sha256" per line, sorted by name.
"""
import fnmatch
import sys


def order(c):
    if c == "~":
        return -1
    if c.isdigit():
        return 0
    if c.isalpha():
        return ord(c)
    return ord(c) + 256


def cmp_part(a, b):   # one upstream_version or debian_revision, as dpkg compares them
    i = j = 0
    while i < len(a) or j < len(b):
        first = 0
        while (i < len(a) and not a[i].isdigit()) or (j < len(b) and not b[j].isdigit()):
            ca = order(a[i]) if i < len(a) and not a[i].isdigit() else 0
            cb = order(b[j]) if j < len(b) and not b[j].isdigit() else 0
            if ca != cb:
                return ca - cb
            i += 1
            j += 1
        while i < len(a) and a[i] == "0":
            i += 1
        while j < len(b) and b[j] == "0":
            j += 1
        while i < len(a) and a[i].isdigit() and j < len(b) and b[j].isdigit():
            if not first:
                first = ord(a[i]) - ord(b[j])
            i += 1
            j += 1
        if i < len(a) and a[i].isdigit():
            return 1
        if j < len(b) and b[j].isdigit():
            return -1
        if first:
            return first
    return 0


def split(v):
    e, _, rest = v.partition(":") if ":" in v else ("0", "", v)   # the epoch ends at the first colon
    u, _, r = rest.rpartition("-") if "-" in rest else (rest, "", "0")
    return int(e or 0), u, r


def vcmp(a, b):
    ea, ua, ra = split(a)
    eb, ub, rb = split(b)
    if ea != eb:
        return ea - eb
    return cmp_part(ua, ub) or cmp_part(ra, rb)


def names(path):
    with open(path) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def stanzas(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        cur, key = {}, None
        for line in f:
            if line == "\n":
                if cur:
                    yield cur
                cur, key = {}, None
            elif line[0] in " \t":
                if key:
                    cur[key] += line
            else:
                key, _, val = line.partition(":")
                cur[key] = val.strip()
        if cur:
            yield cur


def deps(field):   # "a (>= 1), b | c:any" -> [["a"], ["b", "c"]]
    out = []
    for group in field.split(","):
        alts = []
        for alt in group.split("|"):
            n = alt.strip().split(" ")[0].split("(")[0].split(":")[0].strip()
            if n:
                alts.append(n)
        if alts:
            out.append(alts)
    return out


def main():
    asked, exclude, minimum = names(sys.argv[1]), names(sys.argv[2]), names(sys.argv[3])
    pkgs, provides = {}, {}
    for path in sys.argv[4:]:
        for s in stanzas(path):
            n, v = s.get("Package"), s.get("Version")
            if not n or not v:
                continue
            if n not in pkgs or vcmp(v, pkgs[n]["Version"]) > 0:
                pkgs[n] = s
    for n, s in pkgs.items():
        for alts in deps(s.get("Provides", "")):
            provides.setdefault(alts[0], []).append(n)

    def skipped(n):
        return any(fnmatch.fnmatchcase(n, g) for g in exclude)

    chosen, queue, problems = {}, list(asked), []
    for n in asked + minimum:
        if n not in pkgs:
            problems.append(f"no package {n}")
    while queue:
        n = queue.pop(0)
        if n in chosen or n not in pkgs:
            continue
        chosen[n] = pkgs[n]
        for alts in deps(pkgs[n].get("Pre-Depends", "")) + deps(pkgs[n].get("Depends", "")):
            if any(skipped(a) for a in alts):
                continue            # not needed to link
            pick = None
            for a in alts:          # a real package already chosen, then a real one, then a provider
                if a in chosen:
                    pick = a
                    break
            if not pick:
                for a in alts:
                    if a in pkgs:
                        pick = a
                        break
            if not pick:
                for a in alts:
                    prov = [p for p in provides.get(a, []) if not skipped(p)]
                    if prov:
                        pick = next((p for p in prov if p in chosen), sorted(prov)[0])
                        break
            if not pick:
                print(f"warning: {n} depends on {' | '.join(alts)}, which no package provides (skipped)", file=sys.stderr)
                continue
            if pick not in chosen:
                queue.append(pick)
    if problems:
        sys.exit("error: " + "; ".join(problems))
    for n in minimum:
        print("minimum", n, pkgs[n]["Version"])
    for n in sorted(chosen):
        s = chosen[n]
        print(n, s["Version"], s["Filename"], s["SHA256"])


if __name__ == "__main__":
    main()
