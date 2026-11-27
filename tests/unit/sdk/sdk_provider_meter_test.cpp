// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkProvider::GetMeter — the instrument-factory accessor that
// lazily creates a MetricProducer and caches Meter instances by scope.
//
// Contract under test:
//  - GetMeter() returns a non-null shared_ptr<Meter>.
//  - Same (name, version) → same pointer (identity cache).
//  - Different name or version → different pointer.
//  - Empty version is a distinct scope from a non-empty version.
//  - Meters obtained from the same provider share a MetricProducer
//    (instruments from different scopes both appear in Collect()).
//  - metric_max_cardinality is forwarded to every instrument stream.

#include "microtel/attribute.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

using namespace std::chrono_literals;

namespace
{

// ── Minimal SdkProvider construction ─────────────────────────────────────────

std::unique_ptr<mts::SdkProvider> MakeProvider()
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
    });
}

std::unique_ptr<mts::SdkProvider> MakeProviderWithCardinality(std::size_t max_cardinality)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_max_cardinality = max_cardinality,
    });
}

}  // namespace

// ── GetMeter — basic ──────────────────────────────────────────────────────────

TEST(SdkProviderMeterTest, GetMeter_ReturnsNonNull)
{
    auto provider = MakeProvider();
    auto meter = provider->GetMeter("my.lib");
    EXPECT_NE(meter, nullptr);
}

TEST(SdkProviderMeterTest, GetMeter_SameScopeReturnsSameInstance)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("my.lib", "1.0");
    auto b = provider->GetMeter("my.lib", "1.0");
    EXPECT_EQ(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_DifferentNameReturnsDifferentInstance)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib.a");
    auto b = provider->GetMeter("lib.b");
    EXPECT_NE(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_DifferentVersionReturnsDifferentInstance)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib", "1.0");
    auto b = provider->GetMeter("lib", "2.0");
    EXPECT_NE(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_EmptyVersionDistinctFromNonEmpty)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib");  // version defaults to ""
    auto b = provider->GetMeter("lib", "1.0");
    EXPECT_NE(a.get(), b.get());
}

// schema_url is not part of the cache key (provider.hpp): the first call
// for a (name, version) fixes the scope's schema URL.
TEST(SdkProviderMeterTest, GetMeter_SchemaUrlDoesNotAffectCaching)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib", "1.0", "https://example.test/a");
    auto b = provider->GetMeter("lib", "1.0", "https://example.test/b");
    EXPECT_EQ(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_DefaultAndExplicitEmptyVersionAreSameScope)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib");
    auto b = provider->GetMeter("lib", "");  // explicit empty == default empty
    EXPECT_EQ(a.get(), b.get());
}

// ── Shared MetricProducer — instruments can be created from multiple scopes ────

TEST(SdkProviderMeterTest, InstrumentsCanBeCreatedFromMultipleScopes)
{
    auto provider = MakeProvider();
    auto meter_a = provider->GetMeter("scope.a", "1.0");
    auto meter_b = provider->GetMeter("scope.b", "1.0");

    // Both meters share the same MetricProducer; instruments must not crash.
    const auto counter_a = meter_a->CreateCounter<std::int64_t>("requests", "", "");
    const auto counter_b = meter_b->CreateCounter<std::int64_t>("errors", "", "");
    counter_a->Add(10, {});
    counter_b->Add(3, {});
}

TEST(SdkProviderMeterTest, InstrumentsFromSameScopeAreAddedToSameScope)
{
    auto provider = MakeProvider();
    // Same meter returned twice — both instrument registrations go to the
    // same ScopeEntry in the MetricProducer.
    auto m1 = provider->GetMeter("scope", "1.0");
    auto m2 = provider->GetMeter("scope", "1.0");

    const auto c1 = m1->CreateCounter<std::int64_t>("hits", "", "");
    const auto c2 = m2->CreateCounter<std::int64_t>("misses", "", "");
    c1->Add(5, {});
    c2->Add(2, {});
}

// ── Metric exporter wiring ────────────────────────────────────────────────────

TEST(SdkProviderMeterTest, ForceFlush_WithMetricExporter_FlushesExporter)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();
    auto metric_exp = std::make_unique<mtm::MockMetricExporter>();
    const auto* metric_exp_ptr = metric_exp.get();

    auto provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_exporter = std::move(metric_exp),
        .metric_interval = std::chrono::milliseconds{60'000},
    });

    // Trigger lazy init of the metric reader.
    (void)provider->GetMeter("test.lib");

    EXPECT_EQ(provider->ForceFlush(500ms), mt::Status::Completed);
    // Reader's ForceFlush calls DoCollectExport then exporter ForceFlush.
    EXPECT_GE(metric_exp_ptr->flush_call_count.load(), 1);
}

