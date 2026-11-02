| Bucket | % of span time | % of worker time |
|---|---:|---:|
| libprotobuf | 33.5 | 30.6 |
| allocation | 12.9 | 11.8 |
| atomics and locks | 2.2 | 2.0 |
| clock reads | 10.8 | 9.8 |
| sampler | 0.7 | 0.6 |
| processor enqueue | 9.7 | 8.8 |
| attribute storage | 0.6 | 0.5 |
| other | 29.7 | 27.0 |
| harness | – | 8.8 |

Top of "other" (innermost named frame, % of span time):

- 6.1%  `opentelemetry::v1::nostd::shared_ptr<opentelemetry::v1::trace::TraceState>::shared_ptr_wrapper::~shared_ptr_wrapper`
- 3.5%  `opentelemetry::v1::sdk::trace::Tracer::StartSpan`
- 2.4%  `opentelemetry::v1::sdk::trace::Span::Span`
- 2.0%  `bench::(anonymous namespace)::OtelCppBackend::EmitSpan`
- 1.7%  `opentelemetry::v1::nostd::shared_ptr<opentelemetry::v1::trace::TraceState>::shared_ptr_wrapper::CopyTo(opentelemetry::v1::nostd::shared_ptr<opentelemetry::v1::trace::TraceState>::PlacementBuffer&) const`
- 1.3%  `std::_Rb_tree_increment`
- 1.2%  `opentelemetry::v1::trace::Tracer::GetCurrentSpan`
- 0.8%  `opentelemetry::v1::sdk::common::Random::GenerateRandomBuffer`
- 0.6%  `opentelemetry::v1::nostd::shared_ptr<opentelemetry::v1::trace::TraceState>::shared_ptr_wrapper::MoveTo`
- 0.6%  `opentelemetry::v1::sdk::trace::Span::~Span`
- 0.6%  `opentelemetry::v1::sdk::trace::MultiRecordable::SetName`
- 0.5%  `opentelemetry::v1::nostd::shared_ptr<opentelemetry::v1::context::Context::DataList>::shared_ptr_wrapper::MoveTo`

Worker share of all sampled user-space cycles in the process: 65.8%
