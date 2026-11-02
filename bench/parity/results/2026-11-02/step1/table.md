| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | Spans/s | p50 ÷ parity-microtel-http-clang |
|---|---:|---:|---:|---:|---:|
| parity-microtel-http-clang | 50 (5 rounds) | **216** (210–230) | 478 | 1,434,665 | 1.00× |
| parity-otelcpp-http-clang-inmem | 50 (5 rounds) | **411** (407–415) | 1789 | – | 1.90× |
| parity-otelcpp-http-clang | 50 (5 rounds) | **793** (777–811) | 2786 | 835,774 | 3.66× |
