#!/usr/bin/env python3
"""Compare the maintained model-ingest fixture with a PR's integration base."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile


FIXTURE = Path("tests/history_row_record_codec/retained_history_benchmark.cpp")
TARGET = "vnm_terminal_retained_history_benchmark"
MODEL_PATHS = [
    "src/terminal_screen_model.cpp",
    "include/vnm_terminal/internal/terminal_screen_model.h",
    str(FIXTURE),
]


def run(command, cwd, capture=False):
    completed = subprocess.run(command, cwd=cwd, text=True, capture_output=capture)
    if completed.returncode:
        if capture:
            print(completed.stdout, end="")
            print(completed.stderr, end="")
        completed.check_returncode()
    return completed.stdout if capture else None


def read_cache(path):
    entries = {}
    for line in path.read_text().splitlines():
        if line.startswith(("#", "//")) or ":" not in line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        entries[key.split(":", 1)[0]] = value
    return entries


def observe(executable, cwd):
    output = run([str(executable), "--active-grid-ingest"], cwd, capture=True)
    observations = [json.loads(line) for line in output.splitlines() if line.strip()]
    if len(observations) != 4 or len({item["workload"] for item in observations}) != 4:
        raise RuntimeError("model-ingest benchmark did not report all four workloads")
    return {item["workload"]: item for item in observations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    build = args.build.resolve()
    baseline_ref = run(["git", "rev-parse", "--verify", args.baseline + "^{commit}"], source, True).strip()
    candidate_ref = run(["git", "rev-parse", "HEAD"], source, True).strip()
    changed = run(["git", "diff", "--name-only", baseline_ref, candidate_ref, "--", *MODEL_PATHS], source, True)
    if not changed.strip():
        print("No model or ingest-fixture change; active-grid comparison is unnecessary.")
        return

    cache = read_cache(build / "CMakeCache.txt")
    if cache["CMAKE_BUILD_TYPE"] != "Release" or cache["VNM_TERMINAL_ENABLE_PROFILING"] != "OFF":
        raise RuntimeError("comparison requires the ordinary Release build with profiling disabled")
    fixture_bytes = (source / FIXTURE).read_bytes()
    report = {
        "measurement_boundary": "native model.ingest calls; no snapshot, renderer, or paint timing",
        "allocation_boundary": "ordinary C++ new/new[] on the ingest thread; excludes aligned new and Qt malloc/realloc",
        "baseline_revision": baseline_ref,
        "candidate_revision": candidate_ref,
        "fixture_sha256": hashlib.sha256(fixture_bytes).hexdigest(),
        "build_type": "Release",
        "profiling": False,
        "trials": [],
    }

    with tempfile.TemporaryDirectory(prefix="vnm-lookup-", dir=os.environ.get("RUNNER_TEMP")) as temporary:
        root = Path(temporary)
        baseline = root / "source"
        baseline_build = root / "build"
        run(["git", "worktree", "add", "--detach", str(baseline), baseline_ref], source)
        original_fixture = (baseline / FIXTURE).read_bytes()
        try:
            (baseline / FIXTURE).write_bytes(fixture_bytes)
            command = [
                "cmake", "-S", str(baseline), "-B", str(baseline_build),
                "-C", str(build / "tests/package-dependencies.cmake"),
                "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
                "-DVNM_TERMINAL_BUILD_BENCHMARKS=OFF",
                "-DVNM_TERMINAL_ENABLE_PROFILING=OFF",
                "-DVNM_TERMINAL_ENABLE_TRANSCRIPT_CAPTURE_REPLAY=OFF",
                "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
            ]
            for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER"):
                if cache.get(key):
                    command.append(f"-D{key}={cache[key]}")
            # Both builds consume the already-resolved dependency source trees.
            # This is comparison provenance, not a new dependency revision policy.
            dependency_sources = {}
            for directory in sorted((build / "_deps").glob("*-src")):
                if directory.is_dir():
                    key = "FETCHCONTENT_SOURCE_DIR_" + directory.name[:-4].upper()
                    dependency_sources[key] = str(directory)
            dependency_sources.update({key: value for key, value in cache.items()
                                       if key.startswith("FETCHCONTENT_SOURCE_DIR_") and value})
            command.extend(f"-D{key}={value}" for key, value in dependency_sources.items())
            report["dependency_sources"] = dependency_sources
            run(command, source)
            run(["cmake", "--build", str(baseline_build), "--target", TARGET, "--parallel", "4"], source)

            executables = {"baseline": baseline_build / "tests" / TARGET,
                           "candidate": build / "tests" / TARGET}
            for trial in range(5):
                order = ["baseline", "candidate"] if trial % 2 == 0 else ["candidate", "baseline"]
                observations = {name: observe(executables[name], source) for name in order}
                for workload, before in observations["baseline"].items():
                    after = observations["candidate"][workload]
                    for field in ("rows", "columns", "warmup_ingests", "ingests", "chunk_bytes",
                                  "consumed_bytes", "history_rows", "checksum"):
                        if before[field] != after[field]:
                            raise RuntimeError(f"{workload}: baseline and candidate differ in {field}")
                report["trials"].append({"order": order, **observations})
        finally:
            (baseline / FIXTURE).write_bytes(original_fixture)
            run(["git", "worktree", "remove", str(baseline)], source)

    report["summary"] = {}
    for workload in report["trials"][0]["baseline"]:
        summary = {}
        for metric in ("ingest_ns", "cpp_new_calls", "cpp_new_bytes", "lookup_entries_visited"):
            summary[metric] = {}
            for name in ("baseline", "candidate"):
                values = [trial[name][workload][metric] for trial in report["trials"]]
                summary[metric][name] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        before = summary["ingest_ns"]["baseline"]["median"]
        after = summary["ingest_ns"]["candidate"]["median"]
        summary["median_ingest_change_percent"] = 100.0 * (after / before - 1.0)
        report["summary"][workload] = summary
        print(workload + ": " + json.dumps(summary, sort_keys=True))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print("Timing is descriptive; no timing threshold determines test success.")


if __name__ == "__main__":
    main()
