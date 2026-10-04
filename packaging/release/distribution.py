#!/usr/bin/env python3
"""Build and publish the hardware-specific, self-updating release matrix."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
from queue import SimpleQueue
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "packaging/release/manifest.json"
SUPPORTED_SM = (50, 52, 53, 60, 61, 62, 70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 120)
SUPPORTED_GFX = ("gfx1100", "gfx1101", "gfx1102", "gfx1200", "gfx1201")


def read_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    version = manifest["release"]["version"]
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("release version must be major.minor.patch")
    if manifest["release"]["tag"] != f"v{version}":
        raise ValueError("release tag does not match the version")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    project_version = re.search(r"project\(ForeverTAS\s+VERSION\s+([0-9.]+)", cmake)
    if not project_version or project_version.group(1) != version:
        raise ValueError("CMake and release manifest versions differ")
    if tuple(manifest["distribution"]["nvidia_sm"]) != SUPPORTED_SM:
        raise ValueError("NVIDIA SM list differs from the client package catalog")
    if tuple(manifest["distribution"]["amd_gfx"]) != SUPPORTED_GFX:
        raise ValueError("AMD gfx list differs from the validated RX 7000/9000 targets")
    if manifest["distribution"]["bootstrap_flavor"] != "universal":
        raise ValueError("the first-install package must be universal")
    return manifest


def flavors(manifest: dict) -> list[str]:
    return (["universal"] +
            [f"nvidia-sm{sm}" for sm in manifest["distribution"]["nvidia_sm"]] +
            ["amd-rx7000-rx9000"])


def linux_build_schedule(selected: list[str]) -> tuple[list[str], list[str]]:
    template = "nvidia-sm75"
    prefix = (["universal"] if any(flavor != "universal" for flavor in selected)
              else [])
    if any(flavor.startswith("nvidia-sm") for flavor in selected):
        prefix.append(template)
    ordered = list(dict.fromkeys(prefix + selected))
    roots = [flavor for flavor in ordered if flavor in
             ("universal", template, "amd-rx7000-rx9000")]
    return roots, [flavor for flavor in ordered if flavor not in roots]


def cpu_partitions(cpus: list[int], workers: int) -> list[list[int]]:
    if workers < 1 or not cpus:
        raise ValueError("release workers and available CPUs must be positive")
    count = min(workers, len(cpus))
    return [cpus[index::count] for index in range(count)]


def asset_name(version: str, platform: str, flavor: str) -> str:
    suffix = "-Setup.exe" if platform == "windows" else ".AppImage"
    return f"ForeverTAS-{version}-{platform}-{flavor}-x86_64{suffix}"


def expected_assets(manifest: dict, platform: str | None = None) -> list[str]:
    platforms = (platform,) if platform else ("linux", "windows")
    return [asset_name(manifest["release"]["version"], system, flavor)
            for system in platforms for flavor in flavors(manifest)]


def bootstrap_aliases(manifest: dict) -> dict[str, str]:
    version = manifest["release"]["version"]
    return {
        "ForeverTAS-Linux.AppImage": asset_name(version, "linux", "universal"),
        "ForeverTAS-Windows-Setup.exe": asset_name(version, "windows", "universal"),
    }


def published_assets(manifest: dict) -> list[str]:
    packages = expected_assets(manifest)
    return packages + [f"{name}.sha256" for name in packages] + \
        list(bootstrap_aliases(manifest)) + ["updates.json", "build-provenance.json"]


def prepare_aliases(manifest: dict, dist: Path) -> None:
    records = {system: json.loads((dist / f"{system}-source.json").read_text(
        encoding="utf-8")) for system in ("linux", "windows")}
    source_commit = records["linux"].get("forevertas", "")
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("invalid ForeverTAS source commit")
    for system, record in records.items():
        if record.get("schema") != 1 or \
                record.get("forevertas") != source_commit or \
                record.get("forevervalidator") != \
                manifest["sources"]["forevervalidator"]["commit"] or \
                set(record.get("packages", {})) != set(flavors(manifest)):
            raise ValueError(f"incomplete {system} source provenance")
        for flavor, evidence in record["packages"].items():
            artifact = dist / asset_name(manifest["release"]["version"],
                                             system, flavor)
            if evidence.get("sha256") != digest(artifact) or \
                    not re.fullmatch(r"[0-9a-f]{64}",
                                     evidence.get("binary_sha256", "")) or \
                    not evidence.get("toolchain"):
                raise ValueError(f"invalid {system} evidence for {flavor}")
    for alias, source in bootstrap_aliases(manifest).items():
        shutil.copy2(dist / source, dist / alias)
    catalog = {
        "schema": 1,
        "version": manifest["release"]["version"],
        "release_url": f"https://github.com/{manifest['release']['repository']}/"
                       f"releases/tag/{manifest['release']['tag']}",
        "assets": [{"name": name, "sha256": digest(dist / name)}
                   for name in expected_assets(manifest)],
    }
    (dist / "updates.json").write_text(
        json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    provenance = {
        "schema": 1,
        "release": manifest["release"]["tag"],
        "manifest_sha256": manifest_digest(manifest),
        "sources": {"forevertas": source_commit,
                    "forevervalidator": manifest["sources"]["forevervalidator"]["commit"]},
        "platforms": records,
        "artifacts": {name: digest(dist / name)
                      for name in expected_assets(manifest)},
    }
    (dist / "build-provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def manifest_digest(manifest: dict) -> str:
    encoded = json.dumps(manifest, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def verify(manifest: dict, dist: Path, platform: str | None = None) -> None:
    for name in expected_assets(manifest, platform):
        artifact = dist / name
        sidecar = dist / f"{name}.sha256"
        if not artifact.is_file() or not sidecar.is_file():
            raise ValueError(f"missing {name} or its SHA-256 sidecar")
        parts = sidecar.read_text(encoding="ascii").split()
        if len(parts) != 2 or parts[1] != name or parts[0].lower() != digest(artifact):
            raise ValueError(f"incorrect SHA-256 for {name}")
        with artifact.open("rb") as stream:
            signature = stream.read(4)
        if name.endswith(".AppImage") and signature != b"\x7fELF":
            raise ValueError(f"not an ELF AppImage: {name}")
        if name.endswith(".exe") and signature[:2] != b"MZ":
            raise ValueError(f"not a Windows installer: {name}")
    if platform is None:
        provenance = json.loads((dist / "build-provenance.json").read_text(
            encoding="utf-8"))
        if provenance.get("schema") != 1 or \
                provenance.get("release") != manifest["release"]["tag"] or \
                provenance.get("manifest_sha256") != manifest_digest(manifest) or \
                provenance.get("sources", {}).get("forevervalidator") != \
                manifest["sources"]["forevervalidator"]["commit"] or \
                provenance.get("artifacts") != {
                    name: digest(dist / name) for name in expected_assets(manifest)}:
            raise ValueError("build provenance differs from artifacts")
        for system in ("linux", "windows"):
            record = provenance.get("platforms", {}).get(system, {})
            if record.get("forevertas") != \
                    provenance["sources"].get("forevertas") or \
                    record.get("forevervalidator") != \
                    provenance["sources"]["forevervalidator"] or \
                    set(record.get("packages", {})) != set(flavors(manifest)):
                raise ValueError(f"incomplete {system} build provenance")
            for flavor, evidence in record["packages"].items():
                name = asset_name(manifest["release"]["version"], system, flavor)
                if evidence.get("sha256") != provenance["artifacts"][name]:
                    raise ValueError(f"provenance differs from {name}")
        for alias, source in bootstrap_aliases(manifest).items():
            if not (dist / alias).is_file() or digest(dist / alias) != digest(dist / source):
                raise ValueError(f"bootstrap download is missing or stale: {alias}")
        catalog_path = dist / "updates.json"
        if not catalog_path.is_file():
            raise ValueError("updates.json is missing")
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        expected_release_url = (f"https://github.com/{manifest['release']['repository']}/"
                                f"releases/tag/{manifest['release']['tag']}")
        if catalog.get("schema") != 1 or \
                catalog.get("version") != manifest["release"]["version"] or \
                catalog.get("release_url") != expected_release_url or \
                [asset.get("name") for asset in catalog.get("assets", [])] != \
                expected_assets(manifest):
            raise ValueError("updates.json has an incomplete package matrix")
        for asset in catalog["assets"]:
            if asset.get("sha256") != digest(dist / asset["name"]):
                raise ValueError("updates.json has a stale package digest")


def run(*command: str, env: dict[str, str] | None = None) -> str:
    return subprocess.run(command, cwd=ROOT, env=env, check=True,
                          text=True, stdout=subprocess.PIPE).stdout.strip()


def build_linux(manifest: dict, selected: list[str]) -> None:
    validator = Path(os.environ.get("FOREVERTAS_VALIDATOR_SOURCE",
                                    ROOT.parent / "ForeverValidator")).resolve()
    if not (validator / "CMakeLists.txt").is_file():
        raise ValueError(f"ForeverValidator source is missing: {validator}")
    source_commit = run("git", "rev-parse", "HEAD")
    source_date_epoch = run("git", "show", "-s", "--format=%ct", "HEAD")
    validator_commit = run("git", "-C", str(validator), "rev-parse", "HEAD")
    if run("git", "status", "--porcelain=v1", "--untracked-files=all"):
        raise ValueError("commit the ForeverTAS source before building packages")
    if validator_commit != manifest["sources"]["forevervalidator"]["commit"]:
        raise ValueError("ForeverValidator does not match the release pin")
    if run("git", "-C", str(validator), "status", "--porcelain=v1"):
        raise ValueError("ForeverValidator has uncommitted changes")
    roots, remaining = linux_build_schedule(selected)
    required_flavors = roots + remaining
    missing = [flavor for flavor in required_flavors if not (
        ROOT / "build" / f"distribution-{flavor}" / "CMakeCache.txt").is_file()]
    donor = os.environ.get("FOREVERTAS_WARM_BUILD_SOURCE")
    if donor and missing:
        donor_validator = os.environ.get("FOREVERTAS_WARM_VALIDATOR_SOURCE")
        if not donor_validator:
            raise ValueError("FOREVERTAS_WARM_VALIDATOR_SOURCE is required")
        subprocess.run((sys.executable, str(ROOT / "packaging/release/warm_builds.py"),
                        "--donor", donor, "--donor-validator", donor_validator,
                        "--target", str(ROOT), "--target-validator", str(validator),
                        *(part for flavor in required_flavors
                          for part in ("--flavor", flavor))), check=True)
        missing = [flavor for flavor in required_flavors if not (
            ROOT / "build" / f"distribution-{flavor}" / "CMakeCache.txt").is_file()]
    if missing and os.environ.get("FOREVERTAS_ALLOW_COLD_BUILD") != "1":
        raise ValueError("refusing cold Linux builds: " + ", ".join(missing))
    base = os.environ.get("FOREVERTAS_LINUX_TOOLCHAIN_IMAGE") or run(
        str(ROOT / "packaging/release/ensure-linux-toolchain.sh"))
    hip = (os.environ.get("FOREVERTAS_HIP_TOOLCHAIN_IMAGE") or run(
        str(ROOT / "packaging/release/ensure-linux-hip-toolchain.sh"))) \
        if any(flavor != "universal" for flavor in selected) else ""
    cache = Path(os.environ.get(
        "FOREVERTAS_RELEASE_CACHE",
        ROOT.parent / ".forevertas-release-cache/linux")).resolve()
    cache.mkdir(parents=True, exist_ok=True)
    nvidia_template = "nvidia-sm75"

    def build_flavor(flavor: str, cpu_set: list[int] | None = None) -> tuple[str, dict]:
        nvidia = flavor.startswith("nvidia-sm")
        amd = flavor == "amd-rx7000-rx9000"
        cuda_arch = flavor.removeprefix("nvidia-sm")
        environment = {
            "FOREVERTAS_BUILD_DIR": f"/workspace/build/distribution-{flavor}",
            "FOREVERTAS_APPDIR": f"/workspace/build/distribution-{flavor}/AppDir",
            "FOREVERTAS_DIST_DIR": "/workspace/dist",
            "FOREVERTAS_TOOLS_DIR": "/cache/appimage-tools",
            "FOREVERTAS_VALIDATOR_SOURCE": "/validator",
            "FOREVERTAS_DISTRIBUTION_FLAVOR": flavor,
            "FOREVERTAS_ENABLE_VULKAN": "ON",
            "FOREVERTAS_ENABLE_CUDA": "ON" if nvidia else "OFF",
            "FOREVERTAS_ENABLE_HIP": "ON" if nvidia or amd else "OFF",
            "FOREVERTAS_HIP_PLATFORM": "nvidia" if nvidia else "amd" if amd else "",
            "FOREVERTAS_HIP_ARCHITECTURES": cuda_arch if nvidia else ";".join(SUPPORTED_GFX) if amd else "",
            "FOREVERTAS_HIP_FLAGS": "-fPIC" if amd else "",
            "FOREVERTAS_CUDA_ARCHITECTURES": f"{cuda_arch}-real;{cuda_arch}-virtual" if nvidia else "",
            "FOREVERVALIDATOR_CUDA_SPLIT_COMPILE_JOBS": str(manifest["cuda"]["split_compile_jobs"]),
            "HIP_PLATFORM": "nvidia" if nvidia else "amd" if amd else "",
            "HIP_PATH": "/opt/rocm" if nvidia or amd else "",
            "ROCM_PATH": "/opt/rocm" if nvidia or amd else "",
            "APPIMAGE_EXTRACT_AND_RUN": "1",
            "FOREVERTAS_RELEASE_JOBS": str(min(
                int(os.environ.get("FOREVERTAS_RELEASE_JOBS", "4")),
                len(cpu_set) if cpu_set else len(os.sched_getaffinity(0)))),
            "SCCACHE_DIR": "/cache/sccache",
            "SCCACHE_CACHE_SIZE": "50G",
            "SOURCE_DATE_EPOCH": source_date_epoch,
        }
        if flavor != "universal":
            environment["FOREVERTAS_QML_MODULE_EXTERNAL"] = "ON"
            environment["FOREVERTAS_COMMON_QML_DIR"] = (
                "/workspace/build/distribution-universal/AppDir/usr/bin/"
                "qml/ForeverTAS")
        if nvidia and flavor != nvidia_template:
            environment["FOREVERTAS_PACKAGE_TEMPLATE_APPDIR"] = (
                f"/workspace/build/distribution-{nvidia_template}/AppDir")
        command = ["docker", "run", "--rm", "--init",
                   "--user", f"{os.getuid()}:{os.getgid()}",
                   "--tmpfs", f"/home/builder:rw,uid={os.getuid()},gid={os.getgid()},mode=0755",
                   "--env", "HOME=/home/builder",
                   "--volume", f"{ROOT}:/workspace",
                   "--volume", f"{validator}:/validator:ro",
                   "--volume", f"{cache}:/cache",
                   "--workdir", "/workspace"]
        if cpu_set:
            command.extend(("--cpuset-cpus", ",".join(map(str, cpu_set))))
        for key, value in environment.items():
            command.extend(("--env", f"{key}={value}"))
        command.extend((hip if nvidia or amd else base,
                        "bash", "packaging/linux/build-appimage.sh"))
        print(f"Building Linux {flavor} from ForeverTAS {source_commit} "
              f"and ForeverValidator {validator_commit}", flush=True)
        subprocess.run(command, check=True, cwd=ROOT)
        if run("git", "rev-parse", "HEAD") != source_commit or \
                run("git", "status", "--porcelain=v1", "--untracked-files=all") or \
                run("git", "-C", str(validator), "rev-parse", "HEAD") != validator_commit or \
                run("git", "-C", str(validator), "status", "--porcelain=v1"):
            raise ValueError(f"sources changed while building Linux {flavor}")
        if nvidia:
            binary = f"/workspace/build/distribution-{flavor}/bin/ForeverTAS"
            inspection = command[:command.index(hip)] + [hip, "cuobjdump", "--list-elf", binary]
            cubins = set(re.findall(r"sm_([0-9]+)\.cubin", run(*inspection)))
            if cubins != {cuda_arch}:
                raise ValueError(f"{flavor} contains unexpected CUDA cubins: {cubins}")
        if amd:
            inspection = command[:command.index(hip)] + [
                hip, "bash", "-lc",
                "cp /workspace/build/distribution-amd-rx7000-rx9000/bin/ForeverTAS "
                "/tmp/ForeverTAS && cd /tmp && "
                "/opt/rocm/lib/llvm/bin/llvm-objdump --offloading ForeverTAS"]
            targets = set(re.findall(
                r"hipv4-amdgcn-amd-amdhsa--(gfx[0-9]+)", run(*inspection)))
            if targets != set(SUPPORTED_GFX):
                raise ValueError(f"AMD package has unexpected HIP targets: {targets}")
        artifact = ROOT / "dist" / asset_name(manifest["release"]["version"],
                                              "linux", flavor)
        binary = ROOT / "build" / f"distribution-{flavor}" / "bin/ForeverTAS"
        return flavor, {
            "sha256": digest(artifact),
            "binary_sha256": digest(binary),
            "toolchain": run("docker", "image", "inspect", "--format",
                             "{{.Id}}", hip if nvidia or amd else base),
            "runtime_template": nvidia_template if nvidia else flavor,
        }

    # The NVIDIA runtime and Qt deployment are identical for each SM. Deploy
    # them once, then assemble warm per-SM executables over that AppDir.
    workers = int(os.environ.get("FOREVERTAS_RELEASE_VARIANT_JOBS", "4"))
    if workers < 1:
        raise ValueError("FOREVERTAS_RELEASE_VARIANT_JOBS must be positive")
    cpus = sorted(os.sched_getaffinity(0))
    evidence = {}
    if "universal" in roots:
        flavor, result = build_flavor("universal")
        evidence[flavor] = result
    def build_group(group: list[str]) -> None:
        if not group:
            return
        partitions = cpu_partitions(cpus, min(workers, len(group)))
        slots: SimpleQueue[list[int]] = SimpleQueue()
        for partition in partitions:
            slots.put(partition)

        def assigned(flavor: str) -> tuple[str, dict]:
            cpu_set = slots.get()
            try:
                return build_flavor(flavor, cpu_set)
            finally:
                slots.put(cpu_set)

        with ThreadPoolExecutor(max_workers=len(partitions)) as pool:
            for future in [pool.submit(assigned, flavor) for flavor in group]:
                flavor, result = future.result()
                evidence[flavor] = result

    build_group([flavor for flavor in roots if flavor != "universal"])
    build_group(remaining)
    record_path = ROOT / "dist/linux-source.json"
    record = {"schema": 1, "forevertas": source_commit,
              "forevervalidator": validator_commit, "packages": {}}
    if record_path.is_file():
        previous = json.loads(record_path.read_text(encoding="utf-8"))
        if all(previous.get(key) == record[key]
               for key in ("schema", "forevertas", "forevervalidator")):
            record = previous
    record["packages"].update(evidence)
    record_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    verify(manifest, ROOT / "dist", "linux") if set(selected) == set(flavors(manifest)) else None


def ensure_tag(manifest: dict) -> None:
    if run("git", "status", "--porcelain=v1", "--untracked-files=all"):
        raise ValueError("commit the release source before publishing")
    tag = manifest["release"]["tag"]
    if run("git", "rev-parse", "HEAD") != run("git", "rev-list", "-n", "1", tag):
        raise ValueError("release tag must point at this source commit")


def ensure_remote_tag(manifest: dict) -> None:
    tag = manifest["release"]["tag"]
    head = run("git", "rev-parse", "HEAD")
    refs = run("git", "ls-remote", "origin", f"refs/tags/{tag}",
               f"refs/tags/{tag}^{{}}")
    commits = [line.split()[0] for line in refs.splitlines()
               if line.endswith(f"refs/tags/{tag}^{{}}")]
    if not commits:
        commits = [line.split()[0] for line in refs.splitlines()
                   if line.endswith(f"refs/tags/{tag}")]
    if commits != [head]:
        raise ValueError("push the release tag to origin at this source commit")


def draft(manifest: dict, dist: Path) -> None:
    ensure_tag(manifest)
    ensure_remote_tag(manifest)
    prepare_aliases(manifest, dist)
    verify(manifest, dist)
    if json.loads((dist / "build-provenance.json").read_text(
            encoding="utf-8"))["sources"]["forevertas"] != run("git", "rev-parse", "HEAD"):
        raise ValueError("release source provenance differs from the tag")
    tag = manifest["release"]["tag"]
    repository = manifest["release"]["repository"]
    exists = subprocess.run(["gh", "release", "view", tag, "--repo", repository],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL).returncode == 0
    if exists:
        release = json.loads(run("gh", "api", f"repos/{repository}/releases/tags/{tag}"))
        if not release.get("draft"):
            raise ValueError("cannot replace assets in a published release")
    else:
        run("gh", "release", "create", tag, "--repo", repository,
            "--draft", "--title", tag,
            "--notes", "Download the universal installer/AppImage once. ForeverTAS detects your GPU, offers the matching compute package, and can switch packages from the Bruteforce panel.")
    files = [str(dist / name) for name in published_assets(manifest)]
    run("gh", "release", "upload", tag, "--repo", repository,
        "--clobber", *files)


def publish(manifest: dict, dist: Path) -> None:
    ensure_tag(manifest)
    ensure_remote_tag(manifest)
    verify(manifest, dist)
    if json.loads((dist / "build-provenance.json").read_text(
            encoding="utf-8"))["sources"]["forevertas"] != run("git", "rev-parse", "HEAD"):
        raise ValueError("release source provenance differs from the tag")
    tag = manifest["release"]["tag"]
    repository = manifest["release"]["repository"]
    release = json.loads(run("gh", "api", f"repos/{repository}/releases/tags/{tag}"))
    if not release.get("draft"):
        raise ValueError("release must be a draft before verification")
    hosted = json.loads(run("gh", "api", f"repos/{repository}/releases/"
                            f"{release['id']}/assets?per_page=100"))
    assets = {asset["name"]: asset for asset in hosted}
    for name in published_assets(manifest):
        if assets.get(name, {}).get("digest") != f"sha256:{digest(dist / name)}":
            raise ValueError(f"GitHub did not store the expected digest for {name}")
    run("gh", "release", "edit", tag, "--repo", repository, "--draft=false")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("plan", "build-linux", "prepare", "verify", "draft", "publish"))
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--dist", type=Path, default=ROOT / "dist")
    parser.add_argument("--flavor", action="append", default=[])
    args = parser.parse_args()
    manifest = read_manifest(args.manifest)
    if args.command == "plan":
        print("\n".join(expected_assets(manifest)))
    elif args.command == "build-linux":
        selected = args.flavor or flavors(manifest)
        if any(flavor not in flavors(manifest) for flavor in selected):
            raise ValueError("unknown distribution flavor")
        build_linux(manifest, selected)
    elif args.command == "prepare":
        prepare_aliases(manifest, args.dist)
    elif args.command == "verify":
        verify(manifest, args.dist)
    elif args.command == "draft":
        draft(manifest, args.dist)
    else:
        publish(manifest, args.dist)


if __name__ == "__main__":
    main()
