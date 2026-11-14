# SPDX-License-Identifier: GPL-3.0-or-later
"""macos/apple_sdk.py - the macOS SDK from Apple's Command Line Tools package, on Linux (used by macos/fetch_sysroot.sh).

    python3 -I apple_sdk.py latest CATALOG.sucatalog PACKAGE          newest product with PACKAGE: "product date url dist-url"
    python3 -I apple_sdk.py title DIST                                 the product's title ("Command Line Tools for Xcode 26.6")
    python3 -I apple_sdk.py verify PKG ROOT_SHA256                     check the package's Apple signature; prints the signer
    python3 -I apple_sdk.py extract PKG OUTDIR [--exclude GLOB ...]    unpack the SDK in the package into OUTDIR

The Command Line Tools are published in Apple's software update catalog (swscan.apple.com) and downloaded from
swcdn.apple.com, without an Apple ID. A .pkg is a xar archive. "verify" checks it the way macOS's installer does: the
sha1 of the xar table of contents must equal the checksum in the heap; that checksum must carry a valid RSA signature
by the first certificate in the table of contents; the certificates must chain (openssl verify) to the root
certificate whose sha256 fingerprint is ROOT_SHA256 (Apple Root CA, pinned by the caller). "extract" also checks the
Payload's sha1 from the (signed) table of contents, then unpacks Payload: pbzx (xz chunks) holding a cpio archive.
Only the SDK folder is unpacked: Library/Developer/CommandLineTools/SDKs/MacOSX<version>.sdk, whose contents go
straight into OUTDIR. Paths matching an --exclude glob (relative to the SDK folder) are left out.
Needs: python3, openssl.
"""
import base64
import fnmatch
import hashlib
import lzma
import os
import plistlib
import re
import struct
import shutil
import subprocess  # nosec B404
import sys
import tempfile
# Python 3.14's expat fetches no external entities and limits entity expansion, which is what defusedxml adds; the
# parsed files are Apple's, checked against a pinned sha256 or Apple's pinned root certificate
import xml.etree.ElementTree as ET  # nosec B405  # nosemgrep
import zlib


def die(msg):
    sys.exit(f"error: {msg}")


def openssl(*args, check=False, text=True):
    # the host's openssl by its full path, the arguments are fixed options and file names in a temporary folder, no shell
    prog = shutil.which("openssl")
    if prog is None:
        die("openssl is not on PATH")
    return subprocess.run([prog, *args], capture_output=True, check=check, text=text)  # nosec B603  # nosemgrep


# --- the catalog ------------------------------------------------------------------------------------------------------


def latest(catalog, package):
    with open(catalog, "rb") as f:
        c = plistlib.load(f)
    best = None
    for pid, p in c.get("Products", {}).items():
        for pk in p.get("Packages", []):
            if pk.get("URL", "").endswith("/" + package):
                date = p.get("PostDate")
                if best is None or date > best[1]:
                    best = (pid, date, pk["URL"], p.get("Distributions", {}).get("English", ""))
    if best is None:
        die(f"no product in {catalog} has {package}")
    print(best[0], best[1].strftime("%Y-%m-%dT%H:%M:%SZ"), best[2], best[3])


def title(dist):
    t = open(dist, encoding="utf-8", errors="replace").read()
    m = re.search(r'"SU_TITLE"\s*=\s*"([^"]+)"', t)
    print(m.group(1) if m else "")


# --- xar ----------------------------------------------------------------------------------------------------------------


def read_xar(path):
    f = open(path, "rb")
    magic, hsize, _ver, toc_c, _toc_u, cksum = struct.unpack(">4sHHQQI", f.read(28))
    if magic != b"xar!":
        die(f"{path} is not a xar archive (.pkg)")
    f.seek(hsize)
    toc_z = f.read(toc_c)
    toc = ET.fromstring(zlib.decompress(toc_z))  # nosec B314  # nosemgrep
    return f, hsize + toc_c, toc_z, toc, cksum


