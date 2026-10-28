// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace microtel
{
class Meter;
class Logger;
/// Defined in `microtel/leaf_receiver.hpp` (ICP 0034).
class LeafReceiver;
/// Defined in `microtel/sdk_builder.hpp`, which already includes this header.
struct BatchOptions;
}  // namespace microtel

namespace microtel
{

/// @brief Connection state observable through `HealthSnapshot`.
enum class ConnectionState : std::uint8_t
{
    /// Never connected, or a first connect attempt failed. For an operator
    /// this points at configuration or reachability — the endpoint, TLS
    /// material, or the network path.
    Disconnected = 0,
    /// A connect attempt is in progress.
    Connecting = 1,
    /// Connected; exports can proceed.
    Connected = 2,
    /// Was connected, the connection dropped, and the transport will
    /// re-establish it on the next export (ICP 0018). Distinct from
    /// `Disconnected` because the operator response differs: this points at
    /// the peer or the link, not at local configuration.
    ///
    /// @note Declared since M0 but never emitted until ICP 0018 was
    ///       implemented. Consumers switching exhaustively on this enum always
    ///       had to handle it; those that did not have a latent bug
    ///       independent of that change. Alerting keyed on `Disconnected` will
    ///       see fewer transitions now, because drops report this instead.
    Reconnecting = 3,
    /// `Close` has run. Terminal — the transport cannot be reconnected.
    Closed = 4,
};

/// @brief Drop reasons mirrored from `docs/error-model.md` §3.
///
/// The enumerator order is part of the `HealthSnapshot::drop_counters` array
/// indexing contract. Adding a new entry is an ICP because every counter is
/// part of the public health surface.
enum class DropReason : std::uint8_t
{
    QueueFull = 0,
    RecordTooLarge = 1,
    SpanAttributeLimit = 2,
    SpanEventLimit = 3,
    SpanLinkLimit = 4,
    EventAttributeLimit = 5,
    LinkAttributeLimit = 6,
    AttributeValueTruncated = 7,
    PostShutdown = 8,
    ResponseTooLarge = 9,
    DecompressionTooLarge = 10,
    MalformedResponse = 11,
    PartialSuccessRejection = 12,
    NonRetryableFailure = 13,
    RetryableFailureRecovered = 14,
    RetryBudgetExhausted = 15,
    TransportBusy = 16,
    ConnectFailure = 17,
    ForceFlushTimeout = 18,
    ShutdownTimeout = 19,
    /// Attribute set exceeded the per-instrument cardinality limit; the
    /// measurement is folded into the `otel.metric.overflow` series, not lost
    /// (ICP 0008, `docs/metrics-design.md` §2).
    CardinalityOverflow = 20,
    /// Async instrument callback exceeded the per-collection deadline; its
    /// measurements were dropped for that cycle (ICP 0008).
    MetricCallbackTimeout = 21,
    /// NaN / ±Inf measurement dropped, as the OTel spec requires (ICP 0008).
    NonFiniteValue = 22,
    /// A `LogRecord`'s attribute set exceeded the per-record attribute limit;
    /// the surplus attributes were dropped and `dropped_attributes_count` was
    /// incremented on the record (ICP 0011, `docs/logs-design.md` §5).
    LogAttributeLimit = 23,
    /// A leaf payload failed to decode or to validate, declared an unsupported
    /// wire version, or declared a time mode its leaf's config does not allow.
    /// Counted in **payloads** (ICP 0034, `docs/leaf-concentrator-design.md`
    /// §3.3).
    LeafPayloadMalformed = 24,
    /// A leaf payload exceeded `max_payload_bytes`, `max_spans_per_payload`,
    /// the decode depth limit or the decode arena cap. Counted in payloads
    /// (ICP 0034).
    LeafPayloadTooLarge = 25,
    /// A leaf payload came from a leaf with no configuration while the receiver
    /// rejects unknown leaves. Counted in payloads (ICP 0034).
    LeafUnknown = 26,
};

/// @brief The number of `DropReason` enumerators. Used to size the counter
/// array in `HealthSnapshot`.
inline constexpr std::size_t kDropReasonCount = 27;

/// @brief Snapshot of exporter health, returned by `Provider::GetExporterHealth`.
///
/// A consistent-at-a-moment view; counters are atomic reads, the last-error
/// fields are a borrowed view of the most-recent error slot (capped).
struct HealthSnapshot
{
    std::array<std::uint64_t, kDropReasonCount> drop_counters{};
    std::uint64_t batches_sent = 0;
    std::uint64_t batches_failed = 0;
    std::uint64_t queue_depth_now = 0;
    std::optional<std::chrono::system_clock::time_point> last_error_time;
    std::string last_error_message;  ///< capped, redacted
    ConnectionState connection_state = ConnectionState::Disconnected;
};

/// @brief Process-level entry point for issuing tracers and managing the
/// export pipeline lifecycle.
///
/// Constructed by `SdkBuilder::Build()`. Application code typically holds the
/// returned `shared_ptr<Provider>` for the duration of the process and calls
/// `Shutdown()` before exit.
///
/// @threadsafety Thread-safe. All methods may be called from any thread.
/// @see docs/architecture.md §3.1, §3.2
/// @see docs/threading-model.md §6
class Provider
{
public:
    Provider() noexcept = default;
    virtual ~Provider() noexcept = default;

    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    Provider(Provider&&) = delete;
    Provider& operator=(Provider&&) = delete;

