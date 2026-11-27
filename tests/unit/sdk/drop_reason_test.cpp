// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the DropReason enum and its diagnostics name mapping
// (M12, ICP 0008 — metric drop reasons in DropReason / HealthSnapshot).

#include "microtel/provider.hpp"

#include "sdk/drop_reason_names.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace mt = microtel;

namespace
{

TEST(DropReason, MetricEnumeratorsHaveIcp0008Values)
{
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::CardinalityOverflow), 20U);
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::MetricCallbackTimeout), 21U);
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::NonFiniteValue), 22U);
    // 24 until ICP 0034 appended the three leaf reasons; 27 until ICP 0047.
    EXPECT_EQ(mt::kDropReasonCount, std::size_t{28});
}

TEST(DropReason, NegativeHistogramValueHasIcp0047Value)
{
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::NegativeHistogramValue), 27U);
    EXPECT_EQ(mt::sdk::DropReasonName(mt::DropReason::NegativeHistogramValue),
              "negative_histogram_value");
}

TEST(DropReason, LogAttributeLimitHasIcp0011Value)
{
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::LogAttributeLimit), 23U);
    EXPECT_EQ(mt::sdk::DropReasonName(mt::DropReason::LogAttributeLimit), "log_attribute_limit");
}

TEST(DropReason, LeafEnumeratorsHaveIcp0034Values)
{
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::LeafPayloadMalformed), 24U);
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::LeafPayloadTooLarge), 25U);
    EXPECT_EQ(static_cast<std::uint8_t>(mt::DropReason::LeafUnknown), 26U);
    EXPECT_EQ(mt::sdk::DropReasonName(mt::DropReason::LeafPayloadMalformed),
              "leaf_payload_malformed");
    EXPECT_EQ(mt::sdk::DropReasonName(mt::DropReason::LeafPayloadTooLarge),
              "leaf_payload_too_large");
    EXPECT_EQ(mt::sdk::DropReasonName(mt::DropReason::LeafUnknown), "leaf_unknown");
}

TEST(DropReason, EveryReasonHasAName)
{
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < mt::kDropReasonCount; ++i)
    {
        const auto name = mt::sdk::DropReasonName(static_cast<mt::DropReason>(i));
        EXPECT_FALSE(name.empty()) << "DropReason " << i << " has no name";
        const bool inserted = seen.insert(name).second;
        EXPECT_TRUE(inserted) << "DropReason " << i << " name '" << name << "' is a duplicate";
    }
    EXPECT_EQ(seen.size(), mt::kDropReasonCount);
}

TEST(DropReason, HealthSnapshotCounterArrayMatchesCount)
{
    const mt::HealthSnapshot snap{};
    EXPECT_EQ(snap.drop_counters.size(), mt::kDropReasonCount);
}

}  // namespace
