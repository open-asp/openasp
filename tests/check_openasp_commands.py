# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Integration checks for the three OpenASP command surfaces."""

from __future__ import annotations

import os
from pathlib import Path
import re
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
FIXTURES = ROOT / "fixtures/pages"


def run_checked(argv: list[str], input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    """Run a command surface and include captured streams in any failure."""
    result = subprocess.run(
        argv,
        input=input_text,
        capture_output=True,
        text=True,
        timeout=45,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(argv)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def read_daily_log(base_path: Path) -> str:
    """Resolve the logger's date suffix while asserting one rotation target."""
    matches = list(base_path.parent.glob(base_path.name + "-*"))
    assert len(matches) == 1, f"expected one daily log for {base_path}, got {matches}"
    return matches[0].read_text()


def assert_startup_banner(stderr: str) -> None:
    """Verify the common build and ownership information printed at startup."""
    lines = stderr.splitlines()
    assert len(lines) == 1, stderr
    assert re.fullmatch(
        r"OpenASP \d+\.\d+\.\d+ \S+ \S+ "
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z "
        r"https://openasp\.dev",
        lines[0],
    ), stderr


def fcgi_record(record_type: int, request_id: int, content: bytes) -> bytes:
    """Encode one unpadded FastCGI record."""
    return struct.pack("!BBHHBB", 1, record_type, request_id, len(content), 0, 0) + content


def fcgi_param(name: str, value: str) -> bytes:
    """Encode one FastCGI name/value pair used by the integration request."""
    name_bytes = name.encode()
    value_bytes = value.encode()

    def encoded_length(length: int) -> bytes:
        return bytes([length]) if length < 128 else struct.pack("!I", length | 0x80000000)

    return encoded_length(len(name_bytes)) + encoded_length(len(value_bytes)) + name_bytes + value_bytes


def recv_exact(client: socket.socket, size: int) -> bytes:
    """Read exactly one FastCGI field from the stream."""
    output = bytearray()
    while len(output) < size:
        chunk = client.recv(size - len(output))
        assert chunk, f"unexpected EOF after {len(output)} of {size} bytes"
        output.extend(chunk)
    return bytes(output)


def request_over_unix_socket(socket_path: Path) -> bytes:
    """Send a complete FastCGI request through a Unix domain socket."""
    params = {
        "SCRIPT_FILENAME": str(FIXTURES / "index.asp"),
        "SCRIPT_NAME": "/index.asp",
        "DOCUMENT_ROOT": str(FIXTURES),
        "REQUEST_METHOD": "GET",
        "QUERY_STRING": "name=UnixSocket",
        "REQUEST_URI": "/index.asp?name=UnixSocket",
        "SERVER_PROTOCOL": "HTTP/1.1",
        "SERVER_NAME": "localhost",
        "SERVER_PORT": "80",
    }
    encoded_params = b"".join(fcgi_param(name, value) for name, value in params.items())
    payload = (
        fcgi_record(1, 1, struct.pack("!HB5x", 1, 0))
        + fcgi_record(4, 1, encoded_params)
        + fcgi_record(4, 1, b"")
        + fcgi_record(5, 1, b"")
    )
    output = bytearray()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(10)
        client.connect(str(socket_path))
        client.sendall(payload)
        while True:
            header = recv_exact(client, 8)
            _, record_type, request_id, content_length, padding_length, _ = struct.unpack("!BBHHBB", header)
            assert request_id == 1
            content = recv_exact(client, content_length)
            if padding_length:
                recv_exact(client, padding_length)
            if record_type == 6:
                output.extend(content)
            if record_type == 3:
                return bytes(output)


def wait_for_fpm_start(fpm: subprocess.Popen[str], address: str) -> str:
    """Read startup output through the listener announcement."""
    assert fpm.stdout is not None
    assert fpm.stderr is not None
    assert_startup_banner(fpm.stderr.readline())
    startup_lines: list[str] = []
    for _ in range(8):
        startup_lines.append(fpm.stdout.readline())
        if address in startup_lines[-1]:
            break
    started = "".join(startup_lines)
    assert address in started, started
    return started


def stop_fpm(fpm: subprocess.Popen[str]) -> None:
    """Terminate the complete master/worker process group."""
    if fpm.poll() is None:
        os.killpg(fpm.pid, signal.SIGTERM)
        fpm.wait(timeout=5)


def main() -> None:
    """Verify file, REPL, and FastCGI startup behavior plus log redaction."""
    with tempfile.TemporaryDirectory(prefix="openasp-command-logs-") as temp_dir:
        log_dir = Path(temp_dir)
        fpm_log = log_dir / "openasp-fpm.log"
        file_log = log_dir / "openasp.log"
        cli_log = log_dir / "openasp-cli.log"

        fpm_help_result = run_checked([str(BUILD / "openasp-fpm"), "--help"])
        assert_startup_banner(fpm_help_result.stderr)
        fpm_help = fpm_help_result.stdout
        assert "FastCGI server for OpenASP" in fpm_help
        assert "--unix-socket <path>" in fpm_help
        assert "--aot=on|off (default: on)" in fpm_help

        conflict_result = subprocess.run(
            [
                str(BUILD / "openasp-fpm"),
                "--unix-socket",
                str(log_dir / "conflict.sock"),
                "--port",
                "19141",
                "--log-file",
                "off",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert conflict_result.returncode == 2
        assert "--unix-socket cannot be combined" in conflict_result.stdout

        file_help_result = run_checked([str(BUILD / "openasp"), "--help"])
        assert_startup_banner(file_help_result.stderr)
        assert file_help_result.stdout.startswith("Usage: openasp ")
        assert "openasp: execute a local ASP entry file" not in file_help_result.stdout

        fpm = subprocess.Popen(
            [
                str(BUILD / "openasp-fpm"),
                "--root",
                str(FIXTURES),
                "--host",
                "127.0.0.1",
                "--port",
                "19141",
                "--workers",
                "1",
                "--log-file",
                str(fpm_log),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
            env={
                **os.environ,
                "EGRET_ASP_AOT_ENTRY_PAGES": "index.asp",
                "EGRET_ASP_STATE_DIR": str(log_dir / "fpm-state"),
            },
        )
        try:
            assert fpm.stdout is not None
            assert fpm.stderr is not None
            assert_startup_banner(fpm.stderr.readline())
            startup_lines: list[str] = []
            for _ in range(5):
                startup_lines.append(fpm.stdout.readline())
                if "fastcgi listening on" in startup_lines[-1]:
                    break
            started = "".join(startup_lines)
            assert "fastcgi listening on" in started and "aot=on" in started
            time.sleep(0.2)
            fpm_text = read_daily_log(fpm_log)
            assert "openasp-fpm starting" in fpm_text and "aot=on" in fpm_text
            assert "openasp-fpm master listening" in fpm_text
            assert "openasp-fpm worker forked" in fpm_text
        finally:
            stop_fpm(fpm)

        unix_socket = log_dir / "openasp-fpm.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as stale_socket:
            stale_socket.bind(str(unix_socket))
        assert unix_socket.exists()

        unix_fpm = subprocess.Popen(
            [
                str(BUILD / "openasp-fpm"),
                "--root",
                str(FIXTURES),
                "--unix-socket",
                str(unix_socket),
                "--workers",
                "1",
                "--aot=off",
                "--log-file",
                "off",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
            env={
                **os.environ,
                "EGRET_ASP_STATE_DIR": str(log_dir / "unix-fpm-state"),
            },
        )
        try:
            started = wait_for_fpm_start(unix_fpm, f"unix:{unix_socket}")
            assert "aot=off" in started
            assert stat.S_IMODE(unix_socket.stat().st_mode) == 0o660
            response = request_over_unix_socket(unix_socket)
            assert b"Status: 200 OK" in response
            assert b"<div>hello=UnixSocket</div>" in response

            active_result = subprocess.run(
                [
                    str(BUILD / "openasp-fpm"),
                    "--root",
                    str(FIXTURES),
                    "--unix-socket",
                    str(unix_socket),
                    "--aot=off",
                    "--log-file",
                    "off",
                ],
                capture_output=True,
                text=True,
                timeout=10,
            )
            assert active_result.returncode == 2
            assert "socket is already active" in active_result.stdout
        finally:
            stop_fpm(unix_fpm)

        unix_socket.unlink()
        unix_socket.write_text("must not be removed")
        regular_file_result = subprocess.run(
            [
                str(BUILD / "openasp-fpm"),
                "--root",
                str(FIXTURES),
                "--unix-socket",
                str(unix_socket),
                "--aot=off",
                "--log-file",
                "off",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        assert regular_file_result.returncode == 2
        assert "path exists and is not a socket" in regular_file_result.stdout
        assert unix_socket.read_text() == "must not be removed"

        file_result = run_checked([
            str(BUILD / "openasp"),
            str(FIXTURES / "index.asp"),
            "--root",
            str(FIXTURES),
            "--query",
            "name=CommandTest",
            "--log-file",
            str(file_log),
        ])
        assert_startup_banner(file_result.stderr)
        output = file_result.stdout
        assert "<div>hello=CommandTest</div>" in output
        file_text = read_daily_log(file_log)
        assert "openasp starting" in file_text
        assert "openasp render completed" in file_text
        assert "CommandTest" not in file_text

        response_end_result = run_checked([
            str(BUILD / "openasp"),
            str(FIXTURES / "response_end_in_condition.asp"),
            "--root",
            str(FIXTURES),
            "--log-file",
            "off",
        ])
        assert_startup_banner(response_end_result.stderr)
        response_end_output = response_end_result.stdout
        assert response_end_output == "before", response_end_output

        repl = run_checked(
            [
                str(BUILD / "openasp-cli"),
                "--root",
                str(FIXTURES),
                "--no-prompt",
                "--log-file",
                str(cli_log),
            ],
            "Dim x\nx = 40\n= x + 2\n.exit\n",
        )
        assert_startup_banner(repl.stderr)
        assert repl.stdout.strip() == "42"
        cli_text = read_daily_log(cli_log)
        assert "openasp-cli starting" in cli_text
        assert "openasp-cli statement completed sequence=3" in cli_text
        assert "x = 40" not in cli_text

    print("通过：OpenASP 三命令入口集成验证")


if __name__ == "__main__":
    main()
