| Bucket | % of span time | % of worker time |
|---|---:|---:|
| libprotobuf | 0.0 | 0.0 |
| allocation | 11.3 | 8.9 |
| atomics and locks | 3.0 | 2.4 |
| clock reads | 22.5 | 17.8 |
| sampler | 0.5 | 0.4 |
| processor enqueue | 23.6 | 18.7 |
| attribute storage | 0.9 | 0.7 |
| other | 38.1 | 30.1 |
| harness | – | 21.0 |

Top of "other" (innermost named frame, % of span time):

- 16.8%  `microtel::sdk::SdkTracer::StartSpanInternal`
- 6.3%  `microtel::sdk::SdkSpan::SdkSpan`
- 4.4%  `std::mersenne_twister_engine<unsigned long, 64ul, 312ul, 156ul, 31ul, 13043109905998158313ul, 29ul, 6148914691236517205ul, 17ul, 8202884508482404352ul, 37ul, 18444473444759240704ul, 43ul, 6364136223846793005ul>::operator()`
- 2.2%  `bench::(anonymous namespace)::MicrotelBackend::EmitSpan`
- 2.2%  `microtel::sdk::SdkSpan::~SdkSpan`
- 2.0%  `microtel::internal::InstrumentationScope::InstrumentationScope`
- 1.3%  `microtel::internal::SpanRecord::~SpanRecord`
- 0.6%  `microtel::internal::SamplingResult::~SamplingResult`
- 0.5%  `memcpy@plt`
- 0.5%  `void std::_Destroy_aux<false>::__destroy<microtel::internal::SpanEvent*>`
- 0.4%  `microtel::CurrentContext`
- 0.4%  `microtel::sdk::SdkSpan::End`

Worker share of all sampled user-space cycles in the process: 53.7%
