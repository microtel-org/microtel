| | v1.30.0 | main | main − v1.30.0 |
|---|---:|---:|---:|
| worker-thread cycles per span (all) | 2657 | 2547 | -110 |
| bucket: libprotobuf | 943 | 950 | +7 |
| bucket: other | 706 | 635 | -70 |
| bucket: clock reads | 278 | 261 | -16 |
| bucket: harness | 238 | 234 | -5 |
| bucket: processor enqueue | 225 | 191 | -34 |
| bucket: allocation | 190 | 190 | +1 |
| bucket: atomics and locks | 50 | 55 | +5 |
| bucket: sampler | 14 | 17 | +3 |
| bucket: attribute storage | 13 | 13 | +0 |

Largest self-cycle differences per span (leaf frame), worker thread:
   -72.7  (v130  115.1, main   42.4)  opentelemetry::v1::sdk::trace::Span::Span
   -34.9  (v130   40.7, main    5.8)  opentelemetry::v1::sdk::trace::MultiSpanProcessor::OnStart
   +10.3  (v130   10.0, main   20.3)  opentelemetry::v1::nostd::shared_ptr<opentelemetry::v1::trace::Span>::shared_ptr_wrapper::MoveTo
   -10.0  (v130  409.4, main  399.4)  [[vdso]]
    +8.3  (v130  564.0, main  572.2)  [libc.so.6]
    +7.4  (v130  189.4, main  196.8)  malloc
    -6.2  (v130   29.6, main   23.4)  std::chrono::_V2::steady_clock::now
    +5.2  (v130   92.1, main   97.3)  [[unknown]]
    -4.7  (v130   59.8, main   55.2)  cfree
    -4.6  (v130   19.7, main   15.1)  opentelemetry::v1::nostd::shared_ptr<opentelemetry::v1::trace::Span>::shared_ptr_wrapper::~shared_ptr_wrapper
    +4.6  (v130    5.4, main   10.0)  opentelemetry::v1::sdk::trace::MultiRecordable::AddRecordable
    -4.5  (v130  107.3, main  102.7)  opentelemetry::v1::sdk::trace::Tracer::StartSpan
