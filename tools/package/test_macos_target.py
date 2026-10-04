"""Packaging regressions, including real Mach-O executable and library load commands."""
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from check_macos_target import check_bundle, minimum_versions, version


class LoadCommandTests(unittest.TestCase):
    def test_legacy_and_universal_versions(self):
        output = """Load command 8
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
  version 10.15
      sdk 15.0
Load command 9
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 15.0
      sdk 26.2
"""
        self.assertEqual(minimum_versions(output), ["10.15", "15.0"])

    def test_missing_version_is_rejected(self):
        with self.assertRaises(ValueError):
            minimum_versions("not a Mach-O file")

    def test_other_platform_is_rejected(self):
        with self.assertRaises(ValueError):
            minimum_versions("Load command 1\n cmd LC_BUILD_VERSION\n platform 2\n minos 15.0\n")

    def test_numeric_version_comparison(self):
        self.assertEqual(version("15"), version("15.0.0"))
        self.assertGreater(version("15.10"), version("15.9"))
        with self.assertRaises(ValueError):
            version("15.0garbage")


@unittest.skipUnless(sys.platform == "darwin", "requires macOS build tools")
class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / "OnAir.app"
        self.contents = self.app / "Contents"
        (self.contents / "MacOS").mkdir(parents=True)
        (self.contents / "Frameworks").mkdir()
        with (self.contents / "Info.plist").open("wb") as stream:
            plistlib.dump({"CFBundleExecutable": "OnAir", "LSMinimumSystemVersion": "15.0"}, stream)
        self.compile(self.contents / "MacOS/OnAir", "15.0")

    def compile(self, path, target, library=False):
        command = ["xcrun", "clang", "-x", "c", "-", "-o", str(path),
                   "-mmacosx-version-min=" + target]
        if library:
            command.append("-dynamiclib")
        subprocess.run(command, input="int main(void) { return 0; }\n", text=True,
                       check=True, capture_output=True)

    def test_compatible_bundle(self):
        self.compile(self.contents / "Frameworks/libgood.dylib", "15.0", library=True)
        self.assertIn("Verified 2 binaries", check_bundle(self.app))

    def test_newer_executable_is_rejected(self):
        self.compile(self.contents / "MacOS/OnAir", "15.1")
        with self.assertRaisesRegex(ValueError, "MacOS/OnAir requires macOS 15.1"):
            check_bundle(self.app)

    def test_newer_transitive_library_is_rejected(self):
        self.compile(self.contents / "Frameworks/libdependency.dylib", "15.1", library=True)
        with self.assertRaisesRegex(ValueError, "libdependency.dylib requires macOS 15.1"):
            check_bundle(self.app)

    def test_missing_executable_is_rejected(self):
        (self.contents / "MacOS/OnAir").unlink()
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            check_bundle(self.app)

    def test_failed_build_stops_packaging(self):
        # A stale executable must not get packaged when the current build fails.
        repo = self.root / "repo"
        package = repo / "tools/package"
        package.mkdir(parents=True)
        script = package / "make_dmg.sh"
        shutil.copy(Path(__file__).with_name("make_dmg.sh"), script)
        stale_app = repo / "build-release/OnAir.app"
        stale_app.mkdir(parents=True)
        marker = stale_app / "previous-build"
        marker.write_text("preserve until a successful build")
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        cmake = fake_bin / "cmake"
        cmake.write_text('#!/bin/sh\nif [ "$1" = "--build" ]; then exit 42; fi\n')
        cmake.chmod(0o755)
        result = subprocess.run(["bash", str(script), "0.0.0"], capture_output=True, text=True,
                                env={**os.environ, "PATH": str(fake_bin) + ":" + os.environ["PATH"]})
        self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
        self.assertEqual(marker.read_text(), "preserve until a successful build")


if __name__ == "__main__":
    unittest.main()
