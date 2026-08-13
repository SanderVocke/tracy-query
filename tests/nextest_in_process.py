#!/usr/bin/env python3
"""Run and validate in-process per-nextest-attempt Tracy retention."""

import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys


def run(command, *, env, expected=None, timeout=180):
    result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=timeout)
    if expected is not None and result.returncode not in expected:
        raise RuntimeError(f"command returned {result.returncode}, expected {expected}: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def occupy_ports():
    sockets = []
    for port in range(8086, 8106):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen(1)
        sockets.append(listener)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 8086))
    sockets.append(udp)
    return sockets


def validate_trace(query, trace, required):
    for command in ([query, "check", trace], [query, "range", trace], [query, "info", trace]):
        run([str(item) for item in command], env=os.environ.copy(), expected={0})
    result = run([str(query), "query", "--kind", "cpu-zone,message", str(trace)], env=os.environ.copy(), expected={0})
    missing = [marker for marker in required if marker not in result.stdout]
    if missing:
        raise RuntimeError(f"{trace} misses {missing}:\n{result.stdout}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nextest", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--query", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--target-dir", type=Path, required=True)
    parser.add_argument("--policy", choices=("off", "failure", "always", "all"), default="all")
    args = parser.parse_args()
    if args.work.exists(): shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    environment = os.environ.copy()
    environment["CARGO_TARGET_DIR"] = str(args.target_dir.resolve())
    environment["TRACY_NEXTEST_OUTPUT_DIR"] = str(args.work.resolve())
    environment["CARGO_TERM_COLOR"] = "never"

    tree = run(["cargo", "tree", "--manifest-path", str(args.manifest), "-i", "tracy-client-sys"], env=environment, expected={0}).stdout
    if tree.count("tracy-client-sys v0.28.0") != 1 or "rust/tracy-client-sys" not in tree.replace("\\", "/"):
        raise RuntimeError(f"expected one patched sys package:\n{tree}")
    duplicates = run(["cargo", "tree", "--manifest-path", str(args.manifest), "-d"], env=environment, expected={0}).stdout.strip()
    if duplicates: raise RuntimeError(f"duplicate packages:\n{duplicates}")

    # Ordinary cargo test and both cargo/nextest discovery remain inert even if
    # a policy is present. A partial nextest identity is also a strict no-op.
    environment["TRACY_NEXTEST_CAPTURE"] = "failure"
    inactive = args.work / "inactive"
    inactive.mkdir()
    environment["TRACY_NEXTEST_OUTPUT_DIR"] = str(inactive.resolve())
    run(["cargo", "test", "--manifest-path", str(args.manifest), "tests::passes_unit", "--", "--exact"], env=environment, expected={0})
    run(["cargo", "test", "--manifest-path", str(args.manifest), "--", "--list"], env=environment, expected={0})
    run([str(args.nextest), "nextest", "list", "--manifest-path", str(args.manifest)], env=environment, expected={0})
    incomplete = environment.copy()
    incomplete["NEXTEST_ATTEMPT_ID"] = "incomplete-identity"
    for name in ("NEXTEST_TEST_NAME", "NEXTEST_BINARY_ID", "NEXTEST_ATTEMPT"):
        incomplete.pop(name, None)
    run(["cargo", "test", "--manifest-path", str(args.manifest), "tests::passes_unit", "--", "--exact"], env=incomplete, expected={0})
    if list(inactive.iterdir()): raise RuntimeError("inactive cargo/nextest operation created capture artifacts")

    invalid = environment.copy()
    invalid["TRACY_NEXTEST_CAPTURE"] = "sometimes"
    invalid_result = run([
        str(args.nextest), "nextest", "run", "--manifest-path", str(args.manifest),
        "--profile", "tracy-in-process", "-E", "test(passes_unit)"
    ], env=invalid, expected={100, 101})
    if "invalid TRACY_NEXTEST_CAPTURE policy" not in invalid_result.stdout + invalid_result.stderr:
        raise RuntimeError("invalid policy did not fail with an actionable pre-body diagnostic")
    if list(inactive.iterdir()): raise RuntimeError("invalid policy created capture artifacts")

    modes = {"off": 0, "failure": 3, "always": 6}
    if args.policy != "all":
        modes = {args.policy: modes[args.policy]}
    occupied = occupy_ports()
    try:
        for mode, expected_count in modes.items():
            output = args.work / mode
            output.mkdir()
            environment["TRACY_NEXTEST_CAPTURE"] = mode
            environment["TRACY_NEXTEST_OUTPUT_DIR"] = str(output.resolve())
            result = run([str(args.nextest), "nextest", "run", "--manifest-path", str(args.manifest), "--profile", "tracy-in-process", "--no-fail-fast"], env=environment, expected={100, 101})
            if mode != "off":
                if "intentional nextest in-process panic" not in result.stdout + result.stderr:
                    raise RuntimeError("nextest did not preserve original panic diagnostic")
                if "FixtureError" not in result.stdout + result.stderr:
                    raise RuntimeError("nextest did not preserve Result::Err diagnostic")
            traces = sorted(output.glob("*.tracy"))
            if len(traces) != expected_count:
                raise RuntimeError(f"{mode} expected {expected_count} traces, got {traces}")
            if list(output.glob("*.partial")) or list(output.glob("*.tracy.*.partial")):
                raise RuntimeError(f"{mode} left partial files")
            for trace in traces:
                required = ["nextest-in-process:", "nextest-in-process.direct-zone", "nextest-in-process.tracing-span"]
                if mode == "failure":
                    required.append("nextest-in-process:panic-caught" if "panic_failure" in trace.name or "retry_then_passes" in trace.name else "nextest-in-process:result-error")
                validate_trace(args.query, trace, required)
    finally:
        for item in occupied: item.close()
    print("in-process nextest off/failure/always contract passed")


if __name__ == "__main__":
    try: main()
    except Exception as error:
        print(f"nextest_in_process.py: {error}", file=sys.stderr)
        raise SystemExit(1)
