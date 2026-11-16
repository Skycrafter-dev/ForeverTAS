#!/usr/bin/env python3
"""A ForeverTAS worker on a machine of yours (docs/REMOTE.md): a Google Colab
or Kaggle notebook, a rented GPU, a Docker container, another computer.

    python3 forevertas_worker.py --code CODE[,CODE...] --packs PACKS_DIRECTORY

--code takes what this machine searches for (Edit > Preferences > Remote):
your own machines' pairing code, groups' invites, public (the public pool),
separated by commas. It searches for one at a time, in that order.

It fetches ForeverTAS (the same version as yours: --version, default the
latest release) and the GPU support for this machine's card, then searches
for the ForeverTAS that showed the code (Edit > Preferences > Remote) until
that one closes its room. The Packs are the game's own, from your copy (a
Google Drive folder, a Kaggle dataset, a mounted folder): nothing of the game
is downloaded. Standard library only.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

REPOSITORY = "Skycrafter-dev/ForeverTAS"
APP_ASSET = "ForeverTAS-Linux.AppImage"


def say(text):
    print(f"[forevertas-worker] {text}", flush=True)


def fetch(url, accept=None):
    request = urllib.request.Request(url, headers={"User-Agent": "forevertas-worker",
                                                   **({"Accept": accept} if accept else {})})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def download(url, target, sha256=None):
    if target.exists() and (sha256 is None or hashlib.sha256(target.read_bytes()).hexdigest() == sha256):
        return target
    say(f"downloading {url.rsplit('/', 1)[-1]}")
    data = fetch(url)
    if sha256 is not None and hashlib.sha256(data).hexdigest() != sha256:
        sys.exit(f"{url}: the download does not match its hash")
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix(target.suffix + ".part")
    partial.write_bytes(data)
    partial.replace(target)
    return target


def release(version):
    path = "latest" if version == "latest" else f"tags/v{version.lstrip('v')}"
    info = json.loads(fetch(f"https://api.github.com/repos/{REPOSITORY}/releases/{path}",
                            "application/vnd.github+json"))
    assets = {asset["name"]: asset["browser_download_url"] for asset in info["assets"]}
    if APP_ASSET not in assets or "updates.json" not in assets:
        sys.exit(f"release {info.get('tag_name')} has no {APP_ASSET} or updates.json")
    return info["tag_name"].lstrip("v"), assets


def nvidia_capability():
    """The first NVIDIA GPU's compute capability as an sm number (89 for 8.9), or None."""
    if shutil.which("nvidia-smi") is None:
        return None
    result = subprocess.run(["nvidia-smi", "--query-gpu=compute_cap,name", "--format=csv,noheader"],
                            capture_output=True, text=True)
    match = re.match(r"\s*(\d+)\.(\d+)\s*,\s*(.*)", result.stdout.splitlines()[0] if result.stdout else "")
    if result.returncode != 0 or not match:
        return None
    say(f"GPU: {match.group(3).strip()} (compute {match.group(1)}.{match.group(2)})")
    return int(match.group(1)) * 10 + int(match.group(2))


def amd_present():
    return Path("/dev/kfd").exists()


def pick_module(modules):
    """The module for this machine's GPU from updates.json's compute.linux list."""
    sm = nvidia_capability()
    if sm is not None:
        cuda = sorted((m for m in modules if m["backend"] == "cuda"), key=lambda m: m["sm"])
        exact = [m for m in cuda if m["sm"] == sm]
        if exact:
            return exact[0]
        # a newer card than any module: the newest (its PTX); an older generation: the closest below
        below = [m for m in cuda if m["sm"] <= sm]
        if below:
            return below[-1]
        sys.exit(f"no CUDA module runs on compute {sm // 10}.{sm % 10}")
    if amd_present():
        hip = [m for m in modules if m["backend"] == "hip"]
        if hip:
            say("GPU: AMD (HIP)")
            return hip[0]
    # no GPU: the machine lends its CPU threads (ForeverTAS searches on them without a module)
    say("no NVIDIA or AMD GPU: this machine lends its CPU threads")
    return None


