| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | Spans/s | p50 ÷ parity-microtel-http-clang |
|---|---:|---:|---:|---:|---:|
| parity-microtel-http-clang | 100 (10 rounds) | **212** (210–216) | 469 | 1,454,384 | 1.00× |
| parity-otelcpp130-http-clang-q16k | 100 (10 rounds) | **655** (600–697) | 1837 | 749,040 | 3.10× |
| parity-otelcppmain-http-clang-q16k | 100 (10 rounds) | **734** (715–754) | 1856 | 749,456 | 3.47× |
| parity-otelcpp-http-clang-q16k | 100 (10 rounds) | **808** (790–820) | 1870 | 998,115 | 3.82× |
