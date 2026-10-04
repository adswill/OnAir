#!/usr/bin/env python3
"""Reject app bundles whose binaries need a newer macOS than Info.plist promises."""
import plistlib
import re
import subprocess
import sys
from pathlib import Path


def version(value):
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", value):
        raise ValueError("invalid macOS version: " + value)
    parts = tuple(map(int, value.split(".")))
    return parts + (0,) * (3 - len(parts))


def minimum_versions(output):
    # otool prints one set of load commands per architecture in a universal binary.
    values = []
    for block in re.split(r"Load command \d+", output):
        if re.search(r"\bcmd LC_BUILD_VERSION\b", block):
            field = "minos"
            if not re.search(r"^\s*platform (?:1|MACOS)\s*$", block, re.M):
                raise ValueError("binary has a non-macOS platform")
        elif re.search(r"\bcmd LC_VERSION_MIN_MACOSX\b", block):
            field = "version"
        else:
            continue
        match = re.search(r"^\s*" + field + r"\s+(\S+)", block, re.M)
        if not match:
            raise ValueError("missing minimum macOS version in load command")
        values.append(match.group(1))
    if not values:
        raise ValueError("no minimum macOS version found")
    return values


def check_bundle(app):
    contents = Path(app) / "Contents"
    with (contents / "Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    target = info["LSMinimumSystemVersion"]
    target_version = version(target)
    binaries = [contents / "MacOS" / info["CFBundleExecutable"]]
    binaries += sorted((contents / "Frameworks").rglob("*.dylib"))
    errors = []
    for binary in binaries:
        output = subprocess.run(["otool", "-l", str(binary)], check=True,
                                capture_output=True, text=True).stdout
        for minimum in minimum_versions(output):
            if version(minimum) > target_version:
                errors.append(f"{binary.relative_to(contents)} requires macOS {minimum}, "
                              f"but Info.plist declares {target}")
    if errors:
        raise ValueError("\n".join(errors) + "\nRebuild with dependencies that support macOS "
                         + target + "; lowering the app deployment target alone is not sufficient.")
    return f"Verified {len(binaries)} binaries support the declared macOS {target} target"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: check_macos_target.py OnAir.app")
    try:
        print(check_bundle(sys.argv[1]))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
