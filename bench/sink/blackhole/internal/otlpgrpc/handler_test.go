// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlpgrpc_test

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/tls"
	"encoding/binary"
	"io"
	"net"
	"net/http"
	"testing"

	logpb "go.opentelemetry.io/proto/otlp/collector/logs/v1"
	metricpb "go.opentelemetry.io/proto/otlp/collector/metrics/v1"
	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	otlplogs "go.opentelemetry.io/proto/otlp/logs/v1"
	otlptrace "go.opentelemetry.io/proto/otlp/trace/v1"
	"golang.org/x/net/http2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/proto"

	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/counters"
	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/otlpgrpc"
)

const bufSize = 1 << 20 // 1 MiB

func newTestServer(t *testing.T) (tracepb.TraceServiceClient, *counters.Counters) {
	t.Helper()
	c := counters.New()
	lis := bufconn.Listen(bufSize)
	srv := grpc.NewServer(otlpgrpc.StatsHandlerOption())
	tracepb.RegisterTraceServiceServer(srv, otlpgrpc.New(c, 0))
	t.Cleanup(func() { srv.Stop() })
	go srv.Serve(lis) //nolint:errcheck

	conn, err := grpc.NewClient("passthrough://bufnet",
		grpc.WithContextDialer(func(_ context.Context, _ string) (net.Conn, error) {
			return lis.Dial()
		}),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("grpc.NewClient: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	return tracepb.NewTraceServiceClient(conn), c
}

func buildTraceRequest(nSpans int) *tracepb.ExportTraceServiceRequest {
	spans := make([]*otlptrace.Span, nSpans)
	for i := range spans {
		spans[i] = &otlptrace.Span{Name: "bench-span"}
	}
	return &tracepb.ExportTraceServiceRequest{
		ResourceSpans: []*otlptrace.ResourceSpans{
			{ScopeSpans: []*otlptrace.ScopeSpans{{Spans: spans}}},
		},
	}
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

func TestGRPC_ValidRequest_CountsSpans(t *testing.T) {
	client, c := newTestServer(t)

	req := buildTraceRequest(4)
	_, err := client.Export(context.Background(), req)
	if err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 4 {
		t.Errorf("spans_received: want 4, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors: want 0, got %d", snap.Errors)
	}
}

func TestGRPC_BytesRecorded(t *testing.T) {
	client, c := newTestServer(t)

	req := buildTraceRequest(2)
	_, err := client.Export(context.Background(), req)
	if err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.BytesReceived == 0 {
		t.Errorf("bytes_received: want > 0, got 0")
	}
}

func TestGRPC_EmptyRequest_ZeroSpans(t *testing.T) {
	client, c := newTestServer(t)

	_, err := client.Export(context.Background(), &tracepb.ExportTraceServiceRequest{})
	if err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
}

func TestGRPC_MultipleRequests_Accumulate(t *testing.T) {
	client, c := newTestServer(t)

	for _, n := range []int{3, 2, 5} {
		_, err := client.Export(context.Background(), buildTraceRequest(n))
		if err != nil {
			t.Fatalf("Export: %v", err)
		}
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 10 {
		t.Errorf("spans_received: want 10, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 3 {
		t.Errorf("grpc_requests_received: want 3, got %d", snap.GRPCRequestsReceived)
	}
	if snap.RequestsReceived != 3 {
		t.Errorf("requests_received: want 3, got %d", snap.RequestsReceived)
	}
}

// ---------------------------------------------------------------------------
// grpc-encoding: gzip  (microtel-grpc-gzip SUT)
//
// microtel is not a grpc-go client: it frames the request itself over
// nghttp2.  These tests therefore speak raw HTTP/2 so the sink is exercised
// on exactly the bytes microtel puts on the wire — a CF=0x01 length-prefixed
// message plus `grpc-encoding: gzip` — rather than through a grpc-go client
// whose own compressor registration would mask a missing one on the server.
// ---------------------------------------------------------------------------

const traceExportPath = "/opentelemetry.proto.collector.trace.v1.TraceService/Export"

// newRawServer starts a TraceService on a bufconn listener and returns an
// HTTP/2 transport (prior-knowledge cleartext) wired to it.
func newRawServer(t *testing.T) (*http2.Transport, *counters.Counters) {
	t.Helper()
	c := counters.New()
	lis := bufconn.Listen(bufSize)
	srv := grpc.NewServer(otlpgrpc.StatsHandlerOption())
	tracepb.RegisterTraceServiceServer(srv, otlpgrpc.New(c, 0))
	t.Cleanup(func() { srv.Stop() })
	go srv.Serve(lis) //nolint:errcheck

	return &http2.Transport{
		AllowHTTP: true,
		DialTLSContext: func(ctx context.Context, _, _ string, _ *tls.Config) (net.Conn, error) {
			return lis.DialContext(ctx)
		},
	}, c
}

// grpcFrame wraps payload in a gRPC length-prefixed message.
func grpcFrame(payload []byte, compressed bool) []byte {
	frame := make([]byte, 5, 5+len(payload))
	if compressed {
		frame[0] = 1
	}
	binary.BigEndian.PutUint32(frame[1:5], uint32(len(payload)))
	return append(frame, payload...)
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

// grpcStatus returns the response's grpc-status.  An error status arrives in
// a trailers-only HEADERS frame (resp.Header); a success status arrives in the
// trailers, which are only populated once the body has been drained.
func grpcStatus(t *testing.T, resp *http.Response) (string, string) {
	t.Helper()
	if _, err := io.Copy(io.Discard, resp.Body); err != nil {
		t.Fatalf("read body: %v", err)
	}
	if s := resp.Header.Get("Grpc-Status"); s != "" {
		return s, resp.Header.Get("Grpc-Message")
	}
	return resp.Trailer.Get("Grpc-Status"), resp.Trailer.Get("Grpc-Message")
}

func exportRaw(t *testing.T, tr *http2.Transport, body []byte, encoding string) *http.Response {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, "http://bufnet"+traceExportPath,
		bytes.NewReader(body))
	if err != nil {
		t.Fatalf("http.NewRequest: %v", err)
	}
	req.Header.Set("Content-Type", "application/grpc+proto")
	req.Header.Set("TE", "trailers")
	req.Header.Set("Grpc-Accept-Encoding", "gzip")
	if encoding != "" {
		req.Header.Set("Grpc-Encoding", encoding)
	}
	resp, err := tr.RoundTrip(req)
	if err != nil {
		t.Fatalf("RoundTrip: %v", err)
	}
	t.Cleanup(func() { resp.Body.Close() })
	return resp
}

func TestGRPC_GzipEncodedMessage_CountsSpans(t *testing.T) {
	tr, c := newRawServer(t)
	payload, err := proto.Marshal(buildTraceRequest(2))
	if err != nil {
		t.Fatalf("proto.Marshal: %v", err)
	}

	resp := exportRaw(t, tr, grpcFrame(gzipBytes(t, payload), true), "gzip")
	if status, msg := grpcStatus(t, resp); status != "0" {
		t.Fatalf("grpc-status: want 0, got %q (%s)", status, msg)
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 2 {
		t.Errorf("spans_received: want 2, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
}

// The identity path over the same raw framing — guards against the gzip fix
// regressing the uncompressed SUTs.
func TestGRPC_RawUncompressedMessage_CountsSpans(t *testing.T) {
	tr, c := newRawServer(t)
	payload, err := proto.Marshal(buildTraceRequest(5))
	if err != nil {
		t.Fatalf("proto.Marshal: %v", err)
	}

	resp := exportRaw(t, tr, grpcFrame(payload, false), "")
	if status, msg := grpcStatus(t, resp); status != "0" {
		t.Fatalf("grpc-status: want 0, got %q (%s)", status, msg)
	}

	if snap := c.Snapshot(); snap.SpansReceived != 5 {
		t.Errorf("spans_received: want 5, got %d", snap.SpansReceived)
	}
}

// ---------------------------------------------------------------------------
// Wire-byte accounting (#228)
//
// bytes_received must mean the same thing on both protocols: the compressed
// size of what arrived. Counting proto.Size of the message grpc-go had
// already inflated made the microtel-grpc-gzip SUT look like it compressed
// nothing, while microtel-gzip showed the real saving.
// ---------------------------------------------------------------------------

func TestGRPC_GzipEncodedMessage_CountsCompressedWireBytes(t *testing.T) {
	// 64 identical span names compress hard, so the gap is unambiguous.
	payload, err := proto.Marshal(buildTraceRequest(64))
	if err != nil {
		t.Fatalf("proto.Marshal: %v", err)
	}

	trGzip, cGzip := newRawServer(t)
	resp := exportRaw(t, trGzip, grpcFrame(gzipBytes(t, payload), true), "gzip")
	if status, msg := grpcStatus(t, resp); status != "0" {
		t.Fatalf("gzip grpc-status: want 0, got %q (%s)", status, msg)
	}

	trPlain, cPlain := newRawServer(t)
	resp = exportRaw(t, trPlain, grpcFrame(payload, false), "")
	if status, msg := grpcStatus(t, resp); status != "0" {
		t.Fatalf("identity grpc-status: want 0, got %q (%s)", status, msg)
	}

	compressed := cGzip.Snapshot().BytesReceived
	uncompressed := cPlain.Snapshot().BytesReceived
	if compressed >= uncompressed {
		t.Errorf("bytes_received: gzip'd export counted %d, identity counted %d; "+
			"want the gzip'd export strictly smaller", compressed, uncompressed)
	}
}

// The identity path pins the exact wire size, including the 5-byte gRPC
// length-prefix header the old proto.Size accounting left out.
func TestGRPC_UncompressedMessage_CountsWireBytesWithFraming(t *testing.T) {
	const grpcHeaderLen = 5

	tr, c := newRawServer(t)
	payload, err := proto.Marshal(buildTraceRequest(5))
	if err != nil {
		t.Fatalf("proto.Marshal: %v", err)
	}

	resp := exportRaw(t, tr, grpcFrame(payload, false), "")
	if status, msg := grpcStatus(t, resp); status != "0" {
		t.Fatalf("grpc-status: want 0, got %q (%s)", status, msg)
	}

	want := uint64(len(payload) + grpcHeaderLen)
	if got := c.Snapshot().BytesReceived; got != want {
		t.Errorf("bytes_received: want %d, got %d", want, got)
	}
}

// ---------------------------------------------------------------------------
// Metric handler
// ---------------------------------------------------------------------------

func newMetricTestServer(t *testing.T) (metricpb.MetricsServiceClient, *counters.Counters) {
	t.Helper()
	c := counters.New()
	lis := bufconn.Listen(bufSize)
	srv := grpc.NewServer(otlpgrpc.StatsHandlerOption())
	metricpb.RegisterMetricsServiceServer(srv, otlpgrpc.NewMetricHandler(c, 0))
	t.Cleanup(func() { srv.Stop() })
	go srv.Serve(lis) //nolint:errcheck

	conn, err := grpc.NewClient("passthrough://bufnet",
		grpc.WithContextDialer(func(_ context.Context, _ string) (net.Conn, error) {
			return lis.Dial()
		}),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("grpc.NewClient: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	return metricpb.NewMetricsServiceClient(conn), c
}

func TestGRPC_MetricExport_CountsRequestNotSpans(t *testing.T) {
	client, c := newMetricTestServer(t)

	_, err := client.Export(context.Background(), &metricpb.ExportMetricsServiceRequest{})
	if err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
}

func TestGRPC_MetricExport_AccumulatesRequests(t *testing.T) {
	client, c := newMetricTestServer(t)

	for range 3 {
		_, err := client.Export(context.Background(), &metricpb.ExportMetricsServiceRequest{})
		if err != nil {
			t.Fatalf("Export: %v", err)
		}
	}

	snap := c.Snapshot()
	if snap.GRPCRequestsReceived != 3 {
		t.Errorf("grpc_requests_received: want 3, got %d", snap.GRPCRequestsReceived)
	}
	if snap.RequestsReceived != 3 {
		t.Errorf("requests_received: want 3, got %d", snap.RequestsReceived)
	}
}

// ---------------------------------------------------------------------------
// Log handler
// ---------------------------------------------------------------------------

func newLogTestServer(t *testing.T) (logpb.LogsServiceClient, *counters.Counters) {
	t.Helper()
	c := counters.New()
	lis := bufconn.Listen(bufSize)
	srv := grpc.NewServer(otlpgrpc.StatsHandlerOption())
	logpb.RegisterLogsServiceServer(srv, otlpgrpc.NewLogHandler(c, 0))
	t.Cleanup(func() { srv.Stop() })
	go srv.Serve(lis) //nolint:errcheck

	conn, err := grpc.NewClient("passthrough://bufnet",
		grpc.WithContextDialer(func(_ context.Context, _ string) (net.Conn, error) {
			return lis.Dial()
		}),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("grpc.NewClient: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	return logpb.NewLogsServiceClient(conn), c
}

func buildLogRequest(nRecords int) *logpb.ExportLogsServiceRequest {
	records := make([]*otlplogs.LogRecord, nRecords)
	for i := range records {
		records[i] = &otlplogs.LogRecord{SeverityText: "INFO"}
	}
	// Two scopes so the count must sum across ScopeLogs, not read one.
	half := nRecords / 2
	return &logpb.ExportLogsServiceRequest{
		ResourceLogs: []*otlplogs.ResourceLogs{
			{ScopeLogs: []*otlplogs.ScopeLogs{
				{LogRecords: records[:half]},
				{LogRecords: records[half:]},
			}},
		},
	}
}

func TestGRPC_LogExport_CountsLogRecords(t *testing.T) {
	client, c := newLogTestServer(t)

	req := buildLogRequest(5)
	if _, err := client.Export(context.Background(), req); err != nil {
		t.Fatalf("Export: %v", err)
	}

	snap := c.Snapshot()
	if snap.LogRecordsReceived != 5 {
		t.Errorf("log_records_received: want 5, got %d", snap.LogRecordsReceived)
	}
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
	// Uncompressed: wire length is the message plus the 5-byte gRPC prefix.
	want := uint64(proto.Size(req)) + 5
	if snap.BytesReceived != want {
		t.Errorf("bytes_received: want %d, got %d", want, snap.BytesReceived)
	}
}

func TestGRPC_LogExport_Accumulates(t *testing.T) {
	client, c := newLogTestServer(t)

	for range 3 {
		if _, err := client.Export(context.Background(), buildLogRequest(4)); err != nil {
			t.Fatalf("Export: %v", err)
		}
	}

	if got := c.Snapshot().LogRecordsReceived; got != 12 {
		t.Errorf("log_records_received: want 12, got %d", got)
	}
}

// ---------------------------------------------------------------------------

func TestGRPC_ReturnsEmptyResponse(t *testing.T) {
	client, _ := newTestServer(t)

	resp, err := client.Export(context.Background(), buildTraceRequest(1))
	if err != nil {
		t.Fatalf("Export: %v", err)
	}
	if resp == nil {
		t.Fatal("response: want non-nil, got nil")
	}
	if resp.PartialSuccess != nil && resp.PartialSuccess.RejectedSpans != 0 {
		t.Errorf("rejected_spans: want 0, got %d", resp.PartialSuccess.RejectedSpans)
	}
}
