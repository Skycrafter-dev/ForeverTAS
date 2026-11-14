#!/usr/bin/env python3
"""The release tools (packaging/release): relinking a module per GPU
generation (Unix and Windows link lines), reproducible module bundles, and
updates.json."""

from __future__ import annotations

import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

RELEASE = Path(__file__).resolve().parents[1] / "packaging" / "release"


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, RELEASE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


compute_modules = load("compute_modules")
release = load("release")

# a "linker" that writes what it was given into its output
FAKE_LINK = """
import sys
from pathlib import Path
args = sys.argv[1:]
lines = []
for arg in args:
    lines.append(arg)
    if arg.startswith("@"):
        lines += Path(arg[1:]).read_text().split()
out = next(a[5:] for a in args if a.lower().startswith("/out:")) if any(
    a.lower().startswith("/out:") for a in args) else args[args.index("-o") + 1]
Path(out).write_text("\\n".join(lines))
"""


class Relink(unittest.TestCase):
    def setUp(self):
        self.build = Path(tempfile.mkdtemp())
        (self.build / "link.py").write_text(FAKE_LINK)
        self.addCleanup(shutil.rmtree, self.build)

    def test_unix_link_line(self):
        command = f"{sys.executable} link.py -shared -o forevertas-compute-cuda.so lib/libfv.a lib/libhost.a"
        output = self.build / "sm86" / "forevertas-compute-cuda.so"
        output.parent.mkdir()
        compute_modules.relink(self.build, command, {"lib/libfv.a": "/pruned/sm86/libfv.a"}, output)
        linked = output.read_text().split()
        self.assertIn("/pruned/sm86/libfv.a", linked)
        self.assertNotIn("lib/libfv.a", linked)
        self.assertIn("lib/libhost.a", linked)

    def test_windows_link_line_with_response_file(self):
        (self.build / "CMakeFiles").mkdir()
        (self.build / "CMakeFiles" / "module.rsp").write_text("fv\\core.lib host.lib\n")
        command = (f"{sys.executable} link.py /nologo @CMakeFiles/module.rsp "
                   "/out:forevertas-compute-cuda.dll /implib:forevertas-compute-cuda.lib "
                   "/pdb:forevertas-compute-cuda.pdb /dll")
        output = self.build / "sm86" / "forevertas-compute-cuda.dll"
        output.parent.mkdir()
        compute_modules.relink(self.build, command, {"fv\\core.lib": "C:/pruned/sm86/core.lib"}, output)
        linked = output.read_text().split()
        self.assertIn("C:/pruned/sm86/core.lib", linked)
        self.assertNotIn("fv\\core.lib", linked)
        self.assertIn(f"/implib:{output.with_suffix('.lib')}", linked)
        self.assertIn(f"/pdb:{output.with_suffix('.pdb')}", linked)
        # the original response file is untouched (other generations use it)
        self.assertIn("fv\\core.lib", (self.build / "CMakeFiles" / "module.rsp").read_text())


@unittest.skipUnless(shutil.which("ar"), "no ar")
class ArchiveMembers(unittest.TestCase):
    def test_members_in_order_with_long_names(self):
        import subprocess
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            names = ["a.o", "a_rather_long_member_name.cu.o", "b.o"]
            for index, name in enumerate(names):
                (root / name).write_bytes(bytes([index]) * (index + 3))
            subprocess.run(["ar", "rcs", "lib.a", *names], cwd=root, check=True)
            members = compute_modules.archive_members(root / "lib.a")
            self.assertEqual([name for name, _ in members], names)
            self.assertEqual([body for _, body in members], [(root / n).read_bytes() for n in names])


@unittest.skipUnless(shutil.which("zstd") or importlib.util.find_spec("zstandard"), "no zstd")
class Bundle(unittest.TestCase):
    def test_same_content_same_name(self):
        with tempfile.TemporaryDirectory() as root:
            folder = Path(root) / "module"
            folder.mkdir()
            (folder / "forevertas-compute-cuda.so").write_bytes(b"\x7fELF" + bytes(4096))
            (folder / "module.json").write_text('{"id": "cuda-sm86"}\n')
            first, first_sha = compute_modules.bundle(folder, Path(root), "m")
            again = Path(root) / "again"
            again.mkdir()
            second, second_sha = compute_modules.bundle(folder, again, "m")
            self.assertEqual(first.name, second.name)
            self.assertEqual(first_sha, second_sha)
            (folder / "module.json").write_text('{"id": "cuda-sm89"}\n')
            third, _ = compute_modules.bundle(folder, again, "m")
            self.assertNotEqual(first.name, third.name)


class Catalog(unittest.TestCase):
    def test_apps_and_modules(self):
        with tempfile.TemporaryDirectory() as root:
            out = Path(root)
            linux = out / "linux"
            (linux / "compute" / "cuda").mkdir(parents=True)
            (linux / release.app_asset("linux")).write_bytes(b"app")
            module = linux / "compute" / "cuda" / "forevertas-compute-linux-cuda-sm86-0123.tar.zst"
            module.write_bytes(b"module")
            (linux / "compute" / "cuda" / "modules.json").write_text(json.dumps([{
                "id": "cuda-sm86", "backend": "cuda", "sm": 86, "ptx": False, "abi": "k",
                "file": module.name, "sha256": release.sha256(module), "size": 6}]))
            catalog = release.catalog(out)
            self.assertEqual(catalog["schema"], 1)
            self.assertEqual(catalog["assets"], [{"name": release.app_asset("linux"),
                                                  "sha256": release.sha256(linux / release.app_asset("linux"))}])
            entry = catalog["compute"]["linux"][0]
            self.assertEqual(entry["id"], "cuda-sm86")
            self.assertTrue(entry["url"].endswith(f"/releases/download/compute-modules/{module.name}"))
            self.assertNotIn("file", entry)
            self.assertEqual(catalog["compute"]["windows"], [])


if __name__ == "__main__":
    unittest.main()
