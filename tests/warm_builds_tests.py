"""Content-aware warm build transfer across isolated worktrees."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "warm_builds", ROOT / "packaging/release/warm_builds.py")
warm_builds = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(warm_builds)


def git(root, *args):
    subprocess.run(("git", "-C", str(root), *args), check=True,
                   stdout=subprocess.DEVNULL)


class WarmBuildTests(unittest.TestCase):
    def test_matching_content_recovers_mtime_without_overwriting_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            donor, recipient = base / "donor", base / "recipient"
            for root in (donor, recipient):
                root.mkdir()
                git(root, "init", "-q")
                (root / "same.cpp").write_text("same", encoding="ascii")
                (root / "changed.cpp").write_text(
                    "old" if root == donor else "new", encoding="ascii")
                git(root, "add", ".")
                git(root, "-c", "user.name=Test", "-c", "user.email=test@example.com",
                    "commit", "-qm", "source")
            for name in ("same.cpp", "changed.cpp"):
                os.utime(donor / name, ns=(100, 100))
                os.utime(recipient / name, ns=(200, 200))
            self.assertEqual(warm_builds._retime_matching(donor, recipient, 300),
                             (1, 1))
            self.assertEqual((recipient / "same.cpp").stat().st_mtime_ns, 100)
            self.assertEqual((recipient / "changed.cpp").stat().st_mtime_ns, 300)
            self.assertEqual((recipient / "changed.cpp").read_text(), "new")

    def test_seed_preserves_warm_objects_and_checks_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            donor, recipient = base / "donor", base / "recipient"
            donor_validator = base / "donor-validator"
            recipient_validator = base / "recipient-validator"
            for root in (donor, recipient, donor_validator, recipient_validator):
                root.mkdir()
                git(root, "init", "-q")
                (root / ".gitignore").write_text("build/\n", encoding="ascii")
                (root / "kernel.cu").write_text("unchanged", encoding="ascii")
                if root in (donor, recipient):
                    for relative in warm_builds.TOOLCHAIN_FILES:
                        path = root / relative
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text("pinned toolchain", encoding="ascii")
                    manifest = root / "packaging/release/manifest.json"
                    manifest.write_text(json.dumps({
                        "toolchains": {"linux": {"base": "pinned"}},
                        "cuda": {"version": "pinned"}}), encoding="ascii")
                git(root, "add", ".")
                git(root, "-c", "user.name=Test", "-c", "user.email=test@example.com",
                    "commit", "-qm", "source")
            donor_build = donor / "build/distribution-nvidia-sm75"
            donor_build.mkdir(parents=True)
            (donor_build / "CMakeCache.txt").write_text(
                "CMAKE_HOME_DIRECTORY:INTERNAL=/workspace\n"
                "FETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR:PATH=/validator\n"
                "FOREVERTAS_DISTRIBUTION_FLAVOR:STRING=nvidia-sm75\n",
                encoding="ascii")
            (donor_build / "gpu.o").write_bytes(b"warm compute object")
            os.utime(donor / "kernel.cu", ns=(100, 100))
            os.utime(recipient / "kernel.cu", ns=(200, 200))
            evidence = warm_builds.seed_warm_builds(
                donor, donor_validator, recipient, recipient_validator,
                ["nvidia-sm75"])
            self.assertEqual(evidence["flavors"], ["nvidia-sm75"])
            self.assertEqual((recipient / "build/distribution-nvidia-sm75/gpu.o").read_bytes(),
                             b"warm compute object")
            self.assertEqual((recipient / "kernel.cu").stat().st_mtime_ns, 100)
            self.assertTrue((recipient / "build/release-warm-seed.json").is_file())


if __name__ == "__main__":
    unittest.main()
