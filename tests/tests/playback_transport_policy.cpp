#include <main.h>

#include "../../src/playback_transport_policy.h"

TEST(playback_transport_policy, inspection_emits_each_call_and_supersedes_previous_token) {
	using Policy = PlaybackTransportPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto first = policy.Apply({ Policy::InputKind::InspectionStep, Policy::InteractionKind::Navigation, 10, false }, t0);
	ASSERT_EQ(1u, first.size());
	EXPECT_EQ(Policy::OutputKind::Inspection, first[0].kind);
	EXPECT_EQ(10, first[0].target);
	ASSERT_TRUE(first[0].token.IsValid());
	auto const first_token = first[0].token;

	auto second = policy.Apply({ Policy::InputKind::InspectionStep, Policy::InteractionKind::Navigation, 11, false }, t0 + std::chrono::milliseconds(1));
	ASSERT_EQ(1u, second.size());
	EXPECT_EQ(Policy::OutputKind::Inspection, second[0].kind);
	EXPECT_EQ(11, second[0].target);
	EXPECT_TRUE(second[0].token.IsValid());
	EXPECT_FALSE(first_token.IsValid());
}

TEST(playback_transport_policy, play_to_scrub_release_to_step) {
	using Policy = PlaybackTransportPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto play = policy.Apply({ Policy::InputKind::PlayToggle, Policy::InteractionKind::Navigation, 0, false }, t0);
	ASSERT_EQ(1u, play.size());
	EXPECT_EQ(Policy::OutputKind::StartPlayback, play[0].kind);
	EXPECT_TRUE(play[0].token.IsValid());
	EXPECT_TRUE(policy.IsPlaying());

	auto motion = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::DragEdit, 100, true }, t0 + std::chrono::milliseconds(1));
	ASSERT_EQ(2u, motion.size());
	EXPECT_EQ(Policy::OutputKind::StopPlayback, motion[0].kind);
	EXPECT_EQ(Policy::OutputKind::Preview, motion[1].kind);
	EXPECT_EQ(100, motion[1].target);
	ASSERT_TRUE(motion[1].token.IsValid());
	auto const preview_token = motion[1].token;
	EXPECT_FALSE(policy.IsPlaying());

	auto coalesced = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::DragEdit, 200, false }, t0 + std::chrono::milliseconds(5));
	EXPECT_TRUE(coalesced.empty());
	EXPECT_FALSE(preview_token.IsValid());
	ASSERT_TRUE(policy.NextPreviewTime());

	auto timer = policy.Apply({ Policy::InputKind::Timer, Policy::InteractionKind::DragEdit, 0, false }, *policy.NextPreviewTime());
	ASSERT_EQ(1u, timer.size());
	EXPECT_EQ(Policy::OutputKind::Preview, timer[0].kind);
	EXPECT_EQ(200, timer[0].target);
	ASSERT_TRUE(timer[0].token.IsValid());
	auto const timer_token = timer[0].token;

	auto release = policy.Apply({ Policy::InputKind::PreviewRelease, Policy::InteractionKind::DragEdit, 200, false }, t0 + std::chrono::milliseconds(40));
	ASSERT_EQ(1u, release.size());
	EXPECT_EQ(Policy::OutputKind::Commit, release[0].kind);
	EXPECT_EQ(200, release[0].target);
	EXPECT_TRUE(release[0].token.IsValid());
	EXPECT_FALSE(timer_token.IsValid());
	EXPECT_FALSE(policy.NextPreviewTime());

	auto step = policy.Apply({ Policy::InputKind::InspectionStep, Policy::InteractionKind::Navigation, 201, false }, t0 + std::chrono::milliseconds(41));
	ASSERT_EQ(1u, step.size());
	EXPECT_EQ(Policy::OutputKind::Inspection, step[0].kind);
	EXPECT_EQ(201, step[0].target);
	EXPECT_TRUE(step[0].token.IsValid());
}

