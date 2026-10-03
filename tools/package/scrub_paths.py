#!/usr/bin/env python3
"""Make sure no build-machine details end up in shipped programs.
Compilers and libraries embed the folder they were built in (assert messages, FFmpeg's configuration line, ...). This replaces the
current user's name inside those paths with filler of the same length (so the files stay valid) and reports what it found.
  scrub_paths.py FILE_OR_DIR ...        scrub in place
  scrub_paths.py --check FILE_OR_DIR ...  only report (exit 1 if anything was found)
"""
import os, sys, getpass

def targets(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    full = os.path.join(root, f)
                    if os.path.isfile(full) and not os.path.islink(full):
                        yield full
        elif os.path.isfile(p):
            yield p

def main():
    args = sys.argv[1:]
    check = "--check" in args
    args = [a for a in args if a != "--check"]
    user = getpass.getuser().encode()
    home = os.path.expanduser("~").encode()
    needles = [n for n in {user, os.path.basename(home)} if len(n) >= 4]
    found = 0
    for path in targets(args):
        try:
            data = open(path, "rb").read()
        except OSError:
            continue
        new = data
        hits = 0
        for n in needles:
            c = new.count(n)
            if c:
                hits += c
                filler = (b"build" * (len(n) // 5 + 1))[:len(n)]
                new = new.replace(n, filler)
        if hits:
            found += hits
            print(("found" if check else "scrubbed"), hits, "x in", path)
            if not check:
                mode = os.stat(path).st_mode
                open(path, "wb").write(new)
                os.chmod(path, mode)
    print("total:", found)
    return 1 if (check and found) else 0

if __name__ == "__main__":
    sys.exit(main())
