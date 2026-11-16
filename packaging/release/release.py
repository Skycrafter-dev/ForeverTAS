#!/usr/bin/env python3
"""ForeverTAS releases: one app per platform, GPU code as modules.

    release.py build            build everything that changed (Linux in a
                                container here, Windows on the Windows builder)
    release.py check            check what was built (see below)
    release.py publish          upload it (GitHub; asks before anything)
    release.py render-module    build the In-game render module alone (in a
                                container; build does it too)

What a release is (docs/RELEASES.md): the app for Linux (AppImage) and for
Windows (installer), with no GPU code; GPU modules, the search core built
with CUDA for each NVIDIA generation and with HIP for AMD's, which the app
downloads for the GPU it finds; and updates.json, which lists them all.

Fast: sources are synced into the build folders by content, the build folders
and the compiler caches stay between releases, and the GPU code is compiled
once for all generations (modules are split from it by relinking). A release
that changes no GPU code compiles no GPU code. Modules are named by the hash
of their content: an unchanged one is not uploaded again.

Every release ships the In-game render mode: a private module (ForeverViewer's
renderer), never in this repository, built from its own repository at the
commit packaging/release/release.local.json pins (untracked: see
docs/RELEASES.md). --without-render-module builds without it, for tests only:
publish refuses such a build.

Checked (release.py check): every file against its hash; every module of a
platform made for its app (the same interface key); every module unpacked and
loaded by the shipped app itself, on Linux in a clean container with no
GPU toolkit, on Windows with only the system on PATH, and so is the render
module; with --packs/--replay, a search on this computer's GPU must equal the
same search on the CPU.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RELEASE = Path(__file__).resolve().parent
CONFIG = json.loads((RELEASE / "release.json").read_text())
# This computer's own settings, never committed: the render module's repository and commit.
LOCAL = RELEASE / "release.local.json"
RENDER_MODULE_SYSTEMS = ("linux", "windows")


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def run(command: list[str], **kwargs) -> str:
    return subprocess.run(command, check=True, text=True, capture_output=True, **kwargs).stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def version() -> str:
    match = re.search(r"project\(ForeverTAS\s+VERSION\s+([0-9.]+)", (ROOT / "CMakeLists.txt").read_text())
    return match.group(1)


def validator_pin() -> str:
    match = re.search(r"FetchContent_Declare\(\s*ForeverValidator\b.*?GIT_TAG\s+([0-9a-f]{40})",
                      (ROOT / "CMakeLists.txt").read_text(), re.S)
    return match.group(1)


def dist_dir(args) -> Path:
    return Path(args.dist).resolve() / version()


# --- sources -----------------------------------------------------------------

def stage(repository: Path, revision: str, target: Path, leave_out: tuple[str, ...] = ()) -> int:
    """repository at revision, in target, by content: files that did not
    change keep their time, so the build folders rebuild only what did.
    leave_out: its folders the build never reads."""
    incoming = target.with_name(target.name + ".incoming")
    shutil.rmtree(incoming, ignore_errors=True)
    incoming.mkdir(parents=True)
    archive = subprocess.run(["git", "-C", str(repository), "archive", revision, "--", ".",
                              *(f":(exclude){folder}" for folder in leave_out)],
                             check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(incoming, filter="data")
        else:
            tar.extractall(incoming)
    target.mkdir(parents=True, exist_ok=True)
    run(["rsync", "-rlc", "--delete", f"{incoming}/", f"{target}/"])
    shutil.rmtree(incoming)
    return int(run(["git", "-C", str(repository), "log", "-1", "--format=%ct", revision]).strip())


def prepare_sources(args) -> int:
    cache = Path(args.cache).expanduser()
    if run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"]).strip() and not args.allow_dirty:
        raise SystemExit("ForeverTAS has uncommitted changes; a release is built from a commit "
                         "(--allow-dirty builds HEAD anyway, without them)")
    validator = Path(args.validator).expanduser()
    pin = validator_pin()
    if subprocess.run(["git", "-C", str(validator), "cat-file", "-e", f"{pin}^{{commit}}"]).returncode != 0:
        raise SystemExit(f"{validator} does not have the pinned ForeverValidator commit {pin}")
    epoch = stage(ROOT, "HEAD", cache / "src")
    stage(validator, pin, cache / "validator")
    log(f"sources: ForeverTAS {run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD']).strip()}, "
        f"ForeverValidator {pin[:7]}")
    return epoch


# --- the In-game render module -------------------------------------------------

def render_module_pin() -> tuple[Path | None, str]:
    """The render module's repository (on this computer, when it builds the
    module) and pinned commit, from release.local.json."""
    settings = json.loads(LOCAL.read_text()).get("render_module") if LOCAL.exists() else None
    if not settings or not settings.get("commit"):
        raise SystemExit(f"{LOCAL.name} does not pin the render module (every release ships it): see "
                         "docs/RELEASES.md, or --without-render-module for a test build")
    repository = settings.get("repository")
    return (Path(repository).expanduser() if repository else None), settings["commit"]


def render_module(args, systems: list[str]) -> dict[str, Path]:
    """The render module's files per system (and their licenses, in
    licenses/), built in a container (render-module-<system>.Dockerfile)
    from the pinned commit into the cache (kept there until the pin moves).
    A computer without Docker or Podman takes them from its cache, copied from
    one that has."""
    if args.without_render_module:
        log("render module: none (--without-render-module: a test build, not a release)")
        return {}
    repository, commit = render_module_pin()
    cache = Path(args.cache).expanduser() / "render-module"
    built = {}
    for system in systems:
        folder = cache / "out" / system
        stamp = cache / f"{system}.commit"
        if not (stamp.exists() and stamp.read_text().strip() == commit and folder.is_dir()):
            if not shutil.which("docker") and not shutil.which("podman"):
                raise SystemExit(f"the {system} render module of {commit[:10]} is not in {cache}: build it on a "
                                 f"computer with Docker or Podman (release.py render-module --os {system}) and "
                                 f"copy {cache} here")
            if repository is None or subprocess.run(
                    ["git", "-C", str(repository), "cat-file", "-e", f"{commit}^{{commit}}"]).returncode != 0:
                raise SystemExit(f"the render module's repository ({repository}) does not have {commit}")
            # without its test oracles (gigabytes)
            stage(repository, commit, cache / "src", leave_out=("artifacts",))
            container = engine()
            tag = image(container, RELEASE / f"render-module-{system}.Dockerfile",
                        f"forevertas-render-module-{system}")
            shutil.rmtree(folder, ignore_errors=True)
            stamp.unlink(missing_ok=True)
            log(f"render module: building {system} from {commit[:10]}")
            subprocess.run([
                    container, "run", "--rm", "--init", *user_args(container),
                    "-v", f"{cache / 'src'}:/render:ro", "-v", f"{cache}:/work",
                    "-e", "FVM_WORK=/work", "-e", "OUT=/work/out", "-e", f"JOBS={args.jobs}",
                    "-e", "FVM_LAUNCHER=ccache", "-e", "CCACHE_DIR=/work/ccache",
                    tag, "bash", "/render/forevertas/build-module.sh", system], check=True)
            stamp.write_text(commit + "\n")
        files = sorted(path.name for path in folder.iterdir() if path.is_file())
        if not files or not any((folder / "licenses").glob("*.txt")):
            raise SystemExit(f"the {system} render module build left no files or no licenses in {folder}")
        log(f"render module ({system}, {commit[:10]}): {', '.join(files)}")
        built[system] = folder
    return built


def build_render_module(args) -> None:
    render_module(args, [system for system in args.os if system in RENDER_MODULE_SYSTEMS])


def record_render_module(out: Path, system: str, folder: Path | None) -> None:
    """dist/<version>/<system>/render-module: what the build shipped (check and publish read it)."""
    marker = out / system / "render-module"
    marker.parent.mkdir(parents=True, exist_ok=True)
    if folder is None:
        marker.write_text("none\n")
    else:
        marker.write_text((folder.parent.parent / f"{system}.commit").read_text())


# --- Linux -------------------------------------------------------------------

def engine() -> str:
    for candidate in ("docker", "podman"):
        if shutil.which(candidate):
            return candidate
    raise SystemExit("Linux builds need Docker or Podman")


def image(container: str, dockerfile: Path, name: str) -> str:
    tag = f"{name}:{sha256(dockerfile)[:12]}"
    if subprocess.run([container, "image", "inspect", tag], capture_output=True).returncode != 0:
        log(f"building the {name} image (once per change of {dockerfile.name})")
        with tempfile.TemporaryDirectory() as context:
            subprocess.run([container, "build", "-f", str(dockerfile), "-t", tag, context], check=True)
    return tag


def user_args(container: str) -> list[str]:
    # files the container writes belong to whoever runs the release
    if container == "podman":
        return ["--userns=keep-id"]
    return ["--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp"]


def build_linux(args, epoch: int, out: Path, render: Path | None) -> None:
    cache = Path(args.cache).expanduser()
    container = engine()
    tag = image(container, RELEASE / "linux.Dockerfile", "forevertas-linux")
    for folder in ("build-linux", "cache-linux"):
        (cache / folder).mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    parts = " ".join(args.parts)
    log(f"Linux: {parts}")
    # the In-game render module and its libraries, beside the app, and their licenses with its own
    extra = []
    if render is not None:
        files = sorted(path.name for path in render.iterdir() if path.is_file())
        licenses = sorted(path.name for path in (render / "licenses").iterdir() if path.is_file())
        extra = ["-v", f"{render}:/extra:ro",
                 "-e", "FOREVERTAS_EXTRA_LIBRARIES=" + ":".join(f"/extra/{name}" for name in files),
                 "-e", "FOREVERTAS_EXTRA_LICENSES=" + ";".join(f"/extra/licenses/{name}" for name in licenses)]
    subprocess.run([
        container, "run", "--rm", "--init", *user_args(container),
        "-v", f"{cache / 'src'}:/src:ro", "-v", f"{cache / 'validator'}:/validator:ro",
        "-v", f"{cache / 'build-linux'}:/build", "-v", f"{cache / 'cache-linux'}:/cache",
        "-v", f"{out}:/out", *extra,
        "-e", f"PARTS={parts}", "-e", f"JOBS={args.jobs}",
        "-e", f"CUDA_SM={','.join(str(sm) for sm in CONFIG['cuda']['sm'])}",
        "-e", f"CUDA_PTX={CONFIG['cuda']['ptx']}",
        "-e", f"HIP_GFX={';'.join(CONFIG['hip']['gfx'])}",
        "-e", f"SOURCE_DATE_EPOCH={epoch}", "-e", "APPIMAGE_EXTRACT_AND_RUN=1",
        *(["-e", f"CCACHE_MAXSIZE={os.environ['CCACHE_MAXSIZE']}"] if "CCACHE_MAXSIZE" in os.environ else []),
        tag, "bash", "/src/packaging/release/build-linux.sh"], check=True)
    # the interface key the Linux modules must carry
    abi = (cache / "build-linux" / "app" / "generated" / "forevertas_compute_abi.h")
    if abi.exists():
        shutil.copy2(abi, out / "linux" / "compute-abi.h")


# --- Windows -----------------------------------------------------------------

def local_windows() -> dict:
    """This computer's Windows builder, from release.local.json: {"host": "user@address", "sccache": "C:/..."}."""
    return json.loads(LOCAL.read_text()).get("windows", {}) if LOCAL.exists() else {}


