// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlphttp_test

import (
	"bytes"
	"compress/gzip"
	"net/http"
	"net/http/httptest"
	"testing"

	logpb "go.opentelemetry.io/proto/otlp/collector/logs/v1"
	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	otlplogs "go.opentelemetry.io/proto/otlp/logs/v1"
	otlptrace "go.opentelemetry.io/proto/otlp/trace/v1"
	"google.golang.org/protobuf/proto"

	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/counters"
	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/otlphttp"
)

func buildTraceRequest(nSpans int) []byte {
	spans := make([]*otlptrace.Span, nSpans)
	for i := range spans {
		spans[i] = &otlptrace.Span{Name: "bench-span"}
	}
	req := &tracepb.ExportTraceServiceRequest{
		ResourceSpans: []*otlptrace.ResourceSpans{
			{ScopeSpans: []*otlptrace.ScopeSpans{{Spans: spans}}},
		},
	}
	b, _ := proto.Marshal(req)
	return b
}

func gzipBytes(t *testing.T, b []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := gzip.NewWriter(&buf)
	if _, err := zw.Write(b); err != nil {
		t.Fatalf("gzip write: %v", err)
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("gzip close: %v", err)
	}
	return buf.Bytes()
}

func newTestHandler() (http.Handler, *counters.Counters) {
	c := counters.New()
	return otlphttp.NewHandler(c, 0), c
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

func TestHTTP_ValidRequest_CountsSpans(t *testing.T) {
	h, c := newTestHandler()
	body := buildTraceRequest(3)

	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	snap := c.Snapshot()
	if snap.SpansReceived != 3 {
		t.Errorf("spans_received: want 3, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != uint64(len(body)) {
		t.Errorf("bytes_received: want %d, got %d", len(body), snap.BytesReceived)
	}
	if snap.HTTPRequestsReceived != 1 {
		t.Errorf("http_requests_received: want 1, got %d", snap.HTTPRequestsReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors: want 0, got %d", snap.Errors)
	}
}

func TestHTTP_EmptyBody_ZeroSpans(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader([]byte{}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	if c.Snapshot().SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", c.Snapshot().SpansReceived)
	}
}

func TestHTTP_MultipleRequests_Accumulate(t *testing.T) {
	h, c := newTestHandler()
	for _, n := range []int{2, 5, 1} {
		body := buildTraceRequest(n)
		req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/x-protobuf")
		h.ServeHTTP(httptest.NewRecorder(), req)
	}
	if c.Snapshot().SpansReceived != 8 {
		t.Errorf("spans_received: want 8, got %d", c.Snapshot().SpansReceived)
	}
	if c.Snapshot().RequestsReceived != 3 {
		t.Errorf("requests_received: want 3, got %d", c.Snapshot().RequestsReceived)
	}
}

func TestHTTP_ResponseContentType(t *testing.T) {
	h, _ := newTestHandler()
	body := buildTraceRequest(1)
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if ct := rec.Header().Get("Content-Type"); ct != "application/x-protobuf" {
		t.Errorf("response Content-Type: want application/x-protobuf, got %q", ct)
	}
}

// ---------------------------------------------------------------------------
// content-encoding: gzip  (microtel's EMIT_COMPRESSION_GZIP=1 SUTs)
// ---------------------------------------------------------------------------

func TestHTTP_GzipEncodedBody_CountsSpans(t *testing.T) {
	h, c := newTestHandler()
	plain := buildTraceRequest(3)
	body := gzipBytes(t, plain)
	if len(body) == len(plain) {
		t.Fatalf("test setup: gzipped body is the same size as the plain body (%d)", len(body))
	}

	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/x-protobuf")
	req.Header.Set("Content-Encoding", "gzip")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	snap := c.Snapshot()
	if snap.SpansReceived != 3 {
		t.Errorf("spans_received: want 3, got %d", snap.SpansReceived)
	}
	// bytes_received must stay the compressed wire size — the whole point of
	// the compression profile is to measure bytes on the wire.
	if snap.BytesReceived != uint64(len(body)) {
		t.Errorf("bytes_received: want %d (compressed), got %d", len(body), snap.BytesReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors: want 0, got %d (last_error %q)", snap.Errors, snap.LastError)
	}
}

func TestHTTP_GzipEncodedBody_MixedWithPlain_Accumulates(t *testing.T) {
	h, c := newTestHandler()

	gz := httptest.NewRequest(http.MethodPost, "/v1/traces",
		bytes.NewReader(gzipBytes(t, buildTraceRequest(4))))
	gz.Header.Set("Content-Type", "application/x-protobuf")
	gz.Header.Set("Content-Encoding", "gzip")
	h.ServeHTTP(httptest.NewRecorder(), gz)

	plain := httptest.NewRequest(http.MethodPost, "/v1/traces",
		bytes.NewReader(buildTraceRequest(2)))
	plain.Header.Set("Content-Type", "application/x-protobuf")
	h.ServeHTTP(httptest.NewRecorder(), plain)

	if snap := c.Snapshot(); snap.SpansReceived != 6 {
		t.Errorf("spans_received: want 6, got %d", snap.SpansReceived)
	}
}

func TestHTTP_GzipHeaderWithBadBody_Returns400(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/traces",
		bytes.NewReader([]byte("not gzip at all")))
	req.Header.Set("Content-Type", "application/x-protobuf")
	req.Header.Set("Content-Encoding", "gzip")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("status: want 400, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

func TestHTTP_WrongMethod_Returns405(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodGet, "/v1/traces", nil)
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("status: want 405, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

func TestHTTP_WrongContentType_Returns415(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader([]byte("hello")))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusUnsupportedMediaType {
		t.Errorf("status: want 415, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

func TestHTTP_InvalidProto_Returns400(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/traces", bytes.NewReader([]byte("not protobuf!!!")))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("status: want 400, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

// ---------------------------------------------------------------------------
// Unknown path catch-all
// ---------------------------------------------------------------------------

func TestHTTP_UnknownPath_Returns404AndRecordsError(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/wrong/path", bytes.NewReader([]byte{}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("status: want 404, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
	if c.Snapshot().LastError == "" {
		t.Error("last_error should be non-empty for unknown path")
	}
}

// ---------------------------------------------------------------------------
// Stub paths (metrics, logs)
// ---------------------------------------------------------------------------

func TestHTTP_MetricsStub_Returns200AndCounts(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/metrics", bytes.NewReader([]byte{}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	snap := c.Snapshot()
	if snap.HTTPRequestsReceived != 1 {
		t.Errorf("http_requests_received: want 1, got %d", snap.HTTPRequestsReceived)
	}
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
}

// ---------------------------------------------------------------------------
// Logs
// ---------------------------------------------------------------------------

func buildLogRequest(nRecords int) []byte {
	records := make([]*otlplogs.LogRecord, nRecords)
	for i := range records {
		records[i] = &otlplogs.LogRecord{SeverityText: "INFO"}
	}
	half := nRecords / 2
	req := &logpb.ExportLogsServiceRequest{
		ResourceLogs: []*otlplogs.ResourceLogs{
			{ScopeLogs: []*otlplogs.ScopeLogs{
				{LogRecords: records[:half]},
				{LogRecords: records[half:]},
			}},
		},
	}
	b, _ := proto.Marshal(req)
	return b
}

func TestHTTP_Logs_CountsLogRecords(t *testing.T) {
	h, c := newTestHandler()
	body := buildLogRequest(6)

	req := httptest.NewRequest(http.MethodPost, "/v1/logs", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	snap := c.Snapshot()
	if snap.LogRecordsReceived != 6 {
		t.Errorf("log_records_received: want 6, got %d", snap.LogRecordsReceived)
	}
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != uint64(len(body)) {
		t.Errorf("bytes_received: want %d, got %d", len(body), snap.BytesReceived)
	}
}

func TestHTTP_Logs_GzipEncodedBody_CountsRecordsAndWireBytes(t *testing.T) {
	h, c := newTestHandler()
	body := gzipBytes(t, buildLogRequest(4))

	req := httptest.NewRequest(http.MethodPost, "/v1/logs", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/x-protobuf")
	req.Header.Set("Content-Encoding", "gzip")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	snap := c.Snapshot()
	if snap.LogRecordsReceived != 4 {
		t.Errorf("log_records_received: want 4, got %d", snap.LogRecordsReceived)
	}
	if snap.BytesReceived != uint64(len(body)) {
		t.Errorf("bytes_received: want compressed %d, got %d", len(body), snap.BytesReceived)
	}
}

func TestHTTP_Logs_InvalidProto_Returns400(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/logs",
		bytes.NewReader([]byte{0xff, 0xff, 0xff, 0xff}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("status: want 400, got %d", rec.Code)
	}
	if c.Snapshot().Errors != 1 {
		t.Errorf("errors: want 1, got %d", c.Snapshot().Errors)
	}
}

func TestHTTP_LogsStub_Returns200AndCounts(t *testing.T) {
	h, c := newTestHandler()
	req := httptest.NewRequest(http.MethodPost, "/v1/logs", bytes.NewReader([]byte{}))
	req.Header.Set("Content-Type", "application/x-protobuf")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	snap := c.Snapshot()
	if snap.HTTPRequestsReceived != 1 {
		t.Errorf("http_requests_received: want 1, got %d", snap.HTTPRequestsReceived)
	}
}
