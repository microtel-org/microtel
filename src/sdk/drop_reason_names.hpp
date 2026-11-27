// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/provider.hpp"

#include <string_view>

namespace microtel::sdk
{

/// @brief Map a `DropReason` to its stable snake_case counter name.
///
/// The names mirror the `DropReason` table in `docs/error-model.md` §3 and are
/// used by internal diagnostics/logging (ICP 0008). Every enumerator has a
/// distinct, non-empty name; an out-of-range value maps to `"unknown"`.
[[nodiscard]] constexpr std::string_view DropReasonName(DropReason reason) noexcept
{
    switch (reason)
    {
        case DropReason::QueueFull:
            return "queue_full";
        case DropReason::RecordTooLarge:
            return "record_too_large";
        case DropReason::SpanAttributeLimit:
            return "span_attribute_limit";
        case DropReason::SpanEventLimit:
            return "span_event_limit";
        case DropReason::SpanLinkLimit:
            return "span_link_limit";
        case DropReason::EventAttributeLimit:
            return "event_attribute_limit";
        case DropReason::LinkAttributeLimit:
            return "link_attribute_limit";
        case DropReason::AttributeValueTruncated:
            return "attribute_value_truncated";
        case DropReason::PostShutdown:
            return "post_shutdown";
        case DropReason::ResponseTooLarge:
            return "response_too_large";
        case DropReason::DecompressionTooLarge:
            return "decompression_too_large";
        case DropReason::MalformedResponse:
            return "malformed_response";
        case DropReason::PartialSuccessRejection:
            return "partial_success_rejection";
        case DropReason::NonRetryableFailure:
            return "non_retryable_failure";
        case DropReason::RetryableFailureRecovered:
            return "retryable_failure_recovered";
        case DropReason::RetryBudgetExhausted:
            return "retry_budget_exhausted";
        case DropReason::TransportBusy:
            return "transport_busy";
        case DropReason::ConnectFailure:
            return "connect_failure";
        case DropReason::ForceFlushTimeout:
            return "force_flush_timeout";
        case DropReason::ShutdownTimeout:
            return "shutdown_timeout";
        case DropReason::CardinalityOverflow:
            return "cardinality_overflow";
        case DropReason::MetricCallbackTimeout:
            return "metric_callback_timeout";
        case DropReason::NonFiniteValue:
            return "non_finite_value";
        case DropReason::LogAttributeLimit:
            return "log_attribute_limit";
        case DropReason::LeafPayloadMalformed:
            return "leaf_payload_malformed";
        case DropReason::LeafPayloadTooLarge:
            return "leaf_payload_too_large";
        case DropReason::LeafUnknown:
            return "leaf_unknown";
        case DropReason::NegativeHistogramValue:
            return "negative_histogram_value";
    }
    return "unknown";
}

}  // namespace microtel::sdk
