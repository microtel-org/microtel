// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

package control_test

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/control"
	"github.com/microtel-org/microtel/bench/sink/blackhole/internal/counters"
)

func newHandler() (http.Handler, *counters.Counters) {
	c := counters.New()
	return control.NewHandler(c), c
}

// ---------------------------------------------------------------------------
// /health
// ---------------------------------------------------------------------------

func TestHealth_OK(t *testing.T) {
	h, _ := newHandler()
	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	if strings.TrimSpace(rec.Body.String()) != "ok" {
		t.Errorf("body: want %q, got %q", "ok", rec.Body.String())
	}
}

func TestHealth_WrongMethod(t *testing.T) {
	h, _ := newHandler()
	req := httptest.NewRequest(http.MethodPost, "/health", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("status: want 405, got %d", rec.Code)
	}
}

// ---------------------------------------------------------------------------
// /stats
// ---------------------------------------------------------------------------

func TestStats_ReturnsValidJSON(t *testing.T) {
	h, c := newHandler()
	c.RecordHTTPExport(5, 200, 0)

	req := httptest.NewRequest(http.MethodGet, "/stats", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("status: want 200, got %d", rec.Code)
	}
	ct := rec.Header().Get("Content-Type")
	if ct != "application/json" {
		t.Errorf("content-type: want application/json, got %q", ct)
	}

	var snap counters.Snapshot
	if err := json.Unmarshal(rec.Body.Bytes(), &snap); err != nil {
		t.Fatalf("unmarshal: %v\nbody: %s", err, rec.Body.String())
	}
	if snap.SpansReceived != 5 {
		t.Errorf("spans_received: want 5, got %d", snap.SpansReceived)
	}
	if snap.RequestsReceived != 1 {
		t.Errorf("requests_received: want 1, got %d", snap.RequestsReceived)
	}
}

func TestStats_IncludesLogRecordsReceived(t *testing.T) {
	h, c := newHandler()
	c.RecordHTTPLogExport(9, 100, 0)

	req := httptest.NewRequest(http.MethodGet, "/stats", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	var raw map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &raw); err != nil {
		t.Fatalf("unmarshal: %v\nbody: %s", err, rec.Body.String())
	}
	if got, ok := raw["log_records_received"].(float64); !ok || got != 9 {
		t.Errorf("log_records_received: want 9, got %v", raw["log_records_received"])
	}
}

func TestStats_WrongMethod(t *testing.T) {
	h, _ := newHandler()
	req := httptest.NewRequest(http.MethodPost, "/stats", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("status: want 405, got %d", rec.Code)
	}
}

func TestStats_LastErrorIncluded(t *testing.T) {
	h, c := newHandler()
	c.RecordError("something went wrong")

	req := httptest.NewRequest(http.MethodGet, "/stats", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	var snap counters.Snapshot
	if err := json.Unmarshal(rec.Body.Bytes(), &snap); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if snap.LastError != "something went wrong" {
		t.Errorf("last_error: want %q, got %q", "something went wrong", snap.LastError)
	}
	if snap.Errors != 1 {
		t.Errorf("errors: want 1, got %d", snap.Errors)
	}
}

// ---------------------------------------------------------------------------
// /reset
// ---------------------------------------------------------------------------

func TestReset_ZeroesCounters(t *testing.T) {
	h, c := newHandler()
	c.RecordHTTPExport(5, 200, 0)
	c.RecordError("oops")

	req := httptest.NewRequest(http.MethodPost, "/reset", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("reset status: want 200, got %d", rec.Code)
	}

	snap := c.Snapshot()
	if snap.SpansReceived != 0 {
		t.Errorf("spans_received after reset: want 0, got %d", snap.SpansReceived)
	}
	if snap.Errors != 0 {
		t.Errorf("errors after reset: want 0, got %d", snap.Errors)
	}
	if snap.LastError != "" {
		t.Errorf("last_error after reset: want empty, got %q", snap.LastError)
	}
}

func TestReset_ReturnsEmptyJSON(t *testing.T) {
	h, _ := newHandler()
	req := httptest.NewRequest(http.MethodPost, "/reset", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if strings.TrimSpace(rec.Body.String()) != "{}" {
		t.Errorf("body: want {}, got %q", rec.Body.String())
	}
}

func TestReset_WrongMethod(t *testing.T) {
	h, _ := newHandler()
	req := httptest.NewRequest(http.MethodGet, "/reset", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("status: want 405, got %d", rec.Code)
	}
}
