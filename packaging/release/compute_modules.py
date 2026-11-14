#!/usr/bin/env python3
"""GPU modules from one GPU build (see src/compute).

    compute_modules.py pack --build BUILD --backend cuda --os linux \\
        --sm 50,52,60,61,70,75,80,86,89,90,100,120 --ptx 120 --out DIR

CUDA: the build compiles every generation at once. Per generation, nvprune
strips the archives that carry GPU code to that generation (on Windows their
nvcc objects only, see prune_archive) and the module's
own link command (from ninja) runs again on them: one module per generation
in seconds, no recompiling. HIP: one module for all its GPUs.

Each module is a flat .tar.zst (the library, its GPU runtime, module.json)
named by the hash of its contents, so an unchanged module keeps its name
release after release and is never uploaded again. Writes DIR/modules.json:
the "compute" entries of updates.json, minus their URLs.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path


def run(command: list[str] | str, **kwargs) -> str:
    result = subprocess.run(command, text=True, capture_output=True,
                            shell=isinstance(command, str), **kwargs)
    if result.returncode != 0:
        # what the tool said, not only that it failed
        sys.stderr.write(f"{command}\n{result.stdout}{result.stderr}")
        raise subprocess.CalledProcessError(result.returncode, command, result.stdout, result.stderr)
    return result.stdout


def link_command(build: Path, target: str) -> str:
    commands = run(["ninja", "-C", str(build), "-t", "commands", target]).strip().splitlines()
    if not commands:
        raise SystemExit(f"ninja knows no commands for {target}")
    return commands[-1]


def abi(build: Path) -> str:
    text = (build / "generated" / "forevertas_compute_abi.h").read_text()
    match = re.search(r'kComputeAbi\[\] = "([0-9a-f]+)"', text)
    if not match:
        raise SystemExit("no kComputeAbi in the build")
    return match.group(1)


def link_inputs(build: Path, command: str) -> str:
    """The link command with its response files (Windows) inlined as text."""
    text = command
    for match in re.finditer(r"@(\S+\.rsp)", command):
        text += "\n" + (build / match.group(1)).read_text()
    return text


def device_archives(build: Path, command: str, cuobjdump: str) -> list[str]:
    """The archives of the link command that carry GPU code."""
    archives = []
    for token in sorted(set(re.findall(r'[^\s"]+\.(?:a|lib)\b', link_inputs(build, command)))):
        if Path(token).suffix in (".a", ".lib"):
            path = Path(token) if Path(token).is_absolute() else build / token
            if not path.exists():
                continue
            listing = subprocess.run([cuobjdump, "--list-elf", str(path)], text=True,
                                     capture_output=True)
            if ".cubin" in listing.stdout:
                archives.append(token)
    return archives


def archive_members(path: Path) -> list[tuple[str, bytes]]:
    """The object members of an ar archive (GNU or COFF .lib), in order."""
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise SystemExit(f"{path} is not an archive")
    members, names, at = [], b"", 8
    while at + 60 <= len(data):
        header = data[at:at + 60]
        name = header[:16].rstrip(b" ")
        size = int(header[48:58])
        body = data[at + 60:at + 60 + size]
        at += 60 + size + (size & 1)
        if name == b"//":
            names = body
        elif name == b"/" or name.startswith(b"/<"):
            continue  # symbol tables (written again by the archiver)
        else:
            if name.startswith(b"/"):
                start = int(name[1:])
                end = min(i for i in (names.find(b"\0", start), names.find(b"/\n", start), len(names))
                          if i >= 0)
                name = names[start:end]
            members.append((name.rstrip(b"/").decode(), body))
    return members


def prune_archive(nvprune: str, codes: list[str], source: Path, target: Path,
                  archiver: str | None) -> None:
    """source with its GPU code cut to codes. A Windows .lib is pruned member
    by member: nvprune damages the linker directives of objects that are not
    nvcc's (clang-cl's /DEFAULTLIB), and only nvcc's carry GPU code (the
    .cu objects and the library's device link)."""
    if source.suffix != ".lib":
        run([nvprune, *codes, str(source), "-o", str(target)])
        return
    if not archiver:
        raise SystemExit("pruning a .lib needs --archiver (llvm-lib)")
    folder = target.with_suffix(".members")
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    files = []
    for index, (name, body) in enumerate(archive_members(source)):
        file = folder / f"{index:04d}-{Path(name.replace(chr(92), '/')).name}"
        # nvcc's objects (.cu and the device link) have a fat binary
        # segment; its long section name is in the object's string table
        if b".nvFatBinSegment" in body:
            original = file.with_suffix(".full")
            original.write_bytes(body)
            run([nvprune, *codes, str(original), "-o", str(file)])
            original.unlink()
        else:
            file.write_bytes(body)
        files.append(file)
    listing = folder / "members.rsp"
    listing.write_text("\n".join(f'"{file}"' for file in files) + "\n")
    run([archiver, "/nologo", f"/out:{target}", f"@{listing}"])


