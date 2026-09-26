"""Checks the release matrix before long GPU builds begin."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "distribution", ROOT / "packaging/release/distribution.py")
distribution = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(distribution)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.manifest = distribution.read_manifest(distribution.MANIFEST)

    def test_each_sm_has_separate_asset(self):
        assets = distribution.expected_assets(self.manifest)
        self.assertEqual(len(assets), 2 * (2 + len(distribution.SUPPORTED_SM)))
        self.assertEqual(len(assets), len(set(assets)))
        for sm in distribution.SUPPORTED_SM:
            for platform in ("linux", "windows"):
                self.assertIn(distribution.asset_name(
                    self.manifest["release"]["version"],
                    platform, f"nvidia-sm{sm}"), assets)

    def test_checksums_and_stable_bootstrap_downloads(self):
        with tempfile.TemporaryDirectory() as directory:
            dist = Path(directory)
            for name in distribution.expected_assets(self.manifest):
                artifact = dist / name
                artifact.write_bytes(b"MZ00" if name.endswith(".exe")
                                     else b"\x7fELF")
                (dist / f"{name}.sha256").write_text(
                    f"{distribution.digest(artifact)}  {name}\n", encoding="ascii")
            distribution.prepare_aliases(self.manifest, dist)
            distribution.verify(self.manifest, dist)
            first = dist / distribution.expected_assets(self.manifest)[0]
            first.write_bytes(b"corrupted")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                distribution.verify(self.manifest, dist)


if __name__ == "__main__":
    unittest.main()
