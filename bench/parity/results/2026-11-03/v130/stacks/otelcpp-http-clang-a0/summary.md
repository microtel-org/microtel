# Allocation call sites: `bench-sut-parity-otelcpp-http-clang`, 0 attributes, 10000 spans

Worker thread: 14.0002 allocations per span. Background threads: 8.6923. Table overflow: 0.

| Thread | Role | Allocations | Per span |
|---|---|---:|---:|
| 7 (exited) | worker | 140002 | 14.0002 |
| 3 (emit_app) | background | 61388 | 6.1388 |
| 5 (emit_app) | background | 25523 | 2.5523 |
| 1 (emit_app) | background | 12 | 0.0012 |

## Background: first frame outside allocator and container code

| Call site | Allocations | Per span |
|---|---:|---:|
| `?? [libcurl.so.4.8.0]` | 13730 | 1.3730 |
| `opentelemetry::proto::trace::v1::Span* google::protobuf::Arena::CreateMaybeMessage` | 10000 | 1.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordableUtils::PopulateRequest` | 9303 | 0.9303 |
| `strdup@@GLIBC_2.2.5` | 7363 | 0.7363 |
| `opentelemetry::v1::exporter::otlp::OtlpPopulateAttributeUtils::PopulateAttribute` | 5759 | 0.5759 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::GetResponseHeaders[abi:cxx11]` | 3987 | 0.3987 |
| `opentelemetry::proto::common::v1::AnyValue* google::protobuf::Arena::CreateMaybeMessage` | 3544 | 0.3544 |
| `opentelemetry::proto::common::v1::KeyValue* google::protobuf::Arena::CreateMaybeMessage` | 3544 | 0.3544 |
| `opentelemetry::proto::common::v1::KeyValue::MergeImpl` | 3101 | 0.3101 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::SendAsync` | 2215 | 0.2215 |
| `opentelemetry::v1::ext::http::client::curl::Request::AddHeader` | 1772 | 0.1772 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::WriteVectorHeaderCallback` | 1772 | 0.1772 |
| `opentelemetry::v1::ext::http::client::curl::Session::SendRequest` | 1772 | 0.1772 |
| `opentelemetry::proto::common::v1::AnyValue::MergeImpl` | 1772 | 0.1772 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpClient::createSession` | 1772 | 0.1772 |
| `opentelemetry::v1::exporter::otlp::OtlpPopulateAttributeUtils::PopulateAnyValue` | 1772 | 0.1772 |
| `opentelemetry::v1::ext::http::client::curl::HttpOperation::HttpOperation` | 1772 | 0.1772 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::CreateSession` | 1329 | 0.1329 |
| `opentelemetry::v1::ext::http::client::curl::Session::CreateRequest` | 886 | 0.0886 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpClient::Export` | 886 | 0.0886 |
| `opentelemetry::proto::common::v1::InstrumentationScope::MergeImpl` | 886 | 0.0886 |
| `curl_slist_append@@CURL_OPENSSL_4` | 886 | 0.0886 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::ScheduleAddSession` | 886 | 0.0886 |
| `opentelemetry::v1::ext::http::client::curl::HttpClient::ScheduleRemoveSession` | 886 | 0.0886 |
| `opentelemetry::proto::common::v1::InstrumentationScope* google::protobuf::Arena::CreateMaybeMessage` | 443 | 0.0443 |

## Worker: first frame outside allocator and container code

| Call site | Allocations | Per span |
|---|---:|---:|
| `opentelemetry::v1::exporter::otlp::OtlpRecordable::SetIdentity` | 50000 | 5.0000 |
| `opentelemetry::v1::sdk::trace::Tracer::StartSpan` | 30000 | 3.0000 |
| `opentelemetry::v1::sdk::trace::MultiSpanProcessor::MakeRecordable` | 20000 | 2.0000 |
| `opentelemetry::v1::trace::Tracer::GetCurrentSpan` | 20000 | 2.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpRecordable::SetName` | 10000 | 1.0000 |
| `opentelemetry::v1::exporter::otlp::OtlpHttpExporter::MakeRecordable` | 10000 | 1.0000 |
| `__cxa_thread_atexit_impl@@GLIBC_2.18` | 1 | 0.0001 |
| `opentelemetry::v1::sdk::common::::TlsRandomNumberGenerator::Seed` | 1 | 0.0001 |
