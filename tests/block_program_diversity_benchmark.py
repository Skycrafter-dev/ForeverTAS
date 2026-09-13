"""Paired native/Blocks measurements across equivalent, structurally different graphs."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "packs", "replay", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--candidates", type=int, default=256)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--cuda-batch", type=int, default=64)
    parser.add_argument("--horizon", type=int, default=6000)
    parser.add_argument("--target-start", type=int)
    parser.add_argument("--block-batch", type=int)
    parser.add_argument("--backends", nargs="+", default=["cuda"],
                        choices=["optimized-cpu", "multi-threaded-cpu", "cuda"])
    parser.add_argument("--structures", nargs="+",
                        default=["workbench", "indirect", "broadcast", "tick-events", "loop-chunks",
                                 "checkpoint-events", "conditional-events", "procedure-chain", "shadowed-globals", "overwritten-globals", "dynamic-globals", "read-globals", "shrinking-globals", "mapped-restore", "mapped-horizon", "mapped-horizon-events", "mapped-history",
                                 "mapped-history-tail", "mapped-history-events", "mapped-history-indirect", "mapped-restart", "mapped-restart-indirect",
                                 "mapped-restart-history", "mapped-restore-history", "mapped-result-restore-history", "nested-map-tail",
                                 "nested-map-tick-events", "nested-map-physics-tail"],
                        choices=["workbench", "indirect", "broadcast", "tick-events", "loop-chunks",
                                 "checkpoint-events", "conditional-events", "procedure-chain", "shadowed-globals", "overwritten-globals",
                                 "dynamic-globals", "read-globals", "shrinking-globals", "mapped-restore", "mapped-horizon", "mapped-horizon-events", "mapped-history",
                                 "mapped-history-tail", "mapped-history-events", "mapped-history-indirect", "mapped-restart", "mapped-restart-indirect",
                                 "mapped-restart-history", "mapped-restore-history", "mapped-result-restore-history", "nested-map-tail",
                                 "nested-map-tick-events", "nested-map-physics-tail"])
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    expected = None
    summary = []
    for structure in args.structures:
        directory = output / structure
        command = [sys.executable, root / "block_program_benchmark.py",
                   "--build", args.build.resolve(), "--packs", args.packs.resolve(),
                   "--replay", args.replay.resolve(), "--output", directory,
                   "--candidates", args.candidates, "--repeats", args.repeats,
                   "--workers", args.workers, "--cuda-batch", args.cuda_batch,
                   "--horizons", args.horizon, "--structure", structure,
                   "--verify-interpreter", "--backends", *args.backends]
        if args.block_batch is not None:
            command += ["--block-batch", args.block_batch]
        if args.target_start is not None:
            command += ["--target-start", args.target_start]
        with (output / f"{structure}.log").open("w") as log:
            subprocess.run([str(value) for value in command], stdout=log, stderr=subprocess.STDOUT, check=True)
        records = json.loads((directory / "runs.json").read_text())
        for record in records:
            if record["path"] != "blocks":
                continue
            identity = {key: record[key] for key in ("score", "evaluationTimeMs", "inputsSha256")}
            if expected is None:
                expected = identity
            if identity != expected:
                raise RuntimeError(f"{structure}: graph transformation changed block results")
        for result in json.loads((directory / "summary.json").read_text()):
            summary.append({"structure": structure, **result})
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(f"Verified source/backend/cross-structure parity: {structure}", flush=True)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
