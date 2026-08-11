#!/usr/bin/env python3
"""Cross-platform protocol and live Tracy collector acceptance test."""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

MAGIC = b"TCOL"
VERSION = 1


def text(value: str) -> bytes:
    encoded = value.encode("utf-8")
    assert len(encoded) <= 4096
    return struct.pack("!H", len(encoded)) + encoded


def u16(value: int) -> bytes:
    return struct.pack("!H", value)


def u32(value: int) -> bytes:
    return struct.pack("!I", value)


def take_text(data: bytes, offset: int):
    length = struct.unpack_from("!H", data, offset)[0]
    offset += 2
    return data[offset : offset + length].decode("utf-8"), offset + length


def recv_exact(sock: socket.socket, length: int) -> bytes:
    result = b""
    while len(result) < length:
        part = sock.recv(length - len(result))
        if not part:
            raise RuntimeError("truncated protocol response")
        result += part
    return result


class Client:
    def __init__(self, endpoint: str, token: str):
        host, port = endpoint.rsplit(":", 1)
        self.address = (host, int(port))
        self.token = token

    def request(self, kind: int, payload: bytes = b"", *, token=None, version=VERSION,
                fragmented=False, extra=b""):
        auth = self.token if token is None else token
        body = text(auth) + payload
        frame = MAGIC + struct.pack("!HHI", version, kind, len(body)) + body
        with socket.create_connection(self.address, timeout=10) as sock:
            sock.settimeout(30)
            if fragmented:
                for byte in frame:
                    sock.sendall(bytes([byte]))
            else:
                sock.sendall(frame + extra)
            header = recv_exact(sock, 12)
            if header[:4] != MAGIC:
                raise RuntimeError("bad response magic")
            response_version, response_kind, size = struct.unpack("!HHI", header[4:])
            if response_version != VERSION or response_kind != (kind | 0x8000):
                raise RuntimeError("bad response header")
            data = recv_exact(sock, size)
        status = struct.unpack_from("!H", data, 0)[0]
        message, offset = take_text(data, 2)
        return status, message, data[offset:]

    def ok(self, kind: int, payload: bytes = b"", **kwargs) -> bytes:
        status, message, response = self.request(kind, payload, **kwargs)
        if status != 0:
            raise RuntimeError(f"protocol request {kind} failed ({status}): {message}")
        return response

    def malformed_truncated(self):
        body = text(self.token)
        frame = MAGIC + struct.pack("!HHI", VERSION, 7, len(body) + 1) + body
        with socket.create_connection(self.address, timeout=10) as sock:
            sock.settimeout(10)
            sock.sendall(frame)
            sock.shutdown(socket.SHUT_WR)
            header = recv_exact(sock, 12)
            size = struct.unpack("!I", header[8:12])[0]
            data = recv_exact(sock, size)
        status = struct.unpack_from("!H", data, 0)[0]
        return status

    def acquire(self, run_id: str) -> str:
        data = self.ok(1, text(run_id), fragmented=True)
        return take_text(data, 0)[0]

    def heartbeat(self, lease: str):
        self.ok(2, text(lease))

    def register(self, run_id: str, attempt: str, test_name: str, retry=0):
        data = self.ok(3, text(run_id) + text(attempt) + text("fixture-bin") +
                       text(test_name) + u32(retry) + text("stress=none"))
        session, offset = take_text(data, 0)
        port = struct.unpack_from("!H", data, offset)[0]
        return session, port

    def status(self, session: str):
        data = self.ok(4, text(session))
        fields = []
        offset = 0
        for _ in range(4):
            value, offset = take_text(data, offset)
            fields.append(value)
        return dict(zip(("state", "handshake", "error", "output"), fields))

    def decide(self, session: str, decision: int, source="integration"):
        data = self.ok(5, text(session) + u16(decision) + text(source))
        return take_text(data, 0)[0]

    def finalize(self, lease: str):
        data = self.ok(6, text(lease))
        return struct.unpack_from("!I", data, 0)[0]


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def free_range(count=24):
    for _ in range(100):
        first = free_port()
        if first + count >= 65535:
            continue
        sockets = []
        try:
            for port in range(first, first + count):
                sock = socket.socket()
                sock.bind(("127.0.0.1", port))
                sockets.append(sock)
            return first, first + count - 1
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError("could not find a free contiguous port range")


