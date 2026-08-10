#!/usr/bin/env python3
"""Route the explicitly object-cached CUDA search build around sccache.

CMake invokes this as:
    python cuda_compiler_launcher.py <sccache> <nvcc> <nvcc args...>

The general search executor is cached as a complete validated object because it
is unusually expensive and large. Compile that object directly while retaining
sccache coverage for smaller CUDA translation units and the compact LTO image.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(
            "usage: cuda_compiler_launcher.py <sccache> <compiler> [args...]",
            file=sys.stderr,
        )
        return 2

    sccache, compiler, *compiler_args = argv[1:]
    compiles_search_executor = any(
        Path(argument.strip('"')).name.lower() == "cuda_search_executor.cu"
        for argument in compiler_args
    )
    produces_lto_ir = any(
        argument.lower() in {"-ltoir", "--ltoir"}
        for argument in compiler_args
    )

    if compiles_search_executor and not produces_lto_ir:
        try:
            architecture_jobs = int(
                os.environ.get(
                    "FOREVERVALIDATOR_CUDA_SEARCH_ARCHITECTURE_JOBS", "1"))
        except ValueError:
            print("invalid CUDA search architecture job count", file=sys.stderr)
            return 2
        if architecture_jobs < 1:
            print("invalid CUDA search architecture job count", file=sys.stderr)
            return 2
        command = [compiler, f"--threads={architecture_jobs}", *compiler_args]
    else:
        command = [sccache, compiler, *compiler_args]

    try:
        return subprocess.run(command, check=False).returncode
    except OSError as error:
        print(f"failed to launch CUDA compiler: {error}", file=sys.stderr)
        return 127


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
