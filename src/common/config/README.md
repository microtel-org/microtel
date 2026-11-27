# `src/common/config/`

## What lives here

The configuration layer that `SdkBuilder` runs at `Build()`: parse
`microtel.toml` ([`toml_loader.cpp`](toml_loader.cpp)), overlay environment
variables ([`env_resolver.cpp`](env_resolver.cpp)), validate
([`config_validator.cpp`](config_validator.cpp)), and hand back a frozen
`Config` ([`config.hpp`](config.hpp)). The two auth-provider implementations
live here too ([`auth_providers.cpp`](auth_providers.cpp)). Built as
`microtel_config`.

The frozen `Config` is the input every other component sees during
construction, and nothing mutates it afterwards. v1.1 adds four runtime setters
on `Provider` (`SetBatchOptions`, `SetMetricInterval`, `SetSamplerRatio`,
`SetLogLevel`, [ICP 0026](../../../docs/icps/0026-provider-setters.md)); they
act on the built pipeline, not on `Config`. File-driven hot reload through a
control plane is deferred to v1.4
([`docs/control-plane-design.md`](../../../docs/control-plane-design.md)).

## Owner

Track E — Configuration.

## What it implements

- TOML parsing with toml++ v3.4.0, selected in
  [ICP 0004](../../../docs/icps/0004-vendor-tomlplusplus.md).
- Env-var reading, following the precedence in `docs/configuration.md` §1.
  [`env_resolver.hpp`](env_resolver.hpp) lists the `OTEL_*` variables it
  reads; the `MICROTEL_*` ones (`MICROTEL_LOG_LEVEL`,
  `MICROTEL_RESOURCE_DETECTORS_STRICT`) are applied in
  [`env_resolver.cpp`](env_resolver.cpp). The per-signal endpoint variables
  are read there and resolved to one connection origin by `Validate`
  (`docs/configuration.md` §3.3); the other per-signal exporter variables are
  not implemented, and each one set adds a line to `Config::config_warnings`,
  which `Build()` logs at `Warn` once the log level is applied.
- The resolved `Config` value type.
- The `[concentrator]` table and the `MICROTEL_CONCENTRATOR_*` variables of
  the leaf receiver, with the byte-size and duration syntax they share and the
  merge of `SdkBuilder::WithLeafReceiver` over them
  ([`concentrator_config.cpp`](concentrator_config.cpp); design
  [`leaf-concentrator-design.md`](../../../docs/leaf-concentrator-design.md)
  §4.2–§4.3). Parsed in every build; the receiver's own checks run in
  `src/sdk/leaf_options.cpp` at `Build()`.
- Per-key merging of the table-valued settings (`resource_attrs`, `headers`)
  across sources ([`table_merge.cpp`](table_merge.cpp), issue #257). The env
  overlay and `SdkBuilder`'s code overrides both go through it; header names
  compare case-insensitively, resource keys exactly.
- Validation. Every entry in `docs/configuration.md` §3 has its validator here,
  and failures map to `ConfigError::Kind`.
- The `internal::IAuthProvider` realisations `StaticHeadersAuthProvider` and
  `CallbackAuthProvider` (with a TTL cache).

## Dependencies

- toml++ (`tomlplusplus::tomlplusplus`, header-only, fetched with
  `FetchContent`). It is linked `PRIVATE` and wrapped in
  `$<BUILD_INTERFACE:>`, so `libmicrotel_config.a` carries no toml symbol and
  toml++ never reaches the install export set.
- `microtel::Error` and `microtel::ConfigError` from
  [`include/microtel/error.hpp`](../../../include/microtel/error.hpp).

## Tests

- `tests/unit/common/config/`: `config_test.cpp` checks each setting's
  precedence (code vs env vs file vs default), validation (the happy path and
  every documented failure) and edge cases; `signal_endpoint_test.cpp` the
  per-signal exporter variables; `concentrator_config_test.cpp`
  the `[concentrator]` sources; `forbid_insecure_tls_test.cpp`
  covers the ON half of the `MICROTEL_FORBID_INSECURE_TLS` gate (issue #200).
- `tests/unit/common/auth/auth_providers_test.cpp`: the static-headers and
  callback paths, and TTL-cache behaviour.
- `tests/fuzz/toml_fuzz.cpp`: adversarial TOML inputs.

## Style notes

- **Eager validation.** `SdkBuilder::Build` validates at construction
  time and returns `microtel::Expected<std::shared_ptr<Provider>, ConfigError>`.
  There is no network preflight here; that is the `microtel-preflight` CLI
  (`--preflight=…`, `docs/error-model.md` §8).
- **Strict by default.** Unknown TOML keys raise `ConfigError::UnknownKey`
  unless `[config] unknown_keys = "warn" | "ignore"`.
- **Auth callback runs on the exporter worker thread** (LOCKED —
  `interfaces.md` §4.9). Since the trace, metric and log codecs share one auth
  provider, up to three exporter workers may call it concurrently
  ([ICP 0029](../../../docs/icps/0029-auth-caller-count-correction.md)).
  The public `WithAuthProvider` API documents this; user code must be
  thread-safe.
- **Secrets.** `docs/configuration.md` §5 specifies a redacted resolved-config
  dump at `info` level (`Authorization` headers, client secrets and
  token-provider outputs redacted). It is not implemented: `Build()` logs no
  config dump, only the two `Warn` lines for plaintext OTLP/HTTP and
  `tls.insecure = true`. Redaction belongs at that boundary when the dump
  lands. The resolved Resource is logged (at `Info`, from
  `src/sdk/resource_builder.cpp`) with a by-key redaction of its own; see
  `docs/configuration.md` §5.
- **Env-var precedence** per `docs/configuration.md` §1: code > env >
  file > defaults. `MICROTEL_*` wins over `OTEL_*` on overlap. v1 defines no
  overlap, so the rule is dormant but documented.
- **Frozen at `Build` time.** `Config` has no mutation API.