TEST(playback_transport_policy, inspection_cancels_pending_preview) {
	using Policy = PlaybackTransportPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto first = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::Navigation, 100, true }, t0);
	ASSERT_EQ(1u, first.size());
	EXPECT_EQ(Policy::OutputKind::Preview, first[0].kind);

	auto pending = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::Navigation, 200, false }, t0 + std::chrono::milliseconds(5));
	EXPECT_TRUE(pending.empty());
	ASSERT_TRUE(policy.NextPreviewTime());

	auto step = policy.Apply({ Policy::InputKind::InspectionStep, Policy::InteractionKind::Navigation, 42, false }, t0 + std::chrono::milliseconds(10));
	ASSERT_EQ(1u, step.size());
	EXPECT_EQ(Policy::OutputKind::Inspection, step[0].kind);
	EXPECT_FALSE(policy.NextPreviewTime());

	auto timer = policy.Apply({ Policy::InputKind::Timer, Policy::InteractionKind::Navigation, 0, false }, t0 + std::chrono::milliseconds(100));
	EXPECT_TRUE(timer.empty());
}

TEST(playback_transport_policy, cancel_preview_allows_forced_motion_to_start_new_session_immediately) {
	using Policy = PlaybackTransportPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto first = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::StepRepeat, 10, true }, t0);
	ASSERT_EQ(1u, first.size());
	EXPECT_EQ(Policy::OutputKind::Preview, first[0].kind);
	auto const first_token = first[0].token;

	auto pending = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::StepRepeat, 20, false }, t0 + std::chrono::milliseconds(5));
	EXPECT_TRUE(pending.empty());
	EXPECT_FALSE(first_token.IsValid());
	ASSERT_TRUE(policy.NextPreviewTime());

	auto cancelled = policy.Apply({ Policy::InputKind::CancelPreview, Policy::InteractionKind::StepRepeat, 0, false }, t0 + std::chrono::milliseconds(10));
	EXPECT_TRUE(cancelled.empty());
	EXPECT_FALSE(policy.NextPreviewTime());

	auto restart = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::StepRepeat, 30, true }, t0 + std::chrono::milliseconds(11));
	ASSERT_EQ(1u, restart.size());
	EXPECT_EQ(Policy::OutputKind::Preview, restart[0].kind);
	EXPECT_EQ(30, restart[0].target);
	EXPECT_TRUE(restart[0].token.IsValid());
}

TEST(playback_transport_policy, repeat_step_uses_preview_then_commit) {
	using Policy = PlaybackTransportPolicy;
	using Clock = Policy::Clock;

	Policy policy(std::chrono::milliseconds(33));
	auto const t0 = Clock::time_point{};

	auto first = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::StepRepeat, 10, true }, t0);
	ASSERT_EQ(1u, first.size());
	EXPECT_EQ(Policy::OutputKind::Preview, first[0].kind);
	EXPECT_EQ(10, first[0].target);
	ASSERT_TRUE(first[0].token.IsValid());
	auto const first_token = first[0].token;

	auto pending = policy.Apply({ Policy::InputKind::PreviewMotion, Policy::InteractionKind::StepRepeat, 20, false }, t0 + std::chrono::milliseconds(5));
	EXPECT_TRUE(pending.empty());
	EXPECT_FALSE(first_token.IsValid());
	ASSERT_TRUE(policy.NextPreviewTime());

	auto timer = policy.Apply({ Policy::InputKind::Timer, Policy::InteractionKind::StepRepeat, 0, false }, *policy.NextPreviewTime());
	ASSERT_EQ(1u, timer.size());
	EXPECT_EQ(Policy::OutputKind::Preview, timer[0].kind);
	EXPECT_EQ(20, timer[0].target);
	ASSERT_TRUE(timer[0].token.IsValid());
	auto const timer_token = timer[0].token;

	auto release = policy.Apply({ Policy::InputKind::PreviewRelease, Policy::InteractionKind::StepRepeat, 20, false }, t0 + std::chrono::milliseconds(40));
	ASSERT_EQ(1u, release.size());
	EXPECT_EQ(Policy::OutputKind::Commit, release[0].kind);
	EXPECT_EQ(20, release[0].target);
	EXPECT_TRUE(release[0].token.IsValid());
	EXPECT_FALSE(timer_token.IsValid());
}
