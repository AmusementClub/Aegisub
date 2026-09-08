#include <main.h>

#include "../../src/navigation_preview_policy.h"

TEST(navigation_preview_policy, playing_click_commits_without_preview_or_timer) {
	using Policy = NavigationPreviewPolicy;
	auto const t0 = Policy::Clock::time_point{};
	Policy policy;
	policy.BeginGesture(true, 9);

	EXPECT_FALSE(policy.OnMotion(100, t0, true));
	EXPECT_FALSE(policy.OnMotion(109, t0 + std::chrono::milliseconds(5), false));
	EXPECT_FALSE(policy.OnMotion(91, t0 + std::chrono::milliseconds(10), false));
	EXPECT_FALSE(policy.OnMotion(100, t0 + std::chrono::milliseconds(15), true));
	EXPECT_FALSE(policy.NextPreviewTime());
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(100)));

	auto const commit = policy.OnRelease(105, t0 + std::chrono::milliseconds(110));
	EXPECT_EQ(Policy::OutputKind::Commit, commit.kind);
	EXPECT_EQ(105, commit.target);
	EXPECT_TRUE(commit.token.IsValid());
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(200)));
}

TEST(navigation_preview_policy, deferred_drag_starts_immediately_and_keeps_reverse_motion) {
	using Policy = NavigationPreviewPolicy;
	auto const t0 = Policy::Clock::time_point{};
	Policy policy(std::chrono::milliseconds(33));
	policy.BeginGesture(true, 9);
	EXPECT_FALSE(policy.OnMotion(100, t0, true));

	auto const first = policy.OnMotion(90, t0 + std::chrono::milliseconds(1), false);
	ASSERT_TRUE(first);
	EXPECT_EQ(Policy::OutputKind::Preview, first->kind);
	EXPECT_EQ(90, first->target);
	EXPECT_TRUE(first->token.IsValid());
	EXPECT_FALSE(policy.OnMotion(105, t0 + std::chrono::milliseconds(2), false));
	EXPECT_FALSE(first->token.IsValid());
	ASSERT_TRUE(policy.NextPreviewTime());
	EXPECT_EQ(t0 + std::chrono::milliseconds(34), *policy.NextPreviewTime());

	auto const reverse = policy.OnTimer(t0 + std::chrono::milliseconds(34));
	ASSERT_TRUE(reverse);
	EXPECT_EQ(Policy::OutputKind::Preview, reverse->kind);
	EXPECT_EQ(105, reverse->target);
	EXPECT_TRUE(reverse->token.IsValid());
	auto const commit = policy.OnRelease(107, t0 + std::chrono::milliseconds(35));
	EXPECT_EQ(Policy::OutputKind::Commit, commit.kind);
	EXPECT_EQ(107, commit.target);
	EXPECT_FALSE(reverse->token.IsValid());
	EXPECT_FALSE(policy.NextPreviewTime());
}

TEST(navigation_preview_policy, new_gesture_cancels_pending_drag_and_paused_press_previews) {
	using Policy = NavigationPreviewPolicy;
	auto const t0 = Policy::Clock::time_point{};
	Policy policy;
	auto const old_preview = policy.OnMotion(100, t0, true);
	ASSERT_TRUE(old_preview);
	EXPECT_FALSE(policy.OnMotion(200, t0 + std::chrono::milliseconds(1), false));
	ASSERT_TRUE(policy.NextPreviewTime());

	policy.BeginGesture(true, 9);
	EXPECT_FALSE(old_preview->token.IsValid());
	EXPECT_FALSE(policy.NextPreviewTime());
	EXPECT_FALSE(policy.OnMotion(200, t0 + std::chrono::milliseconds(2), true));
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(40)));
	policy.Cancel();

	policy.BeginGesture(false);
	auto const paused_preview = policy.OnMotion(200, t0 + std::chrono::milliseconds(41), true);
	ASSERT_TRUE(paused_preview);
	EXPECT_EQ(Policy::OutputKind::Preview, paused_preview->kind);
	EXPECT_EQ(200, paused_preview->target);
	EXPECT_TRUE(paused_preview->token.IsValid());
}

TEST(navigation_preview_policy, consecutive_clicks_at_same_target_each_commit_once) {
	using Policy = NavigationPreviewPolicy;
	auto const t0 = Policy::Clock::time_point{};
	Policy policy;
	policy.BeginGesture(true, 9);
	EXPECT_FALSE(policy.OnMotion(100, t0, true));
	auto const first = policy.OnRelease(100, t0 + std::chrono::milliseconds(5));
	EXPECT_TRUE(first.token.IsValid());

	policy.BeginGesture(true, 9);
	EXPECT_FALSE(first.token.IsValid());
	EXPECT_FALSE(policy.OnMotion(100, t0 + std::chrono::milliseconds(6), true));
	auto const second = policy.OnRelease(100, t0 + std::chrono::milliseconds(10));
	EXPECT_EQ(Policy::OutputKind::Commit, second.kind);
	EXPECT_EQ(100, second.target);
	EXPECT_TRUE(second.token.IsValid());
	EXPECT_FALSE(policy.NextPreviewTime());
	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(50)));
}

