#!/usr/bin/env python3
"""Analyse an OnAir debug report together with the symbols of the same build."""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess  # nosec B404 - runs gdb and addr2line with argument lists, never a shell
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

USAGE = """\
The report is the zip a user saved with Ctrl+D, after a crash or after a hang (debug_dump_*.zip). The symbols are the zip of the
CMake target debug_symbols_zip of the same build (symbols_v<version>[_<os>-<arch>].zip, an artifact of the release workflow).

  python3 -I analyse_debug_dump.py symbols_v3.9.0_linux-x86_64.zip debug_dump_v3.9.0_....zip --src <OnAir checkout> --out analysis.md
          [--fetch-system-libs]

Only GNU tools are used: gdb (Linux dumps) and addr2line.
- Linux dumps: the minidump becomes an ELF core (the registers of every thread, the stacks and the code of the modules there are
  files for); gdb unwinds it with the programs from the symbols zip (add-symbol-file at the load address). System libraries that are
  not in the zip are taken from this computer when their build id matches, or with --fetch-system-libs from debuginfod.
- Windows dumps: gdb cannot read minidumps, so the script unwinds x64 itself with the unwind tables (.pdata) of the modules it has,
  and names the frames with addr2line (DWARF in the MinGW build). --fetch-system-libs adds ntdll, kernelbase, ... from Microsoft.
- Always: a stack scan (words on a stack that point just after a call instruction), hints for the usual causes, the end of the log
  and the user's answers. Output: one Markdown file.
The network is used only with --fetch-system-libs: it sends build ids or file names, time stamps and sizes, nothing from the dump.
The zips are untrusted input: run with python3 -I in an empty folder; zip entries outside the work folder are skipped.
"""

# ----------------------------------------------------------------------------- minidump

AMD64 = 9
PLATFORM_LINUX = 0x8201
PLATFORM_WINDOWS = 2


# A minidump: threads, stacks, modules, exception and the Linux streams OnAir writes.
class Dump:

    def __init__(self, data):
        """Read the stream directory and parse the streams."""
        self.d = data
        if self.u32(0) != 0x504D444D:
            raise ValueError("not a minidump (no MDMP signature)")
        n, rva = self.u32(8), self.u32(12)
        self.streams = {}
        for k in range(n):
            typ, size, srva = struct.unpack_from("<III", data, rva + 12 * k)
            if typ:
                self.streams[typ] = (srva, size)
        self.arch, self.platform, self.os_text = None, None, ""
        self.mem, self.threads, self.modules, self.exc = [], [], [], None
        self._parse_sysinfo()
        self._parse_memory()
        self._parse_threads()
        self._parse_names()
        self._parse_modules()
        self._parse_exception()
        self.notes = self.raw(0x4F4E0001).decode("utf-8", "replace")
        self.maps = self.raw(0x47670009).decode("utf-8", "replace")
        self.status = self.raw(0x47670004).decode("utf-8", "replace")
        self.auxv = self.raw(0x47670008)

    def u16(self, off):
        """Little-endian 16-bit value at a file offset."""
        return struct.unpack_from("<H", self.d, off)[0]

    def u32(self, off):
        """Little-endian 32-bit value at a file offset."""
        return struct.unpack_from("<I", self.d, off)[0]

    def u64(self, off):
        """Little-endian 64-bit value at a file offset."""
        return struct.unpack_from("<Q", self.d, off)[0]

    def string(self, rva):
        """A MINIDUMP_STRING (UTF-16)."""
        if not rva or rva + 4 > len(self.d):
            return ""
        n = self.u32(rva)
        return self.d[rva + 4: rva + 4 + n].decode("utf-16-le", "replace")

    def raw(self, typ):
        """The bytes of one stream (empty when it is missing)."""
        if typ not in self.streams:
            return b""
        rva, size = self.streams[typ]
        return self.d[rva: rva + size]

    @property
    def is_linux(self):
        """Whether the dump was written on Linux."""
        return self.platform == PLATFORM_LINUX

    @property
    def is_windows(self):
        """Whether the dump was written on Windows."""
        return self.platform == PLATFORM_WINDOWS

    def _parse_sysinfo(self):
        if 7 in self.streams:
            rva, _ = self.streams[7]
            self.arch = self.u16(rva)
            self.platform = self.u32(rva + 20)
            self.os_text = self.string(self.u32(rva + 24))

    def _parse_memory(self):
        d = self.d
        if 5 in self.streams:
            rva, _ = self.streams[5]
            for i in range(self.u32(rva)):
                start, size, mrva = struct.unpack_from("<QII", d, rva + 4 + 16 * i)
                self.mem.append((start, d[mrva: mrva + size]))
        if 9 in self.streams:   # Memory64List (full dumps)
            rva, _ = self.streams[9]
            n, off = self.u64(rva), self.u64(rva + 8)
            for i in range(n):
                start, size = struct.unpack_from("<QQ", d, rva + 16 + 16 * i)
                self.mem.append((start, d[off: off + size]))
                off += size

    def _parse_threads(self):
        if 3 not in self.streams:
            return
        d = self.d
        rva, _ = self.streams[3]
        for i in range(self.u32(rva)):
            t = rva + 4 + 48 * i
            sstart, ssize, srva = struct.unpack_from("<QII", d, t + 24)
            csize, crva = struct.unpack_from("<II", d, t + 40)
            self.threads.append({"tid": self.u32(t), "stack_start": sstart, "stack": d[srva: srva + ssize],
                                 "ctx": d[crva: crva + csize], "name": ""})
            if ssize and not any(m[0] == sstart for m in self.mem):
                self.mem.append((sstart, d[srva: srva + ssize]))

    def _parse_names(self):
        if 24 not in self.streams:
            return
        rva, _ = self.streams[24]
        names = {}
        for i in range(self.u32(rva)):
            names[self.u32(rva + 4 + 12 * i)] = self.string(self.u64(rva + 8 + 12 * i))
        for t in self.threads:
            t["name"] = names.get(t["tid"], "")

    def _cv_record(self, mod, cv_size, cv_rva):
        if cv_size < 4:
            return
        d = self.d
        sig = self.u32(cv_rva)
        if sig == 0x4C457042:   # 'BpEL': ELF build id
            mod["kind"], mod["id"] = "elf", d[cv_rva + 4: cv_rva + cv_size].hex()
        elif sig == 0x53445352 and cv_size >= 24:   # 'RSDS': GUID + age
            mod["kind"], mod["id"] = "pe", d[cv_rva + 4: cv_rva + 20].hex()

    def _parse_modules(self):
        if 4 not in self.streams:
            return
        rva, _ = self.streams[4]
        for j in range(self.u32(rva)):
            m = rva + 4 + 108 * j
            base, size, _cs, stamp, name_rva = struct.unpack_from("<QIIII", self.d, m)
            cv_size, cv_rva = struct.unpack_from("<II", self.d, m + 76)
            mod = {"base": base, "size": size, "stamp": stamp, "path": self.string(name_rva), "id": None, "kind": None,
                   "file": None, "from": ""}
            self._cv_record(mod, cv_size, cv_rva)
            mod["name"] = re.split(r"[\\/]", mod["path"])[-1]
            self.modules.append(mod)

    def _parse_exception(self):
        if 6 not in self.streams:
            return
        rva, _ = self.streams[6]
        code, flags, _rec, addr, nparams = struct.unpack_from("<IIQQI", self.d, rva + 8)
        params = struct.unpack_from("<15Q", self.d, rva + 40)
        csz, crva = struct.unpack_from("<II", self.d, rva + 160)
        self.exc = {"tid": self.u32(rva), "code": code, "flags": flags, "addr": addr, "params": params[:nparams],
                    "ctx": self.d[crva: crva + csz]}

    def read(self, addr, n):
        """N bytes of process memory at addr, or None when the dump does not have them."""
        for start, b in self.mem:
            if start <= addr and addr + n <= start + len(b):
                return b[addr - start: addr - start + n]
        return None

    def module_at(self, addr):
        """The module that contains addr, or None."""
        for m in self.modules:
            if m["base"] <= addr < m["base"] + m["size"]:
                return m
        return None