def verify(path, root_sha256):
    f, heap, toc_z, toc, cksum = read_xar(path)
    if cksum != 1:
        die("the xar table of contents is not sha1-checked")
    t = toc.find("toc")
    ck = t.find("checksum")
    off, size = int(ck.find("offset").text), int(ck.find("size").text)
    f.seek(heap + off)
    stored = f.read(size)
    # SHA-1 is what the xar format uses for its table of contents; the signature checked below and the pinned sha256
    # of the package are the security
    if hashlib.sha1(toc_z, usedforsecurity=False).digest() != stored:  # nosemgrep
        die("the xar table of contents does not match its checksum")
    sig = t.find("signature")
    if sig is None or sig.get("style") != "RSA":
        die(f"{path} is not signed")
    soff, ssize = int(sig.find("offset").text), int(sig.find("size").text)
    f.seek(heap + soff)
    signature = f.read(ssize)
    ns = "{http://www.w3.org/2000/09/xmldsig#}"
    certs = [base64.b64decode(c.text) for c in sig.iter(ns + "X509Certificate")]
    if len(certs) < 2:
        die("the signature has no certificate chain")
    with tempfile.TemporaryDirectory() as d:
        pem = []
        for i, der in enumerate(certs):
            p = os.path.join(d, f"c{i}.der")
            open(p, "wb").write(der)
            out = openssl("x509", "-inform", "DER", "-in", p, check=True, text=False).stdout
            pem.append(out)
        root = [i for i, der in enumerate(certs) if hashlib.sha256(der).hexdigest() == root_sha256.lower()]
        if not root:
            die("the certificate chain does not end in the pinned Apple root certificate")
        open(os.path.join(d, "root.pem"), "wb").write(pem[root[0]])
        open(os.path.join(d, "leaf.pem"), "wb").write(pem[0])
        open(os.path.join(d, "mid.pem"), "wb").write(b"".join(pem[1:]))
        r = openssl("verify", "-purpose", "any", "-CAfile", os.path.join(d, "root.pem"),
                    "-untrusted", os.path.join(d, "mid.pem"), os.path.join(d, "leaf.pem"))
        if r.returncode != 0:
            die("the certificate chain does not verify: " + (r.stdout + r.stderr).strip())
        openssl("x509", "-in", os.path.join(d, "leaf.pem"), "-pubkey", "-noout", "-out", os.path.join(d, "pub.pem"), check=True)
        open(os.path.join(d, "digest"), "wb").write(stored)
        open(os.path.join(d, "sig"), "wb").write(signature)
        r = openssl("pkeyutl", "-verify", "-pubin", "-inkey", os.path.join(d, "pub.pem"),
                    "-in", os.path.join(d, "digest"), "-sigfile", os.path.join(d, "sig"), "-pkeyopt", "digest:sha1")
        if r.returncode != 0:
            die("bad signature on the xar table of contents")
        subj = openssl("x509", "-in", os.path.join(d, "leaf.pem"), "-noout", "-subject", "-nameopt", "oneline",
                       check=True).stdout.strip()
    print(subj.replace("subject=", "").strip())


# --- pbzx and cpio --------------------------------------------------------------------------------------------------------


class Pbzx:
    """a file-like reader of the pbzx stream in [start, start+length) of f"""

    def __init__(self, f, start, length):
        self.f, self.end, self.buf = f, start + length, b""
        f.seek(start)
        if f.read(4) != b"pbzx":
            die("Payload is not a pbzx stream")
        f.read(8)
        self.more = True

    def _chunk(self):
        if self.f.tell() >= self.end:
            self.more = False
            return
        _flags, size = struct.unpack(">QQ", self.f.read(16))
        data = self.f.read(size)
        self.buf += lzma.decompress(data) if data[:6] == b"\xfd7zXZ\x00" else data

    def read(self, n):
        while len(self.buf) < n and self.more:
            self._chunk()
        out, self.buf = self.buf[:n], self.buf[n:]
        if len(out) < n:
            die("Payload ends early")
        return out