TEST(navigation_preview_policy, motion_coalesces_to_latest_and_release_flushes_commit) {
	using Policy = NavigationPreviewPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto first = policy.OnMotion(100, t0, true);
	ASSERT_TRUE(first);
	EXPECT_EQ(Policy::OutputKind::Preview, first->kind);
	EXPECT_EQ(100, first->target);
	ASSERT_TRUE(first->token.IsValid());

	auto second = policy.OnMotion(200, t0 + std::chrono::milliseconds(5), false);
	EXPECT_FALSE(second);
	EXPECT_FALSE(first->token.IsValid());
	ASSERT_TRUE(policy.NextPreviewTime());
	EXPECT_EQ(t0 + std::chrono::milliseconds(33), *policy.NextPreviewTime());

	auto third = policy.OnMotion(300, t0 + std::chrono::milliseconds(10), false);
	EXPECT_FALSE(third);
	ASSERT_TRUE(policy.NextPreviewTime());
	EXPECT_EQ(t0 + std::chrono::milliseconds(33), *policy.NextPreviewTime());

	auto timer = policy.OnTimer(t0 + std::chrono::milliseconds(33));
	ASSERT_TRUE(timer);
	EXPECT_EQ(Policy::OutputKind::Preview, timer->kind);
	EXPECT_EQ(300, timer->target);
	EXPECT_TRUE(timer->token.IsValid());
	auto const preview_token = timer->token;

	auto commit = policy.OnRelease(300, t0 + std::chrono::milliseconds(40));
	EXPECT_EQ(Policy::OutputKind::Commit, commit.kind);
	EXPECT_EQ(300, commit.target);
	EXPECT_TRUE(commit.token.IsValid());
	EXPECT_FALSE(preview_token.IsValid());
	EXPECT_FALSE(policy.NextPreviewTime());

	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(100)));
}

TEST(navigation_preview_policy, rapid_reverse_drag_keeps_latest_target) {
	using Policy = NavigationPreviewPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto first = policy.OnMotion(100, t0, true);
	ASSERT_TRUE(first);

	policy.OnMotion(200, t0 + std::chrono::milliseconds(5), false);
	policy.OnMotion(150, t0 + std::chrono::milliseconds(10), false);

	auto timer = policy.OnTimer(t0 + std::chrono::milliseconds(33));
	ASSERT_TRUE(timer);
	EXPECT_EQ(Policy::OutputKind::Preview, timer->kind);
	EXPECT_EQ(150, timer->target);

	auto commit = policy.OnRelease(150, t0 + std::chrono::milliseconds(50));
	EXPECT_EQ(Policy::OutputKind::Commit, commit.kind);
	EXPECT_EQ(150, commit.target);
	EXPECT_TRUE(commit.token.IsValid());
}

TEST(navigation_preview_policy, coalesces_input_over_200hz) {
	using Policy = NavigationPreviewPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	size_t preview_emits = 0;
	int last_preview_target = -1;

	// 250Hz input for ~200ms (events every 4ms).
	for (int i = 0; i < 200; i += 4) {
		auto out = policy.OnMotion(i, t0 + std::chrono::milliseconds(i), i == 0);
		if (out) {
			EXPECT_EQ(Policy::OutputKind::Preview, out->kind);
			EXPECT_GE(out->target, last_preview_target);
			last_preview_target = out->target;
			++preview_emits;
		}
	}

	auto commit = policy.OnRelease(196, t0 + std::chrono::milliseconds(200));
	EXPECT_EQ(Policy::OutputKind::Commit, commit.kind);
	EXPECT_EQ(196, commit.target);
	EXPECT_TRUE(commit.token.IsValid());

	// With a 33ms throttle interval, 200ms of input should emit at most ~7 previews.
	EXPECT_LE(preview_emits, 10u);
}

TEST(navigation_preview_policy, release_cancels_pending_preview) {
	using Policy = NavigationPreviewPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto first = policy.OnMotion(100, t0, true);
	ASSERT_TRUE(first);

	policy.OnMotion(200, t0 + std::chrono::milliseconds(5), false);
	ASSERT_TRUE(policy.NextPreviewTime());

	auto commit = policy.OnRelease(200, t0 + std::chrono::milliseconds(10));
	EXPECT_EQ(Policy::OutputKind::Commit, commit.kind);
	EXPECT_EQ(200, commit.target);
	EXPECT_TRUE(commit.token.IsValid());
	EXPECT_FALSE(policy.NextPreviewTime());

	EXPECT_FALSE(policy.OnTimer(t0 + std::chrono::milliseconds(100)));
}
