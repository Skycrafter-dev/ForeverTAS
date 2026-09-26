"""Rehydrate verified warm CMake builds in a new release worktree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


TOOLCHAIN_FILES = (
    "packaging/release/linux-toolchain.Dockerfile",
    "packaging/release/linux-hip-toolchain.Dockerfile",
)


def _git(root: Path, *args: str) -> str:
    return subprocess.run(("git", "-C", str(root), *args), check=True,
                          capture_output=True, text=True).stdout.strip()


def _digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def _tracked(root: Path) -> set[Path]:
    result = subprocess.run(("git", "-C", str(root), "ls-files", "-z"),
                            check=True, capture_output=True).stdout
    return {Path(os.fsdecode(name)) for name in result.split(b"\0") if name}


def _retime_matching(donor: Path, recipient: Path, changed_time: int) -> tuple[int, int]:
    matching = changed = 0
    donor_files = _tracked(donor)
    for relative in _tracked(recipient):
        destination = recipient / relative
        source = donor / relative
        if not destination.is_file():
            continue
        if relative in donor_files and source.is_file() and \
                _digest(source) == _digest(destination):
            timestamp = source.stat().st_mtime_ns
            matching += 1
        else:
            timestamp = changed_time
            changed += 1
        os.utime(destination, ns=(destination.stat().st_atime_ns, timestamp))
    return matching, changed


def seed_warm_builds(donor: Path, donor_validator: Path, target: Path,
                     target_validator: Path, flavors: list[str]) -> dict:
    donor, donor_validator, target, target_validator = (
        path.resolve() for path in (donor, donor_validator, target, target_validator))
    if donor == target or donor_validator == target_validator:
        raise ValueError("warm seed must come from separate read-only worktrees")
    for root in (donor, donor_validator, target, target_validator):
        if _git(root, "status", "--porcelain=v1"):
            raise ValueError(f"warm seed requires clean source: {root}")
    for relative in TOOLCHAIN_FILES:
        if _digest(donor / relative) != _digest(target / relative):
            raise ValueError(f"warm seed toolchain differs: {relative}")
    donor_manifest = json.loads((donor / "packaging/release/manifest.json").read_text())
    target_manifest = json.loads((target / "packaging/release/manifest.json").read_text())
    if donor_manifest["toolchains"]["linux"] != target_manifest["toolchains"]["linux"] or \
            donor_manifest["cuda"] != target_manifest["cuda"]:
        raise ValueError("warm seed toolchain manifest differs")
    if not flavors:
        raise ValueError("warm seed requires at least one flavor")
    for flavor in flavors:
        source = donor / "build" / f"distribution-{flavor}"
        destination = target / "build" / f"distribution-{flavor}"
        cache = source / "CMakeCache.txt"
        if destination.exists() or not cache.is_file():
            raise ValueError(f"warm build must be present only in donor: {flavor}")
        entries = cache.read_text(encoding="utf-8")
        for required in ("CMAKE_HOME_DIRECTORY:INTERNAL=/workspace",
                         "FETCHCONTENT_SOURCE_DIR_FOREVERVALIDATOR:PATH=/validator",
                         f"FOREVERTAS_DISTRIBUTION_FLAVOR:STRING={flavor}"):
            if required not in entries.splitlines():
                raise ValueError(f"incompatible warm build {flavor}: {required}")
    (target / "build").mkdir(exist_ok=True)
    for flavor in flavors:
        subprocess.run(("cp", "-a", "--reflink=auto",
                        str(donor / "build" / f"distribution-{flavor}"),
                        str(target / "build" / f"distribution-{flavor}")),
                       check=True)
    changed_time = time.time_ns()
    tas_matching, tas_changed = _retime_matching(donor, target, changed_time)
    validator_matching, validator_changed = _retime_matching(
        donor_validator, target_validator, changed_time)
    evidence = {
        "schema": 1,
        "donor": {"forevertas": _git(donor, "rev-parse", "HEAD"),
                  "forevervalidator": _git(donor_validator, "rev-parse", "HEAD")},
        "recipient": {"forevertas": _git(target, "rev-parse", "HEAD"),
                      "forevervalidator": _git(target_validator, "rev-parse", "HEAD")},
        "flavors": flavors,
        "matching_files": {"forevertas": tas_matching,
                           "forevervalidator": validator_matching},
        "changed_files": {"forevertas": tas_changed,
                          "forevervalidator": validator_changed},
    }
    (target / "build/release-warm-seed.json").write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return evidence


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--donor", type=Path, required=True)
    parser.add_argument("--donor-validator", type=Path, required=True)
    parser.add_argument("--target", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--target-validator", type=Path, required=True)
    parser.add_argument("--flavor", action="append", required=True)
    args = parser.parse_args()
    evidence = seed_warm_builds(args.donor, args.donor_validator,
                                args.target, args.target_validator, args.flavor)
    print(f"Seeded {len(evidence['flavors'])} warm builds; "
          f"{sum(evidence['matching_files'].values())} source files reused")


if __name__ == "__main__":
    main()
