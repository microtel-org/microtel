# microtel Configuration

**Status:** M0 deliverable. Documents precedence rules and per-setting resolution.
**Authority:** this document is canonical for setting names and semantics. **Companion:** `error-model.md` §8 (init-failure taxonomy).
**Troubleshooting:** for the message each `Build()` failure prints and what to change, see [`troubleshooting.md`](troubleshooting.md).
**Maintenance:** the precedence rules in §1 are stable; the per-setting tables in §3 are appended to as new settings land in M3+. New settings without a row here are a documentation bug.
**Last verified against source:** the v1.0 release cut — every setter name below checked against [`include/microtel/sdk_builder.hpp`](../include/microtel/sdk_builder.hpp), every TOML key against [`src/common/config/toml_loader.cpp`](../src/common/config/toml_loader.cpp), and every environment variable against [`src/common/config/env_resolver.cpp`](../src/common/config/env_resolver.cpp) (issues #194, #196). Sections that describe an *intended* surface rather than a built one now say so explicitly.

---

## 1. Precedence

Resolved precedence, **highest to lowest** (LOCKED — cites `src/sdk/sdk_builder.cpp:LoadConfig`):

```
1. Explicit code options (e.g., SdkBuilder::WithEndpoint)
2. Environment variables (OTEL_* and MICROTEL_*)
3. microtel.toml entries
4. Built-in defaults
```

**Resolution is per-setting**, not per-source. If `service.name` is set in code and `service.version` is set in env, both win in their respective slots; neither shadows the other.

**Each key of a table is its own setting.** For the table-valued settings — `[resource]` / `WithResource` / `OTEL_RESOURCE_ATTRIBUTES` (§3.2) and `[exporter.headers]` / `WithHeaders` / `OTEL_EXPORTER_OTLP_HEADERS` (§3.3) — a higher-precedence source overrides only the keys it names; every other key survives from the lower-precedence sources. `OTEL_RESOURCE_ATTRIBUTES=host.rack=b12` over a `[resource]` table that sets `deployment.environment` and `service.namespace` resolves to all three. No source replaces a whole table (issue #257; changed in v1.1.1, before which a higher source replaced the table wholesale).

**OTel-standard `OTEL_*` env vars are honoured alongside microtel's own `MICROTEL_*` ones.** Where both an OTEL and a MICROTEL env var name the same setting, MICROTEL wins (it is more specific to this implementation). v1 does not currently define any such overlap; if one is added later it must be called out here.

**Strict-by-default unknown keys.** Unknown keys in `microtel.toml` raise `ConfigError::Kind::UnknownKey` at `Build()` time. Mixed-version deployments may relax via:

```toml
[config]
unknown_keys = "error"   # error (default) | warn | ignore
```

Under `"warn"`, `Build()` logs one Warn line per unknown key, naming its dotted path, and succeeds:
`[microtel warn] unknown configuration key "exporter.endpiont" ignored ([config] unknown_keys = "warn")`.
Under `"ignore"` the key is skipped silently. Either way the key has no effect. (Through v1.2.0, `"warn"` logged nothing and so behaved exactly like `"ignore"`.)

---

## 2. Sources

### 2.1 Code

Method calls on `SdkBuilder`. Highest precedence. Each `WithXxx` call records the setting; `Build()` resolves precedence and validates.

### 2.2 Environment variables

Read at `Build()` time. Both `OTEL_*` and `MICROTEL_*` namespaces. Per-signal exporter env vars (e.g., `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT`, `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT`) are not implemented and are **ignored, not rejected**. Traces, metrics and logs all export through the shared `OTEL_EXPORTER_OTLP_*` settings. The one per-signal variable read is `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE` (§3.13).

The key=value list variables (`OTEL_EXPORTER_OTLP_HEADERS`, `OTEL_RESOURCE_ATTRIBUTES`, `MICROTEL_CONCENTRATOR_RESOURCE_ATTRIBUTES`) use the W3C Baggage list format without metadata, as the OTel spec specifies. Whitespace around each key and value is dropped, so `a=1, b=2` sets `b`. Values are percent-decoded: `authorization=Bearer%20<token>` sends `Bearer <token>`, and a literal `%` is `%25`. Keys are not decoded. A `%` not followed by two hex digits fails `Build()` with `EnvParseFailure` (issue #413).

### 2.3 `microtel.toml`

**v1 loads a config file only when you name one.** The single path is the one
passed to `SdkBuilder::FromFile(path)`, read at `Build()` time. If it does not
exist, `Build()` fails with `ConfigError::Kind::FileNotFound` — a file you asked
for and did not get is an error, not a fallback.

There is no search path. `MICROTEL_CONFIG_FILE`, `./microtel.toml` in the
working directory and `/etc/microtel/microtel.toml` are **not consulted**;
earlier revisions of this section listed all three as tiers 2–4 and none was
ever implemented (`src/sdk/sdk_builder.cpp` has exactly one `LoadToml` call
site, guarded by `FromFile`). Without `FromFile`, microtel resolves from env
plus defaults and never touches the filesystem.

Discovery is deferred, not rejected; if it lands it belongs in this section with
the precedence between tiers spelled out.

**Top-level tables recognised in the file:** `[config]`, `[exporter]`,
`[service]`, `[resource]`, `[tls]`, `[sdk]`, `[timeouts]`, `[logging]`,
`[concentrator]`. Any other top-level
table is an unknown key and, under the default policy, fails `Build()` — so the
§3 tables below are the whole TOML surface, not a subset of it.

### 2.4 Built-in defaults

Compiled into the library. Lowest precedence; the only source guaranteed to be present.

---

## 3. Per-setting precedence tables

Each row covers one setting. Columns: TOML key, equivalent code call (where applicable), OTEL env var (if any), MICROTEL env var (if any), default, validation.

Rows are alphabetised within each subsection.

### 3.1 Service identity

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `service.name` | `WithServiceName(s)` | `OTEL_SERVICE_NAME` | — | `"unknown_service"` | §3.2 |
| `service.version` | `WithServiceVersion(s)` | (in `OTEL_RESOURCE_ATTRIBUTES`) | — | empty | §3.2 |

The `service.name` resource attribute is always present. When nothing supplies
a value, `config::Validate` resolves it to `unknown_service`, the placeholder
the OTel resource semantic conventions specify so that a backend always has
something to group on. microtel does not append an executable name — the OTel
conventions permit `unknown_service:<process name>`, but the plain form is what
it emits. Since v1.1 the placeholder is a *default* rather than a final answer:
it sits below the detector layer in the §3.2 composition order, so a detector
that supplies `service.name` replaces it. `service.version` has no such
requirement and stays absent when unset.

(Before #203 landed, no fallback existed and the attribute was omitted
entirely.)

### 3.2 Resource attributes

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `[resource]` table | `WithResource({...})` | `OTEL_RESOURCE_ATTRIBUTES` (csv `k=v,k=v`; see §2.2) | — | empty | Merged per key across sources (§1): each source overrides only the keys it names. Keys compare exactly (case-sensitive). Detector contributions sit below all of them. |
| — | `WithResourceDetector(d)` | — | — | no detectors | Registration order is significant; a later detector overrides an earlier one. |
| `sdk.resource_detectors_strict` | — | — | `MICROTEL_RESOURCE_DETECTORS_STRICT` | `false` (lenient) | `true`/`1` or `false`/`0`; any other env value is `ConfigError::EnvParseFailure`. |

**Composition order.** `Build()` merges three layers, key by key, each
overriding the one before it (`Resource::Merge`):

1. Built-in defaults — the `unknown_service` placeholder, and only when nothing
   configured a service name.
2. Detectors, in `WithResourceDetector` registration order.
3. Configuration — itself resolved key by key in §1's order: `microtel.toml`
   (`[service]`, `[resource]`), then environment (`OTEL_SERVICE_NAME`,
   `OTEL_RESOURCE_ATTRIBUTES`), then code (`WithServiceName`, `WithResource`).

A detector therefore cannot override anything an operator configured, but it
*can* replace the `unknown_service` placeholder, which is a default rather than
a configured value.

**Built-in detectors**, from `microtel/resource_detectors.hpp` — neither is
registered automatically:

| Factory | Attributes |
|---|---|
| `MakeProcessDetector(root = "/")` | `process.pid`, `process.executable.path`, `process.executable.name`, `process.command`, `process.command_args` |
| `MakeHostDetector(root = "/")` | `host.name`, `host.id` |

Both take a path root that every filesystem source is resolved against — `/proc/self/exe`,
`/proc/self/cmdline`, `/etc/machine-id` with a `/var/lib/dbus/machine-id`
fallback. It defaults to the real filesystem and exists so tests can point a
detector at a fixture tree; it is string composition, not a chroot, and does not
affect `getpid(2)` or `gethostname(2)`.

**Strict vs lenient.** Under the default lenient policy a detector that returns
a `ConfigError` is logged at `Warn` and skipped, and the rest of the pipeline
builds. Under `resource_detectors_strict` the first such failure fails `Build()`
with that detector's error and no later detector runs. An attribute a detector
merely *omits* — a hidden `/proc/self/exe`, a container with no machine-id — is
not a failure under either policy.

### 3.3 Exporter — endpoint and protocol

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `exporter.endpoint` | `WithEndpoint(s)` | `OTEL_EXPORTER_OTLP_ENDPOINT` | — | none (required) | If unset, `Build` fails with `ConfigError::EndpointMalformed`. |
| `exporter.protocol` | `WithProtocol(p)` | `OTEL_EXPORTER_OTLP_PROTOCOL` | — | **`http`**, or `grpc` for a `grpc://` / `grpcs://` endpoint | `http` or `grpc`. See "Endpoint scheme and protocol" below. |
| `exporter.compression` | `WithCompressionGzip(b)` | `OTEL_EXPORTER_OTLP_COMPRESSION` | — | off | TOML/env value is `gzip` to enable; anything else is off. The code setter is a `bool`, not a codec name — gzip is the only compression v1 implements. Controls **requests**: gzip request bodies with `content-encoding: gzip` (HTTP) or frame flag `0x01` with `grpc-encoding: gzip` (gRPC). Responses are independent — `accept-encoding` / `grpc-accept-encoding: gzip` is advertised whatever this is set to, and a compressed response is inflated under `MemoryLimitOptions::max_decompressed_bytes`. |
| `[exporter.headers]` table | `WithHeaders({...})` | `OTEL_EXPORTER_OTLP_HEADERS` (csv `k=v,k=v`; see §2.2) | — | empty | Merged per key across sources (§1): each source overrides only the headers it names. Header names compare ASCII case-insensitively (`authorization` overrides `Authorization`; the higher source's spelling is kept). Static headers, fixed for the process lifetime; runtime auth via `WithAuthProvider` is separate. `Build()` fails with `InvalidValue` on `exporter.headers.<name>` for a name that HTTP/2 forbids or that microtel sets itself: a pseudo-header or other non-token name; `connection`, `keep-alive`, `proxy-connection`, `transfer-encoding`, `upgrade`, `te`, `host`; on gRPC `content-type`, `user-agent`, `grpc-encoding`, `grpc-accept-encoding`, `content-length`; on HTTP/protobuf `content-type`, `content-length`, `content-encoding`, `accept-encoding`. A static `authorization` fails the same way when `WithAuthProvider` is set ([ICP 0038](icps/0038-reject-reserved-request-headers.md)), and so does a value with CR, LF or NUL, or with a space or tab at either end (RFC 9113 §8.2.1, #412). `WithAuthProvider` has no TOML or env surface — it takes a callable. See [`auth-callback-recipes.md`](auth-callback-recipes.md) for OAuth2 and SigV4, and for what the callback runs on (an exporter worker) before you put I/O in it. |

**Endpoint scheme and protocol.** Four schemes are accepted. Two of them are
microtel shorthand that carries a protocol; two say nothing about it.

| Endpoint scheme | Transport | Effect on `protocol` |
|---|---|---|
| `https://` | TLS | none — `protocol` keeps its configured value (default `http`) |
| `http://` | plaintext h2c | none — `protocol` keeps its configured value (default `http`) |
| `grpcs://` | TLS | selects `grpc` unless `protocol` was set explicitly |
| `grpc://` | plaintext h2c | selects `grpc` unless `protocol` was set explicitly |

> **Warning: `http://` with OTLP/HTTP is HTTP/2 with prior knowledge (h2c).**
> microtel has no HTTP/1.1 mode, and most plaintext OTLP/HTTP receivers serve
> HTTP/1.1 only, including the stock OpenTelemetry Collector `otlp` receiver
> on `:4318`. Against one of those, `Build()` logs a warning and `Connect()`
> fails with `Error::Kind::Protocol` ("peer answered the HTTP/2 preface with an
> HTTP/1.1 response"), and nothing is delivered. What works instead:
> `grpc://host:4317` (OTLP/gRPC is h2c by definition), or `https://host:4318`,
> where TLS negotiates `h2` through ALPN. Keep `http://` with OTLP/HTTP only
> for a receiver that speaks h2c. See
> [`compatibility-matrix.md` §4](compatibility-matrix.md#4-plaintext-otlphttp-cannot-reach-a-stock-collector).

So `WithEndpoint("grpc://collector:4317")` alone speaks OTLP/gRPC, and picks up
the gRPC default port 4317 when the URL omits a port. "Set explicitly" means any
of `WithProtocol(p)`, the `exporter.protocol` TOML key, or
`OTEL_EXPORTER_OTLP_PROTOCOL` — not the built-in default.

**A `grpc://` or `grpcs://` endpoint combined with an explicit `protocol =
"http"` is rejected** at `Build()` with `ConfigError::Kind::ProtocolMismatch`
and `field = "exporter.protocol"`. The two statements contradict each other and
microtel resolves the contradiction in neither direction. Use an `http://` or
`https://` endpoint for OTLP/HTTP, or drop the explicit protocol.

`https://` plus an explicit `protocol` is still the canonical
form, and it remains the unambiguous spelling; the shorthand is a convenience,
not a replacement. (Before #203 landed, the shorthand was normalised for TLS
only and left `protocol` at its default, so `grpc://` silently spoke OTLP/HTTP
at a gRPC port.)

**No per-signal env vars.** `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` and
`OTEL_EXPORTER_OTLP_TRACES_PROTOCOL` were listed here and are read by nothing —
`src/common/config/env_resolver.cpp` handles the unsuffixed forms only. Setting
a `_TRACES_` variable changes no behaviour and produces no warning. Per §2.2
that silence is deliberate for signals microtel does not implement; for traces
it is a gap, not a policy.

### 3.4 Exporter — timeouts (six independent)

All six are set in code through the single `WithTimeouts(TimeoutOptions)` setter
— there are no per-axis `With…Timeout` methods — and in TOML under one
`[timeouts]` table, whose keys are integer milliseconds.

| TOML (`[timeouts]`) | Code (`WithTimeouts({…})`) | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `connect_ms` | `.connect = d` | — | — | 10s |
| `tls_ms` | `.tls_handshake = d` | — | — | 10s |
| `per_export_ms` | `.per_export = d` | `OTEL_EXPORTER_OTLP_TIMEOUT` (ms) | — | 10s |
| `retry_budget_ms` | `.retry_budget = d` | — | — | 60s |
| `flush_ms` | `.flush = d` (default for `ForceFlush`) | — | — | 5s |
| `shutdown_ms` | `.shutdown = d` (default for `Shutdown`) | — | — | 5s |

```toml
[timeouts]
retry_budget_ms = 5000
```

Caller-provided timeouts to `ForceFlush(timeout)` and `Shutdown(timeout)` override the configured default.

`retry_budget` caps the total elapsed time across all retry attempts for a
single batch, for traces, metrics and logs alike (issue #222); the remaining retry parameters (attempt count, backoff shape,
jitter) are not configurable in v1 and keep the OTLP-recommended defaults in
`RetryPolicyConfig`. The budget is checked between attempts, not enforced
against one in flight: before each backoff the loop looks ahead and stops if
that sleep would reach or pass the budget, so no backoff ever runs beyond it
(issue #195). An attempt already in flight still can — it is bounded by
`per_export`, not by `retry_budget`. Treat `retry_budget` as the point at which
microtel stops starting new attempts, not as a hard deadline.

### 3.5 Exporter — TLS

All five fields are set in code through the single `WithTls(TlsOptions)` setter
— there are no per-field `With…` methods — and in TOML under a **top-level
`[tls]` table**, not under `[exporter]`.

| TOML (`[tls]`) | Code (`WithTls({…})`) | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `insecure` | `.insecure = b` | — | — | `false` |
| `ca_bundle` | `.ca_bundle = p` | `OTEL_EXPORTER_OTLP_CERTIFICATE` | — | (system trust) |
| `client_cert` | `.client_cert = p` | — | — | (none) |
| `client_key` | `.client_key = p` | — | — | (none) |
| `sni_override` | `.sni_override = s` | — | — | (host from endpoint) |

```toml
[tls]
ca_bundle = "/etc/pki/tls/certs/collector-ca.pem"
```

`insecure=true` emits a `Warn`-level log line from `Build()`. There is no way to
suppress it short of installing a filtering `SetLogSink`.

**`MICROTEL_FORBID_INSECURE_TLS=ON` refuses `insecure=true` at initialisation.**
The CMake option compiles `MICROTEL_FORBID_INSECURE_TLS=1` into
`microtel_config`, and `config::Validate` fails a configuration carrying
`tls.insecure = true` with `ConfigError::Kind::InsecureDisallowed` and
`field = "tls.insecure"` — so `SdkBuilder::Build()` returns the error instead of
a `Provider`, and there is no runtime way to turn verification back off. The
option is a property of the build, not of the configuration: a library compiled
with it OFF cannot be made to refuse, and one compiled with it ON cannot be made
to accept. Default builds (`OFF`) keep the `Warn` line described above.

Two corrections against what this section used to claim (#196): the TOML paths
were given as `exporter.tls.*`, which is an **unknown key** — `[exporter]`
accepts only `endpoint`, `protocol`, `compression` and `headers`, so that
spelling fails `Build()` under the default policy rather than configuring TLS.
And `OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE` / `OTEL_EXPORTER_OTLP_CLIENT_KEY`
are read by nothing: mTLS material is code or TOML only. `OTEL_EXPORTER_OTLP_CERTIFICATE`
is the one TLS env var that works.

### 3.6 Exporter — proxy

**Not implemented in v1.** microtel connects directly to the configured
endpoint. There is no TOML or code surface, and the environment variables below
are **reserved names, read by nothing** — setting one changes no behaviour and
produces no warning. Deferred; see [`roadmap.md`](roadmap.md)
and [`compatibility-matrix.md`](compatibility-matrix.md) §3.

| Variable | Effect in v1 | Intended effect |
|---|---|---|
| `https_proxy` / `HTTPS_PROXY` | none | proxy `https://` endpoints via `CONNECT` |
| `http_proxy` / `HTTP_PROXY` | none | proxy `http://` endpoints |
| `no_proxy` / `NO_PROXY` | none | comma-separated bypass list, leading-dot subdomain matching |

### 3.7 Batch span processor

All four are set in code through the single `WithBatch(BatchOptions)` setter and
in TOML under the **`[sdk]` table** — not `[batch]`, which is an unknown key.

| TOML (`[sdk]`) | Code (`WithBatch({…})`) | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `max_queue_size` | `.max_queue_size = N` | — | — | 8192 |
| `max_export_batch_size` | `.max_export_batch_size = N` | — | — | 512 |
| `schedule_delay_ms` | `.schedule_delay = d` | — | — | 5000 ms |
| `drop_policy` | `.drop_policy = p` | — | — | `"newest"` / `DropPolicy::DropNewest` |

The `[sdk]` table also carries `resource_detectors_strict`, which is not a batch
setting — see §3.2.

```toml
[sdk]
max_queue_size = 16384
drop_policy = "oldest"
```

`drop_policy` takes `"newest"` or `"oldest"` in TOML (any other value is
`ConfigError::InvalidValue` on field `sdk.drop_policy`) and
`DropPolicy::DropNewest` / `DropPolicy::DropOldest` in code. The drop path is
[`sequences/backpressure-and-drop.md`](sequences/backpressure-and-drop.md).

`Build()` rejects an incoherent combination with `ConfigError::InvalidValue`,
naming the first field at fault, whether it came from code or from TOML:

| Rejected | `field` |
|---|---|
| `max_queue_size = 0` | `sdk.max_queue_size` |
| `max_export_batch_size = 0` | `sdk.max_export_batch_size` |
| `max_export_batch_size > max_queue_size` | `sdk.max_export_batch_size` |
| `schedule_delay_ms <= 0` | `sdk.schedule_delay_ms` |

A zero queue refuses every record, a zero batch size never drains the queue,
and a non-positive delay turns the worker's wait into a spin. The smallest
accepted combination is `1` / `1` / `1 ms`, and a batch size equal to the queue
size is accepted. These are exactly the rules `Provider::SetBatchOptions`
applies at runtime — both call one validator — so a value the setter answers
`InvalidArgument` for also fails `Build()`, and the reverse. Before v1.2
`Build()` checked only the third rule (ICP 0026 Discrepancy 1, issue #267).

Three corrections (#196): the TOML table is `[sdk]`, not `[batch]`; the delay
key is `schedule_delay_ms` (integer milliseconds), not `schedule_delay`; and
there is no `WithDropPolicy` setter — it is a `BatchOptions` field. The
`OTEL_BSP_*` env vars listed here are read by nothing.

### 3.8 Memory budgets (`memory-model.md` §6)

**Code-only in v1.** All five are fields of `MemoryLimitOptions`, set through
`WithMemoryLimits(MemoryLimitOptions)`. There is no `[limits]` TOML table and no
environment variable; the values are plain integer bytes.

| Code (`WithMemoryLimits({…})`) | TOML | Env | Default | Enforced at |
|---|---|---|---|---|
| `.max_total_queue_bytes = n` | — | — | 16 MiB | `BatchSpanProcessor::OnEnd`, against the summed estimate of everything queued (counter `queue_full`) |
| `.max_record_bytes = n` | — | — | 64 KiB | `BatchSpanProcessor::OnEnd`, before the record is queued (counter `record_too_large`) |
| `.max_response_bytes = n` | — | — | 1 MiB | the transport, as the response body is accumulated (counter `response_too_large`) |
| `.max_trailer_bytes = n` | — | — | 64 KiB | the transport, as the trailers are accumulated (also counter `response_too_large`); also advertised as `SETTINGS_MAX_HEADER_LIST_SIZE`, which is what bounds the response headers (issue #213) |
| `.max_decompressed_bytes = n` | — | — | 4 MiB | the wire codec, as a gzipped response inflates (counter `decompression_too_large`) |

Corrections (#196): the five `WithMax…Bytes(n)` setters this section named do
not exist, the `limits.*` TOML keys are unknown keys that fail `Build()`, and
the SI-suffix parsing (`"16MiB"`, `"4MB"`) was never implemented. A TOML surface
for these is deferred; adding one means a `[limits]` entry in the top-level
known-key list in `src/common/config/toml_loader.cpp` and a row in this table.

### 3.9 Span structural limits (`memory-model.md` §7)

**Code-only in v1**, through `WithSpanLimits(SpanLimitOptions)`. No TOML table,
no environment variables.

| Code (`WithSpanLimits({…})`) | TOML | OTEL env | Default | Enforced at |
|---|---|---|---|---|
| `.attribute_count_limit = N` | — | — | 128 | `SdkSpan::SetAttribute` (counter `span_attribute_limit`) |
| `.event_count_limit = N` | — | — | 128 | `SdkSpan::AddEvent` (counter `span_event_limit`) |
| `.link_count_limit = N` | — | — | 128 | `SdkSpan::AddLink` (counter `span_link_limit`) |
| `.attribute_value_length_limit = N` | — | — | 4096 | every string attribute copy — `SetAttribute` and the event / link attribute copies. **Bytes**, cut back to a UTF-8 code point boundary; the attribute is truncated, not dropped (counter `attribute_value_truncated`) |
| `.event_attribute_count_limit = N` | — | — | 128 | `SdkSpan::AddEvent` (counter `event_attribute_limit`) |
| `.link_attribute_count_limit = N` | — | — | 128 | `SdkSpan::AddLink` (counter `link_attribute_limit`) |

Correction (#196): the `span_limits.*` TOML keys and the six `OTEL_SPAN_*` /
`OTEL_ATTRIBUTE_VALUE_LENGTH_LIMIT` / `OTEL_EVENT_ATTRIBUTE_COUNT_LIMIT` /
`OTEL_LINK_ATTRIBUTE_COUNT_LIMIT` env vars this table listed are read by
nothing. The limits themselves are enforced; only the two configuration
surfaces were fictional.

### 3.10 Sampling

**Code-only in v1**, through `WithSampler(SamplerHandle)`. The handle comes from
a factory in [`microtel/sampler.hpp`](../include/microtel/sampler.hpp):

| Factory | Behaviour |
|---|---|
| `MakeAlwaysOnSampler()` | sample every span (the builder's default) |
| `MakeAlwaysOffSampler()` | sample nothing |
| `MakeTraceIdRatioSampler(r)` | sample a deterministic fraction `r` ∈ [0.0, 1.0] of trace IDs |
| `MakeParentBasedSampler(root)` | follow the parent's decision; use `root` when there is no parent |

`MakeTraceIdRatioSampler` clamps `r` into `[0.0, 1.0]` and normalises NaN to
`0.0`, so a NaN ratio samples nothing (#247). `Provider::SetSamplerRatio`
rejects NaN and out-of-range values instead of clamping — see
[ICP 0026](icps/0026-provider-setters.md) Decision 3.

v1.1 adds rule combinators and chain composition, still code-only:

| Factory | Behaviour |
|---|---|
| `MakeAttributeRuleSampler(key, value, on_match, on_no_match)` | delegate by whether an initial attribute equals `value` |
| `MakeSpanNameRuleSampler(name, on_match, on_no_match)` | delegate by exact span-name match |
| `MakeSpanKindRuleSampler(kind, on_match, on_no_match)` | delegate by span kind |
| `MakeChainSampler(children, ChainMode::FirstMatch)` | the first child whose predicate matches decides; a child with no predicate matches unconditionally |
| `MakeChainSampler(children, ChainMode::AllMustAgree)` | every child must answer `RecordAndSample`, else `Drop` |

There is no sample-on-duration rule: `ShouldSample` runs at span start, so
duration-based selection is tail sampling and belongs in the collector — see
[ICP 0024](icps/0024-v1.1-rescope.md). `SamplerHandle` is move-only, so
`MakeChainSampler` takes a `std::vector<SamplerHandle>` (or a variadic pack)
rather than an initializer list.

Corrections (#196): there is no `[sampling]` TOML table, and `OTEL_TRACES_SAMPLER`
/ `OTEL_TRACES_SAMPLER_ARG` are read by nothing — so the sampler **names**
(`always_on`, `parentbased_traceidratio`, …) this section listed as "accepted"
are accepted nowhere. Nothing in the tree parses a sampler from a string.
`parentbased_always_on` was also given as the default; the builder's default is
a bare always-on sampler, not a parent-based one.

### 3.11 Logging

**Sink selection is still a runtime injection, not a setting.**
`SetLogSink(LogSink)` installs a callback receiving `(LogLevel,
std::string_view)`; the application routes microtel's internal logs wherever it
likes (`error-model.md` §9.3), and there is no route into spdlog inside
microtel — an application that wants one installs `microtel_spdlog_bridge` as
its sink, in its own build (ICP 0026 §6, issue #190).

**The minimum level is a setting** as of v1.1 (ICP 0026 §6):

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `logging.level` | `Provider::SetLogLevel(level)` | — | `MICROTEL_LOG_LEVEL` | `info` |

Values: `trace`, `debug`, `info`, `warn`, `error`. Anything else is rejected at
`Build()` — `ConfigError::Kind::InvalidValue` for the TOML key,
`EnvParseFailure` for the env var — rather than silently read as the default,
because an operator who raises the level to debug a problem and quietly gets
`info` back has been told nothing.

Records below the level are dropped before the sink is consulted. The default
changes nothing observable: every production emission site logs at `Warn`.

Unlike every other row in this document, the code column is **not** an
`SdkBuilder` setter. The filter is process-global, because the internal log
entry point (`microtel::internal::LogImpl`) is a free function called from code
with no provider in scope — so providers built from different profiles share
the knob and the last writer wins. `Provider::SetLogLevel` is the operator
surface, and it is live: it retunes the level after `Build()` without a
restart, which is the whole point of putting it among the four hot-reload
setters. `[logging]` accepts no other key; anything else there is an unknown
key under §1's strict default.

Correction (#196), still standing for the rest of the section: the
`logging.sink` / `logging.file` TOML keys and the `MICROTEL_LOG_SINK` /
`MICROTEL_LOG_FILE` env vars are read by nothing, and neither the `journald`
nor the `syslog` sink once named here exists. They are the sink's business
rather than the level's, and ICP 0026 retires the correction for
`logging.level` only. The v1.0 design named a wider surface; this section
describes the built one.

### 3.12 Configuration meta

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `config.unknown_keys` | — | — | — | `error` |

Values: `error` (default), `warn`, `ignore`. `warn` logs one Warn line per
unknown key at `Build()` (see §1); `ignore` logs nothing. Correction (#196): there is no
`WithUnknownKeysPolicy` setter — the policy is TOML-only, which follows from
what it governs (it is parsed first, out of `[config]`, to establish the mode
every later section's unknown-key check runs under).

### 3.13 Metrics pipeline

Metrics are implemented but experimental until v1.3 (see the root `README.md`
status table). The configuration surface that exists today is code plus three
environment variables; there is no TOML table.

| Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|
| `WithMetricInterval(d)` | `OTEL_METRIC_EXPORT_INTERVAL` (ms) | — | 60 s |
| `WithMetricTemporality(p)` | `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE` (`cumulative` \| `delta` \| `lowmemory`) | — | `Cumulative` |
| `WithMetricLimits({.max_cardinality = n})` | — | `MICROTEL_METRIC_CARDINALITY_LIMIT` (decimal integer) | 2000 |
| `WithView(ViewConfig)` | — | — | no views |

### 3.14 Concentrator (leaf receiver)

Experimental in v1.2, and compiled in only with `-DMICROTEL_WITH_CONCENTRATOR=ON`
(§4). The design is [`leaf-concentrator-design.md`](leaf-concentrator-design.md)
§4–§5. Three sources set the same `LeafReceiverOptions`
([`include/microtel/leaf_receiver.hpp`](../include/microtel/leaf_receiver.hpp)):
the `[concentrator]` TOML table, the `MICROTEL_CONCENTRATOR_*` variables, and
`SdkBuilder::WithLeafReceiver(opts)`. The receiver is off unless one of them
turns it on. In a build without the option, a source that enables it fails
`Build()` with `ConfigError::Kind::InvalidValue` on field `concentrator.enabled`.

| TOML (`[concentrator]`) | Code (`LeafReceiverOptions`) | MICROTEL env | Default |
|---|---|---|---|
| `enabled` | `.enabled` (implied by `WithLeafReceiver`) | `MICROTEL_CONCENTRATOR_ENABLED` (`true`/`1`/`false`/`0`) | `false` |
| `max_payload_bytes` | `.max_payload_bytes` | `MICROTEL_CONCENTRATOR_MAX_PAYLOAD_BYTES` | 64 KiB |
| `max_spans_per_payload` | `.max_spans_per_payload` | — | 512 |
| `max_leaves` | `.max_leaves` | `MICROTEL_CONCENTRATOR_MAX_LEAVES` | 1024 |
| `max_leaf_resource_bytes` | `.max_leaf_resource_bytes` | — | 2 KiB |
| `max_node_resource_bytes` | `.max_node_resource_bytes` | — | 16 KiB |
| `leaf_idle_timeout` | `.leaf_idle_timeout` | — | `"1h"` |
| `unknown_leaf` | `.unknown_leaf` | `MICROTEL_CONCENTRATOR_UNKNOWN_LEAF` | `"accept"` |
| `leaf_id_attribute` | `.leaf_id_attribute` | — | `"device.id"` |
| `default_time_mode` | `.default_time_mode` (unset = `auto`) | `MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE` | `"auto"` |
| `max_sync_age` | `.max_sync_age` | — | `"1h"` |
| `max_clock_skew` | `.max_clock_skew` | — | `"5m"` |
| `boot_anchor_window` | `.boot_anchor_window` | — | `"10m"` |
| `[concentrator.leaf_defaults.resource]` | `.leaf_defaults_resource` | `MICROTEL_CONCENTRATOR_RESOURCE_ATTRIBUTES` (`OTEL_RESOURCE_ATTRIBUTES` syntax) | empty |
| `[concentrator.leaves."<id>"]` `time_mode`, `.resource` | `.leaves` | — | none |
| — | `.resolver` | — | none |

```toml
[concentrator]
enabled           = true
max_payload_bytes = "64KiB"
leaf_idle_timeout = "1h"
unknown_leaf      = "accept"                   # accept | reject
default_time_mode = "auto"                     # auto | concentrator_stamped | sync_relative | boot_relative | unix

[concentrator.leaf_defaults.resource]
"deployment.environment" = "prod"

[concentrator.leaves."can0:0x1a4"]
time_mode = "boot_relative"
[concentrator.leaves."can0:0x1a4".resource]
"service.name" = "burner-controller"

[concentrator.leaves."192.168.7.20:40001"]     # a full C++ node (ICP 0036)
time_mode = "unix"
```

**Values.** A byte size is an integer number of bytes or a string with a `B`,
`KiB` or `MiB` suffix (`"64KiB"`); the environment takes the same strings. A
duration is a string with an `s`, `m` or `h` suffix (`"30s"`, `"5m"`, `"1h"`);
a bare number is refused, so it can never be read in the wrong unit. A leaf's
`time_mode` is one of the three wire modes or `unix` (not `auto`). Resource values are
strings, integers, floats or booleans; quote dotted keys (`"service.name"`),
since an unquoted one is a nested TOML table and is refused. Unlike the older
tables, a key present with the wrong type is an error, not skipped.

**Precedence.** Code over environment over file over default, per setting, as
§1 says, with these specifics (design §4.3):

- `WithLeafReceiver` sets every scalar and the resolver. A
  `LeafReceiverOptions` value cannot say which of its fields were set on
  purpose, so, as with `WithBatch`, the environment's and the file's scalars
  apply only when `WithLeafReceiver` is not called.
- `leaf_defaults.resource` merges per key: code over environment over file.
- `leaves` merges per leaf id; within one leaf, `resource` merges per key and a
  code `time_mode` replaces the file's only when the code sets one.
- The resolver's answer for a leaf sits above that leaf's static entry, per
  key, and its `time_mode` replaces the static one when it sets one.

Per-leaf settings have no environment form: leaf ids hold characters that are
not valid in variable names.

**Full nodes: `unix` and `max_node_resource_bytes`**
([ICP 0036](icps/0036-custom-export-transport.md), design §5.6). `unix` is a
configuration value only, for a sender that is a full C++ microtel node
exporting through an `ExportTransport`: its payloads carry no leaf header and
its timestamps are already Unix time. A payload with no `microtel.leaf.*` key
is accepted only from a sender whose effective mode (its own `time_mode`, else
`default_time_mode`, including `MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE=unix`)
is `unix`, and its timestamps are used unchanged; `auto` still refuses it.
Every leaf-table entry whose effective mode is `unix` gets
`max_node_resource_bytes` as its Resource budget in place of
`max_leaf_resource_bytes`; it has no environment variable, like
`max_leaf_resource_bytes`. When an entry's Resource loses attributes to its
budget, one `Warn` names the leaf, the count and the setting to raise.

**Validation.** Parse errors name the key: `ConfigError::Kind::InvalidValue`
for a wrong type or value (field `concentrator.<key>`, or
`concentrator.leaves.<id>.<key>`), `EnvParseFailure` for a variable (field =
the variable), and `UnknownKey` anywhere in the table under the default
`[config] unknown_keys = "error"`. `Build()` then refuses, with
`InvalidValue`:

| Rejected | `field` |
|---|---|
| `max_payload_bytes`, `max_spans_per_payload` or `max_leaves` = 0 | `concentrator.<key>` |
| `leaf_idle_timeout`, `max_sync_age`, `max_clock_skew` or `boot_anchor_window` ≤ 0 | `concentrator.<key>` |
| a leaf id that is empty or over 128 bytes | `concentrator.leaves` |
| a `microtel.leaf.*` key, or the `leaf_id_attribute` key, in a configured Resource | `concentrator.leaf_defaults.resource` or `concentrator.leaves.<id>.resource` |
| a configured Resource over its budget: `max_node_resource_bytes` for a leaf whose effective time mode is `unix` (for `leaf_defaults.resource`, when `default_time_mode` is), else `max_leaf_resource_bytes` | the same |

A resolver's answer cannot be refused at `Build()`: in it, reserved keys and
the `leaf_id_attribute` key are ignored, and keys over the Resource budget are
dropped and counted in `LeafReceiverStats::resource_attributes_dropped`. A
resolver that throws is treated as answering "not configured".

With `unknown_leaf = "reject"`, a "not configured" answer is remembered for up
to 60 s (at most 256 leaf ids, oldest first out) and the leaf's payloads are
refused before decoding meanwhile; a leaf added to the resolver's inventory is
accepted within that time. Refused leaves take no place in the `max_leaves`
table (design §4.5).

### 3.15 Application export transport (`WithExportTransport`)

`SdkBuilder::WithExportTransport(transport, ExportTransportOptions)` replaces the HTTP/2 exporter with an application-supplied `microtel::ExportTransport` ([ICP 0036](icps/0036-custom-export-transport.md)). It is code-only: there is no TOML key or environment variable.

| `ExportTransportOptions` field | Default | Notes |
|---|---|---|
| `traces` | `true` | Off: the sampler is replaced by always-off, so no span is recorded; one `Warn` at `Build()`. |
| `metrics` | `false` | Off: `GetMeter` returns a no-op meter and no metrics pipeline or worker is built; one `Warn` the first time. |
| `logs` | `false` | Off: `GetLogger` returns a no-op logger and no logs pipeline or worker is built; one `Warn` the first time. |
| `max_request_bytes` | `65536` | Cap, in encoded uncompressed bytes, on joining trace batches into one request; `0` for no cap. A single batch above it is sent whole with a rate-limited `Warn`. Keep it at or below the receiver's limit (a concentrator's `max_payload_bytes`) and, to avoid fragmenting, the link's frame size (UDP: under 65,507). |

The other exporter settings have no meaning on this path:

| Setting | Set in code with `WithExportTransport` | Set by the environment or file |
|---|---|---|
| endpoint, protocol, headers, TLS, compression (`WithEndpoint`, `WithProtocol`, `WithHeaders`, `WithTls`, `WithCompressionGzip`; `OTEL_EXPORTER_OTLP_*`, `[exporter]`) | `Build()` fails: `ConfigError::Kind::InvalidValue`, `field = "exporter.transport"` | Ignored, with one `Warn` naming them. Not validated either, so a malformed inherited `OTEL_EXPORTER_OTLP_ENDPOINT` does not fail the build. |
| `WithAuthProvider` | `Build()` fails, as above | — (code-only) |
| `timeouts.per_export` | Each `Send`'s deadline is now plus this, clamped to the shutdown deadline once `Shutdown` has begun | same |
| `timeouts.retry_budget`, `flush`, `shutdown` | Apply as for HTTP | same |
| `timeouts.connect`, `tls_handshake`; `memory_limits.max_response_bytes`, `max_trailer_bytes`, `max_decompressed_bytes` | Unused | Unused |

---

## 4. Build-time options

Distinct from runtime configuration: build options are set with CMake at compile
time, never appear in `microtel.toml`, and have no environment-variable
equivalents. They are properties of the binary, not of the runtime
configuration.

Every option, with its default, is in [build-options.md](build-options.md).
The two that change runtime behaviour described in this document are
`MICROTEL_FORBID_INSECURE_TLS` (§3.5) and `MICROTEL_WITH_CONCENTRATOR` (§3.14).

---

## 5. Resolved-config dump and secrets

**Not implemented in v1.** `Build()` logs no configuration dump at all — neither
redacted nor otherwise. The two `Warn` lines it can emit (plaintext OTLP/HTTP,
and `insecure=true`) are the whole of what `Build()` says about the resolved
configuration, and neither carries a header value, a credential or a path.

The one exception is the resolved Resource (§3.2, issue #284). Each
`Build()` logs it once at `Info`, as a single line through the internal log
path (so a `LogSink` receives it and `logging.level` filters it):

```
resolved resource (profile "default", 3 attributes): deployment.environment="prod", host.name="node-7", service.name="checkout"
```

Keys are sorted. In keys and string values (array elements included), a
backslash prints as `\\`, a double quote as `\"`, newline, carriage return and
tab as `\n`, `\r` and `\t`, and any other control character as `\xHH`, so no
attribute can split the line; UTF-8 passes through unchanged (issue #315). At
most 32 attributes are listed, then `...and N more`, and a value longer than
128 characters after escaping is cut, never inside an escape, and ends in
`...`. A value whose key
contains `authorization`, `secret`, `token`, `password`, `passwd`,
`credential`, `api_key` or `apikey` (any case) is printed as `<redacted>`.
That list is the redaction rule below, applied by key; it is the only redaction code in
microtel.

The v1.0 design called for more: an `info`-level dump at `Build()`
success with `Authorization` headers and client-secret-shaped values redacted,
private-key *paths* preserved, token-provider outputs never logged, and an
opt-in `--show-secrets` CLI flag gated by a build-time
`MICROTEL_ALLOW_SHOW_SECRETS=ON`. Correction (#196): this section described that
design in the present tense. None of it exists — there is no redaction code, no
`--show-secrets` flag (the `microtel-preflight` CLI accepts only
`--preflight=…`), and no `MICROTEL_ALLOW_SHOW_SECRETS` option.

The practical consequence is the reassuring one: microtel cannot leak a secret
through a config dump it does not emit. The gap is the diagnostic value of the
dump, not a disclosure risk. When it lands it belongs here in the present tense,
with tests over the redaction rules.

---

## 6. Adding a new setting

When a new setting lands in M3+, the change includes:

1. A row in the appropriate §3 subsection.
2. The default in `src/common/config/`.
3. Validation logic in the same place, mapping failures to a `ConfigError::Kind`.
4. A `WithXxx` method on `SdkBuilder` if there is a code surface.
5. Env-var read in `src/common/config/env_resolver.cpp`, if there is one.
6. Tests for: TOML happy path, TOML invalid value, env-var precedence, code precedence, default fallback.

If the new setting overlaps with an existing OTel env var, this document calls out the relationship and the precedence (per §1, MICROTEL wins on overlap).

If the new setting is hot-reloadable in a future release, that is a v1.1+ concern and is recorded in the v1.1 control-plane design doc when written.
