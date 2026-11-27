// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_producer.hpp"

#include "microtel/provider.hpp"

#include "common/internal_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

namespace
{

/// @brief Decides, stream by stream, which streams one collection collects
///        under its callback deadline (ICP 0046).
class CallbackDeadline
{
public:
    explicit CallbackDeadline(std::chrono::steady_clock::time_point deadline) noexcept
        : m_deadline(deadline)
    {
    }

    /// @brief Whether to collect @p stream: a synchronous stream always, the
    ///        first async stream always, a later one only while the deadline
    ///        has not passed. The clock is monotonic, so once one async
    ///        stream is skipped, every later one is too.
    [[nodiscard]] bool Admits(const IMetricStream& stream) noexcept
    {
        if (!stream.RunsCallback())
        {
            return true;
        }
        if (m_async_ran && std::chrono::steady_clock::now() >= m_deadline)
        {
            ++m_skipped;
            return false;
        }
        m_async_ran = true;
        return true;
    }

    /// @brief The async streams skipped so far.
    [[nodiscard]] std::size_t Skipped() const noexcept
    {
        return m_skipped;
    }

private:
    std::chrono::steady_clock::time_point m_deadline;
    bool m_async_ran = false;
    std::size_t m_skipped = 0;
};

/// @brief One record per stream of @p streams that @p deadline admits.
///
/// A skipped stream is not called at all, so its state is exactly as the last
/// collection that ran its callback left it (ICP 0046).
[[nodiscard]] std::vector<internal::MetricRecord> CollectAdmitted(
    const std::vector<IMetricStream*>& streams,
    const internal::TemporalitySelector& selector,
    std::chrono::system_clock::time_point collection_time,
    CallbackDeadline& deadline)
{
    std::vector<internal::MetricRecord> records;
    records.reserve(streams.size());
    for (IMetricStream* const stream : streams)
    {
        if (deadline.Admits(*stream))
        {
            // The stream resolves its temporality from its own kind.
            records.push_back(stream->Collect(selector, collection_time));
        }
    }
    return records;
}

}  // namespace

void MetricProducer::AddStream(internal::InstrumentationScope scope,
                               std::unique_ptr<IMetricStream> stream)
{
    const std::scoped_lock lk{m_mu};
    for (ScopeEntry& entry : m_scopes)
    {
        // A scope's identity includes its schema URL (ICP 0048).
        if (entry.scope.name == scope.name && entry.scope.version == scope.version &&
            entry.scope.schema_url == scope.schema_url)
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
    const internal::CollectRequest& request)
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
    CallbackDeadline deadline{request.callback_deadline};

    std::vector<internal::MetricBatchHandle> handles;
    handles.reserve(snapshot.size());
    for (const ScopeSnapshot& entry : snapshot)
    {
        std::vector<internal::MetricRecord> records =
            CollectAdmitted(entry.streams, request.selector, collection_time, deadline);
        if (!records.empty())
        {
            handles.emplace_back(std::move(records), m_resource, entry.scope);
        }
    }
    for (std::size_t i = 0; i < deadline.Skipped(); ++i)
    {
        RecordCallbackSkipped();
    }
    return handles;
}

void MetricProducer::RecordCallbackSkipped() noexcept
{
    if (m_diagnostics != nullptr)
    {
        m_diagnostics->RecordDrop(DropReason::MetricCallbackTimeout);
    }
    const std::uint64_t skips = m_callback_skips.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((skips & (skips - 1)) != 0)  // log the 1st, 2nd, 4th, 8th ... only
    {
        return;
    }
    try
    {
        internal::LogImpl(LogLevel::Warn,
                          "metrics: skipped an async instrument callback because the "
                          "collection's deadline had passed; an earlier callback ran past the "
                          "export interval or the flush timeout (" +
                              std::to_string(skips) + " skipped so far, metric_callback_timeout)");
    }
    catch (const std::exception&)
    {
        // Composing the line could only fail to allocate; the skip is already
        // counted and the Warn is advisory.
        return;
    }
}

}  // namespace microtel::sdk
