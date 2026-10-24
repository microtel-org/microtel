// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlpgrpc

import (
	"context"
	"sync/atomic"
	"time"

	logpb "go.opentelemetry.io/proto/otlp/collector/logs/v1"
	metricpb "go.opentelemetry.io/proto/otlp/collector/metrics/v1"
	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	"google.golang.org/grpc"

	// grpc-go inflates `grpc-encoding: gzip` messages only when a compressor
	// is registered for that name, and no compressor is registered by default.
	// Without this import the server answers a compressed export (the
	// microtel-grpc-gzip SUT) with Unimplemented and counts nothing.
	_ "google.golang.org/grpc/encoding/gzip"
	"google.golang.org/grpc/stats"
	"google.golang.org/protobuf/proto"

	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/counters"
)

// wireBytes accumulates the on-the-wire size of one RPC's inbound messages.
type wireBytes struct{ n atomic.Uint64 }

type wireBytesKeyType struct{}

// wireBytesKey addresses the per-RPC wireBytes carried on the handler context.
var wireBytesKey wireBytesKeyType

// statsHandler records stats.InPayload.WireLength — the compressed payload
// plus the 5-byte gRPC length-prefix header — for every inbound message, so
// the handlers can count what actually crossed the wire.
//
// Without it the gRPC path reported proto.Size of the message grpc-go had
// already inflated, which is the uncompressed size; the HTTP path has always
// counted the raw body. The two protocols now measure the same thing (#228).
type statsHandler struct{}

// StatsHandlerOption is the grpc.ServerOption that installs the wire-byte
// accounting. Every server that registers these handlers must pass it, or
// bytes_received silently falls back to the uncompressed proto.Size.
func StatsHandlerOption() grpc.ServerOption {
	return grpc.StatsHandler(statsHandler{})
}

func (statsHandler) TagRPC(ctx context.Context, _ *stats.RPCTagInfo) context.Context {
	return context.WithValue(ctx, wireBytesKey, &wireBytes{})
}

func (statsHandler) HandleRPC(ctx context.Context, s stats.RPCStats) {
	in, ok := s.(*stats.InPayload)
	if !ok {
		return
	}
	if wb, ok := ctx.Value(wireBytesKey).(*wireBytes); ok {
		wb.n.Add(uint64(in.WireLength))
	}
}

func (statsHandler) TagConn(ctx context.Context, _ *stats.ConnTagInfo) context.Context {
	return ctx
}

func (statsHandler) HandleConn(context.Context, stats.ConnStats) {}

// requestWireBytes returns the wire size recorded for this RPC, falling back
// to the decoded size when no statsHandler is installed.
func requestWireBytes(ctx context.Context, fallback uint64) uint64 {
	wb, ok := ctx.Value(wireBytesKey).(*wireBytes)
	if !ok {
		return fallback
	}
	if n := wb.n.Load(); n > 0 {
		return n
	}
	return fallback
}

// TraceHandler implements the OTLP TraceService gRPC endpoint.
type TraceHandler struct {
	tracepb.UnimplementedTraceServiceServer
	c       *counters.Counters
	delayMs int
}

// New returns a TraceHandler that records exports into c.
func New(c *counters.Counters, delayMs int) *TraceHandler {
	return &TraceHandler{c: c, delayMs: delayMs}
}

// Export counts spans and bytes, then returns an empty success response.
func (h *TraceHandler) Export(
	ctx context.Context,
	req *tracepb.ExportTraceServiceRequest,
) (*tracepb.ExportTraceServiceResponse, error) {
	reqBytes := requestWireBytes(ctx, uint64(proto.Size(req)))

	spans := countSpans(req)

	resp := &tracepb.ExportTraceServiceResponse{}
	respBytes := uint64(proto.Size(resp))

	if h.delayMs > 0 {
		time.Sleep(time.Duration(h.delayMs) * time.Millisecond)
	}
	h.c.RecordGRPCExport(spans, reqBytes, respBytes)
	return resp, nil
}

func countSpans(req *tracepb.ExportTraceServiceRequest) uint64 {
	var n uint64
	for _, rs := range req.ResourceSpans {
		for _, ss := range rs.ScopeSpans {
			n += uint64(len(ss.Spans))
		}
	}
	return n
}

// MetricHandler implements the OTLP MetricsService gRPC endpoint.
// It counts requests and bytes but does not decode metric data points (B0 stub).
type MetricHandler struct {
	metricpb.UnimplementedMetricsServiceServer
	c       *counters.Counters
	delayMs int
}

// NewMetricHandler returns a MetricHandler that records exports into c.
func NewMetricHandler(c *counters.Counters, delayMs int) *MetricHandler {
	return &MetricHandler{c: c, delayMs: delayMs}
}

// Export counts bytes and returns an empty success response.
func (h *MetricHandler) Export(
	ctx context.Context,
	req *metricpb.ExportMetricsServiceRequest,
) (*metricpb.ExportMetricsServiceResponse, error) {
	reqBytes := requestWireBytes(ctx, uint64(proto.Size(req)))
	resp := &metricpb.ExportMetricsServiceResponse{}
	respBytes := uint64(proto.Size(resp))
	if h.delayMs > 0 {
		time.Sleep(time.Duration(h.delayMs) * time.Millisecond)
	}
	h.c.RecordGRPCExport(0, reqBytes, respBytes)
	return resp, nil
}

// LogHandler implements the OTLP LogsService gRPC endpoint.
// It counts LogRecords, bytes and requests.
type LogHandler struct {
	logpb.UnimplementedLogsServiceServer
	c       *counters.Counters
	delayMs int
}

// NewLogHandler returns a LogHandler that records exports into c.
func NewLogHandler(c *counters.Counters, delayMs int) *LogHandler {
	return &LogHandler{c: c, delayMs: delayMs}
}

// Export counts log records and bytes, then returns an empty success response.
func (h *LogHandler) Export(
	ctx context.Context,
	req *logpb.ExportLogsServiceRequest,
) (*logpb.ExportLogsServiceResponse, error) {
	reqBytes := requestWireBytes(ctx, uint64(proto.Size(req)))
	resp := &logpb.ExportLogsServiceResponse{}
	respBytes := uint64(proto.Size(resp))
	if h.delayMs > 0 {
		time.Sleep(time.Duration(h.delayMs) * time.Millisecond)
	}
	h.c.RecordGRPCLogExport(countLogRecords(req), reqBytes, respBytes)
	return resp, nil
}

func countLogRecords(req *logpb.ExportLogsServiceRequest) uint64 {
	var n uint64
	for _, rl := range req.ResourceLogs {
		for _, sl := range rl.ScopeLogs {
			n += uint64(len(sl.LogRecords))
		}
	}
	return n
}