    /// @brief Acquire a tracer for one instrumentation scope.
    ///
    /// The returned `shared_ptr<Tracer>` is owned jointly with the provider;
    /// it remains valid until both this call site and the provider are
    /// destroyed.
    ///
    /// @param name    instrumentation library name (e.g., `"my.component"`).
    /// @param version optional library version string.
    [[nodiscard]] virtual std::shared_ptr<Tracer> GetTracer(std::string_view name,
                                                            std::string_view version = {}) = 0;

    /// @brief Eagerly establish the export connection.
    ///
    /// Optional. If not called, the connection is established lazily on the
    /// first export — `microtel::internal::IWireCodec::Send` performs it
    /// transparently (ICP 0017). Returns a network-level outcome — DNS / TCP
    /// / TLS / HTTP/2 SETTINGS exchange. No telemetry is sent.
    ///
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual Expected<void, Error> Connect() = 0;

    /// @brief Export everything recorded before this call, up to `timeout`.
    ///
    /// Does not stop accepting new records. Every signal's pipeline is flushed,
    /// even if an earlier one fails or times out, and all of them share the one
    /// `timeout`. Returns `Status::Completed` only if every record queued before
    /// the call was exported; `Status::Failed` if any of it failed after its
    /// final retry; `Status::TimedOut` if the timeout came first. Exports still
    /// in flight at the timeout carry on.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status ForceFlush(std::chrono::milliseconds timeout) noexcept = 0;

    /// @brief Stop accepting new records, drain queues, close the transport.
    ///
    /// Idempotent. Subsequent calls return `Status::AlreadyShutDown`. After
    /// this returns, all `End()` calls drop with `PostShutdown`.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;

    /// @brief Snapshot of exporter health for diagnostics.
    ///
    /// Returns a consistent-at-a-moment view of counters, queue depth, last
    /// error, and connection state.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual HealthSnapshot GetExporterHealth() const noexcept = 0;

    /// @brief Acquire (or create) a `Meter` for one instrumentation scope.
    ///
    /// Same `(name, version)` returns the cached instance; a new instance is
    /// created on the first call for each unique scope. `schema_url` is stored
    /// in the scope but does not affect caching in v1.
    ///
    /// @param name        instrumentation library name (e.g., `"my.component"`).
    /// @param version     optional library version string.
    /// @param schema_url  optional schema URL (stored; no v1 semantics).
    ///
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual std::shared_ptr<Meter> GetMeter(std::string_view name,
                                                          std::string_view version = {},
                                                          std::string_view schema_url = {}) = 0;

    /// @brief Acquire (or create) a `Logger` for one instrumentation scope.
    ///
    /// Same `(name, version)` returns the cached instance. Returns a no-op
    /// logger when no logs exporter is configured. See ICP 0012 and
    /// `docs/logs-design.md` §8.
    ///
    /// @param name    instrumentation library name (e.g., `"my.component"`).
    /// @param version optional library version string.
    ///
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual std::shared_ptr<Logger> GetLogger(std::string_view name,
                                                            std::string_view version = {}) = 0;

    // ── Hot reload (ICP 0026) ──────────────────────────────────────────────
    //
    // Four knobs retunable without a restart. Each validates its input,
    // changes nothing when it rejects, and returns `AlreadyShutDown` after
    // `Shutdown` — reading the shutdown flag **before** taking any mutex, the
    // pattern `GetMeter` and `GetLogger` already use so a forked child cannot
    // deadlock on a lock held by a thread that no longer exists
    // (`docs/threading-model.md` §7).
    //
    // A rejection is reported as `InvalidArgument` (your value is wrong) or
    // `Unsupported` (this provider has no such knob); the precise detail —
    // which field, what range, which sampler — goes to the internal log at
    // `Warn`. Validation runs before the support check, so a bad value is
    // reported as bad whatever pipelines the provider happens to own.

    /// @brief Retune the batching knobs of the span and log pipelines.
    ///
    /// Mirrors `SdkBuilder::WithBatch` at runtime; one `BatchOptions` drives
    /// both pipelines, exactly as at build time. `Unsupported` if the provider
    /// has no batching span processor.
    ///
    /// Rejected with `InvalidArgument`: a zero `max_queue_size`, a zero
    /// `max_export_batch_size`, a `max_export_batch_size` above
    /// `max_queue_size`, or a `schedule_delay` of zero or less — the same
    /// rules `SdkBuilder::Build` applies to `WithBatch` and the `[sdk]` table,
    /// so a value is accepted here exactly when it would build.
    ///
    /// The change is applied to the two pipelines in turn and is **not atomic
    /// across them**: for the duration of one call a span batch may be cut
    /// under the new options while a log batch is still under the old.
    ///
    /// @param opts borrowed; copied into each pipeline. Not retained.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status SetBatchOptions(const BatchOptions& opts) noexcept = 0;

