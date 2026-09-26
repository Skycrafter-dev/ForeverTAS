"""Checks the release matrix before long GPU builds begin."""

import importlib.util
import json
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
        self.assertEqual(distribution.components_for(
            distribution.flavors(self.manifest)),
            ["universal", "nvidia-matrix", "amd-rx7000-rx9000"])

    def test_checksums_and_stable_bootstrap_downloads(self):
        with tempfile.TemporaryDirectory() as directory:
            dist = Path(directory)
            for name in distribution.expected_assets(self.manifest):
                artifact = dist / name
                artifact.write_bytes(b"MZ00" + name.encode("ascii")
                                     if name.endswith(".exe") else b"\x7fELF")
                (dist / f"{name}.sha256").write_text(
                    f"{distribution.digest(artifact)}  {name}\n", encoding="ascii")
            for platform in ("linux", "windows"):
                packages = {}
                components = {}
                for flavor in distribution.flavors(self.manifest):
                    name = distribution.asset_name(
                        self.manifest["release"]["version"], platform, flavor)
                    packages[flavor] = distribution.digest(dist / name)
                    components[distribution.component_for(flavor)] = {
                        "sha256": packages[flavor], "toolchain": "test"}
                (dist / f"{platform}-source.json").write_text(json.dumps({
                    "schema": 1,
                    "forevertas": "a" * 40,
                    "forevervalidator": self.manifest["sources"]["forevervalidator"]["commit"],
                    "components": components,
                    "packages": packages,
                }), encoding="utf-8")
            distribution.prepare_aliases(self.manifest, dist)
            distribution.verify(self.manifest, dist)
            provenance = json.loads((dist / "build-provenance.json").read_text())
            self.assertEqual(set(provenance["platforms"]["linux"]["components"]),
                             {"universal", "nvidia-matrix", "amd-rx7000-rx9000"})
            provenance["platforms"]["windows"]["packages"]["nvidia-sm75"] = "0" * 64
            (dist / "build-provenance.json").write_text(json.dumps(provenance))
            with self.assertRaisesRegex(ValueError, "package provenance"):
                distribution.verify(self.manifest, dist)
            distribution.prepare_aliases(self.manifest, dist)
            first = dist / distribution.expected_assets(self.manifest)[0]
            first.write_bytes(b"corrupted")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                distribution.verify(self.manifest, dist)


if __name__ == "__main__":
    unittest.main()
