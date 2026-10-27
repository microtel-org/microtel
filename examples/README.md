# `examples/`

Standalone programs that use the public `microtel::*` API, plus the container
stack they all export to. They are meant to be read and run. Nothing should
depend on them.

---

## Quickstart

Three commands take you from a clean checkout to a trace in Grafana. (No
Docker or Podman? [`console_trace/`](console_trace/) prints its spans to the
terminal and needs nothing running.)

```bash
# 1. Start the collector + Tempo + Grafana stack (docker or podman, either).
examples/stack/up.sh

# 2. Build the examples. They are gated behind MICROTEL_BUILD_EXAMPLES=ON;
#    tests off skips fetching GoogleTest and building the test suite.
cmake -S . -B build -DMICROTEL_BUILD_TESTS=OFF -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build -j"$(nproc)"

# 3. Run one. It prints the trace ID it emitted.
./build/examples/microtel_example_basic_trace
```

```
trace_id: 532267361510131fac037e635454ce60
ForceFlush: Completed
batches_sent=1 batches_failed=0 rejected=0 queue_depth=0
Shutdown: Completed
```

Then open <http://localhost:3000>. There's no login; the stack runs anonymous
for local use. The home page is the **microtel — recent traces** dashboard, and
the trace shows up there within about ten seconds. Click the row for the flame
graph, or paste the trace ID into Explore → Tempo.

`examples/stack/down.sh` tears it all down.

Pinned image versions, ports, engine detection, SELinux notes and
troubleshooting for the stack are in [`stack/README.md`](stack/README.md).

---

## The examples

Each example is one directory holding a `main.cpp`, a `README.md`, and a
`CMakeLists.txt` with a single `microtel_add_example()` call. Every binary lands
in `build/examples/microtel_example_<name>`, whichever directory defined it.

| Example | What it shows | Status |
|---|---|---|
| [`basic_trace/`](basic_trace/) | The smallest end-to-end flow: build a provider, emit one request trace, flush, read exporter health, shut down. | Available |
| [`console_trace/`](console_trace/) | No collector needed. The same request trace, exported through an example-local `ExportTransport` that prints the spans to stdout. | Available |
| [`logs/`](logs/) | Log records with severity, body and attributes from `GetLogger`, and automatic trace correlation: records written inside a current span carry its trace and span IDs. The stack's collector prints them. | Available |
| [`sugar_tour/`](sugar_tour/) | The `microtel::sugar` convenience layer (`MICROTEL_TRACE_FUNCTION`, `mt::Span`, `mt::Traced`, `mt::RecordException`, `mt::AttrKey`) in one order pipeline. | Available |
| [`context_propagation/`](context_propagation/) | `StartAsCurrentSpan` and the thread-local context slot: implicit parenting with no arguments passed, and what does *not* cross a thread boundary. | Available |
| [`distributed_handoff/`](distributed_handoff/) | Two binaries. W3C `traceparent` / `tracestate` / `baggage` inject and extract across a process boundary, joined into one trace. | Available |
| [`sampler_chains/`](sampler_chains/) | Composing head samplers into a first-match rule chain, and the same rules under all-must-agree. Prints per-case sampled counts. | Available |
| [`resource_detectors/`](resource_detectors/) | The built-in `process.*` and `host.*` detectors, and the detector → env → user precedence that decides a contested key. | Available |
| [`multi_profile/`](multi_profile/) | Two independent named providers in one process, found by name with `microtel::GetProvider`. | Available |
| [`hot_reload/`](hot_reload/) | The four ICP 0026 setters against a live pipeline (sampler ratio, batch options, log level), including the calls that are refused. | Available |
| [`health_and_backpressure/`](health_and_backpressure/) | Reading `HealthSnapshot`: drop counters and queue depth under load, then a collector that is not there. | Available |
| [`auth_bearer/`](auth_bearer/) | Static headers and `WithAuthProvider`, against a collector that checks the token. Opt-in overlay. | Available |
| [`tls/`](tls/) | TLS, a custom CA and mTLS, and the one configuration in which OTLP/HTTP works. Opt-in overlay. | Available |
| [`leaf/`](leaf/) | Experimental. A C leaf (a device too small for the runtime) sends OTLP payloads over UDP to a C++ concentrator, which gives each device its own Resource and exports every device's spans together. A full C++ node joins the same link through its own `ExportTransport` (`WithExportTransport`), and can command the leaf so one trace spans both devices. Needs extra build options; see below. | Available |
| [`demo-edge-trace/`](demo-edge-trace/) | Experimental. One trace from a Linux service (C++ SDK) into Cortex-M4 firmware (C11 leaf) on QEMU mps2-an386 over a serial line, back through the concentrator, and into Tempo, all from one `docker compose up`. A CI smoke job checks the trace. | Available |
| [`leaf_mqtt/`](leaf_mqtt/) | Experimental. The leaf example over MQTT: a C leaf publishes each payload to `microtel/<device-id>/traces` with coreMQTT, and a C++ concentrator subscribed with libmosquitto takes the leaf id from the topic. Needs the leaf build options and libmosquitto, and brings its own Mosquitto broker as an opt-in overlay. | Available |
| `metrics_exemplars/` | Metrics with exemplars. | Planned, optional (adds Prometheus to the stack) |