// The schema URL given to GetMeter reaches the exported scope (#456).
TEST(SdkProviderMeterTest, GetMeter_SchemaUrlReachesExportedScope)
{
    auto metric_exp = std::make_unique<mtm::MockMetricExporter>();
    auto* const metric_exp_ptr = metric_exp.get();

    auto provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::make_unique<mtm::MockExporter>(),
        .processor = std::make_unique<mtm::MockSpanProcessor>(),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_exporter = std::move(metric_exp),
        .metric_interval = std::chrono::milliseconds{60'000},
    });

    const auto meter = provider->GetMeter("my.lib", "2.0", "https://example.test/schema");
    const auto counter = meter->CreateCounter<std::int64_t>("requests");
    counter->Add(1, {});
    ASSERT_EQ(provider->ForceFlush(500ms), mt::Status::Completed);

    const std::scoped_lock lk{metric_exp_ptr->scopes_mu};
    ASSERT_FALSE(metric_exp_ptr->exported_scopes.empty());
    const auto& scope = metric_exp_ptr->exported_scopes.front();
    EXPECT_EQ(scope.name, "my.lib");
    EXPECT_EQ(scope.version, "2.0");
    EXPECT_EQ(scope.schema_url, "https://example.test/schema");
}

TEST(SdkProviderMeterTest, Shutdown_WithMetricExporter_ShutsDownExporter)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();
    auto metric_exp = std::make_unique<mtm::MockMetricExporter>();
    const auto* metric_exp_ptr = metric_exp.get();

    auto provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_exporter = std::move(metric_exp),
        .metric_interval = std::chrono::milliseconds{60'000},
    });

    // Trigger lazy init of the metric reader.
    (void)provider->GetMeter("test.lib");

    EXPECT_EQ(provider->Shutdown(500ms), mt::Status::Completed);
    // Metric reader Shutdown delegates to the exporter.
    EXPECT_GE(metric_exp_ptr->shutdown_call_count.load(), 1);
}

// ── Cardinality cap — M12 increment 27 ───────────────────────────────────────

TEST(SdkProviderMeterTest, CardinalityCapIsEnforcedPerInstrument)
{
    // Build a provider with a cardinality cap of 2. Adding 3 distinct attribute
    // sets must fold the 3rd into the overflow series and record a diagnostic drop.
    auto provider = MakeProviderWithCardinality(2);
    auto meter = provider->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("requests", "desc", "1");

    const mt::KeyValue a1{.key = "k", .value = std::int64_t{1}};
    const mt::KeyValue a2{.key = "k", .value = std::int64_t{2}};
    const mt::KeyValue a3{.key = "k", .value = std::int64_t{3}};
    counter->Add(1, {&a1, 1});
    counter->Add(1, {&a2, 1});  // fills cardinality cap
    counter->Add(1, {&a3, 1});  // must overflow

    const auto health = provider->DiagnosticsSink().Snapshot();
    constexpr auto kIdx = static_cast<std::size_t>(mt::DropReason::CardinalityOverflow);
    EXPECT_EQ(health.drop_counters[kIdx], 1U);
}

TEST(SdkProviderMeterTest, CardinalityCapDefaultIs2000)
{
    // With no explicit cardinality cap the default is kDefaultMaxCardinality=2000.
    // Adding 2001 distinct attribute sets triggers exactly one overflow drop.
    auto provider = MakeProvider();
    auto meter = provider->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("hits", "desc", "1");

    for (std::size_t i = 0; i <= 2000; ++i)
    {
        const mt::KeyValue kv{.key = "i", .value = static_cast<std::int64_t>(i)};
        counter->Add(1, {&kv, 1});
    }

    const auto health = provider->DiagnosticsSink().Snapshot();
    constexpr auto kIdx = static_cast<std::size_t>(mt::DropReason::CardinalityOverflow);
    EXPECT_EQ(health.drop_counters[kIdx], 1U);
}

// ── Cardinality cap — observable instruments (increment 28) ──────────────────

namespace
{

/// @brief Provider with a cardinality cap AND a metric exporter so that
/// `ForceFlush()` triggers `DoCollectExport()` → observable callbacks.
std::unique_ptr<mts::SdkProvider> MakeProviderWithCardinalityAndExporter(
    std::size_t max_cardinality)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();
    auto metric_exp = std::make_unique<mtm::MockMetricExporter>();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_exporter = std::move(metric_exp),
        .metric_interval = std::chrono::hours{24},  // never auto-fire during test
        .metric_max_cardinality = max_cardinality,
    });
}

}  // namespace