def relink(build: Path, command: str, replacements: dict[str, str], output: Path) -> None:
    """The module's link command again, with other archives and output (its
    response files, on Windows, copied with the same replacements)."""
    def substitute(text: str) -> str:
        for old, new in replacements.items():
            text = text.replace(old, new)
        return text
    work = output.parent
    relinked = substitute(command)
    for match in re.finditer(r"@(\S+\.rsp)", command):
        copy = work / Path(match.group(1)).name
        copy.write_text(substitute((build / match.group(1)).read_text()))
        relinked = relinked.replace(f"@{match.group(1)}", f"@{copy}")
    # the output, and on Windows its import library and debug database
    # (replacements as functions: a Windows path is not a regex template)
    for pattern, value in ((r"(?<=\s)-o\s+\S+", f"-o {output}"),
                           (r"(?i)/out:\S+", f"/out:{output}"),
                           (r"(?i)/implib:\S+", f"/implib:{output.with_suffix('.lib')}"),
                           (r"(?i)/pdb:\S+", f"/pdb:{output.with_suffix('.pdb')}")):
        relinked = re.sub(pattern, lambda _, value=value: value, relinked)
    run(relinked, cwd=build)


def cubins(library: Path, cuobjdump: str) -> tuple[set[str], set[str]]:
    elf = run([cuobjdump, "--list-elf", str(library)])
    ptx = subprocess.run([cuobjdump, "--list-ptx", str(library)], text=True,
                         capture_output=True).stdout
    return set(re.findall(r"sm_(\d+)", elf)), set(re.findall(r"sm_(\d+)", ptx))


def bundle(folder: Path, out: Path, prefix: str) -> tuple[Path, str]:
    """A reproducible flat tar of folder, zstd-compressed, named by its hash:
    the same files give the same name in every release (no time in it)."""
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(folder.iterdir()):
            info = tar.gettarinfo(str(path), arcname=path.name)
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = 0
            info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
            with path.open("rb") as file:
                tar.addfile(info, file)
    content = data.getvalue()
    digest = hashlib.sha256(content).hexdigest()
    target = out / f"{prefix}-{digest[:16]}.tar.zst"
    if not target.exists():
        if shutil.which("zstd"):
            compressed = subprocess.run(
                    ["zstd", "-19", "--long=27", "-T0", "-q", "-c"], input=content,
                    check=True, capture_output=True).stdout
        else:
            import zstandard
            compressed = zstandard.ZstdCompressor(
                    compression_params=zstandard.ZstdCompressionParameters.from_level(
                            19, window_log=27, threads=-1)).compress(content)
        target.write_bytes(compressed)
    return target, hashlib.sha256(target.read_bytes()).hexdigest()


def module_entry(module_id: str, backend: str, module_abi: str, archive: Path,
                 sha256: str, **extra) -> dict:
    return {"id": module_id, "backend": backend, **extra, "abi": module_abi,
            "file": archive.name, "sha256": sha256, "size": archive.stat().st_size}


