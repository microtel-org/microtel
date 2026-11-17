# Copyright (c) 2026 The microtel Authors.
# SPDX-License-Identifier: Apache-2.0
"""Load and validate bench/sut/registry.yaml."""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Dict, List, Optional

from .profile import parse_yaml, _coerce


@dataclasses.dataclass(frozen=True)
class SutPorts:
    control: int = 19090


@dataclasses.dataclass(frozen=True)
class Sut:
    name: str
    description: str
    dockerfile: str
    image_name: str
    protocol: str          # "http" | "grpc"
    library: str           # "microtel" | "otelcpp" | "none" (baseline-*)  (derived from name)
    transport: str         # "http" | "grpc"          (same as protocol)
    ports: SutPorts
    env: Dict[str, str]
    b0: bool
    threads_override: Optional[int] = None


_PROTOCOLS = {"http", "grpc"}


def _derive_library(name: str) -> str:
    """Derive the library field from the SUT name in registry.yaml."""
    if name.startswith("microtel"):
        return "microtel"
    if name.startswith("otelcpp"):
        return "otelcpp"
    if name.startswith("baseline"):
        return "none"   # the no-op emit-app: no SDK
    return name


def load(registry_path: Path) -> List[Sut]:
    """Load all SUTs from registry.yaml."""
    if not registry_path.exists():
        raise FileNotFoundError(f"registry not found: {registry_path}")

    text = registry_path.read_text(encoding="utf-8")
    data = parse_yaml(text)

    raw_suts = data.get("suts", [])
    if not isinstance(raw_suts, list):
        raise ValueError("registry.yaml: 'suts' must be a list")

    result = []
    for entry in raw_suts:
        if not isinstance(entry, dict):
            continue
        name = str(entry["name"])
        protocol = str(entry.get("protocol", "http"))
        if protocol not in _PROTOCOLS:
            raise ValueError(f"SUT {name!r}: unknown protocol {protocol!r}")

        raw_ports = entry.get("ports", {})
        ports = SutPorts(control=int(raw_ports.get("control", 19090))
                         if isinstance(raw_ports, dict) else 19090)

        raw_env = entry.get("env", {})
        env = {k: str(v) for k, v in raw_env.items()} if isinstance(raw_env, dict) else {}

        result.append(Sut(
            name=name,
            description=str(entry.get("description", "")),
            dockerfile=str(entry.get("dockerfile", "")),
            image_name=str(entry.get("image_name", f"bench-sut-{name}")),
            protocol=protocol,
            library=_derive_library(name),
            transport=protocol,
            ports=ports,
            env=env,
            b0=bool(entry.get("b0", False)),
        ))

    return result


def filter_b0(suts: List[Sut]) -> List[Sut]:
    """Return only B0-enabled SUTs."""
    return [s for s in suts if s.b0]


def get(suts: List[Sut], name: str) -> Optional[Sut]:
    """Return a SUT by name, or None."""
    for s in suts:
        if s.name == name:
            return s
    return None
