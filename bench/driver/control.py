# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""TCP control socket client for emit-app."""

from __future__ import annotations

import json
import socket
from typing import Any


class ControlClient:
    """Client for the emit-app TCP ndjson control socket on port 19090."""

    def __init__(self, host: str, port: int, reply_timeout_s: float = 60.0):
        self._host = host
        self._port = port
        self._reply_timeout_s = reply_timeout_s
        self._sock: socket.socket | None = None
        self._buf = b""

    def connect(self) -> None:
        self._sock = socket.create_connection((self._host, self._port), timeout=10.0)
        self._sock.settimeout(self._reply_timeout_s)

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def __enter__(self) -> "ControlClient":
        self.connect()
        return self

    def __exit__(self, *_) -> None:
        self.close()

    def run(self, spans: int, threads: int = 1, rate_hz: int = 0) -> dict[str, Any]:
        """Send a run command and return the parsed RunResult."""
        self._send({"cmd": "run", "spans": spans, "threads": threads, "rate_hz": rate_hz})
        return self._recv()

    def flush(self) -> dict[str, Any]:
        """Send a flush command and return {"flush_ns": N}."""
        self._send({"cmd": "flush"})
        return self._recv()

    def quit(self) -> None:
        """Send quit and wait for acknowledgement."""
        self._send({"cmd": "quit"})
        self._recv()

    def _send(self, obj: dict) -> None:
        assert self._sock is not None
        line = json.dumps(obj) + "\n"
        self._sock.sendall(line.encode())

    def _recv(self) -> dict[str, Any]:
        assert self._sock is not None
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("control socket closed unexpectedly")
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        return json.loads(line.decode())
