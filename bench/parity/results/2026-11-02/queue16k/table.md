| SUT | Samples | Span emit p50, ns (IQR) | p95, ns | Spans/s | p50 ÷ parity-microtel-http-clang |
|---|---:|---:|---:|---:|---:|
| parity-microtel-http-clang | 50 (5 rounds) | **216** (210–227) | 477 | 1,424,908 | 1.00× |
| parity-otelcpp-http-clang-inmem-q16k | 50 (5 rounds) | **410** (408–414) | 1805 | – | 1.89× |
| parity-otelcpp-http-clang-q16k | 50 (5 rounds) | **776** (752–788) | 1848 | 983,944 | 3.58× |
