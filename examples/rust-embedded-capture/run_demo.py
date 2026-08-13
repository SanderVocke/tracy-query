#!/usr/bin/env python3
"""Run and semantically validate one embedded Rust capture mode."""

import argparse
import os
from pathlib import Path
import socket
import subprocess
import sys


def run(command, *, env=None, expected=0):
    result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=180)
    if result.returncode != expected:
        raise RuntimeError(
            f"command returned {result.returncode}, expected {expected}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def occupy_tracy_ports():
    listeners = []
    for port in range(8086, 8106):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen(1)
        listeners.append(listener)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 8086))
    listeners.append(udp)
    return listeners


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("normal", "unwind-panic"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--query", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("Cargo.toml"))
    parser.add_argument("--target-dir", type=Path)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        args.output.unlink()
    for partial in args.output.parent.glob(args.output.name + ".*.partial"):
        partial.unlink()

    environment = os.environ.copy()
    if args.target_dir:
        environment["CARGO_TARGET_DIR"] = str(args.target_dir.resolve())

    tree = run([
        "cargo", "tree", "--manifest-path", str(args.manifest),
        "-i", "tracy-client-sys",
    ], env=environment).stdout
    if tree.count("tracy-client-sys v0.28.0") != 1 or "rust/tracy-client-sys" not in tree.replace("\\", "/"):
        raise RuntimeError(f"Cargo did not resolve exactly one patched tracy-client-sys:\n{tree}")
    duplicates = run([
        "cargo", "tree", "--manifest-path", str(args.manifest), "-d",
    ], env=environment).stdout.strip()
    if duplicates:
        raise RuntimeError(f"duplicate Cargo packages detected:\n{duplicates}")

    occupied = occupy_tracy_ports()
    try:
        expected = 0 if args.mode == "normal" else 101
        result = run([
            "cargo", "run", "--quiet", "--manifest-path", str(args.manifest), "--",
            args.mode, str(args.output),
        ], env=environment, expected=expected)
    finally:
        for listener in occupied:
            listener.close()

    if args.mode == "unwind-panic" and "resuming original panic" not in result.stderr:
        raise RuntimeError(f"panic mode did not reach finalization/resume boundary:\n{result.stderr}")
    if not args.output.is_file() or args.output.stat().st_size == 0:
        raise RuntimeError("example did not publish a non-empty capture")
    partials = list(args.output.parent.glob(args.output.name + ".*.partial"))
    if partials:
        raise RuntimeError(f"example left partial captures: {partials}")

    run([str(args.query), "check", str(args.output)])
    records = run([
        str(args.query), "query", "--kind", "cpu-zone,message", str(args.output)
    ]).stdout
    required = [
        "direct.tracy-client.zone",
        "direct.tracy-client.message",
        "tracing-tracy.child",
        "tracing-tracy.event",
    ]
    if args.mode == "unwind-panic":
        required.append("rust unwind panic caught")
    missing = [marker for marker in required if marker not in records]
    if missing:
        raise RuntimeError(f"capture is missing semantic markers {missing}:\n{records}")

    run([str(args.query), "range", str(args.output)])
    run([str(args.query), "info", str(args.output)])
    print(f"validated {args.mode} capture: {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"run_demo.py: {error}", file=sys.stderr)
        raise SystemExit(1)
