// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package otlphttp

import (
	"bytes"
	"compress/gzip"
	"io"
	"net/http"
	"strings"
	"time"

	logpb "go.opentelemetry.io/proto/otlp/collector/logs/v1"
	tracepb "go.opentelemetry.io/proto/otlp/collector/trace/v1"
	"google.golang.org/protobuf/proto"

	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/counters"
)

// NewHandler returns an http.Handler for the OTLP/HTTP listener on :4318.
// Routes: POST /v1/traces and /v1/logs (parsed), POST /v1/metrics (stubbed).
// The catch-all "/" route records unknown-path requests as errors so the bench
// driver can distinguish "no requests arriving" from "requests hitting wrong path".
func NewHandler(c *counters.Counters, delayMs int) http.Handler {
	mux := http.NewServeMux()
	mux.Handle("/v1/traces", &traceHandler{c: c, delayMs: delayMs})
	mux.HandleFunc("/v1/metrics", makeStubHandler(c))
	mux.Handle("/v1/logs", &logHandler{c: c, delayMs: delayMs})
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		c.RecordError("unknown path: " + r.URL.Path)
		http.NotFound(w, r)
	})
	return mux
}

// makeStubHandler returns a handler that accepts any POST, counts it in the
// shared Counters (http_requests_received), and returns 200 with no body.
// Used for metrics — content is accepted but not decoded.
func makeStubHandler(c *counters.Counters) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		body, _ := io.ReadAll(r.Body)
		w.Header().Set("Content-Type", "application/x-protobuf")
		w.WriteHeader(http.StatusOK)
		c.RecordHTTPExport(0, uint64(len(body)), 0)
	}
}

type traceHandler struct {
	c       *counters.Counters
	delayMs int
}

func (h *traceHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	body, reqBytes, ok := readExportBody(w, r, h.c)
	if !ok {
		return
	}

	var req tracepb.ExportTraceServiceRequest
	if err := proto.Unmarshal(body, &req); err != nil {
		http.Error(w, "invalid protobuf", http.StatusBadRequest)
		h.c.RecordError("unmarshal: " + err.Error())
		return
	}

	spans := countSpans(&req)

	// An empty ExportTraceServiceResponse serializes to zero bytes, which
	// is a valid OTLP/HTTP response body per the spec.
	resp := &tracepb.ExportTraceServiceResponse{}
	respBody, _ := proto.Marshal(resp)

	writeExportResponse(w, respBody, h.delayMs)
	h.c.RecordHTTPExport(spans, reqBytes, uint64(len(respBody)))
}

type logHandler struct {
	c       *counters.Counters
	delayMs int
}

// ServeHTTP decodes an ExportLogsServiceRequest and counts its LogRecords,
// so the logs profile can report delivery and wire bytes per record.
func (h *logHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	body, reqBytes, ok := readExportBody(w, r, h.c)
	if !ok {
		return
	}

	var req logpb.ExportLogsServiceRequest
	if err := proto.Unmarshal(body, &req); err != nil {
		http.Error(w, "invalid protobuf", http.StatusBadRequest)
		h.c.RecordError("unmarshal: " + err.Error())
		return
	}

	respBody, _ := proto.Marshal(&logpb.ExportLogsServiceResponse{})
	writeExportResponse(w, respBody, h.delayMs)
	h.c.RecordHTTPLogExport(countLogRecords(&req), reqBytes, uint64(len(respBody)))
}

// readExportBody validates method and content type, reads the body and
// inflates it when gzip-encoded. reqBytes is the wire size, taken before
// inflation: the compression profile measures bytes per item on the wire.
// On failure it has already written the HTTP error and recorded it.
func readExportBody(w http.ResponseWriter, r *http.Request, c *counters.Counters) ([]byte, uint64, bool) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		c.RecordError("wrong method: " + r.Method)
		return nil, 0, false
	}
	if ct := r.Header.Get("Content-Type"); ct != "application/x-protobuf" {
		http.Error(w, "unsupported media type", http.StatusUnsupportedMediaType)
		c.RecordError("wrong content-type: " + ct)
		return nil, 0, false
	}

	// Read the full body first so reqBytes reflects actual wire bytes,
	// not Content-Length (which may be -1 for chunked transfers).
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "failed to read body", http.StatusInternalServerError)
		c.RecordError("read body: " + err.Error())
		return nil, 0, false
	}
	reqBytes := uint64(len(body))

	if strings.EqualFold(r.Header.Get("Content-Encoding"), "gzip") {
		body, err = gunzip(body)
		if err != nil {
			http.Error(w, "invalid gzip body", http.StatusBadRequest)
			c.RecordError("gunzip: " + err.Error())
			return nil, 0, false
		}
	}
	return body, reqBytes, true
}

// writeExportResponse sleeps for the configured response delay, then writes a
// 200 with the serialized OTLP response.
func writeExportResponse(w http.ResponseWriter, respBody []byte, delayMs int) {
	if delayMs > 0 {
		time.Sleep(time.Duration(delayMs) * time.Millisecond)
	}
	w.Header().Set("Content-Type", "application/x-protobuf")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(respBody)
}

// gunzip inflates a `content-encoding: gzip` body.  Exporters configured with
// compression (the microtel-gzip SUT) send one; without this the sink hands
// deflate output to proto.Unmarshal and reports every span as undelivered.
func gunzip(body []byte) ([]byte, error) {
	zr, err := gzip.NewReader(bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	return io.ReadAll(zr)
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

func countLogRecords(req *logpb.ExportLogsServiceRequest) uint64 {
	var n uint64
	for _, rl := range req.ResourceLogs {
		for _, sl := range rl.ScopeLogs {
			n += uint64(len(sl.LogRecords))
		}
	}
	return n
}
