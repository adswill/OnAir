#!/usr/bin/env python3
"""Copy every non-system dynamic library an executable needs into OnAir.app/Contents/Frameworks and rewrite the load paths,
so the app runs on a Mac without Homebrew. Usage: bundle_macos.py OnAir.app"""
import os, re, shutil, subprocess, sys, glob

app = sys.argv[1]
exe = os.path.join(app, "Contents/MacOS/OnAir")
fw = os.path.join(app, "Contents/Frameworks")
os.makedirs(fw, exist_ok=True)

def run(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout

def deps(path):
    out = run("otool", "-L", path).splitlines()[1:]
    return [l.strip().split(" (")[0] for l in out if l.strip()]

def rpaths(path):
    out = run("otool", "-l", path).splitlines()
    r = []
    for i, l in enumerate(out):
        if "cmd LC_RPATH" in l:
            for j in range(i, min(i + 4, len(out))):
                m = re.search(r"path (.+) \(offset", out[j])
                if m: r.append(m.group(1))
    return r

SEARCH = ["/opt/homebrew/lib", "/usr/local/lib", "/opt/homebrew/opt"]
def resolve(dep, owner):
    d = os.path.dirname(owner)
    if dep.startswith("@loader_path/"): return os.path.normpath(os.path.join(d, dep[len("@loader_path/"):]))
    if dep.startswith("@executable_path/"): return os.path.normpath(os.path.join(os.path.dirname(exe), dep[len("@executable_path/"):]))
    if dep.startswith("@rpath/"):
        name = dep[len("@rpath/"):]
        for rp in rpaths(owner) + SEARCH:
            rp = rp.replace("@loader_path", d).replace("@executable_path", os.path.dirname(exe))
            c = os.path.join(rp, name)
            if os.path.exists(c): return os.path.realpath(c)
        for rp in SEARCH:   # last resort: any copy in the Homebrew tree
            for root, _, files in os.walk(rp):
                if os.path.basename(name) in files: return os.path.realpath(os.path.join(root, os.path.basename(name)))
        raise SystemExit("cannot resolve " + dep + " for " + owner)
    return os.path.realpath(dep)

def is_system(dep):
    return dep.startswith("/usr/lib/") or dep.startswith("/System/")

done = {}   # real path -> bundled name
queue = [exe]
bundled = []

# The radio libraries that OnAir loads at run time when they are present (the native drivers): they go into the app too, so that nothing has
# to be installed next to it. Every version name a driver may ask for is kept (the symlinks of the Homebrew copy become symlinks here).
# A missing one stops the build (a release without it would silently lose that radio), unless ONAIR_ALLOW_MISSING_RADIO_LIBS=1 (local test builds).
# The value is how to get it. libiio has no Homebrew formula: Pluto users install ADI's libiio package, so it is optional here.
REQUIRED = {"rtlsdr": "brew install librtlsdr", "airspy": "brew install airspy", "airspyhf": "brew install airspyhf",
            "bladeRF": "brew install libbladerf", "LimeSuite": "brew install limesuite", "uhd": "brew install uhd"}
OPTIONAL = {"iio": "ADI's libiio package (github.com/analogdevicesinc/libiio/releases)"}
allow_missing = os.environ.get("ONAIR_ALLOW_MISSING_RADIO_LIBS") == "1"
links = []
missing = []
for stem in list(REQUIRED) + list(OPTIONAL):
    found = False
    for d in SEARCH[:2]:
        for src in sorted(glob.glob(os.path.join(d, "lib%s.*dylib" % stem)) + glob.glob(os.path.join(d, "lib%s*.dylib" % stem))):
            name = os.path.basename(src)
            if name.endswith(".a"): continue
            found = True   # also when an earlier stem's glob already copied it (libairspy*.dylib takes libairspyhf too)
            if os.path.exists(os.path.join(fw, name)): continue
            real = os.path.realpath(src)
            if os.path.islink(src):
                links.append((name, os.path.basename(real)))
                if real not in done:
                    dst = os.path.join(fw, os.path.basename(real))
                    shutil.copy2(real, dst); os.chmod(dst, 0o755)
                    done[real] = os.path.basename(real)
                    bundled.append(dst); queue.append(dst)
            else:
                dst = os.path.join(fw, name)
                shutil.copy2(real, dst); os.chmod(dst, 0o755)
                done[real] = name
                bundled.append(dst); queue.append(dst)
    if not found:
        if stem in OPTIONAL: print("note: lib%s is not bundled (users install %s)" % (stem, OPTIONAL[stem]), file=sys.stderr)
        else: missing.append("missing radio library lib%s: %s" % (stem, REQUIRED[stem]))
for m in missing: print("warning:" if allow_missing else "error:", m, file=sys.stderr)
if missing and not allow_missing: sys.exit(1)
for name, target in links:
    if name != target and not os.path.exists(os.path.join(fw, name)): os.symlink(target, os.path.join(fw, name))
while queue:
    cur = queue.pop()
    for dep in deps(cur):
        if is_system(dep) or os.path.basename(dep) == os.path.basename(cur): continue
        real = resolve(dep, cur)
        name = os.path.basename(real)
        if real not in done:
            done[real] = name
            dst = os.path.join(fw, name)
            if os.path.abspath(real) != os.path.abspath(dst): shutil.copy2(real, dst)   # a library found in the app itself is already in place
            os.chmod(dst, 0o755)
            bundled.append(dst)
            queue.append(dst)
        subprocess.run(["install_name_tool", "-change", dep, "@executable_path/../Frameworks/" + done[real], cur], check=True, capture_output=True)

for lib in bundled:
    subprocess.run(["install_name_tool", "-id", "@executable_path/../Frameworks/" + os.path.basename(lib), lib], check=True, capture_output=True)
    for rp in rpaths(lib):
        subprocess.run(["install_name_tool", "-delete_rpath", rp, lib], capture_output=True)
for rp in rpaths(exe):
    subprocess.run(["install_name_tool", "-delete_rpath", rp, exe], capture_output=True)
print("bundled %d libraries" % len(bundled))
