// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"

#include "common/config/config.hpp"

namespace microtel::config
{

/// @brief Overlay OTel / MICROTEL environment variables onto a Config.
///
/// Env-var values override file-sourced values per the precedence rule
/// in `docs/configuration.md` §1. Only variables that are set (non-empty)
/// in the environment participate; absent or empty variables leave the
/// existing Config values unchanged.
///
/// Supported variables:
///   OTEL_EXPORTER_OTLP_ENDPOINT     → endpoint_url (and endpoint_source)
///   OTEL_EXPORTER_OTLP_TRACES_ENDPOINT, OTEL_EXPORTER_OTLP_METRICS_ENDPOINT,
///   OTEL_EXPORTER_OTLP_LOGS_ENDPOINT → signal_endpoint_urls (resolved by
///                                      Validate, `docs/configuration.md` §3.3)
///   OTEL_EXPORTER_OTLP_PROTOCOL     → protocol ("grpc" | "http/protobuf")
///   OTEL_EXPORTER_OTLP_HEADERS      → headers ("k=v,k2=v2")
///   OTEL_EXPORTER_OTLP_TIMEOUT      → timeouts.per_export (integer ms)
///   OTEL_EXPORTER_OTLP_COMPRESSION  → compression_gzip ("gzip" | "none")
///   OTEL_EXPORTER_OTLP_CERTIFICATE  → tls.ca_bundle (path)
///   OTEL_SERVICE_NAME               → service_name
///   OTEL_RESOURCE_ATTRIBUTES        → resource_attrs ("k=v,k2=v2")
///
/// and the `MICROTEL_CONCENTRATOR_*` variables of `docs/configuration.md`
/// §3.14 (`ENABLED`, `MAX_PAYLOAD_BYTES`, `MAX_LEAVES`, `UNKNOWN_LEAF`,
/// `DEFAULT_TIME_MODE`, and `RESOURCE_ATTRIBUTES`, merged per key into
/// `concentrator.leaf_defaults_resource`).
///
/// Every other per-signal exporter variable
/// (`OTEL_EXPORTER_OTLP_{TRACES,METRICS,LOGS}_{HEADERS,PROTOCOL,TIMEOUT,
/// COMPRESSION,CERTIFICATE,CLIENT_KEY,CLIENT_CERTIFICATE,INSECURE}`) is not
/// implemented: each one set adds a line to `cfg.config_warnings` and changes
/// nothing else. (`OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE` is a
/// metrics setting, not a per-signal exporter override, and is read.)
///
/// @param cfg Config to overlay (mutated in place).
/// @return ConfigError::Kind::EnvParseFailure if any set variable has an
///         unparseable value.
[[nodiscard]] microtel::Expected<void, ConfigError> OverlayEnv(Config& cfg);

}  // namespace microtel::config
