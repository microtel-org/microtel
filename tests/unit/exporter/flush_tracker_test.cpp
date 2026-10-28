// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for FlushTracker: the ranges a flush covers and which failures
// it reports (ICP 0041 and its consecutive-ranges amendment). Single-threaded:
// the tracker has no locking of its own, so the owning exporter's mutex is
// not needed to drive it.
#include "exporter/flush_tracker.hpp"

#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace mt = microtel;
namespace mte = microtel::exporter;

namespace
{

std::vector<bool> OneFailed()
{
    return {false};
}

std::vector<bool> OneSent()
{
    return {true};
}

}  // namespace

TEST(FlushTrackerTest, NothingQueued_IsReachedAtOnce)
{
    mte::FlushTracker tracker;
    const mte::FlushTracker::Waiter waiter{tracker};
    EXPECT_TRUE(waiter.Reached());
    EXPECT_EQ(waiter.StatusOf(true), mt::Status::Completed);
}

TEST(FlushTrackerTest, WaitsForTheBatchesAcceptedBeforeIt_NotForLaterOnes)
{
    mte::FlushTracker tracker;
    tracker.Accept();
    const mte::FlushTracker::Waiter waiter{tracker};
    tracker.Accept();  // after the call: not in the range

    EXPECT_FALSE(waiter.Reached());
    tracker.Complete(1, OneSent());
    EXPECT_TRUE(waiter.Reached());
    EXPECT_EQ(waiter.StatusOf(true), mt::Status::Completed);
}

TEST(FlushTrackerTest, NotReachedInTime_IsTimedOut)
{
    mte::FlushTracker tracker;
    tracker.Accept();
    const mte::FlushTracker::Waiter waiter{tracker};
    EXPECT_EQ(waiter.StatusOf(false), mt::Status::TimedOut);
}

TEST(FlushTrackerTest, AMissingOutcome_CountsAsFailed)
{
    mte::FlushTracker tracker;
    tracker.Accept();
    const mte::FlushTracker::Waiter waiter{tracker};
    tracker.Complete(1, {});  // the round threw before its outcome was known
    EXPECT_EQ(waiter.StatusOf(true), mt::Status::Failed);
}

// The Provider case the amendment exists for: the batch fails before the
// exporter's ForceFlush is even called, and the flush still reports it.
TEST(FlushTrackerTest, AFailureBeforeTheCall_IsReportedWhenNoFlushHasCoveredIt)
{
    mte::FlushTracker tracker;
    tracker.Accept();
    tracker.Complete(1, OneFailed());

    const mte::FlushTracker::Waiter waiter{tracker};
    EXPECT_TRUE(waiter.Reached());
    EXPECT_EQ(waiter.StatusOf(true), mt::Status::Failed);
}

TEST(FlushTrackerTest, OverlappingFlushes_BothReportTheFailure_ThenTheNextIsClean)
{
    mte::FlushTracker tracker;
    tracker.Accept();
    tracker.Complete(1, OneFailed());
    tracker.Accept();  // in flight while both flushes wait
    {
        const mte::FlushTracker::Waiter first{tracker};
        const mte::FlushTracker::Waiter second{tracker};
        tracker.Complete(1, OneSent());
        EXPECT_EQ(first.StatusOf(first.Reached()), mt::Status::Failed);
        EXPECT_EQ(second.StatusOf(second.Reached()), mt::Status::Failed);
    }
    const mte::FlushTracker::Waiter third{tracker};
    EXPECT_EQ(third.StatusOf(third.Reached()), mt::Status::Completed);
}

// A flush that timed out has not covered its range, so the next one still
// reports the failure in it.
TEST(FlushTrackerTest, ATimedOutFlush_DoesNotMoveTheRangeOn)
{
    mte::FlushTracker tracker;
    tracker.Accept();
    tracker.Accept();
    tracker.Complete(1, OneFailed());
    {
        const mte::FlushTracker::Waiter timed_out{tracker};
        EXPECT_FALSE(timed_out.Reached());
    }
    tracker.Complete(1, OneSent());
    const mte::FlushTracker::Waiter next{tracker};
    EXPECT_EQ(next.StatusOf(next.Reached()), mt::Status::Failed);
}

TEST(FlushTrackerTest, AFailureAfterTheRange_IsNotReported)
{
    mte::FlushTracker tracker;
    tracker.Accept();
    const mte::FlushTracker::Waiter waiter{tracker};
    tracker.Accept();
    const std::vector<bool> sent_then_failed{true, false};
    tracker.Complete(2, sent_then_failed);
    EXPECT_EQ(waiter.StatusOf(true), mt::Status::Completed);
}
