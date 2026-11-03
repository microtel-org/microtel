Spans emitted: 3,000,000. Background cycles: 5,057,301,255 (1,686 per span); worker cycles: 8,167,922,931 (2,723 per span).

| Stage (background threads) | Cycles per span | % of background | of which in malloc/free |
|---|---:|---:|---:|
| destroy request | 652 | 38.7 | 20% |
| HTTP client (curl) | 338 | 20.1 | 7% |
| serialize request | 242 | 14.3 | 0% |
| build request: other | 226 | 13.4 | 10% |
| batch processor | 205 | 12.2 | 8% |
| other | 17 | 1.0 | 4% |
| destroy recordables | 6 | 0.4 | 0% |

| Thread (top 5) | Cycles per span |
|---|---:|
| emit_app/888563 | 1,389 |
| emit_app/888607 | 283 |
| emit_app/888561 | 6 |
| emit_app/888883 | 0 |
| emit_app/888782 | 0 |
