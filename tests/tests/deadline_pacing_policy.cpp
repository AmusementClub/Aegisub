#include <main.h>

#include "../../src/deadline_pacing_policy.h"

TEST(deadline_pacing_policy, request_at_deadline_emits_without_waiting_for_timer) {
	using Policy = DeadlinePacingPolicy;
	auto const t0 = Policy::TimePoint{};
	Policy policy(std::chrono::milliseconds(16));

	policy.Begin(t0);
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(1)));
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(15)));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + std::chrono::milliseconds(16), *policy.NextDeadline());

	EXPECT_TRUE(policy.Request(t0 + std::chrono::milliseconds(16)));
	EXPECT_FALSE(policy.HasPending());
}

TEST(deadline_pacing_policy, timer_is_trailing_and_early_or_stale_events_do_not_emit) {
	using Policy = DeadlinePacingPolicy;
	auto const t0 = Policy::TimePoint{};
	Policy policy(std::chrono::milliseconds(16));

	policy.Begin(t0);
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(1)));
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(15)));
	EXPECT_TRUE(policy.OnTimer(t0 + std::chrono::milliseconds(16)));

	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(17)));
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(18)));
	ASSERT_TRUE(policy.NextDeadline());
	EXPECT_EQ(t0 + std::chrono::milliseconds(32), *policy.NextDeadline());
	EXPECT_TRUE(policy.OnTimer(t0 + std::chrono::milliseconds(32)));
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(33)));
}

TEST(deadline_pacing_policy, force_clears_pending_and_end_disarms_policy) {
	using Policy = DeadlinePacingPolicy;
	auto const t0 = Policy::TimePoint{};
	Policy policy(std::chrono::milliseconds(33));

	policy.Begin(t0);
	EXPECT_FALSE(policy.Request(t0 + std::chrono::milliseconds(2)));
	EXPECT_TRUE(policy.Force(t0 + std::chrono::milliseconds(3)));
	EXPECT_FALSE(policy.HasPending());

	policy.End();
	EXPECT_FALSE(policy.IsActive());
	EXPECT_FALSE(policy.NextDeadline());
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::seconds(1)));
}
