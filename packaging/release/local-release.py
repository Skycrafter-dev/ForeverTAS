#!/usr/bin/env python3
"""Build, verify, and stage a ForeverTAS release on local machines."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
DEFAULT_MANIFEST = SCRIPT_DIR / "manifest.json"
VALID_40_HEX = re.compile(r"^[0-9a-f]{40}$")
ARCHITECTURE_LIST = [61, 62, 70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 120]
ARCHITECTURE_PTX = 120
ARCHITECTURE_CMAKE = "61-real;62-real;70-real;72-real;75-real;80-real;86-real;87-real;89-real;90-real;100-real;101-real;120-real;120-virtual"
ARCHITECTURE_KEY = "sm61-sm62-sm70-sm72-sm75-sm80-sm86-sm87-sm89-sm90-sm100-sm101-sm120-ptx120"


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    capture: bool = False,
    env: dict[str, str] | None = None,
) -> str:
    print("+ " + shlex.join(command), flush=True)
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        check=True,
        env=env,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture else ""


def git(*args: str, cwd: Path = REPO_ROOT) -> str:
    return run(["git", "-C", str(cwd), *args], capture=True)


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema") != 1:
        raise SystemExit("unsupported release manifest schema")
    cuda = manifest["cuda"]
    if cuda["version"] != "12.8.1":
        raise SystemExit("manifest changed the pinned CUDA release")
    if cuda["architectures"] != ARCHITECTURE_LIST:
        raise SystemExit("manifest CUDA architecture list does not match the required contract")
    if cuda["ptx_architecture"] != ARCHITECTURE_PTX:
        raise SystemExit("manifest CUDA PTX architecture does not match the required contract")
    if cuda["cmake_architectures"] != ARCHITECTURE_CMAKE:
        raise SystemExit("manifest CUDA CMake architecture tuple is stale")
    if cuda["architecture_key"] != ARCHITECTURE_KEY:
        raise SystemExit("manifest CUDA architecture key does not match the required contract")
    if cuda["split_compile_jobs"] != 4:
        raise SystemExit("manifest changed the validated CUDA split-compile value")
    search_object_source_commit = cuda.get("search_object_source_commit")
    validator_commit = manifest["sources"]["forevervalidator"].get("commit")
    if not VALID_40_HEX.fullmatch(validator_commit or ""):
        raise SystemExit("manifest does not contain a valid ForeverValidator commit SHA")
    if not VALID_40_HEX.fullmatch(search_object_source_commit or ""):
        raise SystemExit("manifest has no CUDA search-object source identity")
    if search_object_source_commit != validator_commit:
        raise SystemExit(
            "manifest CUDA search-object source identity must equal the "
            "ForeverValidator pin")
    if manifest["release"]["tag"] != f"v{manifest['release']['version']}":
        raise SystemExit("release tag and version do not match")
    return manifest


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_state(manifest: dict, validator_root: Path) -> dict:
    tas_cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    tas_version = next(
        line.split("VERSION", 1)[1].split()[0]
        for line in tas_cmake.splitlines()
        if line.strip().startswith("VERSION")
    )
    validator_cmake = (validator_root / "CMakeLists.txt").read_text(encoding="utf-8")
    expected_validator_version = manifest["sources"]["forevervalidator"]["version"]
    if f"project(ForeverValidator VERSION {expected_validator_version}" not in validator_cmake:
        raise SystemExit("ForeverValidator CMake version does not match the manifest")
    validator_vcpkg = json.loads((validator_root / "vcpkg.json").read_text(encoding="utf-8"))
    if validator_vcpkg["version-string"] != expected_validator_version:
        raise SystemExit("ForeverValidator vcpkg and CMake versions do not match")
    state = {
        "forevertas": git("rev-parse", "HEAD"),
        "forevervalidator": git("rev-parse", "HEAD", cwd=validator_root),
        "version": tas_version,
    }
    if state["version"] != manifest["release"]["version"]:
        raise SystemExit("ForeverTAS CMake version does not match the manifest")
    validator_commit = manifest["sources"]["forevervalidator"]["commit"]
    if f"GIT_TAG {validator_commit}" not in tas_cmake:
        raise SystemExit(
            "ForeverTAS CMake ForeverValidator pin does not match the manifest")
    if state["forevervalidator"] != validator_commit:
        raise SystemExit("ForeverValidator checkout does not match the manifest commit")
    search_source = manifest["cuda"]["search_object_source_commit"]
    git("cat-file", "-e", f"{search_source}^{{commit}}", cwd=validator_root)
    optional_tag = manifest["sources"]["forevervalidator"].get("tag")
    if optional_tag is not None:
        validator_tag_target = git("rev-parse", f"{optional_tag}^{{}}", cwd=validator_root)
        if validator_tag_target != state["forevervalidator"]:
            raise SystemExit("ForeverValidator optional tag is stale")
    return state


def require_clean(root: Path) -> None:
    status = git("status", "--porcelain=v1", "--untracked-files=all", cwd=root)
    if status:
        raise SystemExit(f"release source is not clean: {root}\n{status}")


def write_lock(manifest_path: Path, manifest: dict, state: dict, output: Path) -> None:
    lock = {
        "schema": 1,
        "manifest_sha256": sha256(manifest_path),
        "manifest": manifest,
        "resolved_sources": state,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def export_source(root: Path, commit: str, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(suffix=".tar") as archive:
        run(["git", "-C", str(root), "archive", "--format=tar", "-o", archive.name, commit])
        with tarfile.open(archive.name) as stream:
            stream.extractall(destination, filter="data")


def stage_manifest(manifest_path: Path, destination: Path) -> None:
    target = destination / "packaging" / "release" / "manifest.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    selected = manifest_path.read_bytes()
    target.write_bytes(selected)
    if target.read_bytes() != selected:
        raise SystemExit("failed to copy selected manifest into release source tree")


def prepare_tree(manifest_path: Path, validator_root: Path, state: dict, destination: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    export_source(REPO_ROOT, state["forevertas"], destination)
    export_source(
        validator_root,
        state["forevervalidator"],
        destination / ".dependencies" / "ForeverValidator",
    )
    (destination / ".release-source-commit").write_text(
        state["forevertas"] + "\n", encoding="ascii"
    )
    (destination / ".dependencies" / "ForeverValidator" / ".release-source-commit").write_text(
        state["forevervalidator"] + "\n", encoding="ascii"
    )
    stage_manifest(manifest_path, destination)


def release_assets(manifest: dict, dist: Path) -> list[Path]:
    artifacts = [dist / manifest["artifacts"][name] for name in ("linux", "windows")]
    return [
        artifacts[0],
        Path(str(artifacts[0]) + ".sha256"),
        artifacts[1],
        Path(str(artifacts[1]) + ".sha256"),
    ]


def release_notes(manifest: dict) -> str:
    return f"""ForeverTAS {manifest['release']['version']} is built and verified entirely on local Linux and Windows machines with the pinned CUDA 12.8.1 toolchain.

