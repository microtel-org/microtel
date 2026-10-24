// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package counters_test

import (
	"testing"

	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/counters"
)

func TestNew_ZeroValues(t *testing.T) {
	c := counters.New()
	snap := c.Snapshot()
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.RequestsReceived != 0 {
		t.Errorf("requests_received: want 0, got %d", snap.RequestsReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors: want 0, got %d", snap.Errors)
	}
	if snap.LastError != "" {
		t.Errorf("last_error: want empty, got %q", snap.LastError)
	}
	if snap.UptimeSeconds < 0 {
		t.Errorf("uptime_seconds: want >= 0, got %f", snap.UptimeSeconds)
	}
}

func TestRecordHTTPExport_Accumulates(t *testing.T) {
	c := counters.New()
	c.RecordHTTPExport(5, 200, 0)
	snap := c.Snapshot()
	if snap.SpansReceived != 5 {
		t.Errorf("spans_received: want 5, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != 200 {
		t.Errorf("bytes_received: want 200, got %d", snap.BytesReceived)
	}
	if snap.RequestsReceived != 1 {
		t.Errorf("requests_received: want 1, got %d", snap.RequestsReceived)
	}
	if snap.HTTPRequestsReceived != 1 {
		t.Errorf("http_requests_received: want 1, got %d", snap.HTTPRequestsReceived)
	}
	if snap.GRPCRequestsReceived != 0 {
		t.Errorf("grpc_requests_received: want 0, got %d", snap.GRPCRequestsReceived)
	}
}

func TestRecordGRPCExport_Accumulates(t *testing.T) {
	c := counters.New()
	c.RecordGRPCExport(3, 150, 5)
	snap := c.Snapshot()
	if snap.SpansReceived != 3 {
		t.Errorf("spans_received: want 3, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != 150 {
		t.Errorf("bytes_received: want 150, got %d", snap.BytesReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
	if snap.ResponseBytes != 5 {
		t.Errorf("response_bytes: want 5, got %d", snap.ResponseBytes)
	}
}

func TestMultipleExports_TotalsAccumulate(t *testing.T) {
	c := counters.New()
	c.RecordHTTPExport(5, 200, 0)
	c.RecordHTTPExport(3, 150, 0)
	snap := c.Snapshot()
	if snap.SpansReceived != 8 {
		t.Errorf("spans_received: want 8, got %d", snap.SpansReceived)
	}
	if snap.RequestsReceived != 2 {
		t.Errorf("requests_received: want 2, got %d", snap.RequestsReceived)
	}
	if snap.BytesReceived != 350 {
		t.Errorf("bytes_received: want 350, got %d", snap.BytesReceived)
	}
}

func TestMixedProtocols_RequestCountsSeparate(t *testing.T) {
	c := counters.New()
	c.RecordHTTPExport(2, 100, 0)
	c.RecordGRPCExport(4, 200, 0)
	snap := c.Snapshot()
	if snap.RequestsReceived != 2 {
		t.Errorf("requests_received: want 2, got %d", snap.RequestsReceived)
	}
	if snap.HTTPRequestsReceived != 1 {
		t.Errorf("http_requests_received: want 1, got %d", snap.HTTPRequestsReceived)
	}
	if snap.GRPCRequestsReceived != 1 {
		t.Errorf("grpc_requests_received: want 1, got %d", snap.GRPCRequestsReceived)
	}
	if snap.SpansReceived != 6 {
		t.Errorf("spans_received: want 6, got %d", snap.SpansReceived)
	}
}

func TestRecordError_IncrementsAndStoresLast(t *testing.T) {
	c := counters.New()
	c.RecordError("first error")
	c.RecordError("second error")
	snap := c.Snapshot()
	if snap.Errors != 2 {
		t.Errorf("errors: want 2, got %d", snap.Errors)
	}
	if snap.LastError != "second error" {
		t.Errorf("last_error: want %q, got %q", "second error", snap.LastError)
	}
}

func TestReset_ZeroesAllCounters(t *testing.T) {
	c := counters.New()
	c.RecordHTTPExport(10, 500, 20)
	c.RecordError("boom")
	c.Reset()
	snap := c.Snapshot()
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != 0 {
		t.Errorf("bytes_received: want 0, got %d", snap.BytesReceived)
	}
	if snap.RequestsReceived != 0 {
		t.Errorf("requests_received: want 0, got %d", snap.RequestsReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors: want 0, got %d", snap.Errors)
	}
	if snap.LastError != "" {
		t.Errorf("last_error: want empty, got %q", snap.LastError)
	}
	if snap.ResponseBytes != 0 {
		t.Errorf("response_bytes: want 0, got %d", snap.ResponseBytes)
	}
}

func TestReset_DoesNotResetUptime(t *testing.T) {
	c := counters.New()
	c.RecordHTTPExport(1, 1, 0)
	c.Reset()
	snap := c.Snapshot()
	if snap.UptimeSeconds < 0 {
		t.Errorf("uptime_seconds after reset: want >= 0, got %f", snap.UptimeSeconds)
	}
}

func TestRecordHTTPLogExport_CountsRecordsNotSpans(t *testing.T) {
	c := counters.New()
	c.RecordHTTPLogExport(7, 300, 0)
	snap := c.Snapshot()
	if snap.LogRecordsReceived != 7 {
		t.Errorf("log_records_received: want 7, got %d", snap.LogRecordsReceived)
	}
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != 300 {
		t.Errorf("bytes_received: want 300, got %d", snap.BytesReceived)
	}
	if snap.HTTPRequestsReceived != 1 || snap.RequestsReceived != 1 {
		t.Errorf("requests: want http=1 total=1, got http=%d total=%d",
			snap.HTTPRequestsReceived, snap.RequestsReceived)
	}
}

func TestRecordGRPCLogExport_CountsRecordsNotSpans(t *testing.T) {
	c := counters.New()
	c.RecordGRPCLogExport(4, 120, 2)
	c.RecordGRPCLogExport(6, 80, 2)
	snap := c.Snapshot()
	if snap.LogRecordsReceived != 10 {
		t.Errorf("log_records_received: want 10, got %d", snap.LogRecordsReceived)
	}
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received: want 0, got %d", snap.SpansReceived)
	}
	if snap.BytesReceived != 200 {
		t.Errorf("bytes_received: want 200, got %d", snap.BytesReceived)
	}
	if snap.GRPCRequestsReceived != 2 {
		t.Errorf("grpc_requests_received: want 2, got %d", snap.GRPCRequestsReceived)
	}
}

func TestReset_ZeroesLogRecords(t *testing.T) {
	c := counters.New()
	c.RecordHTTPLogExport(5, 100, 0)
	c.Reset()
	if got := c.Snapshot().LogRecordsReceived; got != 0 {
		t.Errorf("log_records_received: want 0, got %d", got)
	}
}
