Spans emitted: 3,000,000. Background cycles: 4,711,819,559 (1,571 per span); worker cycles: 8,385,771,607 (2,795 per span).

| Stage (background threads) | Cycles per span | % of background | of which in malloc/free |
|---|---:|---:|---:|
| destroy request | 691 | 44.0 | 22% |
| build request: other | 392 | 25.0 | 11% |
| serialize request | 251 | 16.0 | 0% |
| HTTP client (curl) | 154 | 9.8 | 6% |
| batch processor | 47 | 3.0 | 4% |
| other | 32 | 2.0 | 2% |
| build request: copy spans (MergeImpl) | 3 | 0.2 | 29% |

| Thread (top 5) | Cycles per span |
|---|---:|
| emit_app/886958 | 1,406 |
| emit_app/886981 | 135 |
| emit_app/886956 | 6 |
| emit_app/887284 | 1 |
| emit_app/887112 | 1 |
