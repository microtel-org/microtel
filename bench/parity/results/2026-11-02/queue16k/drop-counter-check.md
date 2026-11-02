Drop counter check: one round, default BatchSpanProcessor queue (2048), images built at d58009a.

| SUT | Spans emitted | Received by sink | Missing | Drop counter (cumulative, per sample) |
|---|---:|---:|---:|---|
| parity-otelcpp-http-clang | 100,000 | 97,801 | 2,199 | 481, 672, 993, 1259, 1614, 1662, 1774, 1775, 1775, 2199 |
| parity-otelcpp-http-clang-inmem | 100,000 | 0 | 100,000 | 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 |