def unpack(archive, target):
    if (target / "module.json").exists():
        return
    target.mkdir(parents=True, exist_ok=True)
    try:
        subprocess.run(["tar", "--zstd", "-xf", str(archive), "-C", str(target)], check=True)
    except (OSError, subprocess.CalledProcessError):
        # no zstd: the zstandard module, installed for this
        subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "zstandard"], check=True)
        import zstandard  # noqa: E402
        with archive.open("rb") as raw, zstandard.ZstdDecompressor().stream_reader(raw) as stream:
            with tarfile.open(fileobj=stream, mode="r|") as tar:
                tar.extractall(target, filter="data")


def linux_app(updates):
    """The Linux app among updates.json's assets: {name, sha256}."""
    return next(asset for asset in updates["assets"] if asset["name"].endswith(".AppImage"))


def in_folder(folder, name):
    """A release file in a test build's folder (flat, or as release.py build lays it out)."""
    found = next(folder.rglob(name), None)
    if found is None:
        sys.exit(f"{folder} holds no {name}")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--code", required=True,
                        help="what it searches for, separated by commas: a pairing code, groups' invites, public")
    parser.add_argument("--name", help="this machine's name in a group (its credit)")
    parser.add_argument("--lend", default="gpu,cpu", help="what it lends: gpu,cpu (default), gpu or cpu")
    parser.add_argument("--cpu-threads", type=int, help="CPU threads it lends (default: all but 2 per GPU)")
    parser.add_argument("--packs", required=True, help="the game's Packs folder (your copy)")
    parser.add_argument("--version", default="latest", help="the ForeverTAS version yours runs (default: the latest)")
    parser.add_argument("--dir", default=str(Path.home() / ".cache" / "forevertas-worker"), help="where it keeps its files")
    parser.add_argument("--app", help="a ForeverTAS to run instead of downloading one (an AppImage or its AppRun)")
    parser.add_argument("--release", help="developers: run a build's files from this folder (its AppImage, "
                                           "updates.json and modules) instead of a published release")
    args = parser.parse_args()

    packs = Path(args.packs).expanduser()
    if not any(packs.glob("*.pak")):
        sys.exit(f"{packs} holds no .pak file: point --packs at the game's Packs folder")
    home = Path(args.dir).expanduser()
    if args.release:
        folder = Path(args.release).expanduser()
        updates = json.loads((folder / "updates.json").read_text())
        version = updates["version"]
        assets = {APP_ASSET: in_folder(folder, linux_app(updates)["name"]).as_uri()}
    else:
        version, assets = release(args.version)
        updates = json.loads(fetch(assets["updates.json"]))
    say(f"ForeverTAS {version}")
    module = pick_module(updates["compute"]["linux"]) if args.lend != "cpu" else None
    compute = home / "compute" / "none"
    if module is not None:
        name = module["url"].rsplit("/", 1)[-1]
        url = in_folder(Path(args.release).expanduser(), name).as_uri() if args.release else module["url"]
        archive = download(url, home / "modules" / name, module["sha256"])
        # (by content: two builds of one version can differ)
        compute = home / "compute" / module["sha256"][:16]
        unpack(archive, compute / module["id"])
        say(f"GPU support: {module['id']}")
    compute.mkdir(parents=True, exist_ok=True)

    if args.app:
        app = Path(args.app).expanduser()
    else:
        app_hash = linux_app(updates)["sha256"]
        image = download(assets[APP_ASSET], home / f"ForeverTAS-{app_hash[:16]}.AppImage", app_hash)
        image.chmod(0o755)
        extracted = home / f"app-{app_hash[:16]}"
        if not (extracted / "AppRun").exists():
            # notebooks and containers have no FUSE: the AppImage is unpacked and run as files
            say("unpacking ForeverTAS")
            subprocess.run([str(image), "--appimage-extract"], cwd=home, check=True, stdout=subprocess.DEVNULL)
            shutil.rmtree(extracted, ignore_errors=True)
            (home / "squashfs-root").rename(extracted)
        app = extracted / "AppRun"
    environment = dict(os.environ, FOREVERTAS_COMPUTE_DIR=str(compute), QT_QPA_PLATFORM="offscreen")
    say(f"searching for {args.code}")
    command = [str(app), "--worker", args.code.replace(" ", ""), "--packs", str(packs)]
    if args.name:
        command += ["--name", args.name]
    command += ["--lend", args.lend]
    if args.cpu_threads is not None:
        command += ["--cpu-threads", str(args.cpu_threads)]
    sys.exit(subprocess.run(command, env=environment).returncode)


if __name__ == "__main__":
    main()