def start_daemon(collector: Path, work: Path, *, owner_timeout=60000,
                 connect_timeout=5000):
    output = work / "output"
    ready = work / "ready.json"
    output.mkdir(parents=True)
    control = free_port()
    first, last = free_range()
    process = subprocess.Popen(
        [str(collector), "--output-root", str(output), "--ready-file", str(ready),
         "--control-port", str(control), "--data-port-first", str(first),
         "--data-port-last", str(last), "--owner-timeout-ms", str(owner_timeout),
         "--connect-timeout-ms", str(connect_timeout), "--finalize-timeout-ms", "15000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and not ready.exists():
        if process.poll() is not None:
            raise RuntimeError(f"daemon startup failed: {process.stderr.read()}")
        time.sleep(0.02)
    if not ready.exists():
        process.kill()
        raise RuntimeError("daemon did not publish ready descriptor")
    descriptor = json.loads(ready.read_text(encoding="utf-8"))
    token = Path(descriptor["secret_file"]).read_text(encoding="utf-8").strip()
    return process, Client(descriptor["endpoint"], token), descriptor, output


def fixture_process(executable: Path, port: int, marker=None):
    env = os.environ.copy()
    env["TRACY_PORT"] = str(port)
    command = [str(executable)]
    if marker is not None:
        command.append(marker)
    return subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def wait_state(client: Client, session: str, wanted, timeout=15):
    deadline = time.monotonic() + timeout
    latest = None
    while time.monotonic() < deadline:
        latest = client.status(session)
        if latest["state"] in wanted:
            return latest
        time.sleep(0.02)
    raise RuntimeError(f"session {session} did not reach {wanted}; latest={latest}")


def wait_fixture(process: subprocess.Popen, name: str, timeout=15):
    try:
        out, err = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        out, err = process.communicate()
        raise RuntimeError(f"{name} timed out: stdout={out!r} stderr={err!r}")
    return process.returncode, out, err


def check_trace(query: Path, trace: Path, expected: str):
    subprocess.run([str(query), "check", str(trace)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    subprocess.run([str(query), "range", str(trace)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    subprocess.run([str(query), "info", str(trace)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    subprocess.run(
        [str(query), "query", "--kind", "message,cpu-zone", "--count", str(trace)],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    result = subprocess.run(
        [str(query), "query", "--kind", "message", "--filter",
         f"message.text=^{re.escape(expected)}$", "--count", str(trace)], check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    counts = [json.loads(line)["count"] for line in result.stdout.splitlines() if line.strip()]
    if not counts or sum(counts) < 1:
        raise RuntimeError(f"trace {trace} does not contain {expected!r}: {result.stdout}")


def primary_run(args, work: Path):
    process, client, ready, output = start_daemon(args.collector, work)
    run_id = ready["run_id"]
    try:
        status, _, _ = client.request(7, token="wrong")
        assert status == 2, status
        status, _, _ = client.request(7, version=99)
        assert status == 8, status
        status, _, _ = client.request(99)
        assert status == 8, status
        assert client.malformed_truncated() == 1
        status, _, _ = client.request(1, b"\x00\x01\xff")
        assert status == 1, status
        harmless = text(client.token)
        extra_frame = MAGIC + struct.pack("!HHI", VERSION, 7, len(harmless)) + harmless
        status, _, _ = client.request(7, extra=extra_frame)
        assert status == 1, status

        lease = client.acquire(run_id)
        status, _, _ = client.request(1, text(run_id))
        assert status == 9, status
        client.heartbeat(lease)

        registrations = {}
        attempts = [("save", "save event"), ("discard", "../../discard event")]
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = {name: pool.submit(client.register, run_id, name, test)
                       for name, test in attempts}
            for name, future in futures.items():
                registrations[name] = future.result()
        assert registrations["save"][1] != registrations["discard"][1]

        status, _, _ = client.request(
            3, text(run_id) + text("save") + text("fixture-bin") + text("dupe") +
            u32(0) + text(""))
        assert status == 3, status

        saved_process = fixture_process(args.live, registrations["save"][1], "save-marker")
        discarded_process = fixture_process(args.live, registrations["discard"][1], "discard-marker")
        wait_state(client, registrations["save"][0], {"capturing", "awaiting-decision"})
        wait_state(client, registrations["discard"][0], {"capturing", "awaiting-decision"})
        saved_result = wait_fixture(saved_process, "save fixture")
        discarded_result = wait_fixture(discarded_process, "discard fixture")
        if saved_result[0] != 0:
            raise RuntimeError(f"save fixture failed: {saved_result}")
        if discarded_result[0] != 0:
            raise RuntimeError(f"discard fixture failed: {discarded_result}")
        wait_state(client, registrations["save"][0], {"awaiting-decision"})
        wait_state(client, registrations["discard"][0], {"awaiting-decision"})
        assert client.decide(registrations["save"][0], 1) in {"saved", "save-pending"}
        assert client.decide(registrations["discard"][0], 2) == "discarded"
        status, _, _ = client.request(
            5, text(registrations["discard"][0]) + u16(1) + text("conflict"))
        assert status == 3, status

        crash_session, crash_port = client.register(run_id, "crash", "crash event")
        crash_process = fixture_process(args.crash, crash_port)
        wait_state(client, crash_session, {"capturing", "awaiting-decision"})
        crash_code, _, _ = wait_fixture(crash_process, "crash fixture")
        assert crash_code != 0
        wait_state(client, crash_session, {"awaiting-decision"})

        killed_session, killed_port = client.register(run_id, "killed", "killed event")
        killed_process = fixture_process(args.long, killed_port)
        wait_state(client, killed_session, {"capturing"})
        time.sleep(0.2)  # allow the deterministic first message to reach the Worker
        killed_process.terminate()
        wait_fixture(killed_process, "killed fixture")
        wait_state(client, killed_session, {"awaiting-decision"})

        timeout_session, _ = client.register(run_id, "timeout", "connection timeout")
        wait_state(client, timeout_session, {"failed-to-connect"})

        failure_session, failure_port = client.register(run_id, "writer", "save failure")
        failure_process = fixture_process(args.live, failure_port, "writer-marker")
        wait_state(client, failure_session, {"capturing", "awaiting-decision"})
        writer_result = wait_fixture(failure_process, "writer fixture")
        if writer_result[0] != 0:
            raise RuntimeError(f"writer fixture failed: {writer_result}")
        wait_state(client, failure_session, {"awaiting-decision"})
        (output / f"{failure_session}-save-failure.tracy.partial").mkdir()
        assert client.decide(failure_session, 1) == "save-failed"

        client.heartbeat(lease)
        client.finalize(lease)
        return_code = process.wait(timeout=30)
        assert return_code == 4, return_code  # timeout + injected writer failure are explicit
        stderr = process.stderr.read()
        assert "ready on 127.0.0.1" in stderr

        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        by_attempt = {item["attempt_id"]: item for item in manifest["sessions"]}
        assert by_attempt["save"]["state"] == "saved"
        assert by_attempt["discard"]["state"] == "discarded"
        assert by_attempt["discard"]["output_name"] == ""
        assert by_attempt["crash"]["state"] == "saved"
        assert by_attempt["killed"]["state"] == "saved"
        assert by_attempt["timeout"]["state"] == "save-failed"
        assert by_attempt["writer"]["state"] == "save-failed"
        assert not list(output.glob("*.partial")), "partial artifacts leaked"

        traces = list(output.glob("*.tracy"))
        assert len(traces) == 3, traces
        check_trace(args.query, output / by_attempt["save"]["output_name"], "save-marker")
        check_trace(args.query, output / by_attempt["crash"]["output_name"],
                    "fixture crash incoming")
        check_trace(args.query, output / by_attempt["killed"]["output_name"],
                    "fixture long connected")
        all_messages = "\n".join(
            subprocess.run([str(args.query), "query", "--kind", "message", str(trace)],
                           check=True, stdout=subprocess.PIPE, text=True).stdout
            for trace in traces
        )
        assert "discard-marker" not in all_messages
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        elif process.returncode not in (0, 4):
            print(process.stderr.read(), file=sys.stderr)


def owner_loss_run(args, work: Path):
    process, client, ready, output = start_daemon(
        args.collector, work, owner_timeout=700, connect_timeout=1500)
    try:
        lease = client.acquire(ready["run_id"])
        assert lease
        session, port = client.register(ready["run_id"], "owner-loss", "owner loss")
        fixture = fixture_process(args.long, port)
        wait_state(client, session, {"capturing"})
        # Let the owner lease expire while the client remains connected. The
        # daemon must wait for client disconnect rather than discarding it.
        assert process.wait(timeout=20) == 0
        assert wait_fixture(fixture, "owner-loss fixture")[0] == 0
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        record = manifest["sessions"][0]
        assert record["state"] == "saved"
        assert record["decision_source"] == "owner-loss"
        check_trace(args.query, output / record["output_name"], "fixture long connected")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def signal_shutdown_run(args, work: Path):
    process, client, ready, output = start_daemon(
        args.collector, work, owner_timeout=10000, connect_timeout=1500)
    try:
        client.acquire(ready["run_id"])
        session, port = client.register(ready["run_id"], "signal", "signal shutdown")
        fixture = fixture_process(args.live, port, "signal-marker")
        wait_state(client, session, {"capturing"})
        time.sleep(0.2)
        process.terminate()
        fixture_result = wait_fixture(fixture, "signal fixture")
        if fixture_result[0] != 0:
            raise RuntimeError(f"signal fixture failed: {fixture_result}")
        assert process.wait(timeout=20) == 0
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        record = manifest["sessions"][0]
        assert record["state"] == "saved"
        assert record["decision_source"] == "signal"
        check_trace(args.query, output / record["output_name"], "signal-marker")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--collector", type=Path, required=True)
    parser.add_argument("--query", type=Path, required=True)
    parser.add_argument("--live", type=Path, required=True)
    parser.add_argument("--crash", type=Path, required=True)
    parser.add_argument("--long", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    primary_run(args, args.work / "primary")
    owner_loss_run(args, args.work / "owner-loss")
    signal_shutdown_run(args, args.work / "signal")
    print("collector protocol/live integration passed")


if __name__ == "__main__":
    main()
