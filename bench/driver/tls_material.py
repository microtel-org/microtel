# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Throwaway TLS material for the interop sinks.

The stock OpenTelemetry Collector and Jaeger OTLP/HTTP receivers serve HTTP/1.1
on plaintext, where microtel's OTLP/HTTP exporter falls back to serial HTTP/1.1
(docs/interop-matrix.md section 4, ICP 0042).  Over TLS the same receivers
negotiate ``h2`` via ALPN, so the interop HTTP legs run against a TLS receiver,
using a CA generated here, to measure HTTP/2.  NOTHING HERE IS A SECRET.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

# Hostnames the SUT uses to reach the sink (network aliases) + local access.
_SAN = "DNS:sink,DNS:jaeger,DNS:localhost,IP:127.0.0.1"


def generate(out_dir: Path | None = None) -> Path:
    """Write ca.crt, server.crt and server.key into out_dir and return it.

    Files are world-readable because the collector / Jaeger images run as a
    non-root uid and read them through a bind mount.
    """
    d = Path(out_dir) if out_dir else Path(tempfile.mkdtemp(prefix="bench-tls-"))
    d.mkdir(parents=True, exist_ok=True)

    def run(*args: str) -> None:
        subprocess.run(["openssl", *args], check=True, capture_output=True, cwd=d)

    run("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "ca.key")
    run("req", "-x509", "-new", "-key", "ca.key", "-sha256", "-days", "2",
        "-subj", "/CN=bench-interop-ca", "-out", "ca.crt")
    run("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", "server.key")
    run("req", "-new", "-key", "server.key", "-subj", "/CN=sink", "-out", "server.csr")
    (d / "server.ext").write_text(
        f"subjectAltName={_SAN}\nextendedKeyUsage=serverAuth\n"
        "basicConstraints=CA:FALSE\n"
    )
    run("x509", "-req", "-in", "server.csr", "-CA", "ca.crt", "-CAkey", "ca.key",
        "-CAcreateserial", "-days", "2", "-sha256", "-extfile", "server.ext",
        "-out", "server.crt")
    os.chmod(d, 0o755)
    for name in ("ca.crt", "server.crt", "server.key"):
        os.chmod(d / name, 0o644)
    return d
