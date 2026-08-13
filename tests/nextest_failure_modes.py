#!/usr/bin/env python3
"""Validate setup and finalizer failure diagnostics without misleading output."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def execute(command, env, expected):
    result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=180)
    if result.returncode not in expected:
        raise RuntimeError(f"unexpected status {result.returncode}: {command}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nextest", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--target-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.work.exists(): shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    env = os.environ.copy()
    env["CARGO_TARGET_DIR"] = str(args.target_dir.resolve())
    env["TRACY_NEXTEST_CAPTURE"] = "failure"
    env["CARGO_TERM_COLOR"] = "never"

    missing = args.work / "missing"
    env["TRACY_NEXTEST_OUTPUT_DIR"] = str(missing.resolve())
    setup = execute([str(args.nextest), "nextest", "run", "--manifest-path", str(args.manifest), "--profile", "tracy-in-process", "-E", "test(passes_unit)"], env, {100, 101})
    if "capture output directory does not exist" not in setup.stdout + setup.stderr:
        raise RuntimeError("missing-directory setup failure was not actionable")

    finalizer = args.work / "finalizer"
    finalizer.mkdir()
    env["TRACY_NEXTEST_OUTPUT_DIR"] = str(finalizer.resolve())
    failure = execute([str(args.nextest), "nextest", "run", "--manifest-path", str(args.manifest), "--profile", "tracy-in-process", "--run-ignored", "ignored-only", "-E", "test(finalizer_failure_during_panic)"], env, {100, 101})
    text = failure.stdout + failure.stderr
    if "capture finalization failed while handling string panic" not in text or "exit code 70" not in text:
        raise RuntimeError(f"finalizer failure did not report one controlled path:\n{text}")
    if list(args.work.rglob("*.tracy")) or list(args.work.rglob("*.partial")):
        raise RuntimeError("failure injection published a misleading capture")
    print("in-process nextest failure-mode contract passed")


if __name__ == "__main__":
    try: main()
    except Exception as error:
        print(f"nextest_failure_modes.py: {error}", file=sys.stderr)
        raise SystemExit(1)
