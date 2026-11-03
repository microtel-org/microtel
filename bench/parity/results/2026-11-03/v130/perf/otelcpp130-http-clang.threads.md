Spans emitted: 3,000,000. Background cycles: 6,765,747,489 (2,255 per span); worker cycles: 8,097,094,680 (2,699 per span).

| Stage (background threads) | Cycles per span | % of background | of which in malloc/free |
|---|---:|---:|---:|
| destroy request | 654 | 29.0 | 24% |
| build request: copy spans (MergeImpl) | 498 | 22.1 | 14% |
| batch processor | 355 | 15.7 | 3% |
| HTTP client (curl) | 335 | 14.9 | 7% |
| build request: other | 238 | 10.5 | 10% |
| serialize request | 149 | 6.6 | 0% |
| other | 21 | 0.9 | 2% |
| destroy recordables | 5 | 0.2 | 0% |

| Thread (top 5) | Cycles per span |
|---|---:|
| emit_app/887755 | 1,956 |
| emit_app/887781 | 283 |
| emit_app/887753 | 6 |
| emit_app/888173 | 0 |
| emit_app/888003 | 0 |