def write_module(folder: Path, library: Path, runtime: list[Path], backend: str,
                 module_id: str, module_abi: str, **extra) -> None:
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    shutil.copy2(library, folder / library.name)
    for file in runtime:
        shutil.copy2(file.resolve(), folder / file.name)
    (folder / "module.json").write_text(json.dumps(
            {"backend": backend, "id": module_id, **extra, "file": library.name, "abi": module_abi},
            indent=1) + "\n")


def pack(args: argparse.Namespace) -> None:
    build = Path(args.build).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = out / "work"
    module_abi = abi(build)
    extension = ".dll" if args.os == "windows" else ".so"
    name = f"forevertas-compute-{args.backend}{extension}"
    library = build / name
    runtime = [Path(path) for path in args.runtime]
    entries = []

    if args.backend == "hip":
        folder = work / "hip"
        write_module(folder, library, runtime, "hip", "hip", module_abi)
        archive, sha256 = bundle(folder, out, f"forevertas-compute-{args.os}-hip")
        entries.append(module_entry("hip", "hip", module_abi, archive, sha256))
    else:
        cuobjdump = args.cuobjdump
        nvprune = args.nvprune
        command = link_command(build, "forevertas_compute")
        archives = device_archives(build, command, cuobjdump)
        if not archives:
            raise SystemExit("the module's link command has no archive with GPU code")
        generations = [int(sm) for sm in args.sm.split(",")]

        def one(sm: int) -> dict:
            pruned = work / f"sm{sm}" / "pruned"
            pruned.mkdir(parents=True, exist_ok=True)
            replacements = {}
            for archive in archives:
                source = Path(archive) if Path(archive).is_absolute() else build / archive
                target = pruned / source.name
                codes = ["-gencode", f"arch=compute_{sm},code=sm_{sm}"]
                if sm == args.ptx:
                    codes += ["-gencode", f"arch=compute_{sm},code=compute_{sm}"]
                prune_archive(nvprune, codes, source, target, args.archiver)
                replacements[archive] = str(target)
            linked = work / f"sm{sm}" / name
            relink(build, command, replacements, linked)
            elf, ptx = cubins(linked, cuobjdump)
            expected_ptx = {str(sm)} if sm == args.ptx else set()
            if elf != {str(sm)} or ptx != expected_ptx:
                raise SystemExit(f"sm{sm}: module carries sm {sorted(elf)}, ptx {sorted(ptx)}")
            folder = work / f"sm{sm}" / "module"
            module_id = f"cuda-sm{sm}"
            write_module(folder, linked, runtime, "cuda", module_id, module_abi, sm=sm, ptx=sm == args.ptx)
            archive, sha256 = bundle(folder, out, f"forevertas-compute-{args.os}-{module_id}")
            return module_entry(module_id, "cuda", module_abi, archive, sha256,
                                sm=sm, ptx=sm == args.ptx)

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            entries = list(pool.map(one, generations))

    shutil.rmtree(work, ignore_errors=True)
    (out / "modules.json").write_text(json.dumps(entries, indent=1) + "\n")
    for entry in entries:
        print(f"{entry['file']}  {entry['size'] / 1e6:.1f} MB")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    packer = sub.add_parser("pack")
    packer.add_argument("--build", required=True)
    packer.add_argument("--backend", choices=("cuda", "hip"), required=True)
    packer.add_argument("--os", choices=("linux", "windows"), required=True)
    packer.add_argument("--sm", default="", help="CUDA generations, e.g. 50,61,86")
    packer.add_argument("--ptx", type=int, default=0, help="the generation that also carries PTX")
    packer.add_argument("--runtime", action="append", default=[],
                        help="a GPU runtime library to ship next to the module")
    packer.add_argument("--out", required=True)
    packer.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    packer.add_argument("--nvprune", default="nvprune")
    packer.add_argument("--cuobjdump", default="cuobjdump")
    packer.add_argument("--archiver", help="llvm-lib, for pruning Windows .lib archives")
    args = parser.parse_args()
    if args.command == "pack":
        pack(args)


if __name__ == "__main__":
    sys.exit(main())
