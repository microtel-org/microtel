| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | Spans/s | p50 ÷ parity-microtel-http-clang |
|---|---:|---:|---:|---:|---:|
| parity-microtel-http-clang | 100 (10 rounds) | **219** (210–232) | 479 | 1,414,674 | 1.00× |
| parity-otelcpp130-http-clang-q16k | 100 (10 rounds) | **740** (716–760) | 1826 | 743,251 | 3.38× |
| parity-otelcpp130-http-clang-move-q16k | 100 (10 rounds) | **747** (729–769) | 1792 | 883,936 | 3.41× |
| parity-otelcpp-http-clang-q16k | 100 (10 rounds) | **766** (756–792) | 1854 | 970,293 | 3.50× |
