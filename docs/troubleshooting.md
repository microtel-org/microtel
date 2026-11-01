# Troubleshooting

One page for "something is wrong, what do I do". Each entry starts from what
you actually see (an error message, a log line, a counter, an exit code) and
says what to do about it. Messages are quoted exactly as the code produces
them; `<...>` marks a part that is filled in at run time. Every entry names the
source file it was checked against, so if a message here ever disagrees with
the code, the code is right and this page has a bug.

This page does not define anything. The normative definitions stay where they
are: the error taxonomy, drop counters and retry classification in
[error-model.md](error-model.md), every setting in
[configuration.md](configuration.md), and the plaintext-HTTP limitation in
[compatibility-matrix.md](compatibility-matrix.md) §4.

- [Where microtel reports problems](#where-microtel-reports-problems)
- [`SdkBuilder::Build()` returns an error](#sdkbuilderbuild-returns-an-error)
- [`Build()` succeeds but logs a warning](#build-succeeds-but-logs-a-warning)
- [`Provider::Connect()` fails](#providerconnect-fails)
- [No traces arrive: reading `GetExporterHealth()`](#no-traces-arrive-reading-getexporterhealth)
- [`ForceFlush` / `Shutdown` return something other than `Completed`](#forceflush--shutdown-return-something-other-than-completed)
- [Collector connection problems](#collector-connection-problems)
- [The example stack (`examples/stack/`)](#the-example-stack-examplesstack)
- [`microtel-preflight` exit codes](#microtel-preflight-exit-codes)
- [Leaf receiver (concentrator)](#leaf-receiver-concentrator)

---

## Where microtel reports problems

microtel reports a problem in one of four places, and it helps to know which
one to read:

| Surface | When | What you get |
|---|---|---|
| `SdkBuilder::Build()` | configuration is invalid | `Expected` holding a `ConfigError` with `kind`, `field` (dotted path, may be empty) and `message` ([`include/microtel/error.hpp`](../include/microtel/error.hpp)) |
| Internal log | a legal but risky configuration, a skipped resource detector, a few protocol oddities | a line on **stderr** as `[microtel warn] <message>`, or your callback if you installed one with `microtel::SetLogSink` (`src/common/log_sink.cpp`, `LogImpl`) |
| `Provider::Connect()` | you asked for an eager connect and it failed | `Expected` holding an `Error` with `kind`, `message` and `os_errno` |
| `Provider::GetExporterHealth()` | everything at run time | a `HealthSnapshot`: drop counters, `batches_sent`, `batches_failed`, `queue_depth_now`, `last_error_time`, `last_error_message`, `connection_state` ([`include/microtel/provider.hpp`](../include/microtel/provider.hpp)) |

**If you see** nothing in the log, but traces never arrive **→** read
`GetExporterHealth()`. A failed export is not logged: the exporter and the
transport have no log call sites (`src/exporter/`, `src/transport/`); a lost
batch shows up only as `batches_failed`, a drop counter and
`last_error_message`. The [health_and_backpressure example](../examples/health_and_backpressure/)
shows how to print a snapshot.

**If you cannot tell** whether the problem is your build, your
instrumentation or the collector **→** run
[`examples/console_trace`](../examples/console_trace/README.md). It exports
through an application `ExportTransport` that prints each span to stdout, so
it needs no collector, no container and no network. If spans print there, the
library and the pipeline work and the problem is on the network side.

**If you set** `MICROTEL_LOG_LEVEL=debug` (or `trace`) expecting more output
**→** you will not get any. microtel has no `debug` or `trace` log call sites;
apart from one `info` line at `Build()` (`resolved resource (profile "<name>", <n> attributes): ...`,
`src/sdk/resource_builder.cpp`), everything it logs is `warn`. Lowering the
level changes nothing else; raising it to `error` silences the warnings in this
page. Level semantics: [configuration.md](configuration.md) §3.11.

---

## `SdkBuilder::Build()` returns an error

`Build()` validates eagerly and opens no socket, so every entry here is a
configuration problem. Print both `error().field` and `error().message`: the
message usually already says what to change. The full `ConfigError::Kind` list
and its normative mapping is [error-model.md](error-model.md) §8; the entries
below quote the messages the code actually produces, and the `field` values
are the ones the code sets.

Precedence matters for every entry: code beats environment beats
`microtel.toml` beats defaults ([configuration.md](configuration.md) §1), so a
bad value may come from a layer you did not look at.

### Endpoint and protocol

Source: `src/common/config/config_validator.cpp` (`ParseEndpointUrl`,
`ParsePort`, `ResolveProtocol`, `Validate`).

- **If you see** `endpoint URL is empty` (`EndpointMalformed`, field
  `exporter.endpoint`) **→** no layer set an endpoint. Set one with
  `WithEndpoint`, `OTEL_EXPORTER_OTLP_ENDPOINT` or `exporter.endpoint`. There is
  no built-in default ([configuration.md](configuration.md) §3.3).
- **If you see** `endpoint URL missing scheme (expected https:// or http://)`
  **→** write the scheme: `https://collector:4318`, not `collector:4318`.
- **If you see** `unsupported scheme: <scheme>` **→** use one of the four
  accepted schemes: `https`, `http`, `grpcs`, `grpc`.
- **If you see** `endpoint URL has empty host` or `invalid port in endpoint URL`
  **→** fix the URL; the port must be 1–65535.
- **If a malformed `OTEL_EXPORTER_OTLP_ENDPOINT`** is the cause **→** expect the
  same `EndpointMalformed` errors on field `exporter.endpoint`, not an
  `EnvParseFailure`: `src/common/config/env_resolver.cpp` copies the variable
  verbatim and `Validate` is what rejects it ([error-model.md](error-model.md)
  §8).
- **If you see** `gRPC endpoint URLs must not include a path`
  (`ProtocolMismatch`, field `exporter.endpoint`) **→** drop the path. A
  gRPC endpoint is `https://collector:4317`; `/v1/traces` belongs to OTLP/HTTP
  only.
- **If you see** `endpoint scheme "grpc://" selects OTLP/gRPC but protocol is
  set to "http"; use an http:// or https:// endpoint, or drop the explicit
  protocol` (`ProtocolMismatch`, field `exporter.protocol`) **→** do what it
  says. Look in all three layers for the explicit protocol: `WithProtocol`,
  `OTEL_EXPORTER_OTLP_PROTOCOL`, `exporter.protocol`.

### TLS

Source: `src/common/config/config_validator.cpp` (`ValidateTlsMaterial`,
`CheckInsecureAllowed`). TLS lives in a top-level `[tls]` table, not under
`[exporter]` ([configuration.md](configuration.md) §3.5).

- **If you see** `CA bundle not readable: <path>`, `client cert not readable:
  <path>` or `client key not readable: <path>` (`TlsMaterialUnreadable`, field
  `tls.ca_bundle` / `tls.client_cert` / `tls.client_key`) **→** the path is not
  a readable regular file for this process. Check the path, its permissions,
  and that a container actually mounts it.
- **If you see** `client_cert is set but client_key is missing` or
  `client_key is set but client_cert is missing` (`InvalidValue`) **→** mTLS
  needs both; set the other one or neither.
- **If you see** `tls.insecure = true is refused: this build was compiled with
  MICROTEL_FORBID_INSECURE_TLS=ON` (`InsecureDisallowed`, field `tls.insecure`)
  **→** this library build forbids disabling verification and no runtime
  setting overrides it. Configure a `ca_bundle` instead.

A file that is readable but is not valid PEM passes `Build()` and fails later,
at connect time: see `CA bundle load failed` under
[`Provider::Connect()` fails](#providerconnect-fails).

### The config file

Source: `src/common/config/toml_loader.cpp` (`LoadToml`, `CheckUnknown`, the
section parsers).

- **If you see** `Config file not found: <path>` (`FileNotFound`) **→** the path
  given to `SdkBuilder::FromFile` does not exist, relative to the process's
  working directory if it is relative.
- **If you see** `FileParseFailure` **→** the file is not valid TOML. The
  message is the TOML parser's own description of the syntax error.
- **If you see** `Unknown configuration key: <section.key>` (`UnknownKey`)
  **→** a typo, or a key in the wrong table. Common ones: TLS keys under
  `[exporter]` (they belong in `[tls]`), batch keys under `[batch]` (they
  belong in `[sdk]`). Look the key up in [configuration.md](configuration.md)
  §3. Setting `config.unknown_keys = "warn"` stops the failure and logs one
  line per unknown key at `Build()`; `"ignore"` accepts them silently. (Through
  v1.2.0, `"warn"` logged nothing.)
- **If you see** `[microtel warn] unknown configuration key "<section.key>"
  ignored ([config] unknown_keys = "warn")` **→** the same typo or misplaced key
  as above, accepted because `unknown_keys = "warn"`. Fix the key; the setting
  it was meant to change has its default. Source: `src/sdk/sdk_builder.cpp`
  (`WarnOnUnknownKeys`).
- **If you see** a value message such as `must be "http" or "grpc"`,
  `must be "newest" or "oldest"`, `must be "trace", "debug", "info", "warn" or
  "error"` or `must be "error", "warn", or "ignore"` (`InvalidValue`, with the
  key in `field`) **→** use one of the listed values.

### Environment variables

Source: `src/common/config/env_resolver.cpp`. All are `EnvParseFailure` with
the variable name in `field`.

- **If you see** `expected "grpc" or "http/protobuf"` (field
  `OTEL_EXPORTER_OTLP_PROTOCOL`) **→** use one of those two spellings.
- **If you see** `expected "trace", "debug", "info", "warn" or "error"` (field
  `MICROTEL_LOG_LEVEL`) **→** use one of those. A bad level is rejected rather
  than silently read as the default.
- **If you see** `<VAR>: expected integer milliseconds` **→** the variable
  (for example `OTEL_EXPORTER_OTLP_TIMEOUT`) takes a bare integer: `5000`,
  not `5s`.
- **If you see** `<VAR>: entry <n> is not a key=value pair (missing '=')` **→**
  fix the n-th entry, counting from 1, of the comma-separated `k=v,k=v` list
  (for example `OTEL_EXPORTER_OTLP_HEADERS` or `OTEL_RESOURCE_ATTRIBUTES`). A
  `:` typed instead of `=` is the usual cause. The message does not repeat the
  entry, because in a header list it can be a credential.
- **If you see** `<VAR>: malformed percent-escape in the value of <key> (expected %XX, two hex digits)`
  **→** values in these lists are percent-decoded, so a literal `%` must be
  written `%25`. A space is `%20`, a comma `%2C`, an `=` `%3D`.

### Request headers

Source: `src/common/config/config_validator.cpp` (`HeaderNameFault`,
`CheckNoStaticAuthorization`; [ICP 0038](icps/0038-reject-reserved-request-headers.md)).
All are `InvalidValue`, field `exporter.headers.<name as configured>`. The
header can come from `WithHeaders`, `OTEL_EXPORTER_OTLP_HEADERS` or
`[exporter.headers]`. The message never includes the header's value.

- **If you see** `header "<name>" is not a valid header name (RFC 9110 token); pseudo-headers are set by microtel`
  **→** the name is empty, contains a space or separator, or starts with `:`.
  Remove it: microtel sets `:authority`, `:path`, `:method` and `:scheme`
  itself.
- **If you see** `header "<name>" is connection-specific, which HTTP/2 forbids in a request (RFC 9113 §8.2.2)`
  **→** remove `connection`, `keep-alive`, `proxy-connection`,
  `transfer-encoding`, `upgrade` or `te`. HTTP/2 has no use for them, and a
  receiver rejects the request.
- **If you see** `header "host" is set by microtel from the endpoint, as :authority`
  **→** remove it, and put the host you want in the endpoint.
- **If you see** `header "<name>" is set by microtel for this protocol` **→**
  remove it. On gRPC that is `content-type`, `user-agent`, `grpc-encoding`
  and `grpc-accept-encoding`. On HTTP/protobuf it is `content-type`,
  `content-length`, `content-encoding` and `accept-encoding`. Compression is
  `WithCompressionGzip` / `OTEL_EXPORTER_OTLP_COMPRESSION`.
- **If you see** `header "content-length" cannot be fixed on gRPC, where every request body has its own length`
  **→** remove it. microtel sends no `content-length` on gRPC, and a fixed
  value would be wrong for every batch but one.
- **If you see** `header "<name>" value contains CR, LF or NUL, which HTTP/2 forbids (RFC 9113 §8.2.1)`
  or `header "<name>" value starts or ends with a space or tab, which HTTP/2 forbids (RFC 9113 §8.2.1)`
  **→** fix the value. A TOML escape such as `\n` puts a real line break into
  it. In `OTEL_EXPORTER_OTLP_HEADERS`, write a space that belongs to the value
  as `%20`.
- **If you see** `header "<name>" is set by the WithAuthProvider callback; set one or the other`
  **→** a static `authorization` header and `WithAuthProvider` would send two
  `authorization` headers. Keep one; the static header may be coming from
  `OTEL_EXPORTER_OTLP_HEADERS`.

### Batch settings

Source: `src/common/config/config_validator.cpp` (`CheckBatchOptions`). All are
`InvalidValue`, field `sdk.<key>`.

- **If you see** `max_queue_size must be greater than zero`,
  `max_export_batch_size must be greater than zero`,
  `max_export_batch_size must not exceed max_queue_size` or
  `schedule_delay must be greater than zero` **→** fix the named value; the
  rules are in [configuration.md](configuration.md) §3.7.

### Builder usage

Source: `src/sdk/sdk_builder.cpp` (`Build`, `ResolveProfileName`,
`RegistrationError`, `CheckExportTransport`, `CheckLeafReceiver`).

- **If you see** `SdkBuilder::Build() called more than once`
  (`BuildAlreadyConsumed`) **→** a builder builds once. Create a new
  `SdkBuilder` for a second provider.
- **If you see** `a live provider is already registered under profile name
  '<name>'; shutting a provider down does not release its name, destroying it
  does` (`DuplicateProfileName`, field `profile_name`) **→** give the second
  provider its own `WithProfileName`, or destroy the first before building
  again. `Shutdown` alone does not free the name.
- **If you see** `the process already holds the maximum of <n> live providers`
  (`ProfileLimitExceeded`) **→** destroy providers you no longer use.
- **If you see** `profile name must not be empty; omit WithProfileName to build
  the default profile` **→** do that.
- **If you see** `<WithX> has no meaning with WithExportTransport, which
  replaces the HTTP/2 exporter; set one or the other` or `WithExportTransport
  needs a non-null ExportTransport` (`InvalidValue`, field
  `exporter.transport`) **→** with an application transport, drop the
  endpoint, protocol, header, TLS, auth and compression setters
  ([configuration.md](configuration.md) §3.15).
- **If you see** `the leaf receiver is not compiled into this build of
  microtel; rebuild with -DMICROTEL_WITH_CONCENTRATOR=ON` (`InvalidValue`,
  field `concentrator.enabled`) **→** rebuild with that option, or remove the
  `[concentrator]` table / `WithLeafReceiver` call.
- **If you see** `resource detector "<name>" failed: <reason>` as a `Build()`
  error **→** `sdk.resource_detectors_strict` is on; fix the detector's input
  or turn strict mode off to have it skipped with a warning instead
  (`src/sdk/resource_builder.cpp`).
- **If you see** `reactor init failed: <reason>` or `transport init failed:
  <reason>` (`Unspecified`) **→** the process could not create the I/O thread
  or the transport. A reason of `OOM or EAGAIN` usually means a thread or
  memory limit, for example a container pid limit
  (`src/transport/http2_transport.cpp`).

---

## `Build()` succeeds but logs a warning

These are configurations microtel accepts but that are very likely wrong. They
appear on stderr as `[microtel warn] ...` unless you installed a sink.

- Before v1.2.6, `http://` with OTLP/HTTP warned here that it could not reach
  an HTTP/1.1-only receiver. It can now, through the HTTP/1.1 fallback, so the
  warning is gone; see
  [Collector connection problems](#collector-connection-problems).
- **If you see** `tls.insecure = true - TLS certificate verification is
  disabled and any certificate will be accepted, including an attacker's. Not
  for production; see docs/compatibility-matrix.md` **→** configure a
  `ca_bundle` and remove `insecure`. To make this a hard error, build with
  `MICROTEL_FORBID_INSECURE_TLS=ON`. Source: same function.
- **If you see** `resource detector "<name>" failed: <reason> - skipping its
  contribution (set sdk.resource_detectors_strict to fail Build instead)`
  **→** the detector's attributes are missing from the Resource; fix its input,
  or remove the detector if it does not apply on this host. Source:
  `src/sdk/resource_builder.cpp`.
- **If you see** `export transport: exporter settings from the environment or
  file are ignored with a custom transport: <names>` **→** an `OTEL_EXPORTER_*`
  variable or `[exporter]`/`[tls]` key is set but has no effect with
  `WithExportTransport`; remove it. Source: `src/sdk/sdk_builder.cpp`,
  `WarnOnCustomTransport`.
- **If you see** `export transport: traces are off in ExportTransportOptions;
  the sampler is always-off and no span is recorded` **→** set `traces = true`
  in `ExportTransportOptions` if you meant to export spans. The metrics and
  logs equivalents (`... GetMeter returns a no-op meter`, `... GetLogger
  returns a no-op logger`) are logged on first use (`src/sdk/sdk_provider.cpp`).

---

## `Provider::Connect()` fails

`Connect()` is optional: without it the first export connects lazily. Either
way the failure text is the same, and if the batch is finally lost it is also
what `HealthSnapshot::last_error_message` shows. Every failed attempt
increments the `ConnectFailure` drop counter.

`Error::Kind::Network` and `Cancelled` failures are retried on the export
path; `Protocol` failures are permanent and are not
(`src/wire/http/http_wire_codec.cpp`, `EnsureConnected`; [error-model.md](error-model.md) §7).
With an application `ExportTransport`, `Connect()` always succeeds: there is
nothing to connect.

Source for every message below: `src/transport/http2_transport.cpp` and
`src/transport/connect_error.cpp`.

### Reaching the host

- **If you see** `DNS resolution failed` **→** the host name does not resolve
  from this process. Inside a container, `localhost` is the container itself;
  use the collector's service name or the host's address.
- **If you see** `connection refused` **→** nothing listens on that host and
  port. Check the collector is running and the port is right: 4317 for
  OTLP/gRPC, 4318 for OTLP/HTTP, and the default when the URL has no port is
  chosen by protocol.
- **If you see** `connect failed: <OS reason>` (for example `No route to host`
  or `Network is unreachable`; the errno is in `os_errno`) **→** a network
  path or firewall problem between this host and the collector.
- **If you see** `peer closed the connection during TCP connect: <OS reason>`
  **→** something accepted and immediately dropped the connection: a proxy, a
  load balancer with no healthy backend, or a port-forward with nothing
  behind it.
- **If you see** `connect timeout` (`Cancelled`) **→** the TCP connect did not
  finish within `timeouts.connect_ms` (default 10 s). Usually a firewall that
  drops packets rather than refusing them.

### TLS

- **If you see** `TLS certificate verification failed: <OpenSSL reason>` **→**
  the collector's certificate is not trusted, or does not carry the name you
  connected to. For a private CA, set `tls.ca_bundle` (or
  `OTEL_EXPORTER_OTLP_CERTIFICATE`). For a name mismatch, connect by the name
  on the certificate, or set `tls.sni_override`, which moves both the SNI and
  the name the certificate is checked against.
- **If you see** `TLS handshake failed` **→** the handshake failed for a reason
  other than the certificate: the endpoint may not be TLS at all (an
  `https://` URL pointing at a plaintext port), or client-certificate
  authentication was refused.
- **If you see** `TLS handshake timeout` (`Cancelled`) **→** no handshake
  within `timeouts.tls_ms` (default 10 s). Often a plaintext port that never
  answers a TLS ClientHello.
- **If you see** `CA bundle load failed`, `client cert load failed` or
  `client key load failed` **→** the file was readable at `Build()` but
  OpenSSL could not parse it. Check it is PEM, and that the key matches the
  certificate.
- **If you see** `TLS verify hostname rejected` **→** OpenSSL refused the host
  name (or `sni_override`) as a name to verify against. Check it for stray
  characters.

### HTTP/2

- **If you see** `peer answered the HTTP/2 preface with an HTTP/1.1 response -
  endpoint appears to be HTTP/1.1-only; use https:// (ALPN h2) or OTLP/gRPC;
  see docs/compatibility-matrix.md` (`Protocol`, not retried) **→** an
  OTLP/**gRPC** endpoint that points at an HTTP/1.1 receiver: gRPC needs
  HTTP/2 and never falls back. Point it at the receiver's gRPC port (4317 by
  convention). OTLP/HTTP does not report this any more; it falls back to
  HTTP/1.1. See [Collector connection problems](#collector-connection-problems).
- **If you see** `TLS peer did not negotiate h2 via ALPN (negotiated: <proto>) -
  this endpoint is not an HTTP/2 receiver; use OTLP/gRPC or an h2-capable
  OTLP/HTTP endpoint; see docs/compatibility-matrix.md` (`Protocol`, not
  retried) **→** for OTLP/gRPC, the TLS endpoint speaks only HTTP/1.1 (or
  nothing ALPN at all): point at the receiver's gRPC port. For OTLP/HTTP, an
  answer of `http/1.1` or nothing falls back instead, so this message means
  the receiver chose some other protocol. With OpenSSL 3.2 or later the same
  server may instead fail as a TLS error
  ([compatibility-matrix.md](compatibility-matrix.md) §4).
- **If you see** `endpoint speaks HTTP/1.1 only; OTLP/HTTP continues over
  HTTP/1.1, one request at a time` (`Info`, once per `Provider`) **→** nothing
  is wrong: the receiver has no HTTP/2, and microtel fell back
  ([ICP 0042](icps/0042-http1-fallback-for-otlp-http.md)). Exports are now
  serial. If export throughput matters, use `https://` against a receiver that
  negotiates `h2`, or OTLP/gRPC.
- **If you see** `HTTP/1.1 transport: opened a new connection after the last
  one closed` or `a reused connection closed before responding, so the request
  was sent again on a new connection` (`Info` the first time, then a summary
  at most once a minute) **→** normal on HTTP/1.1: the receiver ended
  keep-alive, or its idle timeout raced a request. A retried request may reach
  the receiver twice, as any OTLP retry can.
- **If you see** `peer closed the connection during the HTTP/2 handshake`
  **→** the peer hung up as microtel started HTTP/2. Typically a non-OTLP
  service on that port, or a TLS-only port reached with `http://`.
- **If you see** `SETTINGS ACK timeout` (`Cancelled`) **→** TCP (and TLS)
  succeeded but the peer never completed the HTTP/2 handshake within
  `timeouts.connect_ms`. Something on the port is not an HTTP/2 server.

### Lifecycle

- **If you see** `transport is closed` **→** `Connect()` was called after
  `Shutdown()`. A shut-down provider cannot reconnect; build a new one.
- **If you see** `already connecting` **→** another thread's connect is in
  progress. Retry after it finishes, or rely on the lazy connect.

---

## No traces arrive: reading `GetExporterHealth()`

`HealthSnapshot` is defined in [`include/microtel/provider.hpp`](../include/microtel/provider.hpp).
`drop_counters` is indexed by `DropReason`:
`snap.drop_counters[static_cast<std::size_t>(microtel::DropReason::QueueFull)]`.
The meaning of every counter, where it is incremented, and in what unit, is
normative in [error-model.md](error-model.md) §3; this section is about what
to do. Counters and `last_error_message` are aggregated across traces, metrics
and logs ([error-model.md](error-model.md) §9.1).

### `batches_failed` and `last_error_message`

`last_error_message` (capped at 256 characters) is written only when a batch
is finally lost: after retries, not on each failed attempt
(`src/sdk/diagnostics_counters.cpp`, `RecordBatchFailed`;
`src/exporter/retry_engine.cpp`). A retry that recovered leaves it alone. Read
it together with the counter that moved:

- **If `batches_failed > 0` and `last_error_message`** is one of the
  [`Connect()` messages above](#providerconnect-fails) **→** apply that entry.
  The lazy connect on the export path reports exactly the same text.
- **If `last_error_message` is** `HTTP <code>` (OTLP/HTTP) **→** the collector
  rejected the request with a non-retryable status
  (`src/wire/http/http_wire_codec.cpp`). `404` usually means a wrong URL path:
  microtel appends `/v1/traces` to the endpoint's path, so an endpoint that
  already ends in `/v1/traces` becomes `/v1/traces/v1/traces`
  (`src/wire/http/http_wire_codec.cpp`, `ResolvePath`; the trace codec is
  built with no signal path in `src/sdk/sdk_builder.cpp`). `401`/`403`: check
  `exporter.headers` or your `WithAuthProvider`. `415`: something between you
  and the collector is not an OTLP/HTTP protobuf receiver.
- **If `last_error_message` is** `<STATUS_NAME> (<code>)` or `<STATUS_NAME>
  (<code>): <server message>` (OTLP/gRPC, for example `UNAUTHENTICATED (16)`)
  **→** the collector's gRPC status; whether it was retried is in
  [error-model.md](error-model.md) §7.2 (`src/wire/grpc/grpc_status.cpp`,
  `FormatGrpcError`). A suffix ` - no RetryInfo, not retried` on
  `RESOURCE_EXHAUSTED (8)` means the server signalled overload without saying
  when to retry, so microtel did not.
- **If `last_error_message` starts with** `authorization header unavailable: `
  **→** your auth callback failed, and the batch was dropped rather than sent
  unauthenticated. `auth callback threw: <what>` after the prefix means it
  threw (`src/wire/auth_failure.hpp`, `src/common/config/auth_providers.cpp`).
  See [auth-callback-recipes.md](auth-callback-recipes.md).
- **If `last_error_message` is** `request deadline exceeded` **→** each attempt
  ran past `timeouts.per_export_ms` / `OTEL_EXPORTER_OTLP_TIMEOUT`. A slow or
  overloaded collector, or a batch too large for the link.
- **If `last_error_message` is** `transport request queue full
  (max_pending_requests)` **→** requests are produced faster than the
  connection drains them. See `transport_busy` below.

### Drop counters

- **If `QueueFull` keeps rising** **→** spans are produced faster than they are
  exported, or exports are failing and the queue backs up. Check
  `batches_failed` first; if exports are healthy, raise `sdk.max_queue_size` or
  reduce volume (sampling). `drop_policy` picks which span is lost, not
  whether ([configuration.md](configuration.md) §3.7; walkthrough in
  [sequences/backpressure-and-drop.md](sequences/backpressure-and-drop.md)).
- **If `RecordTooLarge` rises** **→** single spans exceed `max_record_bytes`;
  look for very large attribute values or event lists
  ([configuration.md](configuration.md) §3.8).
- **If `SpanAttributeLimit`, `SpanEventLimit`, `SpanLinkLimit`,
  `EventAttributeLimit`, `LinkAttributeLimit` or `AttributeValueTruncated`
  rise** **→** spans are hitting the structural limits; the span is kept, the
  surplus is not. Raise the limits if the data matters
  ([configuration.md](configuration.md) §3.9).
- **If `PostShutdown` rises** **→** spans are ended after `Shutdown()`.
  Shut down after the threads that create spans have stopped.
- **If `ConnectFailure` rises** **→** connects are failing; the reason is in
  `last_error_message` once a batch is lost, or in `Connect()`'s return.
- **If `RetryBudgetExhausted` rises** **→** retryable failures (connection
  failures, HTTP `429`/`502`/`503`/`504`, gRPC `UNAVAILABLE` and the others
  marked retryable in [error-model.md](error-model.md) §7) outlasted `timeouts.retry_budget_ms`
  (default 60 s). The collector is down or overloaded.
- **If `NonRetryableFailure` rises** **→** the collector, a proxy or your auth
  callback rejected the batch outright; `last_error_message` says which.
- **If `PartialSuccessRejection` rises** **→** the collector accepted the
  request but rejected some or all items in it (OTLP partial success); check
  the collector's own logs for why ([error-model.md](error-model.md) §6). This
  counter is the only place it shows: the batch counts as sent, so
  `batches_failed` stays 0 and `last_error_message` is not written
  (`src/exporter/retry_engine.cpp`, `RecordOutcome`), and `ForceFlush` still
  returns `Completed`. The examples and `microtel-preflight` treat a non-zero
  count as a failed export.
- **If `TransportBusy` rises** **→** the transport's request queue was full and
  the attempt was refused before it reached the wire. It is retried; if it
  keeps happening, the link cannot keep up with the export rate.
- **If `ResponseTooLarge`, `DecompressionTooLarge` or `MalformedResponse`
  rise** **→** the collector's responses are bigger than the configured caps,
  or not valid OTLP. Usually something other than an OTLP receiver is
  answering ([configuration.md](configuration.md) §3.8).
- **If `ForceFlushTimeout` or `ShutdownTimeout` rise** **→** see the next
  section.

### `connection_state`

- **If it stays `Disconnected`** **→** no connect has ever succeeded: the
  endpoint, TLS material or network path is wrong. Run `microtel-preflight`
  against the same configuration.
- **If it shows `Reconnecting`** **→** it was connected and the connection
  dropped; the next export reconnects. This points at the collector or the
  link, not at your configuration.
- With an application `ExportTransport`, the state means "sends are
  succeeding", not "connected" ([error-model.md](error-model.md) §9.1).

---

## `ForceFlush` / `Shutdown` return something other than `Completed`

`Status` is defined in [`include/microtel/status.hpp`](../include/microtel/status.hpp)
and normatively in [error-model.md](error-model.md) §2.3.

- **If `ForceFlush` returns `Completed` but the collector has nothing** **→**
  `Completed` means the queues were processed within the timeout, not that the
  collector accepted the data: the trace exporter reports `Completed` once its
  worker has drained the queue, whatever the outcome of each batch
  (`src/exporter/otlp_exporter.cpp`, `ForceFlush`). Check `batches_failed`
  and `last_error_message`.
- **If you see `TimedOut`** **→** work was still in flight at the deadline;
  one `ForceFlushTimeout` or `ShutdownTimeout` is counted. Common causes: the
  collector is unreachable and a batch is in its retry backoff, or the
  timeout is shorter than one export. Pass a longer timeout, or lower
  `timeouts.retry_budget_ms` so a dead collector gives up sooner. Defaults are
  `timeouts.flush_ms` and `timeouts.shutdown_ms`, 5 s each
  ([configuration.md](configuration.md) §3.4).
- **If you see `Failed`** **→** a component failed outright rather than timing
  out; in this release that comes from the metrics reader, when collecting or
  exporting a metrics snapshot fails (`src/sdk/periodic_exporting_metric_reader.cpp`).
  `Shutdown` reports the worst status of everything it stopped
  (`src/sdk/sdk_provider.cpp`, `WorseOf`). Read `GetExporterHealth()`.
- **If you see `AlreadyShutDown`** **→** `Shutdown` had already run. Harmless.

---

## Collector connection problems

The OpenTelemetry Collector's plaintext OTLP/HTTP receiver on **:4318 serves
HTTP/1.1 only**. Since v1.2.6, `http://collector:4318` with OTLP/HTTP falls
back to HTTP/1.1 and works; before that, it failed and nothing was delivered
([compatibility-matrix.md](compatibility-matrix.md) §4, issue #166).

- **If exports to `http://…:4318` work but are slow under load** **→** the
  receiver speaks HTTP/1.1, and microtel sends one request at a time on it.
  Use a configuration that keeps HTTP/2:
  - OTLP/gRPC: `http://collector:4317` with `Protocol::Grpc`, or
    `grpc://collector:4317` (the scheme selects gRPC);
  - OTLP/HTTP over TLS: `https://collector:4318` with `Protocol::Http`, where
    ALPN negotiates `h2`.
- **If you see** `peer answered the HTTP/2 preface with an HTTP/1.1 response ...`
  from `Connect()`, or that same text in `last_error_message` **→** this is
  OTLP/gRPC pointed at the HTTP receiver: gRPC never falls back. Use the gRPC
  port (4317).

  The full table, including the h2c-proxy case, is in
  [compatibility-matrix.md](compatibility-matrix.md) §4.
- **If you see** `connection refused` against a collector you believe is up
  **→** check which receivers its config enables and which ports the container
  publishes. 4317 is OTLP/gRPC and 4318 is OTLP/HTTP by convention; pointing
  the gRPC protocol at 4318, or the HTTP protocol at 4317, fails.
- **If the collector logs show nothing at all** **→** the request never reached
  it. Work through [`Provider::Connect()` fails](#providerconnect-fails) with
  `microtel-preflight --preflight=connect`.

---

## The example stack (`examples/stack/`)

The shared Collector + Tempo + Grafana stack the examples export to. Its own
[troubleshooting section](../examples/stack/README.md#troubleshooting) has the
container-level detail; the short version:

- **If an example reports `batches_failed` and a connection error** **→** the
  stack is not up, or something else owns 4317. `curl -s
  http://localhost:13133` should return the collector's health JSON.
- **If `up.sh` says a service never became ready** **→** read the logs; the
  command is printed with the error.
- **If a container exits at once with a permission error on a readable file**
  **→** SELinux labelling under rootless podman: every bind mount needs `:z`.
  `sudo ausearch -m avc -ts recent` confirms it.
- **If a port is already in use** **→** usually 4317, held by a leftover
  conformance collector (`microtel-conformance-<pid>`) or a bench sink. Find it
  with `podman ps`.
- **If traces reach the collector but not Grafana** **→** the collector →
  Tempo hop is at fault, not the example. Search also lags a lookup by trace
  ID by about ten seconds; wait one dashboard refresh.
- **Port 4318 is published but no example uses it**, for the reason in
  [Collector connection problems](#collector-connection-problems).

The leaf-over-MQTT example has its own
[troubleshooting section](../examples/leaf_mqtt/README.md#troubleshooting).

---

## `microtel-preflight` exit codes

`microtel-preflight --preflight={connect|export} [config.toml]` resolves the
configuration as the SDK does (file, then environment), then connects
(`connect`) or connects and exports one span named `microtel.preflight`
(`export`). Source:
`tools/preflight/preflight.cpp`, `tools/preflight/preflight.hpp`.

| Exit | stdout / stderr | Means | Do |
|---|---|---|---|
| 0 | `connect OK` / `export OK` | `connect`: the connection succeeded. `export`: the span was exported and accepted: `ForceFlush` returned `Completed`, `batches_failed` is 0 and the `PartialSuccessRejection` drop counter is 0 | nothing; the collector accepted the span. What it then does with it (its pipelines, processors, sampling) is the collector's business |
| 1 | `Usage: ...` or `error: expected --preflight={connect|export}, got: <arg>` or `error: unknown preflight mode '<mode>' ...` | bad arguments | fix the command line |
| 2 | `error: configuration failed: <message>` | `Build()` failed | look the message up under [`Build()` returns an error](#sdkbuilderbuild-returns-an-error) |
| 3 | `error: connect failed: <message>` | `Connect()` failed | look the message up under [`Provider::Connect()` fails](#providerconnect-fails) |
| 3 | `error: export timed out or failed` | connected, but `ForceFlush` (10 s) did not return `Completed` | the export path: see [No traces arrive](#no-traces-arrive-reading-getexporterhealth) |
| 3 | `error: export failed: the collector did not accept the batch: <last_error_message>` | `ForceFlush` returned `Completed`, but the batch failed (`batches_failed` > 0): the collector rejected it, or it was lost after retries | look the message up under [`batches_failed` and `last_error_message`](#batches_failed-and-last_error_message); `UNIMPLEMENTED (12): unknown service opentelemetry.proto.collector.trace.v1.TraceService` means the collector has no traces pipeline on that receiver |
| 3 | `error: export failed: the collector rejected <N> span(s) via OTLP partial success` | the collector answered success but rejected the span (the `PartialSuccessRejection` drop counter is non-zero); the batch counts as sent, so `batches_failed` is 0 | the collector's own logs say why; see [`PartialSuccessRejection`](#drop-counters) |

Remember that environment variables override the file: an exported
`OTEL_EXPORTER_OTLP_ENDPOINT` in your shell wins over the file you pass.

---

## Leaf receiver (concentrator)

Experimental, and only in builds with `MICROTEL_WITH_CONCENTRATOR=ON`. The API
is [`include/microtel/leaf_receiver.hpp`](../include/microtel/leaf_receiver.hpp);
the design is [leaf-concentrator-design.md](leaf-concentrator-design.md).

- **If `Ingest` returns `IngestStatus::Disabled`** **→** the library was built
  without the concentrator, or the provider was built without
  `WithLeafReceiver` / `[concentrator]`.
- **If `IngestStatus::Malformed` / the `LeafPayloadMalformed` counter rises**
  **→** the payload did not decode or validate, declared an unsupported wire
  version, or declared a time mode its leaf's config does not allow. A
  metrics or logs request handed to `Ingest` usually ends here
  ([error-model.md](error-model.md) §3).
- **If `IngestStatus::TooLarge` / `LeafPayloadTooLarge` rises** **→** raise
  `max_payload_bytes` or `max_spans_per_payload`, or send smaller payloads.
- **If `IngestStatus::UnknownLeaf` / `LeafUnknown` rises** **→** the leaf has no
  configuration and `unknown_leaf = reject`. Add it to `leaves`, answer for it
  from the resolver, or accept unknown leaves.
- **If `IngestStatus::OutOfMemory` / `LeafReceiverStats::payloads_out_of_memory`
  (from `LeafReceiver::Stats()`) rises** (logged as `leaf receiver: allocation failed while ingesting a
  payload; it was not processed (payloads_out_of_memory in
  LeafReceiver::Stats)`) **→** the concentrator itself is short of memory.
- **If you see** `leaf receiver: leaf '<id>': dropped <n> Resource
  attribute(s) over the <bytes>-byte budget; raise <setting> to keep them`
  **→** raise the named setting, or send fewer Resource attributes.
- **If you see** `leaf receiver: the leaf config resolver threw (<what>); the
  leaf is treated as not configured` **→** fix your resolver; until then the
  leaf falls under `unknown_leaf`.

Messages: `src/sdk/leaf_receiver.cpp`.
