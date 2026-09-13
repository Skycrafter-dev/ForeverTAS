"""Cold-process cursor off/on measurements, including short source programs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import threading
import time


def timed_process(command, *, env, stdout, timeout=600):
    # wait(timeout=...) polls on POSIX, quantizing complete-process timings.
    # Keep the timer independent so the measured wait blocks until actual exit.
    started = time.perf_counter()
    with subprocess.Popen(command, env=env, stdout=stdout, stderr=subprocess.STDOUT) as process:
        expired = threading.Event()

        def expire():
            expired.set()
            process.kill()

        watchdog = threading.Timer(timeout, expire)
        watchdog.daemon = True
        try:
            watchdog.start()
            returncode = process.wait()
        except BaseException:
            process.kill()
            process.wait()
            raise
        finally:
            watchdog.cancel()
            if watchdog.ident is not None:
                watchdog.join()
    elapsed = time.perf_counter() - started
    if expired.is_set():
        raise subprocess.TimeoutExpired(command, timeout)
    if returncode:
        raise subprocess.CalledProcessError(returncode, command)
    return elapsed


def block(name, inputs=None, fields=None):
    return {"type": "ft_" + name.replace("/", "_").replace("-", "_"), "fields": fields or {},
            "inputs": {key: {"block": value} for key, value in (inputs or {}).items()}}


def workspace(ticks):
    step = block("simulation/step")
    step["next"] = {"block": block("data/set", {"value": block("simulation/time")}, {"name": "observed time"})}
    repeat = block("flow/repeat", {"count": block("values/number", fields={"value": ticks}), "body": step})
    publish = block("results/publish", {"score": block("simulation/car-speed")})
    repeat["next"] = {"block": publish}
    publish["next"] = {"block": block("results/count")}
    return {"blocks": {"languageVersion": 0, "blocks": [block("flow/when-start", {"body": repeat})]}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "packs", "replay", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--ticks", type=int, nargs="+", default=[1, 10, 100, 600])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--default-policy", action="store_true",
                        help="leave the cursor policy unset for the on variant, verifying the runtime default")
    parser.add_argument("--baseline-executable", type=Path,
                        help="also compare a previous executable with its cursor enabled")
    args = parser.parse_args()
    if args.repeats < 1 or not args.ticks or any(ticks < 1 or ticks > 600 for ticks in args.ticks):
        parser.error("repeats must be positive and ticks must be between 1 and 600")
    if len(set(args.ticks)) != len(args.ticks):
        parser.error("ticks must not contain duplicates")
    binary = args.build.resolve() / "forevertas-block-program-benchmark"
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    executables = {"off": binary, "on": binary}
    if args.baseline_executable:
        executables["before"] = args.baseline_executable.resolve()
    hashes = {policy: hashlib.sha256(executable.read_bytes()).hexdigest()
              for policy, executable in executables.items()}
    environment = dict(os.environ)
    for key in ("FOREVERTAS_BENCH_INTERPRETER", "FOREVERTAS_CUDA_SOURCE_CURSOR", "FOREVERTAS_CUDA_VM_PROFILE"):
        environment.pop(key, None)
    if args.profile:
        environment["FOREVERTAS_CUDA_VM_PROFILE"] = "1"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    metadata = {"executable": str(binary), "sha256": digest, "ticks": args.ticks, "repeats": args.repeats,
                "environment": {key: value for key, value in environment.items()
                                if key.startswith(("FOREVERTAS_", "CUDA_"))},
                "variantEnvironment": {"off": {"FOREVERTAS_CUDA_SOURCE_CURSOR": "0"},
                                       "on": {} if args.default_policy else {"FOREVERTAS_CUDA_SOURCE_CURSOR": "1"}},
                "timingContract": "baseline-inputs-to-completion-v1", "horizonMs": 6000,
                "processTimingContract": "subprocess-launch-to-exit-v1",
                "processWaitMethod": "blocking-wait-with-watchdog",
                "packs": str(args.packs.resolve()), "replay": str(args.replay.resolve())}
    metadata["executables"] = {policy: {"path": str(executable), "sha256": hashes[policy]}
                               for policy, executable in executables.items()}
    if args.baseline_executable:
        metadata["variantEnvironment"]["before"] = {"FOREVERTAS_CUDA_SOURCE_CURSOR": "1"}
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    records, summary = [], []
    for ticks in args.ticks:
        fixture = output / f"source-{ticks}.json"
        fixture.write_text(json.dumps(workspace(ticks)) + "\n")
        pairs = []
        for repeat in range(args.repeats):
            pair = {}
            order = ["off", "before", "on"] if args.baseline_executable else ["off", "on"]
            for policy in (order if repeat % 2 == 0 else reversed(order)):
                telemetry = subprocess.check_output(["nvidia-smi",
                    "--query-gpu=utilization.gpu,memory.used,temperature.gpu,power.draw", "--format=csv,noheader"],
                    text=True).strip()
                log_path = output / f"{ticks}-{repeat}-{policy}.log"
                with log_path.open("w") as log:
                    process_seconds = timed_process(
                        [str(executables[policy]), str(args.packs.resolve()), str(args.replay.resolve()),
                         str(fixture), "cuda", "2", "1", "6000"],
                        env={**environment, **metadata["variantEnvironment"][policy]}, stdout=log)
                record = json.loads(next(line for line in reversed(log_path.read_text().splitlines())
                                         if line.startswith("{")))
                record["processWallSeconds"] = process_seconds
                if (record["evaluationTimeMs"] != ticks * 10 or record["candidates"] != 1 or
                        record["timingContract"] != metadata["timingContract"] or record["executionSeconds"] <= 0):
                    raise RuntimeError(f"Invalid source execution/timing contract: {log_path}")
                modes = record.get("executionModes", [])
                resident = "CUDA resident physics with source block control" in modes
                if resident != (policy != "off") or any("unavailable" in mode for mode in modes):
                    raise RuntimeError(f"Wrong cursor mode or resident fallback: {log_path}")
                pair[policy] = record
                records.append({"ticks": ticks, "repeat": repeat, "policy": policy, "gpuBefore": telemetry, **record})
                (output / "runs.json").write_text(json.dumps(records, indent=2) + "\n")
            if any(pair["off"][key] != record[key] for record in pair.values()
                   for key in ("score", "evaluationTimeMs", "inputsSha256")):
                raise RuntimeError(f"Cursor changed source results at {ticks} ticks, repeat {repeat}")
            pairs.append(pair)
        ratios = [pair["off"]["executionSeconds"] / pair["on"]["executionSeconds"] for pair in pairs]
        result = {"ticks": ticks, "offOverOn": statistics.median(ratios), "pairedRatios": ratios,
                  "seconds": {policy: statistics.median(pair[policy]["executionSeconds"] for pair in pairs)
                              for policy in executables},
                  "processWallSeconds": {policy: statistics.median(pair[policy]["processWallSeconds"] for pair in pairs)
                                         for policy in executables},
                  "processWallOffOverOn": statistics.median(pair["off"]["processWallSeconds"] /
                                                            pair["on"]["processWallSeconds"] for pair in pairs)}
        if args.baseline_executable:
            ratios = [pair["before"]["executionSeconds"] / pair["on"]["executionSeconds"] for pair in pairs]
            result.update(beforeOverOn=statistics.median(ratios), baselinePairedRatios=ratios)
            result["processWallBeforeOverOn"] = statistics.median(pair["before"]["processWallSeconds"] /
                                                                  pair["on"]["processWallSeconds"] for pair in pairs)
        summary.append(result)
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(result), flush=True)
    for policy, executable in executables.items():
        if hashlib.sha256(executable.read_bytes()).hexdigest() != hashes[policy]:
            raise RuntimeError(f"Benchmark executable changed during measurements: {policy}")


if __name__ == "__main__":
    main()