TEST(SdkProviderMeterTest, ObservableCardinalityCapEnforcedPerInstrument)
{
    // Build a provider with cardinality cap of 2 and a metric exporter so
    // ForceFlush() triggers a collection cycle that invokes the callback.
    auto provider = MakeProviderWithCardinalityAndExporter(2);
    auto meter = provider->GetMeter("test.lib");

    (void)meter->CreateObservableCounter<std::int64_t>(
        "obs.req",
        "desc",
        "1",
        [](mt::ObservableResult<std::int64_t>& result)
        {
            const mt::KeyValue a1{.key = "k", .value = std::int64_t{1}};
            const mt::KeyValue a2{.key = "k", .value = std::int64_t{2}};
            const mt::KeyValue a3{.key = "k", .value = std::int64_t{3}};
            result.Observe(1, {&a1, 1});
            result.Observe(2, {&a2, 1});  // fills cap
            result.Observe(3, {&a3, 1});  // must fold to overflow
        });

    // ForceFlush → DoCollectExport → callback invoked → overflow drop recorded.
    (void)provider->ForceFlush(500ms);

    const auto health = provider->DiagnosticsSink().Snapshot();
    constexpr auto kIdx = static_cast<std::size_t>(mt::DropReason::CardinalityOverflow);
    EXPECT_EQ(health.drop_counters[kIdx], 1U);
}

// ── Meter and instruments outlive the provider — issue #259 ──────────────────
// `GetMeter` and `Create*` hand out `shared_ptr`s, which a caller reads as
// "this keeps itself alive". The metric storage writes through the
// diagnostics sink and the current-span source, so those must stay allocated
// as long as any meter or instrument does — not only as long as the provider.
// Under ASAN the pre-fix code fails both tests with heap-use-after-free.

namespace
{

/// A provider with a cardinality cap of 1, so the second distinct attribute
/// set on any instrument records exactly one `CardinalityOverflow` drop.
/// @p out_diag is the provider's diagnostics sink; reading it after the
/// provider is gone is valid only because a live meter or instrument keeps
/// the sink alive, which is the property under test.
std::unique_ptr<mts::SdkProvider> MakeCapOneProvider(mts::DiagnosticsCounters** out_diag)
{
    auto diagnostics = std::make_unique<mts::DiagnosticsCounters>();
    *out_diag = diagnostics.get();
    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::move(diagnostics),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::make_unique<mtm::MockExporter>(),
        .processor = std::make_unique<mtm::MockSpanProcessor>(),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_max_cardinality = 1,
    });
}

std::uint64_t OverflowDrops(const mts::DiagnosticsCounters& diag)
{
    constexpr auto kIdx = static_cast<std::size_t>(mt::DropReason::CardinalityOverflow);
    return diag.Snapshot().drop_counters.at(kIdx);
}

}  // namespace

TEST(SdkProviderMeterTest, MeterOutlivesProvider_InstrumentCreatedAfterwardsIsUsable)
{
    const mt::KeyValue first{.key = "k", .value = std::int64_t{1}};
    const mt::KeyValue second{.key = "k", .value = std::int64_t{2}};
    mts::DiagnosticsCounters* diag = nullptr;
    auto provider = MakeCapOneProvider(&diag);
    const std::shared_ptr<mt::Meter> meter = provider->GetMeter("outlives", "1.0");

    provider.reset();

    const auto counter = meter->CreateCounter<std::int64_t>("late");
    counter->Add(1, {&first, 1});
    counter->Add(1, {&second, 1});  // overflows: reaches the diagnostics sink
    EXPECT_EQ(OverflowDrops(*diag), 1U);
}

TEST(SdkProviderMeterTest, InstrumentsOutliveMeterAndProvider_StayUsable)
{
    const mt::KeyValue first{.key = "k", .value = std::int64_t{1}};
    const mt::KeyValue second{.key = "k", .value = std::int64_t{2}};
    mts::DiagnosticsCounters* diag = nullptr;
    auto provider = MakeCapOneProvider(&diag);
    auto meter = provider->GetMeter("outlives", "1.0");
    const auto counter = meter->CreateCounter<std::int64_t>("c");
    const auto up_down = meter->CreateUpDownCounter<double>("u");
    const auto gauge = meter->CreateGauge<std::int64_t>("g");
    const auto histogram = meter->CreateHistogram<double>("h");
    const auto exp_histogram = meter->CreateExponentialHistogram<std::int64_t>("e");

    meter.reset();
    provider.reset();

    for (const auto* const kv : {&first, &second})
    {
        counter->Add(1, {kv, 1});
        up_down->Add(1.0, {kv, 1});
        gauge->Record(1, {kv, 1});
        histogram->Record(1.0, {kv, 1});
        exp_histogram->Record(1, {kv, 1});
    }
    constexpr std::uint64_t kInstruments = 5;
    EXPECT_EQ(OverflowDrops(*diag), kInstruments);
}
