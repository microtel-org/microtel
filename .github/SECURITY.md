# Security Policy

## Supported versions

| Version | Status | Security updates |
|---|---|---|
| 1.2.x | current release (1.2.5) | ✅ supported — fixes land in the latest 1.2 patch |
| 1.1.x | superseded minor | ❌ unsupported — upgrade to 1.2.x |
| < 1.1 | development snapshots | ❌ unsupported — use 1.2.x |

Only the latest minor of the latest major is supported. The 18-month security
window in the policy below applies to a previous **major**, which does not
exist yet.

The policy this matrix will follow as further releases land:

- The most recent **minor release** of the latest **major version** is fully supported.
- The previous **major version** receives security fixes for **18 months** after the next major version ships (per the LTS policy in `docs/roadmap.md` §2).
- Older versions are end-of-life. If you're running one, the recommended path is to upgrade.

## Reporting a vulnerability

**Please do not open a public GitHub issue for security vulnerabilities.**

Report it privately through GitHub: open the repository's
[Security tab](https://github.com/microtel-org/microtel/security) and choose
**Report a vulnerability**
([new advisory form](https://github.com/microtel-org/microtel/security/advisories/new)).
Only the maintainers see the report.

What to include:

- A description of the vulnerability and its impact.
- Steps to reproduce, ideally with a minimal proof-of-concept.
- The microtel version, build configuration, and platform.
- Whether you've already disclosed this elsewhere.

## What to expect

- **Acknowledgement within 72 hours** of receipt.
- **Initial assessment within 7 days.**
- **Coordinated disclosure preferred.** We aim to publish a fix and advisory together.
- **CVE assignment** when warranted.
- **Credit** in the advisory unless you prefer to remain anonymous.

## Scope

The following are in scope:

- The microtel C++ runtime, including the trace, metrics and logs SDKs, the OTLP encoder, the OTLP/HTTP and OTLP/gRPC codecs, the HTTP/2 transport, and the concentrator (the SDK's leaf receiver).
- The leaf C library (`microtel_leaf`).
- The `microtel-preflight` tool.
- The compatibility shim (`microtel_otelcpp_shim`) and the log bridges (`microtel_spdlog_bridge`, `microtel_glog_bridge`, `microtel_log4cxx_bridge`).
- The vendored upb and nanopb code and the proto-generation pipeline (only insofar as a vulnerability requires changes in microtel's vendoring or wrapper code; vulnerabilities in upstream upb or nanopb are reported to those projects).
- Build and packaging scripts that affect installed artifacts.

## Out of scope

The following are **not** considered vulnerabilities for the purposes of this policy:

- Resource exhaustion caused by legitimate API use without bounds (e.g., creating millions of spans beyond the bounded queue (`max_queue_size` records, `max_total_queue_bytes` in all)).
- Issues in third-party dependencies (nghttp2, OpenSSL, zlib) — please report those upstream.
- Configuration mistakes by operators (e.g., shipping with `insecure = true` in production).
- Self-XSS or attacks requiring physical access to the host.
- Theoretical issues without a demonstrable exploit path.

## Hardening guidance

For deployments handling sensitive telemetry:

- Build with `MICROTEL_FORBID_INSECURE_TLS=ON`. A library compiled that way fails `SdkBuilder::Build()` with `ConfigError::Kind::InsecureDisallowed` when the resolved configuration sets `tls.insecure = true`, so no `microtel.toml`, environment variable or code path can turn certificate verification off. Default builds (`OFF`) accept `insecure = true` and only log a warning.
- Pin to a specific minor version and review release notes before upgrading.
- Run `microtel-preflight --preflight=connect` against your collector before deploying to validate TLS and connectivity.
- Use mTLS (`client_cert` + `client_key`) where the upstream collector supports it.
- Set explicit limits rather than relying on defaults: queue sizes in the `[sdk]` table of `microtel.toml`, memory budgets in code through `WithMemoryLimits` (they have no TOML keys).

## Hall of fame

Researchers who responsibly disclose vulnerabilities will be acknowledged here unless they prefer to remain anonymous.

*(empty — no advisories yet)*