def windows_host(args) -> str:
    host = args.windows_host or local_windows().get("host")
    if not host:
        raise SystemExit(f"no Windows builder: pass --windows-host or set windows.host in {LOCAL.name}")
    return host


def build_windows(args, epoch: int, out: Path, render: Path | None) -> None:
    cache = Path(args.cache).expanduser()
    host = windows_host(args)
    root = CONFIG["windows"]["root"]
    log(f"Windows on {host}: {' '.join(args.parts)}")
    bundle = cache / "windows-sources.tar"
    with tarfile.open(bundle, "w") as tar:
        tar.add(cache / "src", arcname="src")
        tar.add(cache / "validator", arcname="validator")
    ssh = ["ssh", "-o", "BatchMode=yes", host]
    subprocess.run([*ssh, f"New-Item -ItemType Directory -Force '{root}' | Out-Null"], check=True)
    subprocess.run(["scp", "-q", str(bundle), f"{host}:{root}/sources.tar"], check=True)
    subprocess.run(["scp", "-q", str(RELEASE / "build-windows.ps1"), f"{host}:{root}/build-windows.ps1"], check=True)
    subprocess.run([*ssh, f"if (Test-Path '{root}/extra') {{ Remove-Item -Recurse -Force '{root}/extra' }}"],
                   check=True)
    if render is not None:
        # the In-game render module and its DLLs, beside the app (licenses/: theirs)
        subprocess.run(["scp", "-q", "-r", str(render), f"{host}:{root}/extra"], check=True)
    sm = ",".join(str(value) for value in CONFIG["cuda"]["sm"])
    gfx = ";".join(CONFIG["hip"]["gfx"])
    subprocess.run([*ssh, f"powershell -NoProfile -ExecutionPolicy Bypass -File '{root}/build-windows.ps1' "
                          f"-Root '{root}' -Parts '{' '.join(args.parts)}' -CudaSm '{sm}' "
                          f"-CudaPtx {CONFIG['cuda']['ptx']} -HipGfx '{gfx}' -SourceDateEpoch {epoch} "
                          f"-VcpkgCommit {CONFIG['windows']['vcpkg_commit']}"
                          + (f" -Sccache '{local_windows()['sccache']}'" if local_windows().get("sccache") else "")],
                   check=True)
    target = out / "windows"
    shutil.rmtree(target, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(["scp", "-q", "-r", f"{host}:{root}/out/windows", str(out)], check=True)


def build(args) -> None:
    epoch = prepare_sources(args)
    out = dist_dir(args)
    started = time.time()
    render = render_module(args, [system for system in args.os if system in RENDER_MODULE_SYSTEMS]) \
        if "app" in args.parts else {}
    if "linux" in args.os:
        build_linux(args, epoch, out, render.get("linux"))
        if "app" in args.parts:
            record_render_module(out, "linux", render.get("linux"))
    if "windows" in args.os:
        build_windows(args, epoch, out, render.get("windows"))
        if "app" in args.parts:
            record_render_module(out, "windows", render.get("windows"))
    log(f"built in {time.time() - started:.0f} s; checking")
    check(args)


# --- catalog and checks ------------------------------------------------------

def app_asset(platform: str) -> str:
    return (f"ForeverTAS-{version()}-linux-universal-x86_64.AppImage" if platform == "linux"
            else f"ForeverTAS-{version()}-windows-universal-x86_64-Setup.exe")


def modules(out: Path, platform: str) -> list[dict]:
    entries = []
    for listing in sorted((out / platform / "compute").glob("*/modules.json")):
        for entry in json.loads(listing.read_text()):
            entry["path"] = listing.parent / entry["file"]
            entries.append(entry)
    return entries


def catalog(out: Path) -> dict:
    """updates.json: schema 1 for apps up to 0.2.7 (the app is its "universal"
    asset), plus "compute", the GPU modules per platform."""
    repository = CONFIG["repository"]
    assets, compute = [], {}
    for platform in ("linux", "windows"):
        app = out / platform / app_asset(platform)
        if app.exists():
            assets.append({"name": app.name, "sha256": sha256(app)})
        compute[platform] = [
            {key: value for key, value in entry.items() if key not in ("path", "file", "size")} |
            {"url": f"https://github.com/{repository}/releases/download/"
                    f"{CONFIG['compute_release']}/{entry['file']}",
             "size": entry["size"]}
            for entry in modules(out, platform)]
    return {"schema": 1, "version": version(),
            "release_url": f"https://github.com/{repository}/releases/tag/v{version()}",
            "assets": assets, "compute": compute}


def render_module_shipped(out: Path, platform: str) -> str | None:
    """The render module commit the platform's app ships, or None."""
    marker = out / platform / "render-module"
    text = marker.read_text().strip() if marker.exists() else "none"
    return None if text == "none" else text[:10]


def interface_key(out: Path, platform: str) -> str | None:
    header = out / platform / "compute-abi.h"
    if not header.exists():
        return None
    return re.search(r'kComputeAbi\[\] = "([0-9a-f]+)"', header.read_text()).group(1)


def check(args) -> None:
    out = dist_dir(args)
    problems = []
    for platform in ("linux", "windows"):
        if not (out / platform).exists():
            continue
        app = out / platform / app_asset(platform)
        entries = modules(out, platform)
        key = interface_key(out, platform)
        if not app.exists() and "app" in args.parts:
            problems.append(f"{platform}: no app ({app.name})")
        for entry in entries:
            if sha256(entry["path"]) != entry["sha256"]:
                problems.append(f"{entry['file']}: does not match its hash")
            if key and entry["abi"] != key:
                problems.append(f"{entry['file']}: made for another app (key {entry['abi']}, app {key})")
        expected = ({f"cuda-sm{sm}" for sm in CONFIG["cuda"]["sm"]} if "cuda" in args.parts else set()) | \
                   ({"hip"} if "hip" in args.parts else set())
        missing = expected - {entry["id"] for entry in entries}
        if missing:
            problems.append(f"{platform}: no module for {', '.join(sorted(missing))}")
        log(f"{platform}: app {'present' if app.exists() else 'MISSING'}, {len(entries)} modules, "
            f"{sum(entry['size'] for entry in entries) / 1e6:.0f} MB of modules, "
            f"render module {render_module_shipped(out, platform) or 'none'}")
    if "linux" in args.os and (out / "linux" / app_asset("linux")).exists():
        problems += check_linux_modules(args, out)
    if "windows" in args.os and (out / "windows").exists():
        problems += check_windows_modules(args, out)
    if problems:
        for problem in problems:
            log(f"PROBLEM: {problem}")
        raise SystemExit(1)
    (out / "updates.json").write_text(json.dumps(catalog(out), indent=1) + "\n")
    notes = release_notes()
    if notes:
        (out / "notes.md").write_text(notes)
    else:
        log(f"CHANGELOG.md has no section for {version()}: the release will have no notes")
    log(f"all good: {out}")


def release_notes() -> str:
    """This version's section of CHANGELOG.md, with the download links."""
    text = (ROOT / "CHANGELOG.md").read_text()
    match = re.search(rf"^## {re.escape(version())}\s*$(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not match:
        return ""
    repository = CONFIG["repository"]
    downloads = (f"## Downloads\n\n"
                 f"- **Windows:** [the installer](https://github.com/{repository}/releases/download/v{version()}/ForeverTAS-Windows-Setup.exe)\n"
                 f"- **Linux:** [the AppImage](https://github.com/{repository}/releases/download/v{version()}/ForeverTAS-Linux.AppImage)\n\n"
                 "Installed ForeverTAS updates itself, and installs the GPU support for your graphics card.\n\n"
                 "To search on Google Colab's GPU (or lend it to a group, or the public pool): "
                 f"[open the notebook in Colab](https://colab.research.google.com/github/{repository}/blob/v{version()}/worker/colab.ipynb).\n")
    return f"# ForeverTAS {version()}\n\n{downloads}{match.group(1).rstrip()}\n"


def check_linux_modules(args, out: Path) -> list[str]:
    """Each module loaded by the shipped AppImage in a clean system: no CUDA,
    no ROCm, nothing a user would not have. On this computer too, with a
    search, for the modules its GPU runs (--packs/--replay)."""
    container = engine()
    tag = image(container, RELEASE / "linux-check.Dockerfile", "forevertas-linux-check")
    app = out / "linux" / app_asset("linux")
    problems = []
    entries = modules(out, "linux")
    with tempfile.TemporaryDirectory() as extracted:
        # the AppImage unpacked once; one container checks every module
        subprocess.run([str(app), "--appimage-extract"], cwd=extracted, check=True, capture_output=True)
        script = "".join(
                f'echo "@@ {entry["file"]}"; /app/AppRun --check-compute-module /compute/{entry["path"].relative_to(out / "linux" / "compute")}; echo "@@ exit $?"\n'
                for entry in entries)
        render = render_module_shipped(out, "linux")
        if render:
            script += 'echo "@@ render-module"; /app/AppRun --check-render-module; echo "@@ exit $?"\n'
        expected = len(entries) + (1 if render else 0)
        result = subprocess.run([
                container, "run", "--rm", *user_args(container),
                "-v", f"{extracted}/squashfs-root:/app:ro", "-v", f"{out / 'linux' / 'compute'}:/compute:ro",
                "-e", "QT_QPA_PLATFORM=offscreen", tag, "bash", "-c", script],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # each module's output and exit code
        codes = re.findall(r"@@ (\S+)\n(.*?)@@ exit (\d+)", result.stdout, re.S)
        for name, text, code in codes:
            if code != "0":
                problems.append(f"{name}: the app could not load it: {text.strip()[-400:]}")
            elif name == "render-module":
                # its last line (the clean system's locale warnings come first)
                log(f"Linux {text.strip().splitlines()[-1]} (loaded by the app in a clean system)")
        if len(codes) != expected:
            problems.append(f"the clean-system check ran {len(codes)} of {expected} modules: "
                            f"{result.stdout.strip()[-400:]}")
        log(f"Linux modules loaded by the app in a clean system: {expected - len(problems)} of {expected}")
        licenses = Path(extracted) / "squashfs-root/usr/share/doc/ForeverTAS/licenses/render-module"
        if render and not any(licenses.glob("*.txt")):
            problems.append("the AppImage ships the render module without its libraries' licenses")
        if args.packs and args.replay:
            searched = 0
            for entry in entries:
                result = subprocess.run(
                        [f"{extracted}/squashfs-root/AppRun", "--check-compute-module", str(entry["path"]),
                         args.packs, args.replay],
                        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                if result.returncode == 3:
                    continue   # not this GPU's module
                searched += 1
                log(f"  {result.stdout.strip()}")
                if result.returncode != 0:
                    problems.append(f"{entry['file']}: search on this GPU: {result.stdout.strip()[-400:]}")
            log(f"searches on this computer's GPU: {searched}")
    return problems


def check_windows_modules(args, out: Path) -> list[str]:
    host = windows_host(args)
    root = CONFIG["windows"]["root"]
    result = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", host,
             f"powershell -NoProfile -ExecutionPolicy Bypass -File '{root}/build-windows.ps1' -Root '{root}' -Check"
             + (" -ExpectRenderModule" if render_module_shipped(out, "windows") else "")],
            text=True, capture_output=True)
    print(result.stdout.strip())
    return [] if result.returncode == 0 else [f"Windows module check: {result.stderr.strip()[-400:]}"]


# --- publish -----------------------------------------------------------------

def publish(args) -> None:
    out = dist_dir(args)
    tag = f"v{version()}"
    repository = CONFIG["repository"]
    modules_release = CONFIG["compute_release"]
    updates = out / "updates.json"
    if not updates.exists():
        raise SystemExit("run release.py check first (it writes updates.json)")
    for platform in ("linux", "windows"):
        if not render_module_shipped(out, platform):
            raise SystemExit(f"the {platform} app was built without the render module (a test build): "
                             "a release ships it")
    if run(["git", "-C", str(ROOT), "rev-parse", "HEAD"]).strip() != \
            run(["git", "-C", str(ROOT), "rev-parse", f"{tag}^{{commit}}"]).strip():
        raise SystemExit(f"{tag} is not HEAD: tag the commit that was built")
    gh = ["gh", "-R", repository]
    hosted = set()
    listing = subprocess.run([*gh, "release", "view", modules_release, "--json", "assets"],
                             text=True, capture_output=True)
    if listing.returncode == 0:
        hosted = {asset["name"] for asset in json.loads(listing.stdout)["assets"]}
    new_modules = [entry["path"] for platform in ("linux", "windows")
                   for entry in modules(out, platform) if entry["file"] not in hosted]
    apps = [out / platform / app_asset(platform) for platform in ("linux", "windows")]
    # the worker kit (docs/REMOTE.md): what a notebook, a server or a container fetches from the release
    kit = [ROOT / "worker" / "forevertas_worker.py", ROOT / "worker" / "colab.ipynb", ROOT / "worker" / "kaggle.ipynb"]
    print(f"{tag}: {len(apps)} apps, updates.json, the worker kit; {len(new_modules)} new GPU modules "
          f"({sum(path.stat().st_size for path in new_modules) / 1e6:.0f} MB) for {modules_release}")
    if args.dry_run or input("publish? [y/N] ").strip().lower() != "y":
        return
    if listing.returncode != 0:
        # never "latest": updates.json is read from the latest release
        subprocess.run([*gh, "release", "create", modules_release, "--prerelease", "--latest=false",
                        "--title", "GPU modules", "--notes",
                        "GPU modules ForeverTAS downloads for your graphics card. Nothing to download here."],
                       check=True)
    if new_modules:
        subprocess.run([*gh, "release", "upload", modules_release, *map(str, new_modules)], check=True)
    with tempfile.TemporaryDirectory() as stable:
        copies = [Path(stable) / "ForeverTAS-Linux.AppImage", Path(stable) / "ForeverTAS-Windows-Setup.exe"]
        for source, copy in zip(apps, copies):
            shutil.copy2(source, copy)
        subprocess.run([*gh, "release", "create", tag, "--draft", "--title", f"ForeverTAS {version()}",
                        "--notes-file", str(out / "notes.md") if (out / "notes.md").exists() else "/dev/null",
                        *map(str, apps), *map(str, copies), str(updates), *map(str, kit)], check=True)
    print(f"{tag} is a draft: check it on GitHub, then publish it there")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("build", "check", "publish", "render-module"))
    parser.add_argument("--os", default="linux,windows", type=lambda text: text.split(","))
    parser.add_argument("--parts", default="app,cuda,hip", type=lambda text: text.split(","))
    parser.add_argument("--validator", default=str(ROOT.parent / "ForeverValidator"),
                        help="a ForeverValidator git repository with the pinned commit")
    parser.add_argument("--cache", default="~/.cache/forevertas-release")
    parser.add_argument("--dist", default=str(ROOT / "dist"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--windows-host")
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--packs", help="check: a Packs folder, to search on this computer's GPU")
    parser.add_argument("--replay", help="check: a replay for that search")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--without-render-module", action="store_true",
                        help="a test build without the In-game render module (publish refuses it)")
    args = parser.parse_args()
    {"build": build, "check": check, "publish": publish, "render-module": build_render_module}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
