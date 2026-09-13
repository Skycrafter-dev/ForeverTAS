"""Paired real-physics benchmarks; keeps raw evidence and checks block parity."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--packs", type=Path, required=True)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--horizons", nargs="+", type=int, default=[1100, 6000])
    parser.add_argument("--candidates", type=int, default=8192)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--cuda-batch", type=int, default=256)
    parser.add_argument("--block-batch", type=int,
                        help="explicit editable block batch size, useful for small repeated-wave profiling")
    parser.add_argument("--backends", nargs="+", default=["optimized-cpu", "multi-threaded-cpu", "cuda"],
                        choices=["optimized-cpu", "multi-threaded-cpu", "cuda"])
    parser.add_argument("--verify-interpreter", action="store_true")
    parser.add_argument("--structure", default="workbench",
                        choices=["workbench", "indirect", "broadcast", "tick-events", "loop-chunks",
                                 "checkpoint-events", "conditional-events", "procedure-chain", "shadowed-globals", "overwritten-globals",
                                 "dynamic-globals", "read-globals", "shrinking-globals", "mapped-restore", "mapped-horizon", "mapped-horizon-events", "mapped-history",
                                 "mapped-history-tail", "mapped-history-events", "mapped-history-indirect", "mapped-restart", "mapped-restart-indirect",
                                 "mapped-restart-history", "mapped-restore-history", "mapped-result-restore-history", "nested-map-tail",
                                 "nested-map-tick-events", "nested-map-physics-tail"])
    parser.add_argument("--target-start", type=int, help="first scoring time; defaults to the horizon")
    args = parser.parse_args()
    if args.repeats < 1 or args.candidates < 1 or (args.block_batch is not None and args.block_batch < 1):
        parser.error("repeats and candidates must be positive")
    root = Path(__file__).resolve().parent.parent
    build, output = args.build.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = build / "forevertas-block-program-benchmark"
    baseline = build / "forevertas-native-baseline-benchmark"
    if not baseline.is_file():
        parser.error("build the exported v0.2.3 baseline target first")

    def run(command, name, env=None):
        with (output / name).open("w") as log:
            completed = subprocess.run([str(item) for item in command], stdout=log, stderr=subprocess.STDOUT,
                                       cwd=root, env=env, timeout=600)
        if completed.returncode:
            raise RuntimeError(f"Command failed; see {output / name}")
        return (output / name).read_text()

    def digest(file):
        with file.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    def telemetry(backend):
        sample = {"timeUnix": time.time(), "loadAverage": os.getloadavg()}
        sample["activeProcesses"] = subprocess.check_output(
            ["ps", "-eo", "comm,pcpu", "--sort=-pcpu"], text=True).splitlines()[:9]
        if backend == "cuda":
            sample["gpu"] = subprocess.check_output([
                "nvidia-smi", "--query-gpu=utilization.gpu,memory.used,temperature.gpu,power.draw",
                "--format=csv,noheader"], text=True).strip()
        return sample

    catalog = output / "catalog.json"
    run([build / "forevertas-block-editor-bridge-tests", "--dump-catalog"], catalog.name)
    metadata = {
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "baselineRevision": subprocess.check_output(["git", "rev-parse", "v0.2.3"], cwd=root, text=True).strip(),
        "baselinePhysics": "Both executables link the same cached ForeverValidator build; only search source differs.",
        "benchmarkSha256": digest(executable), "baselineSha256": digest(baseline),
        "replaySha256": digest(args.replay), "candidates": args.candidates,
        "repeats": args.repeats, "warmup": "Each search loads a scenario and evaluates its baseline; both wall and steady times retained.",
        "blockBatchSize": args.block_batch,
        "structure": args.structure,
        "targetStartMs": args.target_start,
        "physicsCache": str(build / "_deps/forevervalidator-build"),
        "cpu": subprocess.check_output(["lscpu"], text=True),
    }
    if "cuda" in args.backends:
        metadata["gpu"] = subprocess.check_output([
            "nvidia-smi", "--query-gpu=name,driver_version,memory.total,memory.used", "--format=csv,noheader"], text=True).strip()
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    records = []
    for horizon in args.horizons:
        target_start = horizon if args.target_start is None else args.target_start
        if target_start < 0 or target_start > horizon or target_start % 10:
            parser.error("target-start must be tick-aligned and within each horizon")
        environment = {**os.environ, "FOREVERTAS_BENCH_TARGET_START": str(target_start)}
        fixture = output / f"blocks-{horizon}.json"
        run(["node", root / "tests/block_program_benchmark_fixture.mjs", catalog, horizon, args.candidates,
             "auto" if args.block_batch is None else args.block_batch, args.structure, target_start], fixture.name)
        metadata.setdefault("fixtureSha256", {})[str(horizon)] = digest(fixture)
        (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        expected = None
        if args.verify_interpreter:
            text = run([executable, args.packs, args.replay, fixture, "optimized-cpu", 1, args.candidates, horizon],
                       f"interpreter-{horizon}.log", {**environment, "FOREVERTAS_BENCH_INTERPRETER": "1"})
            expected = json.loads(next(line for line in reversed(text.splitlines()) if line.startswith("{")))
        for backend in args.backends:
            workers = 1 if backend == "optimized-cpu" else args.workers if backend == "multi-threaded-cpu" else args.cuda_batch
            for repeat in range(args.repeats):
                for variant in (["native", "blocks"] if repeat % 2 == 0 else ["blocks", "native"]):
                    run_workers = args.workers if backend == "cuda" and variant == "blocks" else workers
                    name = f"{backend}-{horizon}-{repeat}-{variant}.log"
                    before = telemetry(backend)
                    text = run([baseline if variant == "native" else executable, args.packs, args.replay,
                                "native" if variant == "native" else fixture, backend, run_workers, args.candidates, horizon], name, environment)
                    record = json.loads(next(line for line in reversed(text.splitlines()) if line.startswith("{")))
                    if record["candidates"] != args.candidates:
                        raise RuntimeError(f"Wrong candidate count in {name}")
                    if variant == "blocks":
                        mode = "CUDA block-program kernel" if backend == "cuda" else "Compiled block program on CPU"
                        if record["executionMode"] not in (mode, "Uniform compiled block program"):
                            raise RuntimeError(f"Benchmark fell back in {name}: {record['executionMode']}")
                        allowed_modes = {mode, "Uniform compiled block program", "Preparing CUDA block program"}
                        if backend == "cuda":
                            allowed_modes.add("CUDA resident physics with source block control")
                        unexpected = [value for value in record.get("executionModes", []) if value not in allowed_modes]
                        if unexpected:
                            raise RuntimeError(f"Benchmark had an intermediate fallback in {name}: {unexpected}")
                        if expected is None:
                            expected = record
                        for field in ("score", "evaluationTimeMs", "inputsSha256"):
                            if expected[field] != record[field]:
                                raise RuntimeError(f"Block semantic mismatch ({field}) in {name}")
                    records.append({**record, "repeat": repeat, "log": name,
                                    "before": before, "after": telemetry(backend)})
                    (output / "runs.json").write_text(json.dumps(records, indent=2) + "\n")
        print(f"Verified paired runs at {horizon} ms", flush=True)
    summary = []
    for horizon in args.horizons:
        for backend in args.backends:
            item = {"horizonMs": horizon, "backend": backend}
            for variant in ("native", "blocks"):
                rows = [r for r in records if r["horizonMs"] == horizon and r["backend"] == backend and r["path"] == variant]
                item[variant] = {key: statistics.median(r[key] for r in rows)
                                 for key in ("totalSeconds", "executionSeconds", "candidatesPerSecond")}
                if any(r.get("timingContract") != "baseline-inputs-to-completion-v1" for r in rows):
                    raise RuntimeError("Benchmark executable uses an incompatible timing boundary")
            item["timingContract"] = "baseline-inputs-to-completion-v1"
            item["speedup"] = item["native"]["executionSeconds"] / item["blocks"]["executionSeconds"]
            item["endToEndSpeedup"] = item["native"]["totalSeconds"] / item["blocks"]["totalSeconds"]
            pairs = {}
            for row in records:
                if row["horizonMs"] == horizon and row["backend"] == backend:
                    pairs.setdefault(row["repeat"], {})[row["path"]] = row
            item["pairedSpeedup"] = statistics.median(
                pair["native"]["executionSeconds"] / pair["blocks"]["executionSeconds"]
                for pair in pairs.values())
            item["pairedEndToEndSpeedup"] = statistics.median(
                pair["native"]["totalSeconds"] / pair["blocks"]["totalSeconds"]
                for pair in pairs.values())
            summary.append(item)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
