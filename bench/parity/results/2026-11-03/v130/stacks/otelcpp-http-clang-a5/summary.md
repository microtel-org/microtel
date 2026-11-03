# Allocation call sites: `bench-sut-parity-otelcpp-http-clang`, 5 attributes, 10000 spans

Worker thread: 47.0002 allocations per span. Background threads: 29.7892. Table overflow: 0.

| Thread | Role | Allocations | Per span |
|---|---|---:|---:|
| 7 (exited) | worker | 470002 | 47.0002 |
| 3 (emit_app) | background | 200093 | 20.0093 |
| 5 (emit_app) | background | 97787 | 9.7787 |
| 1 (emit_app) | background | 12 | 0.0012 |

## Background: first frame outside allocator and container code

| Call site | Allocations | Per span |
|---|---:|---:|
| `?? [libcurl.so.4.8.0]` | 52606 | 5.2606 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordableUtils::PopulateRequest` | 28877 | 2.8877 |
| `strdup@@GLIBC_2.2.5` | 28212 | 2.8212 |
| `opentelemetry::v1::exporter::otlp::OtlpPopulateAttributeUtils::PopulateAttribute` | 22061 | 2.2061 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::GetResponseHeaders[abi:cxx11]` | 15273 | 1.5273 |
| `opentelemetry::proto::common::v1::AnyValue* google::protobuf::Arena::CreateMaybeMessage` | 13576 | 1.3576 |
| `opentelemetry::proto::common::v1::KeyValue* google::protobuf::Arena::CreateMaybeMessage` | 13576 | 1.3576 |
| `opentelemetry::proto::common::v1::KeyValue::MergeImpl` | 11879 | 1.1879 |
| `opentelemetry::proto::trace::v1::Span* google::protobuf::Arena::CreateMaybeMessage` | 10000 | 1.0000 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::SendAsync` | 8485 | 0.8485 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpClient::createSession` | 6788 | 0.6788 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::WriteVectorHeaderCallback` | 6788 | 0.6788 |
| `opentelemetry::v1::ext::http::client::curl::Session::SendRequest` | 6788 | 0.6788 |
| `opentelemetry::proto::common::v1::AnyValue::MergeImpl` | 6788 | 0.6788 |
| `opentelemetry::v1::ext::http::client::curl::Request::AddHeader` | 6788 | 0.6788 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::HttpOperation` | 6788 | 0.6788 |
| `opentelemetry::v1::exporter::otlp::OtlpPopulateAttributeUtils::PopulateAnyValue` | 6788 | 0.6788 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::CreateSession` | 5091 | 0.5091 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpClient::Export` | 3394 | 0.3394 |
| `opentelemetry::proto::common::v1::InstrumentationScope::MergeImpl` | 3394 | 0.3394 |
| `curl_slist_append@@CURL_OPENSSL_4` | 3394 | 0.3394 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::ScheduleRemoveSession` | 3394 | 0.3394 |
| `opentelemetry::v1::ext::http::client::curl::Session::CreateRequest` | 3394 | 0.3394 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::ScheduleAddSession` | 3394 | 0.3394 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpClient::ReleaseSession` | 1697 | 0.1697 |

## Worker: first frame outside allocator and container code

| Call site | Allocations | Per span |
|---|---:|---:|
| `void opentelemetry::proto::common::v1::AnyValue::set_string_value` | 150000 | 15.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordable::SetIdentity` | 50000 | 5.0000 |
| `opentelemetry::proto::common::v1::KeyValue* google::protobuf::Arena::CreateMaybeMessage` | 50000 | 5.0000 |
| `opentelemetry::proto::common::v1::AnyValue* google::protobuf::Arena::CreateMaybeMessage` | 50000 | 5.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpPopulateAttributeUtils::PopulateAttribute` | 50000 | 5.0000 |
| `opentelemetry::v1::sdk::trace::Tracer::StartSpan` | 30000 | 3.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordable::SetAttribute` | 30000 | 3.0000 |
| `opentelemetry::v1::sdk::trace::MultiSpanProcessor::MakeRecordable` | 20000 | 2.0000 |
| `opentelemetry::v1::trace::Tracer::GetCurrentSpan` | 20000 | 2.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpExporter::MakeRecordable` | 10000 | 1.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordable::SetName` | 10000 | 1.0000 |
| `opentelemetry::v1::sdk::common::::TlsRandomNumberGenerator::Seed` | 1 | 0.0001 |
| `__cxa_thread_atexit_impl@@GLIBC_2.18` | 1 | 0.0001 |