    /// @brief Retune the periodic metric reader's export interval.
    ///
    /// Takes effect from the reader's next tick, not the current one — a wait
    /// already in flight keeps its old deadline, so a shortened interval costs
    /// at most one old-length tick. Applies to a reader not yet built, so a
    /// later `GetMeter` uses the new value. Does **not** force a collection;
    /// `ForceFlush` is the way to ask for one. `Unsupported` if no metrics
    /// pipeline is configured.
    ///
    /// @param interval must be greater than zero, else `InvalidArgument`.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status SetMetricInterval(std::chrono::milliseconds interval) noexcept = 0;

    /// @brief Retune the active sampler's sampling ratio.
    ///
    /// `Unsupported` — and nothing changed — unless the configured sampler is
    /// a `TraceIdRatio` sampler, or a composite that owns one. Never replaces
    /// a sampler with one of a different kind: the sampler object's identity
    /// is fixed for the provider's life.
    ///
    /// @param ratio in `[0.0, 1.0]`. NaN and out-of-range values are rejected,
    ///        not clamped — `MakeTraceIdRatioSampler` clamps at build time,
    ///        where it is a documented convenience; at reload time a caller
    ///        asking for `1.5` has a bug worth surfacing (ICP 0026 Decision 3).
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status SetSamplerRatio(double ratio) noexcept = 0;

    /// @brief Set the minimum severity for microtel's internal diagnostic logs.
    ///
    /// Records below `level` are dropped before the sink is consulted.
    /// Process-wide, not per-provider: the internal log path is a free
    /// function (`microtel::internal::LogImpl`) with no provider in scope, so
    /// providers built from different profiles share this knob and the last
    /// writer wins.
    ///
    /// @param level one of the declared `LogLevel` enumerators; anything else
    ///        is `InvalidArgument`.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept
    [[nodiscard]] virtual Status SetLogLevel(LogLevel level) noexcept = 0;

    /// @brief The concentrator's leaf receiver (ICP 0034).
    ///
    /// Returns a no-op receiver, whose `Ingest` answers
    /// `IngestStatus::Disabled`, when the provider was built without
    /// `SdkBuilder::WithLeafReceiver` or the library without
    /// `MICROTEL_WITH_CONCENTRATOR` — the way `GetLogger` returns a no-op
    /// logger. The same receiver is returned on every call. It may outlive the
    /// provider; after `Shutdown` its `Ingest` answers `IngestStatus::ShutDown`.
    ///
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual std::shared_ptr<LeafReceiver> GetLeafReceiver() = 0;
};

// ── Multi-profile (ICP 0027) ───────────────────────────────────────────────

/// @brief The profile name a provider built without `WithProfileName` carries.
inline constexpr std::string_view kDefaultProfileName = "default";

/// @brief Find the live provider registered under @p name.
///
/// A process may hold several named providers at once, each built by its own
/// `SdkBuilder::Build()` with its own endpoint, protocol, TLS material, sampler,
/// `Resource`, pipelines and threads. This is how instrumentation that cannot
/// reach the host's wiring — library code, a plugin, a callback — finds one.
/// A host that builds its profiles in one place can pass the `shared_ptr`
/// around instead and never call this.
///
/// @param name the profile name, compared byte-for-byte; no normalisation and
///        no case folding. Defaults to `kDefaultProfileName`, so `GetProvider()`
///        is the default-profile accessor.
/// @return a **borrowed, non-owning** pointer, or `nullptr` if no live provider
///         carries that name. Never transfers ownership: the owner is whoever
///         holds the `std::shared_ptr<Provider>` that `Build()` returned.
///
/// @par Lifetime
/// Registration keeps nothing alive — a lookup does not extend a provider's
/// life, and the slot is occupied for strictly less time than the provider
/// exists.
/// - **Across `Shutdown`: the pointer stays valid.** `Shutdown` does not
///   deregister; only destruction does. A shut-down provider is still found by
///   name, and every lifecycle method on it returns `Status::AlreadyShutDown`.
///   A name is freed for reuse by destruction, not by `Shutdown`.
/// - **Across destruction: the pointer dangles**, like every non-owning pointer
///   in this API. What is guaranteed is that a *subsequent* `GetProvider(name)`
///   returns `nullptr` rather than the corpse. A program that destroys
///   providers while other threads look them up must synchronise that itself;
///   the documented usage — look profiles up after building them, hold the
///   pointer, destroy providers at process teardown — never meets the race.
/// - **Across `fork()`:** every slot is empty in the child, so this returns
///   `nullptr` there until the child re-builds. A pointer obtained *before* the
///   fork is still valid in the child and answers `AlreadyShutDown`
///   (`docs/threading-model.md` §7).
///
/// @threadsafety Thread-safe. Lock-free and allocation-free.
/// @noexcept
[[nodiscard]] Provider* GetProvider(std::string_view name = kDefaultProfileName) noexcept;

}  // namespace microtel