REG64 = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]


def ctx_regs(ctx):
    """Return the AMD64 CONTEXT of a minidump as a dict (empty when it is not one)."""
    if len(ctx) < 0x100:
        return {}
    r = {name: struct.unpack_from("<Q", ctx, 0x78 + 8 * i)[0] for i, name in enumerate(REG64)}
    r["rip"] = struct.unpack_from("<Q", ctx, 0xF8)[0]
    r["eflags"] = struct.unpack_from("<I", ctx, 0x44)[0]
    r["cs"], r["ss"] = struct.unpack_from("<H", ctx, 0x38)[0], struct.unpack_from("<H", ctx, 0x42)[0]
    return r


# ----------------------------------------------------------------------------- object files


def _note_build_id(src, off, end):
    n = off
    while n + 12 <= end:
        namesz, descsz, typ = struct.unpack_from("<III", src, n)
        desc = n + 12 + ((namesz + 3) & ~3)
        if typ == 3 and src[n + 12: n + 16] == b"GNU\0":
            return src[desc: desc + descsz].hex()
        n = desc + ((descsz + 3) & ~3)
    return None


def elf_build_id(path):
    """Return the GNU build id of an ELF file (hex), or None."""
    try:
        with open(path, "rb") as f:
            head = f.read(1 << 16)
            if head[:4] != b"\x7fELF" or head[4] != 2:
                return None
            phoff, = struct.unpack_from("<Q", head, 0x20)
            phentsize, phnum = struct.unpack_from("<HH", head, 0x36)
            for i in range(phnum):
                ptype, _fl, off, _va, _pa, filesz = struct.unpack_from("<IIQQQQ", head, phoff + i * phentsize)
                if ptype == 4:   # PT_NOTE
                    f.seek(off)
                    found = _note_build_id(f.read(filesz), 0, filesz)
                    if found:
                        return found
    except (OSError, struct.error):
        return None
    return None