def cpio_entries(s):
    while True:
        magic = s.read(6)
        if magic == b"070707":                       # odc
            h = s.read(70)
            mode = int(h[12:18], 8)
            ino = int(h[6:12], 8)
            dev = int(h[0:6], 8)
            namesize = int(h[53:59], 8)
            filesize = int(h[59:70], 8)
            name = s.read(namesize)[:-1].decode()
            pad = 0
        elif magic in (b"070701", b"070702"):        # newc
            h = s.read(104)
            fld = [int(h[i:i + 8], 16) for i in range(0, 104, 8)]
            ino, mode, filesize, namesize = fld[0], fld[1], fld[6], fld[11]
            dev = (fld[7] << 32) | fld[8]
            name = s.read(namesize)[:-1].decode()
            s.read((4 - (110 + namesize) % 4) % 4)
            pad = 4
        else:
            die(f"unknown cpio header {magic!r}")
        if name == "TRAILER!!!":
            return
        yield name, mode, dev, ino, filesize
        if pad:
            s.read((4 - filesize % 4) % 4)


def extract(path, out, excludes):
    f, heap, _toc_z, toc, _ = read_xar(path)
    payload = None
    for fe in toc.find("toc").iter("file"):
        if fe.findtext("name") == "Payload":
            payload = fe
    if payload is None:
        die(f"{path} has no Payload")
    data = payload.find("data")
    off, length = int(data.findtext("offset")), int(data.findtext("length"))
    want = data.find("extracted-checksum")
    if data.find("encoding").get("style") != "application/octet-stream":
        die("Payload is compressed inside the xar (not handled)")
    h = hashlib.new(want.get("style"))
    f.seek(heap + off)
    left = length
    while left:
        b = f.read(min(left, 1 << 20))
        h.update(b)
        left -= len(b)
    if h.hexdigest() != want.text.strip():
        die("Payload does not match its checksum in the signed table of contents")

    s = Pbzx(f, heap + off, length)
    sdk_re = re.compile(r"^(?:\./)?Library/Developer/CommandLineTools/SDKs/(MacOSX[0-9.]+\.sdk)(?:/(.*))?$")
    links, sdk, n, skipped = {}, None, 0, 0
    os.makedirs(out, exist_ok=True)
    for name, mode, dev, ino, size in cpio_entries(s):
        m = sdk_re.match(name)
        rel = m.group(2) if m else None
        keep = m is not None and rel and not any(fnmatch.fnmatchcase(rel, g) or fnmatch.fnmatchcase(rel + "/", g)
                                                  for g in excludes)
        if m and rel and sdk is None:            # (MacOSX26.sdk -> MacOSX26.5.sdk: the alias itself has no entries)
            sdk = m.group(1)
        if m and rel and sdk != m.group(1):
            die(f"the package holds two SDKs ({sdk}, {m.group(1)})")
        kind = mode & 0o170000
        if not keep:
            if m and rel:
                skipped += 1
            if size:
                s.read(size)
            continue
        dest = os.path.join(out, rel)
        if os.path.relpath(dest, out).startswith(".."):
            die(f"unsafe path in the payload: {name}")
        if kind == 0o040000:
            os.makedirs(dest, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.lexists(dest):
            os.remove(dest)
        if kind == 0o120000:
            target = s.read(size).decode()
            if os.path.isabs(target):
                die(f"absolute symlink in the SDK: {name} -> {target}")
            os.symlink(target, dest)
        elif kind == 0o100000:
            if size == 0 and (dev, ino) in links and ino:
                os.link(links[(dev, ino)], dest)
            else:
                left = size
                with open(dest, "wb") as o:
                    while left:
                        b = s.read(min(left, 1 << 20))
                        o.write(b)
                        left -= len(b)
                os.chmod(dest, 0o755 if mode & 0o111 else 0o644)
                links[(dev, ino)] = dest
        else:
            if size:
                s.read(size)
            continue
        n += 1
    if sdk is None:
        die(f"{path} holds no macOS SDK")
    print(sdk, n, skipped)


def main():
    a = sys.argv[1:]
    if a[:1] == ["latest"] and len(a) == 3:
        latest(a[1], a[2])
    elif a[:1] == ["title"] and len(a) == 2:
        title(a[1])
    elif a[:1] == ["verify"] and len(a) == 3:
        verify(a[1], a[2])
    elif a[:1] == ["extract"] and len(a) >= 3:
        ex, rest = [], a[3:]
        while rest:
            if rest[0] != "--exclude" or len(rest) < 2:
                sys.exit(__doc__)
            ex.append(rest[1])
            rest = rest[2:]
        extract(a[1], a[2], ex)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