The planned set mirrors the v1.1 public surface; issue
[#279](https://github.com/microtel-org/microtel/issues/279) is the epic.
Logs, supported since v1.2, are in [`logs/`](logs/); the log bridges have
their own READMEs ([spdlog](../src/adapters/spdlog/README.md),
[glog](../src/adapters/glog/README.md), [log4cxx](../src/adapters/log4cxx/README.md)).
Metrics now have conformance coverage against the collector on both protocols
([http](../tests/conformance/http/metrics_test.cpp), [grpc](../tests/conformance/grpc/metrics_test.cpp));
a metrics example is not written yet (see [`docs/roadmap.md`](../docs/roadmap.md)).

### The leaf example

[`leaf/`](leaf/) and [`leaf_mqtt/`](leaf_mqtt/) are the examples the default
example build leaves out, because their two halves are experimental v1.2
features that are off by default. Turn them on, then run the concentrator and one or more leaves
against the same stack as everything else:

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON \
      -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_WITH_CONCENTRATOR=ON
cmake --build build

./build/examples/microtel_example_leaf_concentrator &     # UDP 127.0.0.1:9310 -> :4317
./build/examples/microtel_example_leaf_udp_leaf &         # five payloads, then exits
./build/examples/microtel_example_leaf_full_node          # a C++ node on the same link
```

The leaf binary is C, linked against the leaf library alone, so its name
doesn't follow the `microtel_add_example()` pattern exactly:
`microtel_example_leaf_udp_leaf` and `microtel_example_leaf_concentrator`.
The spans show up in Grafana under `greenhouse-north`, the name
`microtel.toml` gives the leaf on port 9311. The full node's spans show up
under `greenhouse-controller`: it is an ordinary C++ Provider that exports with
`SdkBuilder::WithExportTransport` over UDP instead of HTTP/2, which
`microtel.toml` allows by naming its port with `time_mode = "unix"`.
[`leaf/README.md`](leaf/README.md) walks through a run with two leaves, then
one with the full node, and what the collector receives.

The same programs also show one trace crossing devices. Given a leaf port, the
full node plays the greenhouse controller and sends the leaf a command carrying
its span's W3C `traceparent`; started with `command`, the leaf makes its failed
`sensor.read` a child of that span:

```bash
./build/examples/microtel_example_leaf_concentrator http://localhost:4319 &
./build/examples/microtel_example_leaf_udp_leaf stamped 9310 9311 3 command &
./build/examples/microtel_example_leaf_udp_leaf boot 9310 9312 3 &
sleep 1
./build/examples/microtel_example_leaf_full_node 9310 9313 3 9311
wait                                # the concentrator flushes and exits
examples/leaf/fan-in-check.sh       # which devices each export request carried
```

Tempo then shows `greenhouse.control` from `greenhouse-controller` with the
leaf's `sensor.read` from `greenhouse-north` under it, status `ERROR`. Port
4319 is the stack collector's unbatched receiver, which logs every request as
it arrived, so `fan-in-check.sh` can show several devices' `ResourceSpans` in
one request. See [A fault, traced across devices](leaf/README.md#a-fault-traced-across-devices).

[`leaf_mqtt/`](leaf_mqtt/) is the same pair of programs over MQTT, which
also needs libmosquitto's development files (`mosquitto-devel` on Fedora,
`libmosquitto-dev` on Debian and Ubuntu; without them CMake skips the example
and says why). CMake fetches coreMQTT for the leaf itself. The broker is an
opt-in overlay:

```bash
examples/stack/up.sh
examples/leaf_mqtt/up-mqtt.sh                              # Mosquitto on 127.0.0.1:1883

./build/examples/microtel_example_leaf_mqtt_concentrator & # microtel/+/traces -> :4317
./build/examples/microtel_example_leaf_mqtt_leaf           # five payloads, then exits
```

The leaf id is the device id in the topic, so these spans show up under
`greenhouse-north`, the name `leaf_mqtt/microtel.toml` gives `gh-north-01`.
[`leaf_mqtt/README.md`](leaf_mqtt/README.md) covers why MQTT, the QoS
choice, broker reconnects, and what a real broker needs (TLS on 8883 and an
ACL per device).

### Opt-in overlays

Two examples need a receiver the shared stack deliberately leaves out: one that
checks a bearer token, and one that serves TLS. Each brings its own collector
as a separate compose project on separate ports, started and torn down by its
own scripts. A third, `leaf_mqtt`, brings an MQTT broker the same way:

```bash
examples/auth_bearer/up-auth.sh   # bearer-guarded OTLP/gRPC on :5317
examples/tls/up-tls.sh            # TLS on :5327 (gRPC), :5328 (HTTP), :5337 (mTLS)
examples/leaf_mqtt/up-mqtt.sh     # Mosquitto on 127.0.0.1:1883
```

Nothing in [`stack/`](stack/) changes while they run. The shared collector
keeps 4317/4318, every other example keeps working, and each collector
overlay forwards what it accepts to the shared collector so its traces still
reach Grafana. (The broker forwards nothing; its concentrator exports to the
shared collector itself.) Start the shared stack first if you want to see
them there.

---

## Why `http://localhost:4317` and not `:4318`

The examples export OTLP/gRPC to the collector's 4317, or OTLP/HTTP over TLS.
Plaintext OTLP/HTTP doesn't work, and adding a feature wouldn't fix it; the
cause is in how the two ends speak HTTP.

microtel's transport is HTTP/2-only, so a plaintext `http://` endpoint means
h2c with prior knowledge. A stock collector's plaintext OTLP/HTTP receiver
serves HTTP/1.1 only. It answers the HTTP/2 preface with an HTTP/1.1 response,
and the connection never completes. gRPC is h2c by definition and is
unaffected. Over TLS, ALPN negotiates `h2` and OTLP/HTTP works against the same
collector.

microtel reports this in three places instead of failing silently: a `Build()`
warning, a `Connect()` error of kind `Protocol`, and
`HealthSnapshot::last_error_message`. The full story is in
[`docs/compatibility-matrix.md`](../docs/compatibility-matrix.md) §4 and issue
#166.

---

## Building

```bash
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build                                   # all examples
cmake --build build --target microtel_example_basic_trace   # just one
```

`MICROTEL_BUILD_EXAMPLES` defaults to `OFF`, so a normal build never pays for
them. Each example links `microtel_sdk`, which brings in the public headers and
every runtime link dependency (nghttp2, OpenSSL, zlib). That's the same thing a
consumer gets.

### Adding an example

1. `mkdir examples/<name>/` with `main.cpp` and `README.md`.
2. `examples/<name>/CMakeLists.txt`, one line:

   ```cmake
   microtel_add_example(<name>)
   ```

   `SOURCES` overrides the default of `main.cpp`, and is how one directory
   builds two binaries:

   ```cmake
   microtel_add_example(handoff_sender   SOURCES sender.cpp)
   microtel_add_example(handoff_receiver SOURCES receiver.cpp)
   ```

3. One `add_subdirectory(<name>)` line in
   [`CMakeLists.txt`](CMakeLists.txt), and a row in the table above.

Nothing else in the tree changes, so two examples landing in parallel only
ever touch that list.

House rules, all visible in `basic_trace/main.cpp`:

- Print the trace ID. It's what makes a run verifiable against Tempo, and the
  first thing a reader needs when the dashboard looks empty.
- Default the endpoint to `http://localhost:4317` and accept an override as
  `argv[1]`, so the stack works with no arguments.
- Build under `-Wall -Wextra -Wpedantic -Werror` (the helper adds them). In
  particular, initialise `StartSpanOptions` with all of its fields; a partial
  designated initialiser trips `-Wmissing-field-initializers`.
- Report failure through the API's own channels (`ForceFlush`'s status,
  `GetExporterHealth()`) instead of crashing, so the example still teaches
  something when no collector is running.

---

## Scope, and what CI does with these

Examples sit outside the TDD gates. Neither the diff-coverage gate nor the
test-presence check applies to `examples/**`. They're demonstrations, and a
unit test per example would be a test nobody reads.

CI still keeps them honest. The `cxx20 / clang` job in
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) configures with
`-DMICROTEL_BUILD_EXAMPLES=ON`, so every example has to compile against the
current public API on every PR. The gate paid for itself straight away: when it
was added (alongside this README) it caught `basic_trace` failing to build,
because `microtel::Status` had grown two enumerators in v1.1 and nothing had
asked the example to compile since.

Compiling is the whole contract. CI never runs the examples, because that
needs the container stack, which is a local-developer tool.
