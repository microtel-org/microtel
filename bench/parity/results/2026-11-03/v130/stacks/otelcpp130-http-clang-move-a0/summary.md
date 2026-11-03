# Allocation call sites: `bench-sut-parity-otelcpp130-http-clang-move`, 0 attributes, 10000 spans

Worker thread: 10.0002 allocations per span. Background threads: 1.3017. Table overflow: 0.

| Thread | Role | Allocations | Per span |
|---|---|---:|---:|
| 7 (exited) | worker | 100002 | 10.0002 |
| 3 (emit_app) | background | 11859 | 1.1859 |
| 5 (emit_app) | background | 1146 | 0.1146 |
| 1 (emit_app) | background | 12 | 0.0012 |

## Background: first frame outside allocator and container code

| Call site | Allocations | Per span |
|---|---:|---:|
| `opentelemetry::proto::trace::v1::Span* google::protobuf::Arena::CreateMaybeMessage` | 10000 | 1.0000 |
| `?? [libcurl.so.4.8.0]` | 616 | 0.0616 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordableUtils::PopulateRequest` | 399 | 0.0399 |
| `strdup@@GLIBC_2.2.5` | 330 | 0.0330 |
| `opentelemetry::v1::exporter::otlp::OtlpPopulateAttributeUtils::PopulateAttribute` | 260 | 0.0260 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::GetResponseHeaders[abi:cxx11]` | 180 | 0.0180 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::SendAsync` | 100 | 0.0100 |
| `opentelemetry::proto::common::v1::KeyValue* google::protobuf::Arena::CreateMaybeMessage` | 80 | 0.0080 |
| `opentelemetry::proto::common::v1::AnyValue* google::protobuf::Arena::CreateMaybeMessage` | 80 | 0.0080 |
| `opentelemetry::v1::ext::http::client::curl::Session::SendRequest` | 80 | 0.0080 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::HttpOperation` | 80 | 0.0080 |
| `opentelemetry::v1::ext::http::client::curl::Request::AddHeader` | 80 | 0.0080 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::WriteVectorHeaderCallback` | 80 | 0.0080 |
| `void opentelemetry::proto::common::v1::AnyValue::set_string_value` | 80 | 0.0080 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::CreateSession` | 60 | 0.0060 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpClient::createSession` | 60 | 0.0060 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::ScheduleAddSession` | 40 | 0.0040 |
| `opentelemetry::v1::ext::http::client::curl::Session::CreateRequest` | 40 | 0.0040 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpClient::Export` | 40 | 0.0040 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::ScheduleRemoveSession` | 40 | 0.0040 |
| `curl_slist_append@@CURL_OPENSSL_4` | 40 | 0.0040 |
| `opentelemetry::proto::trace::v1::ResourceSpans* google::protobuf::Arena::CreateMaybeMessage` | 20 | 0.0020 |
| `opentelemetry::proto::common::v1::InstrumentationScope* google::protobuf::Arena::CreateMaybeMessage` | 20 | 0.0020 |
| `opentelemetry::v1::sdk::trace::BatchSpanProcessor::Export` | 20 | 0.0020 |
| `opentelemetry::proto::trace::v1::ScopeSpans* google::protobuf::Arena::CreateMaybeMessage` | 20 | 0.0020 |

## Worker: first frame outside allocator and container code

| Call site | Allocations | Per span |
|---|---:|---:|
| `opentelemetry::v1::exporter::otlp::OtlpRecordable::SetIdentity` | 50000 | 5.0000 |
| `opentelemetry::v1::sdk::trace::MultiRecordable::AddRecordable` | 10000 | 1.0000 |
| `opentelemetry::v1::sdk::trace::Tracer::StartSpan` | 10000 | 1.0000 |
| `opentelemetry::v1::sdk::trace::MultiSpanProcessor::MakeRecordable` | 10000 | 1.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordable::SetName` | 10000 | 1.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpExporter::MakeRecordable` | 10000 | 1.0000 |
| `opentelemetry::v1::sdk::common::::TlsRandomNumberGenerator::Seed` | 1 | 0.0001 |
| `__cxa_thread_atexit_impl@@GLIBC_2.18` | 1 | 0.0001 |
