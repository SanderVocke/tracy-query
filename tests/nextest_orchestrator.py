#!/usr/bin/env python3
"""Reference nextest/JUnit adapter for the collector fixture.

The only child supervising test processes is cargo-nextest: this script launches
cargo-nextest directly, never wraps individual test invocations.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from collector_integration import start_daemon, check_trace  # noqa: E402


def local_tag(element):
    return element.tag.rsplit("}", 1)[-1]


def junit_outcomes(junit: Path, test_names):
    """Return success/failure/flaky; missing and ambiguous entries stay unknown."""
    outcomes = {name: "unknown" for name in test_names}
    matches = {name: [] for name in test_names}
    root = ET.parse(junit).getroot()
    for case in root.iter():
        if local_tag(case) != "testcase":
            continue
        name = case.attrib.get("name", "")
        classname = case.attrib.get("classname", "")
        combined = f"{classname}::{name}" if classname else name
        candidates = [test for test in test_names
                      if name == test or combined.endswith(test) or test.endswith(name)]
        if len(candidates) != 1:
            continue
        child_tags = [local_tag(child) for child in case]
        if any(tag in {"failure", "error"} for tag in child_tags):
            outcome = "failure"
        elif any(tag in {"flakyFailure", "flakyError", "rerunFailure", "rerunError"}
                 for tag in child_tags):
            outcome = "flaky"
        else:
            outcome = "success"
        matches[candidates[0]].append(outcome)
    for name, values in matches.items():
        if len(values) == 1:
            outcomes[name] = values[0]
    return outcomes


def list_sessions(client):
    data = client.ok(7)
    count = int.from_bytes(data[:4], "big")
    offset = 4
    sessions = []
    from collector_integration import take_text
    for _ in range(count):
        fields = []
        for _ in range(5):
            value, offset = take_text(data, offset)
            fields.append(value)
        sessions.append(dict(zip(("session_id", "attempt_id", "state", "output", "error"), fields)))
    return sessions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--collector", type=Path, required=True)
    parser.add_argument("--query", type=Path, required=True)
    parser.add_argument("--nextest", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)

    daemon, client, ready, output = start_daemon(
        args.collector, args.work / "daemon", owner_timeout=60000,
        connect_timeout=10000)
    try:
        lease = client.acquire(ready["run_id"])
        token = Path(ready["secret_file"]).read_text(encoding="utf-8").strip()
        env = os.environ.copy()
        env.update({
            "TRACY_COLLECTOR_ENDPOINT": ready["endpoint"],
            "TRACY_COLLECTOR_TOKEN": token,
            "TRACY_COLLECTOR_RUN_ID": ready["run_id"],
            "CARGO_TERM_COLOR": "never",
        })
        command = [str(args.nextest), "nextest", "run", "--manifest-path",
                   str(args.fixture / "Cargo.toml"), "--profile", "collector",
                   "--no-fail-fast"]
        # No per-test command appears here: nextest directly launches each test binary.
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, timeout=120)
        if result.returncode == 0:
            raise RuntimeError("the expected-failure nextest fixture unexpectedly passed")
        if result.returncode not in (100, 101):
            raise RuntimeError(
                f"nextest failed unexpectedly ({result.returncode})\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")

        # nextest resolves the configured relative JUnit path below target/nextest/profile.
        candidates = list(args.fixture.glob("target/nextest/**/junit.xml"))
        if len(candidates) != 1:
            raise RuntimeError(f"expected one JUnit report, found {candidates}")
        junit = candidates[0]
        shutil.copy2(junit, args.work / "junit.xml")

        # Wait until abrupt/timeout connections have closed and refresh the manifest.
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            infos = list_sessions(client)
            if infos and all(item["state"] in {"awaiting-decision", "failed-to-connect"}
                             for item in infos):
                break
            time.sleep(0.05)
        else:
            raise RuntimeError(f"nextest sessions did not disconnect: {infos}")

        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        records = manifest["sessions"]
        by_test = {}
        for record in records:
            by_test.setdefault(record["test_name"], []).append(record)
        outcomes = junit_outcomes(junit, set(by_test))
        # Deliberately remove one authoritative result to exercise incomplete
        # reconciliation. The corresponding failing capture must still save.
        incomplete_name = next(
            name for name in by_test if name.endswith("tests::assertion_failure"))
        outcomes[incomplete_name] = "unknown"

        # Conservative reconciliation: only the latest attempt of an unambiguous
        # success/flaky result is discardable. Everything else is retained.
        for test_name, attempts in by_test.items():
            attempts.sort(key=lambda item: item["retry"])
            outcome = outcomes.get(test_name, "unknown")
            for index, record in enumerate(attempts):
                latest = index == len(attempts) - 1
                decision = 2 if latest and outcome in {"success", "flaky"} else 1
                source = f"junit:{outcome}"
                client.decide(record["session_id"], decision, source)

        client.finalize(lease)
        code = daemon.wait(timeout=60)
        if code != 0:
            raise RuntimeError(f"collector finalization failed ({code}): {daemon.stderr.read()}")

        final_manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        records = final_manifest["sessions"]
        tests = {record["test_name"] for record in records}
        expected_suffixes = {
            "tests::passes", "tests::assertion_failure", "tests::aborts",
            "tests::times_out", "tests::retry_then_passes",
        }
        for suffix in expected_suffixes:
            if not any(name.endswith(suffix) for name in tests):
                raise RuntimeError(f"missing registration for {suffix}: {tests}")

        pass_records = [record for record in records
                        if record["test_name"].endswith("tests::passes")]
        retry_records = sorted(
            [record for record in records
             if record["test_name"].endswith("tests::retry_then_passes")],
            key=lambda item: item["retry"])
        assert len(pass_records) == 1 and pass_records[0]["state"] == "discarded"
        assert len(retry_records) == 2
        assert retry_records[0]["state"] == "saved"
        assert retry_records[1]["state"] == "discarded"

        failure_records = [record for record in records
                           if any(record["test_name"].endswith(suffix) for suffix in
                                  ("tests::assertion_failure", "tests::aborts",
                                   "tests::times_out"))]
        assert failure_records and all(record["state"] == "saved"
                                       for record in failure_records)
        assert any(record["decision_source"] == "junit:unknown"
                   for record in failure_records)
        assert all(record["output_name"] == "" for record in pass_records + retry_records[1:])
        assert not list(output.glob("*.partial"))
        traces = list(output.glob("*.tracy"))
        saved_records = [record for record in records if record["state"] == "saved"]
        assert len(traces) == len(saved_records)
        for record in saved_records:
            check_trace(args.query, output / record["output_name"],
                        f"nextest:{record['test_name']}:attempt:{record['retry'] + 1}:id:{record['attempt_id']}")

        # The copied report, manifest, and traces are the exact CI publication set.
        shutil.copy2(output / "manifest.json", args.work / "manifest.json")
        print(f"nextest contract passed with {len(records)} attempts and {len(traces)} saved traces")
    finally:
        if daemon.poll() is None:
            daemon.kill()
            daemon.wait()


if __name__ == "__main__":
    main()