### Highlights

- Standalone `Challenge.Gbx` maps now load directly, while replays remain map and scenario sources rather than control or duration authorities.
- Race camera initialization respects rotated spawns. Free-camera arrow keys strafe rather than scrub the timeline. Scripted telemetry has a downward-opening, window-bounded, scrollable field picker, target placement from the current camera or car, and an optional draw-through-blocks target mode.
- Giving up a manual takeover restarts the selected Inputs or Best run from its beginning. Copied takeover inputs reproduce the driven race at the tick boundary, and cars use stable render nodes that remain attached while runs, modes, and ticks change.
- Continuous cuboid moves and resizes update granular model roles and coalesce persistence instead of rebuilding 3D delegates or rewriting the complete settings file for every pointer event.
- Simulation-horizon scrubbing moves in one-second steps and resimulates only when editing ends. Inputs, Best, and Manual trajectories retain one-second physics snapshots so later input edits and horizon changes resume from the latest valid state.
- Browse actions use the Linux and Windows system file pickers, unsuffixed integer counters omit decimal zeroes while compact-unit values retain two digits, and packaged transport controls retain their intended silhouettes.
- Modifier seeds randomize automatically on each search start by default. Modifier windows that extend beyond the Simulation horizon are silently limited at execution time while the saved user configuration remains unchanged.
- Persisted BfV2-compatible condition scripts select eligible evaluation ticks on both CPU and CUDA. A satisfying mutation always outranks a baseline with no eligible tick; the chosen target remains the sole score comparator once conditions pass.
- Disjoint cuboid sweeps are rejected before exact slab math on CPU and CUDA, restoring volume-entry throughput to point-target parity on the validated RTX 5060.
- CUDA incumbents are reconstructed only when the device reports an actual best change. Every CUDA winner and improvement shown in the viewer is reconstructed by the Reference backend, which remains the authority for user-visible results.

