#!/usr/bin/env python3
"""Build and publish the hardware-specific, self-updating release matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "packaging/release/manifest.json"
SUPPORTED_SM = (50, 52, 53, 60, 61, 62, 70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 120)
SUPPORTED_GFX = ("gfx1100", "gfx1101", "gfx1102", "gfx1200", "gfx1201")
NVIDIA_COMPONENT = "nvidia-matrix"


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
    validator_commit = manifest["sources"]["forevervalidator"]["commit"]
    if not re.fullmatch(r"[0-9a-f]{40}", validator_commit) or \
            f"GIT_TAG {validator_commit}" not in cmake:
        raise ValueError("ForeverValidator source pin differs from CMake")
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


def component_for(flavor: str) -> str:
    return NVIDIA_COMPONENT if flavor.startswith("nvidia-sm") else flavor


def components_for(selected: list[str]) -> list[str]:
    return list(dict.fromkeys(component_for(flavor) for flavor in selected))


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
    records = {platform: json.loads((dist / f"{platform}-source.json").read_text(
        encoding="utf-8")) for platform in ("linux", "windows")}
    expected_components = {"universal", NVIDIA_COMPONENT, "amd-rx7000-rx9000"}
    source_commit = records["linux"].get("forevertas")
    if not isinstance(source_commit, str) or \
            not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("ForeverTAS build source commit is invalid")
    for platform, record in records.items():
        if record.get("schema") != 1 or \
                record.get("forevertas") != source_commit or \
                record.get("forevervalidator") != \
                manifest["sources"]["forevervalidator"]["commit"] or \
                set(record.get("components", {})) != expected_components:
            raise ValueError(f"{platform} build source/component record is incomplete")
        for component in record["components"].values():
            if not re.fullmatch(r"[0-9a-f]{64}", component.get("sha256", "")) or \
                    not component.get("toolchain"):
                raise ValueError(f"{platform} has invalid component evidence")
        for flavor in flavors(manifest):
            name = asset_name(manifest["release"]["version"], platform, flavor)
            if record.get("packages", {}).get(flavor) != digest(dist / name):
                raise ValueError(f"{platform} package hash does not match {name}")
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
    if platform in (None, "linux"):
        nvidia_hashes = {digest(dist / asset_name(
            manifest["release"]["version"], "linux", f"nvidia-sm{sm}"))
            for sm in SUPPORTED_SM}
        if len(nvidia_hashes) != 1:
            raise ValueError("NVIDIA Linux assets do not share one component")
    if platform is None:
        provenance_path = dist / "build-provenance.json"
        if not provenance_path.is_file():
            raise ValueError("build provenance is missing")
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        if provenance.get("schema") != 1 or \
                provenance.get("release") != manifest["release"]["tag"] or \
                provenance.get("manifest_sha256") != manifest_digest(manifest) or \
                not re.fullmatch(r"[0-9a-f]{40}",
                                 provenance.get("sources", {}).get("forevertas", "")) or \
                provenance.get("sources", {}).get("forevervalidator") != \
                manifest["sources"]["forevervalidator"]["commit"] or \
                provenance.get("artifacts") != {
                    name: digest(dist / name) for name in expected_assets(manifest)}:
            raise ValueError("build provenance does not match release artifacts")
        for system in ("linux", "windows"):
            record = provenance.get("platforms", {}).get(system, {})
            if record.get("schema") != 1 or \
                    record.get("forevertas") != provenance["sources"]["forevertas"] or \
                    record.get("forevervalidator") != provenance["sources"]["forevervalidator"] or \
                    set(record.get("components", {})) != {"universal", NVIDIA_COMPONENT,
                                                           "amd-rx7000-rx9000"}:
                raise ValueError(f"{system} build provenance is incomplete")
            for flavor in flavors(manifest):
                name = asset_name(manifest["release"]["version"], system, flavor)
                if record.get("packages", {}).get(flavor) != \
                        provenance["artifacts"][name]:
                    raise ValueError(f"{system} package provenance differs from {name}")
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
    validator_commit = run("git", "-C", str(validator), "rev-parse", "HEAD")
    if run("git", "status", "--porcelain=v1", "--untracked-files=all"):
        raise ValueError("commit the ForeverTAS source before building packages")
    if validator_commit != manifest["sources"]["forevervalidator"]["commit"]:
        raise ValueError("ForeverValidator does not match the release pin")
    if run("git", "-C", str(validator), "status", "--porcelain=v1"):
        raise ValueError("ForeverValidator has uncommitted changes")
    base = os.environ.get("FOREVERTAS_LINUX_TOOLCHAIN_IMAGE") or run(
        str(ROOT / "packaging/release/ensure-linux-toolchain.sh"))
    hip = (os.environ.get("FOREVERTAS_HIP_TOOLCHAIN_IMAGE") or run(
        str(ROOT / "packaging/release/ensure-linux-hip-toolchain.sh"))) \
        if any(flavor != "universal" for flavor in selected) else ""
    cache = (ROOT / manifest["cache"]["linux"]).resolve()
    cache.mkdir(parents=True, exist_ok=True)
    for component in components_for(selected):
        nvidia = component == NVIDIA_COMPONENT
        amd = component == "amd-rx7000-rx9000"
        cuda_arch = ";".join(f"{sm}-{kind}"
                             for sm in SUPPORTED_SM for kind in ("real", "virtual")) \
            if nvidia else ""
        environment = {
            "FOREVERTAS_BUILD_DIR": f"/workspace/build/distribution-{component}",
            "FOREVERTAS_APPDIR": f"/workspace/build/distribution-{component}/AppDir",
            "FOREVERTAS_DIST_DIR": "/workspace/dist",
            "FOREVERTAS_TOOLS_DIR": "/cache/appimage-tools",
            "FOREVERTAS_VALIDATOR_SOURCE": "/validator",
            "FOREVERTAS_DISTRIBUTION_FLAVOR": component,
            "FOREVERTAS_ENABLE_VULKAN": "ON",
            "FOREVERTAS_ENABLE_CUDA": "ON" if nvidia else "OFF",
            "FOREVERTAS_ENABLE_HIP": "ON" if nvidia or amd else "OFF",
            "FOREVERTAS_HIP_PLATFORM": "nvidia" if nvidia else "amd" if amd else "",
            "FOREVERTAS_HIP_ARCHITECTURES": ";".join(str(sm) for sm in SUPPORTED_SM) if nvidia else ";".join(SUPPORTED_GFX) if amd else "",
            "FOREVERTAS_HIP_FLAGS": "-fPIC" if amd else "",
            "FOREVERTAS_CUDA_ARCHITECTURES": cuda_arch,
            "HIP_PLATFORM": "nvidia" if nvidia else "amd" if amd else "",
            "HIP_PATH": "/opt/rocm" if nvidia or amd else "",
            "ROCM_PATH": "/opt/rocm" if nvidia or amd else "",
            "APPIMAGE_EXTRACT_AND_RUN": "1",
            "FOREVERTAS_RELEASE_JOBS": os.environ.get("FOREVERTAS_RELEASE_JOBS", "4"),
            "SCCACHE_DIR": "/cache/sccache",
        }
        command = ["docker", "run", "--rm", "--init",
                   "--user", f"{os.getuid()}:{os.getgid()}",
                   "--tmpfs", f"/home/builder:rw,uid={os.getuid()},gid={os.getgid()},mode=0755",
                   "--env", "HOME=/home/builder",
                   "--volume", f"{ROOT}:/workspace",
                   "--volume", f"{validator}:/validator:ro",
                   "--volume", f"{cache}:/cache",
                   "--workdir", "/workspace"]
        for key, value in environment.items():
            command.extend(("--env", f"{key}={value}"))
        command.extend((hip if nvidia or amd else base,
                        "bash", "packaging/linux/build-appimage.sh"))
        print(f"Building Linux {component} from ForeverTAS {source_commit} "
              f"and ForeverValidator {validator_commit}", flush=True)
        subprocess.run(command, check=True, cwd=ROOT)
        if run("git", "rev-parse", "HEAD") != source_commit or \
                run("git", "status", "--porcelain=v1", "--untracked-files=all") or \
                run("git", "-C", str(validator), "rev-parse", "HEAD") != validator_commit or \
                run("git", "-C", str(validator), "status", "--porcelain=v1"):
            raise ValueError(f"sources changed while building Linux {component}")
        if nvidia:
            build = f"/workspace/build/distribution-{component}"
            objects = f"{build}/_deps/forevervalidator-build/CMakeFiles/forevervalidator_core.dir/src/simulation/backends"
            expected = {str(sm) for sm in SUPPORTED_SM}
            inspect = command[:command.index(hip)] + [hip, "cuobjdump"]
            for label, path in (("application", f"{build}/bin/ForeverTAS"),
                                ("CUDA search", f"{objects}/cuda/cuda_search_executor.cu.o"),
                                ("HIP search", f"{objects}/hip/generated/hip_search_executor.cu.o")):
                cubins = set(re.findall(r"sm_([0-9]+)\.cubin",
                                        run(*(inspect + ["--list-elf", path]))))
                if cubins != expected:
                    raise ValueError(f"{label} has unexpected CUDA cubins: {cubins}")
            for label, path in (("application", f"{build}/bin/ForeverTAS"),
                                ("CUDA search", f"{objects}/cuda/cuda_search_executor.cu.o")):
                ptx = set(re.findall(r"sm_([0-9]+)\.ptx",
                                     run(*(inspect + ["--list-ptx", path]))))
                if ptx != expected:
                    raise ValueError(f"{label} has unexpected CUDA PTX: {ptx}")
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
    if NVIDIA_COMPONENT in components_for(selected):
        source = ROOT / "dist" / asset_name(manifest["release"]["version"],
                                            "linux", NVIDIA_COMPONENT)
        source_digest = digest(source)
        for flavor in selected:
            if component_for(flavor) != NVIDIA_COMPONENT:
                continue
            destination = ROOT / "dist" / asset_name(
                manifest["release"]["version"], "linux", flavor)
            destination.unlink(missing_ok=True)
            os.link(source, destination)
            (destination.parent / f"{destination.name}.sha256").write_text(
                f"{source_digest}  {destination.name}\n", encoding="ascii")
        source.unlink()
        source.with_name(source.name + ".sha256").unlink()
    record_path = ROOT / "dist/linux-source.json"
    record = {"schema": 1, "forevertas": source_commit,
              "forevervalidator": validator_commit, "components": {},
              "packages": {}}
    if record_path.is_file():
        previous = json.loads(record_path.read_text(encoding="utf-8"))
        if all(previous.get(key) == record[key]
               for key in ("schema", "forevertas", "forevervalidator")):
            record = previous
    for component in components_for(selected):
        representative = next(flavor for flavor in selected
                              if component_for(flavor) == component)
        artifact = ROOT / "dist" / asset_name(
            manifest["release"]["version"], "linux", representative)
        record["components"][component] = {
            "sha256": digest(artifact),
            "toolchain": hip if component != "universal" else base,
        }
    for flavor in selected:
        artifact = ROOT / "dist" / asset_name(
            manifest["release"]["version"], "linux", flavor)
        record["packages"][flavor] = digest(artifact)
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
