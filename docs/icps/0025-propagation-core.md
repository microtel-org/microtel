# ICP 0025: propagation core — `Context`, baggage, `TraceState` storage, `StartAsCurrentSpan`

**Status:** Accepted — decided 2026-09-15. Docs only; the header and source
changes land in the v1.1 implementation packets named below.
**Affected interfaces / docs:** [`include/microtel/context.hpp`](../../include/microtel/context.hpp)
(`Context`, new `CurrentContext` / `ScopedContext`), a new public header
`include/microtel/baggage.hpp`, [`include/microtel/trace.hpp`](../../include/microtel/trace.hpp)
(`TraceState` gains storage — **ABI change**), [`include/microtel/span.hpp`](../../include/microtel/span.hpp)
(new `ScopedSpan`), [`include/microtel/tracer.hpp`](../../include/microtel/tracer.hpp)
(`StartAsCurrentSpan` return type), [`include/microtel/propagator.hpp`](../../include/microtel/propagator.hpp)
(new `W3CBaggagePropagator` surface). Downstream doc edits listed under
Migration. No CI, no runtime dependency, no wire-format change.
**Affected tracks:** Track A — Trace SDK (`src/sdk/`, `src/api/`). Tracks B–F
are untouched.

## Summary

Give `Context` a baggage slot and a thread-local current-context slot, give
`TraceState` real storage, and turn `Tracer::StartAsCurrentSpan` into an RAII
scope that installs the new span as the caller thread's current span — the four
pieces that issues [#208](https://github.com/microtel-org/microtel/issues/208)
and [#221](https://github.com/microtel-org/microtel/issues/221) each need half
of, and that one shared-ownership decision makes possible at once.

## Motivation

**The pieces are one design, not four.** Every one of them runs into the same
wall: `Span::GetContext() const noexcept` (`include/microtel/span.hpp:62`,
implemented at `src/sdk/sdk_span.cpp:GetContext`) returns a `SpanContext` **by
value**, and hard rule 14 makes it `noexcept`. Any member of `SpanContext` —
or of anything copied alongside it — whose copy constructor can allocate makes
that signature a lie. `TraceState` was left storage-free for exactly this
reason (`src/api/propagator.cpp`, the `TraceState` comment block), and baggage
would hit the wall a second time if it were parked on `SpanContext`. One
decision clears both: **immutable state behind a `shared_ptr`, so every copy is
a refcount bump and every copy is `noexcept`.**

**What is broken today.** `microtel::TraceState` as declared has no data member
and no mutation method, so `FromHeader` returns the empty state for every input
and `Inject` never emits a `tracestate` header: a vendor's `tracestate` is
dropped across a microtel hop (`docs/compatibility-matrix.md` §5,
issue #208). `microtel::Context` is a 25-line stub holding one `SpanContext`;
there is no thread-local slot, so `SdkTracer::StartAsCurrentSpan`
(`src/sdk/sdk_tracer.cpp:StartAsCurrentSpan`) is a passthrough to `StartSpan`
and `StartSpanOptions::parent`'s documented "if unset, current Context is used"
(`include/microtel/span.hpp:24`) is not true of any code path.

**There is a shipped feature waiting on this.** Metrics exemplars shipped
structurally complete and inert: every storage class takes
`StorageOptions::span_source` defaulted to `nullptr`
(`src/sdk/metric_stream_impls.hpp`), all ten construction sites in
`src/sdk/sdk_meter.cpp` omit the field, and `SdkProvider::GetLogger` passes a
literal `nullptr` for the log trace-correlation seam
(`src/sdk/sdk_provider.cpp`, the `ICurrentSpanSource` argument).
`docs/metrics-design.md` §7 says so outright: *"exemplars read the current span
from the thread-local `Context` … M12 depends on that v1.1 work being
complete."* A current-span slot is the missing argument for four exemplar
reservoirs and for log-to-trace correlation, in both cases behind an interface
that already exists.

## Proposed change

### 1. `TraceState` gains storage (issue #208)

`include/microtel/trace.hpp`. The four declared methods keep their signatures;
three accessors and one private member are added.

```cpp
namespace microtel::internal { struct TraceStateImpl; }  // defined in src/api/trace_state.cpp

class TraceState
{
public:
    static constexpr std::size_t kMaxEntries = 32;  // W3C maximum

    TraceState() noexcept = default;

    [[nodiscard]] static TraceState FromHeader(std::string_view header);
    [[nodiscard]] std::string ToHeader() const;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;

    /// Borrowed from this `TraceState`; valid while any copy of it lives.
    [[nodiscard]] std::optional<std::string_view> Get(std::string_view key) const noexcept;

    /// Copy-on-write: returns a new state, leaves this one unchanged.
    [[nodiscard]] TraceState Set(std::string_view key, std::string_view value) const;
    [[nodiscard]] TraceState Erase(std::string_view key) const;

private:
    std::shared_ptr<const internal::TraceStateImpl> m_entries;  // null == the empty state
};
```

- **Rule of zero.** All five special members stay implicit, and all five are
  `noexcept` because `shared_ptr`'s are. The implementing packet asserts it:
  `static_assert(std::is_nothrow_copy_constructible_v<SpanContext>)`.
- **Rule 8 justification** (`shared_ptr` needs one): a `unique_ptr` member
  would force `SpanContext`'s copy constructor to deep-copy the entry list —
  an allocation, and a throw, inside a `noexcept` accessor. Shared ownership of
  an **immutable** list is what buys the `noexcept` copy. Nothing mutates
  through the pointer; `Set` / `Erase` build a new list and return a new
  `TraceState`.
- **`memory-model.md` §8.1 is preserved.** The empty state is a null pointer,
  so the `SpanContext` built for an unsampled span still allocates nothing.
  Only `FromHeader` / `Set` / `Erase` allocate, and none of them is on the
  unsampled path.
- **`internal::TraceStateImpl` is forward-declared**, so the entry
  representation is not part of the ABI and can change later without a second
  ABI event.

**This is an ABI change to a public header.** `TraceState` goes from an empty
class to one `shared_ptr`, so `sizeof(SpanContext)` changes and every type
containing one changes with it. `microtel-spec.md` §19 sanctions it in this
window: *"Binary ABI compatibility is best-effort within a minor release,
**not** guaranteed across minor releases unless explicitly stated."* v1.0 →
v1.1 is a minor bump. Consumers **recompile**; no consumer source changes,
because every declared signature is preserved and the additions are additive.
The source-distributed otel-cpp shim ([ICP 0014](0014-otelcpp-shim-and-rule-13.md))
is compiled inside the consumer's build and is therefore unaffected by the ABI
half; it gains a real `TraceState` round-trip in the same packet
(`src/adapters/otelcpp/context_conversion.hpp` carries the standing TODO).

### 2. Baggage rides `Context`, never `SpanContext`

New public header `include/microtel/baggage.hpp` — additive, and it installs
with no CMake change because `CMakeLists.txt` installs
`include/microtel` as a directory ([ICP 0020](0020-install-and-package-config.md)
Decision 2).

```cpp
namespace microtel::internal { struct BaggageImpl; }  // defined in src/api/baggage.cpp

class Baggage
{
public:
    static constexpr std::size_t kMaxEntries = 180;      // W3C Baggage limits
    static constexpr std::size_t kMaxEntryBytes = 4096;
    static constexpr std::size_t kMaxTotalBytes = 8192;

    Baggage() noexcept = default;

    [[nodiscard]] static Baggage FromHeader(std::string_view header);
    [[nodiscard]] std::string ToHeader() const;

    [[nodiscard]] std::optional<std::string_view> Get(std::string_view key) const noexcept;
    [[nodiscard]] Baggage Set(std::string_view key, std::string_view value) const;
    [[nodiscard]] Baggage Erase(std::string_view key) const;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;

private:
    std::shared_ptr<const internal::BaggageImpl> m_impl;  // null == empty
};
```

**The design wall, recorded so it is not re-litigated:** baggage does **not**
go on `SpanContext`. It is per-context, not per-span — a request carries
baggage whether or not a span is active, and baggage set inside a span must
outlive that span within the enclosing scope. Parking it on `SpanContext` would
also put a second growable member inside the `noexcept` by-value
`Span::GetContext()`, which is the trap §1 exists to get out of, and would push
it onto the wire in `SpanContext`-shaped places where it does not belong.
`Baggage` is one `shared_ptr` for the same reason `TraceState` is: `Context`
copies must be `noexcept`.

`Context` then becomes:

```cpp
class Context
{
public:
    Context() noexcept = default;
    explicit Context(SpanContext active) noexcept;
    Context(SpanContext active, Baggage bag) noexcept;

    SpanContext active_span_context;
    Baggage baggage;
};
```

`static_assert(std::is_nothrow_copy_constructible_v<Context>)` is the
implementing packet's guard on all of the above.

### 3. Current context and `StartAsCurrentSpan` (issue #221)

**The slot.** `include/microtel/context.hpp` gains one accessor and one RAII
type:

```cpp
/// The calling thread's current context. Never null; a thread that has
/// installed nothing sees a default-constructed `Context`.
[[nodiscard]] const Context& CurrentContext() noexcept;

/// Installs `ctx` as the current context for the calling thread and restores
/// the previous one on destruction. Move-constructible (so a factory can
/// return one); not copyable, not move-assignable.
class ScopedContext
{
public:
    explicit ScopedContext(Context ctx) noexcept;
    ~ScopedContext() noexcept;
    ScopedContext(ScopedContext&&) noexcept;
    ScopedContext& operator=(ScopedContext&&) = delete;
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    Context m_previous;
    bool m_armed = true;  // a moved-from scope restores nothing
};
```

`CurrentContext()` is **not** inline; its `thread_local Context` lives in one
translation unit (`src/api/context.cpp`, in `microtel_api`), so a process that
links `microtel_api` once has exactly one current-context slot per thread.

**The "stack" is the C++ stack.** There is no thread-local container: the slot
holds a single `Context` by value, and each live `ScopedContext` /
`ScopedSpan` holds the value it displaced. Nesting is therefore a chain through
the caller's own stack frames — no heap, no depth limit, and
`memory-model.md` §8.1 survives, because installing a context costs two
refcount bumps and a thread-local store. The cost is that restore is
positional: **scopes must be destroyed in reverse order of creation on a
thread.** Destroying out of order restores a stale context; it is a programming
error, documented as such, and a debug-build check for it is deliberately not
specified here.

**`ScopedSpan`**, in `include/microtel/span.hpp` beside `SpanHandle`:

```cpp
class ScopedSpan
{
public:
    ScopedSpan() noexcept = default;                    ///< inert; restores nothing
    ScopedSpan(SpanHandle span, Context ctx) noexcept;  ///< installs ctx, owns span
    ~ScopedSpan() noexcept;
    ScopedSpan(ScopedSpan&&) noexcept;
    ScopedSpan& operator=(ScopedSpan&&) = delete;
    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;

    [[nodiscard]] Span* Get() const noexcept;  ///< borrowed; owned by this object
    Span* operator->() const noexcept;
    Span& operator*() const noexcept;

private:
    ScopedContext m_scope;  ///< declared first, so it is destroyed last
    SpanHandle m_span;
};
```

Member order is load-bearing: members are destroyed in reverse declaration
order, so `m_span` is released — ending the span — **while the span is still
the current one**, and `m_scope` restores the caller's context after that.

**`Tracer::StartAsCurrentSpan` changes return type** from `SpanHandle` to
`ScopedSpan` (`include/microtel/tracer.hpp:63`). It stays `noexcept`: the body
is a `StartSpan` call plus a `SpanHandle` move, a `Context` copy, and a
thread-local store.

**The contract:**

1. **Implicit parent applies only when `StartSpanOptions::parent` is unset.**
   `opts.parent` has three states, and only the first consults the current
   context: *unset* → `CurrentContext().active_span_context`; *set and valid* →
   that context; *set but invalid* → an explicit root, a fresh trace id, and
   the current context is **not** consulted. The third state is already relied
   on by the otel-cpp shim (`src/adapters/otelcpp/tracer_shim.hpp`,
   `ResolveParent` returns a set-but-invalid context for `is_root_span`) and is
   preserved exactly.
2. **Both entry points resolve the implicit parent; only one installs.**
   `StartSpan` and `StartAsCurrentSpan` resolve an unset parent identically —
   that is what `span.hpp:24` has always promised. The difference is that
   `StartAsCurrentSpan` additionally installs the new span as current for the
   scope's lifetime. In `src/sdk/sdk_tracer.cpp:StartSpan` this is one `else`
   branch on the existing `if (opts.parent.has_value())`.
3. **The drop path still installs.** When the sampler returns `Drop`, the
   handle is the no-op singleton (`src/sdk/noop_span.cpp:MakeNoopHandle`) but
   the computed `SpanContext` — real trace id, sampled flag cleared — is still
   installed as current, so children of an unsampled span stay in the same
   trace and `Inject` still emits a coherent `traceparent`. This allocates
   nothing.
4. **No cross-thread inheritance.** A newly created thread starts from a
   default-constructed `Context`. microtel has no hook at thread creation and
   will not grow one; a caller that wants context on a worker thread copies
   `CurrentContext()` across the hand-off and installs it with `ScopedContext`.
   This matches opentelemetry-cpp's `RuntimeContext`, which the shim already
   reads per-thread.
5. **Fork.** The child keeps the forking thread's current context, which is a
   `Context` value and holds no thread, fd, or lock. `threading-model.md` §7 is
   unaffected.
6. **Baggage does not become a parent.** `StartSpan` reads only
   `active_span_context` from the current context. Baggage flows through
   `Context` for propagators and for user reads; it never influences sampling
   or parenting.

**The exemplar payoff.** With the slot in place, a
`sdk::CurrentSpanSource : internal::ICurrentSpanSource` is four lines —
`GetCurrentSpan()` returns `CurrentContext().active_span_context` when it is
valid **and sampled**, and a default-constructed `SpanContext` otherwise, which
is precisely the contract
`include/microtel/internal/icurrent_span_source.hpp` already documents and
precisely what the `trace_based` exemplar filter
(`docs/metrics-design.md` §7) needs. Owned by `SdkProvider`, it replaces the
`nullptr` at the `GetLogger` trace-correlation seam in
`src/sdk/sdk_provider.cpp` and is threaded to `SdkMeter` so the ten
`StorageOptions` sites in `src/sdk/sdk_meter.cpp` can set `.span_source`.
That activates the exemplar machinery in `SumStorage`, `GaugeStorage`,
`HistogramStorage`, and `ExponentialHistogramStorage`, all of which already
have the `if (m_span_source != nullptr)` branch and the tests behind it.

### 4. `W3CBaggagePropagator` — surface only

`include/microtel/propagator.hpp` gains a sibling of
`W3CTraceContextPropagator`, reusing the existing `HeaderGetter` /
`HeaderSetter` callbacks unchanged:

```cpp
/// @threadsafety Thread-safe (stateless).
class W3CBaggagePropagator
{
public:
    W3CBaggagePropagator() noexcept = default;

    /// Sets the `baggage` header; sets nothing if `baggage` is empty.
    void Inject(const Baggage& baggage, const HeaderSetter& setter) const;

    /// Returns an empty `Baggage` if the header is absent or unparseable.
    [[nodiscard]] Baggage Extract(const HeaderGetter& getter) const;
};
```

Grammar, percent-encoding, the `;`-metadata tail, limit enforcement, the W3C
test vectors, and the fuzz target required by the v1.1 ships-when gate clause 3
([ICP 0024](0024-v1.1-rescope.md)) are **packet 2.3c**, not this ICP.

## Not in this ICP

- **Sampler-object swap.** Replacing a provider's sampler at runtime is not
  part of the propagation core and is not proposed here. `SetSamplerRatio` is
  one of ICP 0024's four hot-reload setters and belongs to that work.
- **Baggage propagator implementation** — packet 2.3c, as above.
- **Sugar layer.** `microtel::sugar`'s function-scoped spans are the natural
  first consumer of `ScopedSpan`; their API is ICP 0028 per the v1.1 gate.
- **A non-recording span handle.** On the drop path `Span::GetContext()` still
  returns an invalid context, because the handle is a shared process-wide
  singleton that cannot hold per-span state without breaking
  `memory-model.md` §8.1. `CurrentContext()` is the accurate source there.
  Giving unsampled spans a context-carrying handle is a separate,
  benchmark-driven question.
- **`docs/interfaces.md` is not amended by this PR.** §4.6's `OnStart(Span&,
  const Context&)` signature does not change; what changes is that the
  `Context` it receives will carry the parent's baggage rather than being
  constructed from a bare `SpanContext`
  (`src/sdk/sdk_tracer.cpp`, `parent_propagation_ctx`). Writing that into
  `interfaces.md` now would put a normative claim about code that does not
  exist into the document ICP 0021 exists to stop — so the amendment is an
  obligation of packet 2.3b, listed below, not of this ICP.

## Migration

Nothing to do today; this ICP schedules work. The implementing packets carry:

- **Packet 2.3a — `TraceState` storage (#208).** `trace.hpp` + `src/api/`;
  retire the `tracestate` caveat in `docs/compatibility-matrix.md` §5, the
  `TraceState` comment block in `src/api/propagator.cpp`, and the round-trip
  TODO in `src/adapters/otelcpp/context_conversion.hpp`.
- **Packet 2.3b — context core (#221).** `baggage.hpp`, `context.hpp`,
  `ScopedSpan`, the `StartAsCurrentSpan` return type, `CurrentSpanSource`, and
  the exemplar/log-correlation wiring. Adds `microtel/baggage.hpp` and
  `microtel/context.hpp` to `ci/header_check.cpp` (**`context.hpp` is missing
  from that list today** — pre-existing gap, closed here). Amends
  `docs/interfaces.md` §4.6 (the `Context` handed to `OnStart` carries baggage)
  and `docs/threading-model.md` §10 (rows for `Context` — immutable value,
  thread-safe for read; `ScopedContext` / `ScopedSpan` — **thread-confined**,
  constructed and destroyed on one thread).
- **Packet 2.3c — `W3CBaggagePropagator`.** Parser, W3C vectors, fuzz target.

**Consumers:** recompile against v1.1 headers. No source change is required.
The one visible signature change is `StartAsCurrentSpan`'s return type, and
because `ScopedSpan` keeps `operator->`, `operator*`, and `Get()`, a call site
written with `auto` compiles unchanged.

**Behaviour change to flag:** after packet 2.3b, `StartSpan` with an unset
parent inherits from the current context instead of always starting a root
span. That can only differ for a program that calls `StartAsCurrentSpan` or
`ScopedContext` — both of which are inert or absent today — so no existing
program's traces change shape.

## Rationale & alternatives

- **`TraceState` by value (a small `std::vector` or fixed array inline)** —
  rejected. A `vector` member makes `SpanContext`'s copy allocating and
  throwing inside a `noexcept` accessor. A fixed `std::array<Entry, 32>` of
  strings makes `SpanContext` enormous and still copies 32 strings on every
  `GetContext()`, on a path taken by every propagator inject.
- **Baggage on `SpanContext`** — rejected; the wall in §2.
- **A thread-local stack container (`std::vector<Context>`)** — rejected. It
  allocates on first push, which breaks `memory-model.md` §8.1 for
  `StartAsCurrentSpan` on an unsampled span, and it buys only out-of-order
  detection, which the RAII chain gets closer to for free.
- **A single opaque `Context` with a type-erased key/value map** (the
  opentelemetry-cpp shape) — rejected. It is a heap map per context write, it
  hides the two things microtel actually propagates behind `any`-style lookups,
  and it cannot give the `noexcept` guarantees hard rule 14 demands without the
  same `shared_ptr` underneath. Two named, typed slots are the honest shape for
  a runtime that propagates exactly two things.
- **Implicit cross-thread inheritance** (capture the spawning thread's context)
  — rejected. It needs a hook at thread creation that a library cannot install
  portably, and every mechanism that fakes it (wrapping `std::thread`, a
  `pthread_create` interposer) is a surprise in someone else's process. Explicit
  hand-off is one line at the call site and is what the OTel C++ API does.
- **Keep `StartAsCurrentSpan` returning `SpanHandle`** and pop the thread-local
  from `SpanDeleter` — rejected. `SpanDeleter` is stateless, so the restore
  would have to be "pop whatever is on top", which is wrong the moment a handle
  is moved or ended out of order, and it would silently corrupt the context
  chain rather than fail.
- **Defer `TraceState` to v1.2 and ship only the `Context` work** — rejected.
  Both changes are ABI events on the same header set; doing them in one minor
  release costs consumers one recompile instead of two, and the `shared_ptr`
  rationale is a single decision that both depend on.