### Input timelines

- Replays supply the map and required scenario context, never the simulation length or controls. A persisted user-configured Simulation horizon bounds search, preview, CPU, and CUDA execution.
- Input scripts remain valid beyond that horizon. Later commands are preserved for editing and reconstruction but are not executed unless the user increases the horizon.

### CUDA compatibility

The x86_64 packages contain native cubins for `sm_61`, `sm_62`, `sm_70`, `sm_72`, `sm_75`, `sm_80`, `sm_86`, `sm_87`, `sm_89`, `sm_90`, `sm_100`, `sm_101`, and `sm_120`. On desktop NVIDIA hardware, cubins are forward-compatible within the same compute-capability major version, so the native set covers compute capabilities 6.x, 7.x, 8.x, 9.0, 10.0, 10.1, 10.3, and 12.x. In current product terms this includes supported Pascal through Blackwell architectures that match the listed compute capabilities. See NVIDIA's [GPU compute-capability tables](https://developer.nvidia.com/cuda-gpus) and [binary-compatibility rules](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#cuda-binary-compatibility).

The fatbinary also contains `compute_120` PTX. A future NVIDIA architecture above compute capability 12.x may work by driver JIT compilation, with a slower first startup while the driver cache is populated, but it was not available for hardware validation and is not guaranteed by this release.