def elf_loads(path):
    """Return the PT_LOAD segments of an ELF file: (offset, vaddr, filesz, memsz, flags)."""
    out = []
    with open(path, "rb") as f:
        d = f.read(65536)
    phoff, = struct.unpack_from("<Q", d, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
    for i in range(phnum):
        ptype, fl, off, va, _pa, filesz, memsz = struct.unpack_from("<IIQQQQQ", d, phoff + i * phentsize)
        if ptype == 1:
            out.append((off, va, filesz, memsz, fl))
    return out


def first_load(segs):
    """Return the page-aligned address of the first PT_LOAD segment (the link-time base)."""
    return min(va for _o, va, _f, _ms, _fl in segs) & ~0xFFF if segs else 0


# The parts of a PE file the unwinder needs: sections, data directories, .pdata, the CodeView id.
class PE:

    def __init__(self, path):
        """Read the headers of a PE file."""
        self.path = path
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError("not a PE file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe: pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        self.stamp = struct.unpack_from("<I", d, pe + 8)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        ndirs = struct.unpack_from("<I", d, opt + 108)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        for i in range(nsec):
            _name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", d, opt + opt_size + 40 * i)
            self.sections.append((va, vsize, rptr, rsize))
        self._pdata = None

    def rva_bytes(self, rva, n):
        """N bytes of the image at a relative virtual address, or None."""
        for va, vsize, rptr, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                o = rptr + (rva - va)
                return self.d[o: o + n]
        return None

    def rsds(self):
        """Return the CodeView GUID (hex) of the debug directory, or None."""
        if len(self.dirs) <= 6 or not self.dirs[6][0]:
            return None
        rva, size = self.dirs[6]
        for k in range(size // 28):
            e = self.rva_bytes(rva + 28 * k, 28)
            if not e:
                break
            typ, addr = struct.unpack_from("<I", e, 12)[0], struct.unpack_from("<I", e, 20)[0]   # Type, AddressOfRawData
            cv = self.rva_bytes(addr, 24) if typ == 2 else None
            if cv and cv[:4] == b"RSDS":
                return cv[4:20].hex()
        return None

    def pdata(self):
        """Return the RUNTIME_FUNCTION entries: (begin, end, unwind info), sorted."""
        if self._pdata is None:
            self._pdata = []
            if len(self.dirs) > 3 and self.dirs[3][0]:
                rva, size = self.dirs[3]
                b = self.rva_bytes(rva, size) or b""
                self._pdata = [struct.unpack_from("<III", b, 12 * k) for k in range(len(b) // 12)]
        return self._pdata

    def function_entry(self, rva):
        """Return the RUNTIME_FUNCTION that contains rva, or None (a leaf function)."""
        p = self.pdata()
        lo, hi = 0, len(p) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            b, e, u = p[mid]
            if rva < b:
                hi = mid - 1
            elif rva >= e:
                lo = mid + 1
            else:
                return b, e, u
        return None


# ----------------------------------------------------------------------------- symbols


def _file_id(path, fname):
    try:
        with open(path, "rb") as fh:
            head = fh.read(4)
    except OSError:
        return []
    if head == b"\x7fELF":
        bid = elf_build_id(path)
        return [bid] if bid else []
    if head[:2] == b"MZ":
        try:
            guid = PE(path).rsds()
        except (ValueError, struct.error):
            guid = None
        return ([guid] if guid else []) + ["name:" + fname.lower()]
    return []


def find_symbol_files(root):
    """Map every ELF build id / PE CodeView id (and PE file name) under root to its path."""
    ids = {}
    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            path = os.path.join(dirpath, fname)
            for key in _file_id(path, fname):
                ids[key] = path
    return ids


def fetch(url, dest):
    """Download an https URL to dest; False when it failed."""
    if urllib.parse.urlparse(url).scheme != "https":
        return False
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "onair-debug-dump-analyser"})
        with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:  # nosec B310 - only https, checked above
            shutil.copyfileobj(r, f)
        return True
    except (OSError, urllib.error.URLError, ValueError):
        if os.path.exists(dest):
            os.remove(dest)
        return False


def _match_zip(m, ids):
    if m["kind"] in ("elf", "pe") and m["id"] in ids:
        return ids[m["id"]], "symbols zip"
    key = "name:" + m["name"].lower()
    if m["kind"] == "pe" and key in ids and m["name"].lower().startswith(("onair", "dect2")):
        try:
            pe = PE(ids[key])
        except (ValueError, struct.error):
            return None, ""
        if pe.stamp == m["stamp"] and pe.size_of_image == m["size"]:
            return ids[key], "symbols zip (time stamp and size)"
    return None, ""


def _match_download(dump, m, libdir):
    if m["kind"] == "elf" and m["id"]:
        dest = os.path.join(libdir, m["id"] + "_" + m["name"])
        urls = os.environ.get("DEBUGINFOD_URLS", "https://debuginfod.ubuntu.com https://debuginfod.elfutils.org").split()
        for base in urls:
            if fetch(base.rstrip("/") + "/buildid/" + m["id"] + "/executable", dest):
                return dest, "debuginfod " + base
    elif dump.is_windows and m["stamp"]:
        dest = os.path.join(libdir, m["name"])
        url = "https://msdl.microsoft.com/download/symbols/%s/%08X%x/%s" % (m["name"], m["stamp"], m["size"], m["name"])
        if fetch(url, dest):
            return dest, "Microsoft symbol server"
    return None, ""


def match_modules(dump, ids, work, fetch_libs):
    """Set m['file'] (a local copy of each module) and m['from'] (where it came from)."""
    libdir = os.path.join(work, "fetched")
    os.makedirs(libdir, exist_ok=True)
    for m in dump.modules:
        m["file"], m["from"] = _match_zip(m, ids)
        if m["file"]:
            continue
        if m["kind"] == "elf" and dump.is_linux and os.path.isfile(m["path"]) and elf_build_id(m["path"]) == m["id"]:
            m["file"], m["from"] = m["path"], "this computer (same build id)"
        elif fetch_libs:
            m["file"], m["from"] = _match_download(dump, m, libdir)


def addr2line(path, addrs):
    """Map file addresses to a list of (function, file:line), inlined frames included."""
    if not addrs:
        return {}
    cmd = ["addr2line", "-f", "-C", "-i", "-a", "-e", path] + ["0x%x" % a for a in addrs]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=False).stdout.splitlines()  # nosec B603 - fixed program, no shell
    except (OSError, subprocess.TimeoutExpired):
        return {}
    res, cur, i = {}, None, 0
    while i < len(out):
        line = out[i]
        if line.startswith("0x") and (i + 1 >= len(out) or not out[i + 1].startswith("0x")):
            cur = int(line, 16)
            res[cur] = []
            i += 1
        elif cur is not None and i + 1 < len(out):
            res[cur].append((out[i], out[i + 1]))
            i += 2
        else:
            i += 1
    return res


_BASES = {}


def file_base(path):
    """Return the image base (PE) or the first PT_LOAD address (ELF) of a file, cached."""
    if path not in _BASES:
        with open(path, "rb") as f:
            head = f.read(2)
        _BASES[path] = PE(path).image_base if head == b"MZ" else first_load(elf_loads(path))
    return _BASES[path]


def _names_for_module(m, lst):
    res = {}
    if m.get("file"):
        fa = {a: file_base(m["file"]) + (a - m["base"]) for a in lst}
        r = addr2line(m["file"], sorted(set(fa.values())))
        res = {a: r.get(fa[a]) for a in lst}
    names = {}
    for a in lst:
        where = "%s+0x%x" % (m["name"], a - m["base"])
        frames = res.get(a)
        if frames and frames[0][0] != "??":
            text = " <- ".join("%s (%s)" % (fn, loc.replace("onair/", "")) for fn, loc in frames)
            names[a] = "%s  [%s]" % (text, where)
        else:
            names[a] = where
    return names


def symbolize(dump, addrs):
    """Map run-time addresses to 'function (file:line)  [module+offset]'."""
    by_mod = {}
    for a in addrs:
        m = dump.module_at(a)
        by_mod.setdefault(id(m) if m else None, (m, []))[1].append(a)
    names = {}
    for m, lst in by_mod.values():
        if m:
            names.update(_names_for_module(m, lst))
        else:
            names.update({a: "0x%x (no module)" % a for a in lst})
    return names


# ----------------------------------------------------------------------------- Linux: ELF core for gdb


def _note(name, typ, desc):
    nb = name + b"\0"
    return struct.pack("<III", len(nb), len(desc), typ) + nb + b"\0" * (-len(nb) % 4) + desc + b"\0" * (-len(desc) % 4)


def _prstatus(t, sig):
    r = ctx_regs(t["ctx"])
    if not r:
        return b""
    user = [r["r15"], r["r14"], r["r13"], r["r12"], r["rbp"], r["rbx"], r["r11"], r["r10"], r["r9"], r["r8"], r["rax"], r["rcx"],
            r["rdx"], r["rsi"], r["rdi"], 0xFFFFFFFFFFFFFFFF, r["rip"], r["cs"] or 0x33, r["eflags"], r["rsp"], r["ss"] or 0x2B,
            0, 0, 0, 0, 0, 0]
    pr = struct.pack("<iii", sig, 0, 0) + struct.pack("<h2x", sig)
    pr += struct.pack("<QQ", 0, 0) + struct.pack("<iiii", t["tid"], 0, 0, 0) + b"\0" * 64
    pr += struct.pack("<27Q", *user) + struct.pack("<i4x", 1)
    out = _note(b"CORE", 1, pr)
    if len(t["ctx"]) >= 0x300:
        out += _note(b"CORE", 2, t["ctx"][0x100:0x300])   # the FXSAVE area: NT_FPREGSET
    return out


def _module_segments(dump):
    loads = []
    for m in dump.modules:
        if not m.get("file") or m["kind"] != "elf":
            continue
        try:
            segs = elf_loads(m["file"])
            with open(m["file"], "rb") as f:
                data = f.read()
        except (OSError, struct.error):
            continue
        bias = m["base"] - first_load(segs)
        loads += [(bias + va, data[off: off + filesz], fl) for off, va, filesz, _ms, fl in segs if not fl & 2]   # read-only parts
    return loads


def write_core(dump, path, first_tid):
    """Write an x86-64 ELF core: registers per thread, auxv, the stacks and the read-only segments of the files there are."""
    sig = dump.exc["code"] if dump.exc else 0
    notes = b""
    for t in sorted(dump.threads, key=lambda t: t["tid"] != first_tid):
        notes += _prstatus(t, sig if t["tid"] == first_tid else 0)
    if dump.auxv:
        notes += _note(b"CORE", 6, dump.auxv)
    loads = [(start, b, 6) for start, b in dump.mem] + _module_segments(dump)
    phnum = 1 + len(loads)
    off = 64 + 56 * phnum
    ph = struct.pack("<IIQQQQQQ", 4, 4, off, 0, 0, len(notes), 0, 4)
    off += len(notes)
    body = [notes]
    for va, b, fl in loads:
        pad = -off % 4096
        body.append(b"\0" * pad)
        off += pad
        ph += struct.pack("<IIQQQQQQ", 1, (fl & 7) or 4, off, va, 0, len(b), len(b), 4096)
        body.append(b)
        off += len(b)
    eh = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    eh += struct.pack("<HHIQQQIHHHHHH", 4, 62, 1, 0, 64, 0, 0, 64, 56, phnum, 64, 0, 0)
    with open(path, "wb") as f:
        f.write(eh + ph)
        for b in body:
            f.write(b)


def gdb_backtraces(dump, core, src):
    """Run gdb on the core with the symbols; return its output (all threads, then the first thread in detail)."""
    cmds = ["set pagination off", "set width 0", "set confirm off", "set debuginfod enabled off", "core-file " + core]
    for m in dump.modules:
        if m.get("file") and m["kind"] == "elf":
            cmds.append("add-symbol-file %s -o 0x%x" % (m["file"], m["base"] - first_load(elf_loads(m["file"]))))
    if src:
        cmds.append("set substitute-path onair " + src)
    cmds += ["info threads", "thread apply all bt 40", "thread 1", "info registers", "x/12i $pc-24", "bt full 12"]
    args = ["gdb", "-nx", "-batch"]
    for c in cmds:
        args += ["-ex", c]
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=1200, check=False)  # nosec B603 - fixed program, no shell
    except (OSError, subprocess.TimeoutExpired) as e:
        return "gdb failed: %s" % e
    return r.stdout + ("\n[gdb stderr]\n" + r.stderr[-4000:] if r.stderr.strip() else "")


# ----------------------------------------------------------------------------- Windows: x64 unwinding with .pdata


def _read_reg(dump, r, addr, reg):
    v = dump.read(addr, 8)
    if v is not None:
        r[reg] = struct.unpack("<Q", v)[0]
    return v is not None


def _op_push(dump, _codes, _i, info, r):
    if not _read_reg(dump, r, r["rsp"], REG64[info]):
        return False
    r["rsp"] += 8
    return True


def _op_alloc_large(_dump, codes, i, info, r):
    r["rsp"] += struct.unpack_from("<H", codes, 2 * i + 2)[0] * 8 if info == 0 else struct.unpack_from("<I", codes, 2 * i + 2)[0]
    return True


def _op_alloc_small(_dump, _codes, _i, info, r):
    r["rsp"] += info * 8 + 8
    return True


def _op_save(dump, codes, i, info, r):
    _read_reg(dump, r, r["rsp"] + struct.unpack_from("<H", codes, 2 * i + 2)[0] * 8, REG64[info])
    return True


def _op_save_far(dump, codes, i, info, r):
    _read_reg(dump, r, r["rsp"] + struct.unpack_from("<I", codes, 2 * i + 2)[0], REG64[info])
    return True


def _op_machframe(dump, _codes, _i, info, r):
    _read_reg(dump, r, r["rsp"] + (8 if info else 0) + 24, "rsp")
    return True


def _op_none(_dump, _codes, _i, _info, _r):
    return True


# UNWIND_CODE operations: kind -> (handler, slots it takes; ALLOC_LARGE: 2 or 3 by its info)
UNWIND_OPS = {0: (_op_push, 1), 1: (_op_alloc_large, 2), 2: (_op_alloc_small, 1), 3: (_op_none, 1), 4: (_op_save, 2),
              5: (_op_save_far, 3), 8: (_op_none, 2), 9: (_op_none, 3), 10: (_op_machframe, 1)}


def _unwind_codes(dump, codes, count, func_off, prolog_size, r):
    i = 0
    while i < count:
        code_off, op = codes[2 * i], codes[2 * i + 1]
        kind, info = op & 15, op >> 4
        handler, slots = UNWIND_OPS.get(kind, (_op_none, 1))
        if kind == 1 and info:
            slots = 3
        if (code_off <= func_off or func_off >= prolog_size) and not handler(dump, codes, i, info, r):
            return False
        i += slots
    return True


def apply_unwind(dump, pe, unw_rva, func_off, r):
    """Undo the prologue of one function (chained unwind infos too); r is changed in place and rsp then points at the return address."""
    prolog_size = 0
    for _depth in range(8):
        hdr = pe.rva_bytes(unw_rva, 4)
        if not hdr:
            return False
        prolog_size, count, frame = hdr[1], hdr[2], hdr[3]
        if frame & 15 and func_off >= prolog_size:   # past the prologue the frame register holds the frame
            r["rsp"] = r[REG64[frame & 15]] - (frame >> 4) * 16
        if not _unwind_codes(dump, pe.rva_bytes(unw_rva + 4, 2 * count) or b"", count, func_off, prolog_size, r):
            return False
        chained = pe.rva_bytes(unw_rva + 4 + 2 * (count + (count & 1)), 12) if hdr[0] >> 3 & 4 else None
        if not chained:
            return True
        unw_rva, func_off = struct.unpack_from("<I", chained, 8)[0], 1 << 30   # the parent's whole prologue applies
    return True


def _pop_return(dump, r):
    b = dump.read(r["rsp"], 8)
    if not b:
        return False
    r["rip"], r["rsp"] = struct.unpack("<Q", b)[0], r["rsp"] + 8
    return True


def win_unwind(dump, regs, pes, max_frames=48):
    """Return the frames (rip list) of one thread, unwound with the PE tables there are; stops where none is known."""
    frames, r, leaf_tries = [], dict(regs), 0
    for _ in range(max_frames):
        rip, rsp = r["rip"], r["rsp"]
        if not rip:
            break
        frames.append(rip)
        m = dump.module_at(rip)
        pe = pes.get(m["path"]) if m else None
        entry = pe.function_entry(rip - m["base"]) if pe else None
        if entry:
            ok = apply_unwind(dump, pe, entry[2], rip - m["base"] - entry[0], r) and _pop_return(dump, r) and r["rsp"] > rsp
        else:   # a leaf or a system call stub: the return address is on top; a second unknown function ends it
            leaf_tries += 1
            ok = leaf_tries == 1 and _pop_return(dump, r)
        if not ok:
            break
    return frames


# ----------------------------------------------------------------------------- stack scan


def is_after_call(code_before):
    """Tell whether the 7 bytes before an address end with a call instruction."""
    b = code_before
    if len(b) < 7:
        return False
    if b[2] == 0xE8:   # call rel32
        return True
    return any(b[k] == 0xFF and ((b[k + 1] >> 3) & 7) == 2 for k in (5, 4, 1, 0))   # call r/m64 (FF /2)


def stack_scan(dump, t, code_reader, limit=32):
    """Return (stack address, value) for each word on the stack that looks like a return address."""
    out = []
    for o in range(0, len(t["stack"]) - 7, 8):
        v = struct.unpack_from("<Q", t["stack"], o)[0]
        m = dump.module_at(v)
        before = code_reader(m, v - 7, 7) if m else None
        if before is not None and is_after_call(before):
            out.append((t["stack_start"] + o, v))
            if len(out) >= limit:
                break
    return out


def _load_code(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
        if data[:2] == b"MZ":
            return "pe", PE(path)
        segs = elf_loads(path)
        return "elf", (data, segs, first_load(segs))
    except (OSError, ValueError, struct.error):
        return None, None


def _elf_code(obj, va, n):
    data, segs, _first = obj
    for off, sva, filesz, _ms, fl in segs:
        if fl & 1 and sva <= va and va + n <= sva + filesz:
            return data[off + va - sva: off + va - sva + n]
    return None


def make_code_reader(_dump):
    """Return read(module, address, n): code bytes from the module's file (cached)."""
    cache = {}

    def read(m, addr, n):
        if not m.get("file"):
            return None
        if m["file"] not in cache:
            cache[m["file"]] = _load_code(m["file"])
        kind, obj = cache[m["file"]]
        if kind == "pe":
            return obj.rva_bytes(addr - m["base"], n)
        if kind == "elf":
            return _elf_code(obj, addr - m["base"] + obj[2], n)
        return None
    return read


# ----------------------------------------------------------------------------- hints


SIGNALS = {4: "SIGILL (illegal instruction)", 6: "SIGABRT (abort: assert, std::terminate, a library gave up)", 7: "SIGBUS",
           8: "SIGFPE (arithmetic: integer division by zero)", 11: "SIGSEGV (invalid memory access)"}
WIN_CODES = {0xC0000005: "access violation", 0xC000001D: "illegal instruction", 0xC00000FD: "stack overflow",
             0xC0000094: "integer division by zero", 0xC0000409: "stack buffer overrun / fast fail (often std::terminate)",
             0x40000015: "abort() / std::terminate (STATUS_FATAL_APP_EXIT)", 0xC0000374: "heap corruption"}
LINUX_HINTS = {4: "Illegal instruction: compare the instruction at the crash (gdb 'x/12i $pc' below) with the SIMD line of system.txt. "
                  "An AVX2/FMA/AVX-512 instruction on a CPU (or VM) without it is the usual cause.",
               6: "abort(): look in log.txt / crash.txt for an assertion or 'terminate called after throwing'."}
SYSTEM_HINTS = [("(cpu only)", "system.txt: some SIMD extensions are in the CPU but switched off by the OS or the virtual machine "
                               "('(cpu only)'): code that checks only CPUID would crash there."),
                ("Running under Wine", "Running under Wine: a difference in Wine is a possible cause."),
                ("AVX2 paths in use: no", "The AVX2 code paths are off on this CPU: the slower generic paths run.")]
WAIT_RE = re.compile(r"(pthread_mutex_lock|__lll_lock_wait|futex_wait|WaitForSingleObject|SleepConditionVariable|"
                     r"std::mutex::lock|AcquireSRWLock)")
ERROR_RE = re.compile(r"\b(error|failed|fail|exception|terminate|assert|abort)\b", re.I)


def _linux_hints(exc, crash_regs):
    code, addr = exc["code"], exc["addr"]
    h = ["Crash signal: %s, address 0x%x." % (SIGNALS.get(code, "signal %d" % code), addr)]
    if code == 11 and addr < 0x10000:
        h.append("The address is near 0: a null pointer (or a small offset from one: a member of a null object).")
    elif code == 11 and crash_regs and abs(addr - crash_regs.get("rsp", 0)) < 1 << 16:
        h.append("The address is next to the stack pointer: most likely a stack overflow (deep or endless recursion: look for "
                 "repeating frames).")
    if code in LINUX_HINTS:
        h.append(LINUX_HINTS[code])
    return h


def _windows_hints(exc):
    code, params = exc["code"], exc["params"]
    h = ["Exception 0x%08x (%s), address 0x%x." % (code, WIN_CODES.get(code, "?"), exc["addr"])]
    if code == 0xC0000005 and len(params) >= 2:
        op = {0: "reading", 1: "writing", 8: "executing"}.get(params[0], "?")
        h.append("Access violation %s 0x%x%s." % (op, params[1], " (null pointer)" if params[1] < 0x10000 else ""))
    if code == 0xC000001D:
        h.append("Illegal instruction: compare with the SIMD line in system.txt (AVX2 on an old CPU or a VM without it).")
    return h


def hints(dump, meta, texts, crash_regs, frame_names):
    """Return hints for the usual causes: the exception, the system, threads waiting on locks, errors in the log."""
    h = []
    if dump and dump.exc:
        h += _linux_hints(dump.exc, crash_regs) if dump.is_linux else _windows_hints(dump.exc)
    h += [text for key, text in SYSTEM_HINTS if key in texts["system"]]
    waits = [n for n in frame_names if WAIT_RE.search(n)]
    if waits:
        h.append("%d frames are in lock / wait functions. If the main thread is one of them (a hang), look for the thread that holds "
                 "that lock: often it waits for the main thread in turn (a deadlock)." % len(waits))
    errs = [line for line in texts["log"].splitlines() if ERROR_RE.search(line)]
    if errs:
        h.append("Log lines with error/failed/exception (last 10):\n" + "\n".join("    " + line for line in errs[-10:]))
    if meta.get("dump_error"):
        h.append("No dump: " + meta["dump_error"])
    return h


# ----------------------------------------------------------------------------- report


def unzip(zpath, dest):
    """Extract a zip into dest, skipping entries that would land outside it."""
    root = os.path.realpath(dest)
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            target = os.path.realpath(os.path.join(root, info.filename))
            if target == root or target.startswith(root + os.sep):
                z.extract(info, root)


def unpack(zips, work):
    """Unpack the report zip into work/report and each symbols zip under work/symbols; return both folders."""
    report_dir, sym_dir = os.path.join(work, "report"), os.path.join(work, "symbols")
    os.makedirs(report_dir, exist_ok=True)
    os.makedirs(sym_dir, exist_ok=True)
    have_report = False
    for z in zips:
        with zipfile.ZipFile(z) as zz:
            names = zz.namelist()
        if "report.txt" in names or "dump.json" in names:
            unzip(z, report_dir)
            have_report = True
        else:
            unzip(z, os.path.join(sym_dir, os.path.splitext(os.path.basename(z))[0]))
    if not have_report:
        sys.exit("no debug report zip (one with report.txt / dump.json) among: " + ", ".join(zips))
    return report_dir, sym_dir


def read_texts(report_dir):
    """Return the text files of the report and the parsed dump.json."""
    def text(name):
        p = os.path.join(report_dir, name)
        if not os.path.exists(p):
            return ""
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    texts = {k: text(k + ".txt") for k in ("report", "system", "log", "crash")}
    try:
        meta = json.loads(text("dump.json") or "{}")
    except json.JSONDecodeError:
        meta = {}
    return texts, meta


def load_dump(report_dir, meta, out):
    """Return the parsed dump.dmp of the report, or None (with the reason in out)."""
    path = os.path.join(report_dir, "dump.dmp")
    if not os.path.exists(path):
        out += ["No dump.dmp in the report (%s)." % (meta.get("dump_error") or "the user left it out"), ""]
        return None
    try:
        with open(path, "rb") as f:
            return Dump(f.read())
    except (ValueError, struct.error) as e:
        out += ["Dump unreadable: %s" % e, ""]
        return None


def main_thread_id(dump):
    """Return the thread id of the main thread (Linux: the process id), or None."""
    mt = re.search(r"^Tgid:\s*(\d+)", dump.status, re.M) if dump.status else None
    tgid = int(mt.group(1)) if mt else None
    return tgid if any(t["tid"] == tgid for t in dump.threads) else None


def dump_summary(dump, crash_tid, main_tid):
    """Return the Markdown lines about the dump: system, threads, the OnAir modules and their symbols."""
    kind = "Linux" if dump.is_linux else "Windows" if dump.is_windows else "other"
    out = ["## Dump", "", "- System: %s (%s)" % (dump.os_text or "?", kind),
           "- Threads in the dump: %d; main thread: %s; crashing thread: %s" % (len(dump.threads), main_tid, crash_tid)]
    if dump.notes.strip():
        out.append("- Threads that did not answer: " + "; ".join(dump.notes.strip().splitlines()))
    for m in dump.modules:
        if re.search(r"(onair|dect2)", m["name"], re.I):
            found = "%s (%s)" % (m["file"], m["from"]) if m["file"] else "**NO MATCHING SYMBOLS** (wrong symbols zip, or another build)"
            out.append("- %s: build id %s -> %s" % (m["name"], m["id"], found))
    missing = [m["name"] for m in dump.modules if not m["file"]]
    out.append("- Modules without a file here (%d): %s" % (len(missing), ", ".join(missing[:40]) + (" ..." if len(missing) > 40 else "")))
    return out + [""]


def linux_section(dump, work, src, first_tid):
    """Write the core, run gdb; return the Markdown lines and gdb's lines (for the hints)."""
    core = os.path.join(work, "core")
    write_core(dump, core, first_tid)
    g = gdb_backtraces(dump, core, src)
    names = {t["tid"]: t["name"] for t in dump.threads}
    g = re.sub(r"LWP (\d+)", lambda mm: "LWP %s \"%s\"" % (mm.group(1), names.get(int(mm.group(1)), "")), g)
    out = ["## gdb (all threads)", "", "Core for gdb: `%s` (load the symbols with add-symbol-file as in the script)." % core, "",
           "```", g.strip(), "```", ""]
    return out, g.splitlines()


def windows_section(dump, crash_tid):
    """Unwind every thread with .pdata; return the Markdown lines and the frame names."""
    pes = {}
    for m in dump.modules:
        if m.get("file"):
            try:
                pes[m["path"]] = PE(m["file"])
            except (ValueError, struct.error):
                pass
    order = sorted(dump.threads, key=lambda t: (t["tid"] != crash_tid, t["tid"]))
    frames = {}
    for t in order:
        regs = ctx_regs(dump.exc["ctx"]) if dump.exc and t["tid"] == crash_tid else {}   # the exception's context, not the handler's
        regs = regs or ctx_regs(t["ctx"])
        frames[t["tid"]] = win_unwind(dump, regs, pes) if regs else []
    names = symbolize(dump, sorted({f for fs in frames.values() for f in fs}))
    out, used = ["## Stacks (x64 unwind with the tables of the modules there are)", ""], []
    for t in order:
        out += ["### Thread %d %s%s" % (t["tid"], t["name"], " (CRASHED)" if t["tid"] == crash_tid else ""), "```"]
        out += ["#%-2d %s" % (i, names.get(f, hex(f))) for i, f in enumerate(frames[t["tid"]])]
        out += ["```", ""]
        used += [names.get(f, "") for f in frames[t["tid"]]]
    return out, used


def scan_section(dump, crash_tid, main_tid):
    """Run the stack scan on every thread; return the Markdown lines and the frame names."""
    reader = make_code_reader(dump)
    scans = {t["tid"]: stack_scan(dump, t, reader) for t in dump.threads}
    names = symbolize(dump, sorted({v for s in scans.values() for _a, v in s}))
    out, used = ["## Stack scan (return addresses found on each stack, top first; a fallback where the unwind stops)", ""], []
    for t in sorted(dump.threads, key=lambda t: (t["tid"] not in (crash_tid, main_tid), t["tid"])):
        s = scans[t["tid"]]
        if s:
            tag = " (CRASHED)" if t["tid"] == crash_tid else " (main thread)" if t["tid"] == main_tid else ""
            out += ["### Thread %d %s%s" % (t["tid"], t["name"], tag), "```"]
            out += ["%016x: %s" % (addr, names.get(v, hex(v))) for addr, v in s[:16]] + ["```", ""]
            used += [names.get(v, "") for _a, v in s]
    return out, used


def analyse_dump(dump, args, work, sym_dir):
    """Return the Markdown lines of the dump part, the crashing thread's registers and the frame names."""
    match_modules(dump, find_symbol_files(sym_dir), work, args.fetch_system_libs)
    crash_tid = dump.exc["tid"] if dump.exc else None
    main_tid = main_thread_id(dump)
    out = dump_summary(dump, crash_tid, main_tid)
    crash_thread = next((t for t in dump.threads if t["tid"] == crash_tid), None)
    crash_regs = ctx_regs(crash_thread["ctx"]) if crash_thread else {}
    names = []
    if dump.is_linux and dump.arch == AMD64:
        part, names = linux_section(dump, work, args.src, crash_tid or main_tid)
    elif dump.is_windows and dump.arch == AMD64:
        part, names = windows_section(dump, crash_tid)
    else:
        part = ["(No unwinder for this kind of dump: only the stack scan below.)", ""]
    scan, scan_names = scan_section(dump, crash_tid, main_tid)
    return out + part + scan, crash_regs, names + scan_names


def parse_args():
    """Return the command line arguments."""
    ap = argparse.ArgumentParser(description=__doc__ + "\n\n" + USAGE, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("zips", nargs="+", help="the symbols zip(s) and the debug report zip, in any order")
    ap.add_argument("--src", help="an OnAir checkout of the same version (the file:line of gdb point there)")
    ap.add_argument("--out", default="analysis.md")
    ap.add_argument("--work", help="folder for the unpacked files (default: a new temporary folder, kept)")
    ap.add_argument("--fetch-system-libs", action="store_true",
                    help="download system libraries by build id (debuginfod, Microsoft symbol server)")
    return ap.parse_args()


def main():
    """Unpack, analyse, write the Markdown file."""
    args = parse_args()
    work = args.work or tempfile.mkdtemp(prefix="onair-dump-")
    os.makedirs(work, exist_ok=True)
    report_dir, sym_dir = unpack(args.zips, work)
    texts, meta = read_texts(report_dir)
    out = ["# OnAir debug report analysis", "",
           "Report: %s, version %s, %s/%s, trigger: %s" % (", ".join(args.zips), meta.get("version", "?"), meta.get("os", "?"),
                                                          meta.get("arch", "?"), meta.get("trigger", "?")), "",
           "## The user's report", "", "```", texts["report"].strip(), "```", ""]
    if os.path.exists(os.path.join(report_dir, "screenshot.png")):
        out += ["Screenshot: `%s`" % os.path.join(report_dir, "screenshot.png"), ""]
    dump = load_dump(report_dir, meta, out)
    crash_regs, names = {}, []
    if dump:
        part, crash_regs, names = analyse_dump(dump, args, work, sym_dir)
        out += part
    out += ["## Hints", ""] + ["- " + h for h in hints(dump, meta, texts, crash_regs, names)] + [""]
    if texts["crash"].strip():
        out += ["## crash.txt", "", "```", texts["crash"].strip()[:6000], "```", ""]
    out += ["## system.txt", "", "```", texts["system"].strip(), "```", ""]
    out += ["## Log (last 120 lines of log.txt)", "", "```"] + texts["log"].strip().splitlines()[-120:] + ["```", ""]
    out += ["Work folder (unpacked report, symbols, core): `%s`" % work, ""]
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("written: %s (work folder %s)" % (args.out, work))


if __name__ == "__main__":
    main()
