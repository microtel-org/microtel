// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_producer.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace microtel::sdk
{

void MetricProducer::AddStream(internal::InstrumentationScope scope,
                               std::unique_ptr<IMetricStream> stream)
{
    const std::scoped_lock lk{m_mu};
    for (ScopeEntry& entry : m_scopes)
    {
        if (entry.scope.name == scope.name && entry.scope.version == scope.version)
        {
            entry.streams.push_back(std::move(stream));
            return;
        }
    }
    m_scopes.push_back({.scope = std::move(scope), .streams = {}});
    m_scopes.back().streams.push_back(std::move(stream));
}

std::vector<MetricProducer::ScopeSnapshot> MetricProducer::SnapshotScopes() const
{
    const std::scoped_lock lk{m_mu};
    std::vector<ScopeSnapshot> snapshot;
    snapshot.reserve(m_scopes.size());
    for (const ScopeEntry& entry : m_scopes)
    {
        std::vector<IMetricStream*> borrowed;
        borrowed.reserve(entry.streams.size());
        for (const auto& stream : entry.streams)
        {
            borrowed.push_back(stream.get());
        }
        snapshot.push_back({.scope = entry.scope, .streams = std::move(borrowed)});
    }
    return snapshot;
}

std::vector<internal::MetricBatchHandle> MetricProducer::Collect(
    internal::AggregationTemporality temporality)
{
    // Snapshot the structure, then release m_mu before calling into any
    // stream: IMetricStream::Collect takes its own per-instrument mutex, and
    // holding both at once would nest two non-leaf locks
    // (docs/threading-model.md §4). Borrowed pointers stay valid because there
    // is no stream-removal API — a stream outlives the producer's own lifetime
    // only, and Collect cannot outlive the producer.
    //
    // A stream registered during this call may land in either this cycle or
    // the next. That is inherent to concurrent registration and harmless: it
    // is collected on the following cycle.
    const std::vector<ScopeSnapshot> snapshot = SnapshotScopes();
    // One collection, one time: every point of this cycle carries it.
    const auto collection_time = std::chrono::system_clock::now();

    std::vector<internal::MetricBatchHandle> handles;
    handles.reserve(snapshot.size());
    for (const ScopeSnapshot& entry : snapshot)
    {
        std::vector<internal::MetricRecord> records;
        records.reserve(entry.streams.size());
        for (IMetricStream* const stream : entry.streams)
        {
            records.push_back(stream->Collect(temporality, collection_time));
        }
        handles.emplace_back(std::move(records), m_resource, entry.scope);
    }
    return handles;
}

}  // namespace microtel::sdk