The CUDA backend definitely does not support compute capabilities outside the shipped native/PTX coverage; that includes 6.0 (Tesla P100 / Quadro GP100), 11.x, non-NVIDIA GPUs, non-x86_64 systems, and systems whose NVIDIA driver cannot load CUDA 12.8 applications. Those devices have neither a compatible cubin nor a backward-compatible PTX target. CPU search remains available on supported x86_64 Linux and Windows systems.
"""


def verify_artifacts(manifest: dict, manifest_path: Path, dist: Path) -> dict:
    lock_path = dist / "release-lock.json"
    if not lock_path.is_file():
        raise SystemExit("missing release source lock")
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    if lock.get("schema") != 1 or lock.get("manifest") != manifest:
        raise SystemExit("release source lock does not match the manifest")
    if lock.get("manifest_sha256") != sha256(manifest_path):
        raise SystemExit("release source lock has the wrong manifest hash")
    resolved = lock.get("resolved_sources", {})
    if resolved.get("version") != manifest["release"]["version"]:
        raise SystemExit("release source lock has the wrong ForeverTAS version")
    if resolved.get("forevervalidator") != manifest["sources"]["forevervalidator"]["commit"]:
        raise SystemExit("release source lock has the wrong ForeverValidator commit")
    tas_commit = resolved.get("forevertas", "")
    if len(tas_commit) != 40 or any(
        character not in "0123456789abcdef" for character in tas_commit
    ):
        raise SystemExit("release source lock has an invalid ForeverTAS commit")
    expected = [manifest["artifacts"]["linux"], manifest["artifacts"]["windows"]]
    for name in expected:
        artifact = dist / name
        checksum = Path(str(artifact) + ".sha256")
        if not artifact.is_file() or not checksum.is_file():
            raise SystemExit(f"missing release artifact or checksum: {name}")
        recorded = checksum.read_text(encoding="utf-8").split()[0].lower()
        actual = sha256(artifact)
        if recorded != actual:
            raise SystemExit(f"checksum mismatch: {name}")
        if artifact.suffix == ".zip":
            with zipfile.ZipFile(artifact) as bundle:
                names = bundle.namelist()
                if not any(name.endswith("/ForeverTAS.exe") for name in names):
                    raise SystemExit("Windows bundle does not contain ForeverTAS.exe")
                if any(name.startswith("/") or ".." in Path(name).parts for name in names):
                    raise SystemExit("unsafe path in Windows bundle")
        else:
            if artifact.stat().st_mode & 0o111 == 0:
                raise SystemExit("Linux AppImage is not executable")
    for platform in ("linux", "windows"):
        evidence = dist / f"cuda-fatbinary-{platform}.json"
        data = json.loads(evidence.read_text(encoding="utf-8"))
        if data["cubin_architectures"] != manifest["cuda"]["architectures"]:
            raise SystemExit(f"incomplete {platform} CUDA cubin set")
        if data["ptx_architecture"] != manifest["cuda"]["ptx_architecture"]:
            raise SystemExit(f"incorrect {platform} CUDA PTX fallback")
        if data.get("resolved_sources") != resolved:
            raise SystemExit(f"{platform} package was not built from the locked sources")
    print("PASS release checksums, archive structure, and CUDA fatbinary evidence")
    return resolved


def command_check(args: argparse.Namespace, manifest: dict) -> None:
    validator_root = Path(args.validator).resolve()
    require_clean(REPO_ROOT)
    require_clean(validator_root)
    state = source_state(manifest, validator_root)
    write_lock(args.manifest, manifest, state, Path(args.lock).resolve())
    print(json.dumps(state, indent=2))


def command_linux(args: argparse.Namespace, manifest: dict) -> None:
    validator_root = Path(args.validator).resolve()
    require_clean(REPO_ROOT)
    require_clean(validator_root)
    state = source_state(manifest, validator_root)
    write_lock(args.manifest, manifest, state, Path(args.lock).resolve())
    tree = Path(args.work).resolve() / "linux-source"
    prepare_tree(Path(args.manifest), validator_root, state, tree)
    command = [str(tree / "packaging/release/build-linux-local.sh"), str((tree / "packaging/release/manifest.json").resolve())]
    if args.last_resort_rebuild_cache:
        command.extend(["--last-resort-rebuild-cache",
                        "--confirm-cache-recovery-exhausted"])
    environment = os.environ.copy()
    environment["FOREVERTAS_RELEASE_CACHE"] = str(
        REPO_ROOT / manifest["cache"]["linux"]
    )
    run(command, cwd=tree, env=environment)
    dist = Path(args.dist).resolve()
    dist.mkdir(parents=True, exist_ok=True)
    for item in (tree / "dist").iterdir():
        shutil.copy2(item, dist / item.name)


def command_windows(args: argparse.Namespace, manifest: dict) -> None:
    validator_root = Path(args.validator).resolve()
    require_clean(REPO_ROOT)
    require_clean(validator_root)
    state = source_state(manifest, validator_root)
    write_lock(args.manifest, manifest, state, Path(args.lock).resolve())
    local_tree = Path(args.work).resolve() / "windows-source"
    prepare_tree(Path(args.manifest), validator_root, state, local_tree)
    host = manifest["toolchains"]["windows"]["host"]
    remote = "C:/src/forevertas-release"
    run(["ssh", host, f"Remove-Item -Recurse -Force '{remote}' -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force '{remote}' | Out-Null"])
    run(["scp", "-r", str(local_tree) + "/.", f"{host}:{remote}/"])
    cache_rebuild = (
        " -LastResortRebuildCache -ConfirmCacheRecoveryExhausted"
        if args.last_resort_rebuild_cache else ""
    )
    run(["ssh", host, f"& '{remote}/packaging/release/build-windows-local.ps1' -Manifest '{remote}/packaging/release/manifest.json'{cache_rebuild}"])
    dist = Path(args.dist).resolve()
    dist.mkdir(parents=True, exist_ok=True)
    artifact = manifest["artifacts"]["windows"]
    for name in (artifact, artifact + ".sha256", "cuda-fatbinary-windows.json"):
        run(["scp", f"{host}:{remote}/dist/{name}", str(dist) + "/"])


def command_draft(args: argparse.Namespace, manifest: dict) -> None:
    tag = manifest["release"]["tag"]
    repository = manifest["release"]["repository"]
    target = git("rev-list", "-n", "1", tag)
    if target != git("rev-parse", "HEAD"):
        raise SystemExit("release tag does not point at the built commit")
    dist = Path(args.dist).resolve()
    resolved = verify_artifacts(manifest, args.manifest, dist)
    if resolved["forevertas"] != target:
        raise SystemExit("release assets were not built from the tagged commit")
    assets = [str(path) for path in release_assets(manifest, dist)]
    notes = release_notes(manifest)
    existing = subprocess.run(
        ["gh", "release", "view", tag, "--repo", repository],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0
    if existing:
        run(["gh", "release", "edit", tag, "--repo", repository,
             "--draft=true", "--title", tag, "--notes", notes])
        for name in ("cuda-fatbinary-linux.json",
                     "cuda-fatbinary-windows.json",
                     "release-lock.json"):
            subprocess.run(
                ["gh", "release", "delete-asset", tag, name,
                 "--repo", repository, "--yes"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        run(["gh", "release", "upload", tag, "--repo", repository,
             "--clobber", *assets])
    else:
        run(["gh", "release", "create", tag, "--repo", repository,
             "--draft", "--verify-tag", "--title", tag, "--notes", notes,
             *assets])


def command_publish(args: argparse.Namespace, manifest: dict) -> None:
    if args.confirm != manifest["release"]["tag"]:
        raise SystemExit("publish requires --confirm with the exact release tag")
    tag = manifest["release"]["tag"]
    repository = manifest["release"]["repository"]
    with tempfile.TemporaryDirectory() as temporary:
        run(["gh", "release", "download", tag, "--repo", repository, "--dir", temporary])
        downloaded = Path(temporary)
        for name in manifest["artifacts"].values():
            artifact = downloaded / name
            checksum = downloaded / f"{name}.sha256"
            if not artifact.is_file() or not checksum.is_file():
                raise SystemExit(f"draft is missing release asset: {name}")
            if checksum.read_text(encoding="utf-8").split()[0].lower() != sha256(artifact):
                raise SystemExit(f"downloaded draft checksum mismatch: {name}")
        appimage = downloaded / manifest["artifacts"]["linux"]
        appimage.chmod(appimage.stat().st_mode | 0o111)
    run(["gh", "release", "edit", tag, "--repo", repository, "--draft=false"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--validator", default=str(REPO_ROOT.parent / "ForeverValidator"))
    parser.add_argument("--work", default=str(REPO_ROOT / ".release-work"))
    parser.add_argument("--dist", default=str(REPO_ROOT / "dist/release"))
    parser.add_argument("--lock", default=str(REPO_ROOT / "dist/release/release-lock.json"))
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("check")
    for name in ("linux", "windows"):
        build = subparsers.add_parser(name)
        build.add_argument(
            "--last-resort-rebuild-cache",
            action="store_true",
            help=(
                "delete the persistent platform cache only after warm-build "
                "retries and targeted cache repair have both failed"
            ),
        )
        build.add_argument(
            "--confirm-cache-recovery-exhausted",
            action="store_true",
            help=argparse.SUPPRESS,
        )
    subparsers.add_parser("verify")
    subparsers.add_parser("draft")
    publish = subparsers.add_parser("publish")
    publish.add_argument("--confirm", required=True)
    args = parser.parse_args()
    if getattr(args, "last_resort_rebuild_cache", False) != getattr(
        args, "confirm_cache_recovery_exhausted", False
    ):
        raise SystemExit(
            "last-resort cache rebuilding requires both "
            "--last-resort-rebuild-cache and "
            "--confirm-cache-recovery-exhausted after warm retries and "
            "targeted cache repair have failed"
        )
    args.manifest = args.manifest.resolve()
    manifest = load_manifest(args.manifest)
    if args.command == "check":
        command_check(args, manifest)
    elif args.command == "linux":
        command_linux(args, manifest)
    elif args.command == "windows":
        command_windows(args, manifest)
    elif args.command == "verify":
        verify_artifacts(manifest, args.manifest, Path(args.dist).resolve())
    elif args.command == "draft":
        command_draft(args, manifest)
    else:
        command_publish(args, manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
