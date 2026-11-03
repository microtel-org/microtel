| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | Spans/s | p50 ÷ parity-microtel-http-clang |
|---|---:|---:|---:|---:|---:|
| parity-microtel-http-clang | 100 (10 rounds) | **219** (210–232) | 480 | 1,433,904 | 1.00× |
| parity-otelcpp130-http-clang-q16k | 100 (10 rounds) | **722** (684–749) | 1838 | 735,113 | 3.29× |
| parity-otelcpp-http-clang-q16k | 100 (10 rounds) | **772** (758–792) | 1846 | 974,658 | 3.53× |
