#include <main.h>

#include "../../src/skia/skia_gl_contract.h"

#include <thread>

namespace {
SkiaGlContextToken MakeContext(std::uintptr_t identity = 1, std::uint64_t generation = 1) {
	return { reinterpret_cast<void const *>(identity), generation };
}
}

TEST(skia_gl_contract, ganesh_desktop_gl_floor_is_explicit) {
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl(""));
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl("1.1.0"));
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl("OpenGL ES 3.0"));
	EXPECT_FALSE(SupportsSkiaGaneshDesktopGl("2"));
	EXPECT_TRUE(SupportsSkiaGaneshDesktopGl("2.0"));
	EXPECT_TRUE(SupportsSkiaGaneshDesktopGl("  2.1 Mesa"));
	EXPECT_TRUE(SupportsSkiaGaneshDesktopGl("4.6.0 NVIDIA 591.86"));
}

TEST(skia_gl_contract, device_state_binds_identity_generation_and_thread_once) {
	SkiaGlDeviceState state;
	auto const context = MakeContext(7, 3);
	auto const thread = std::this_thread::get_id();

	ASSERT_TRUE(state.BeginAccess(context, thread));
	state.MarkHealthy();
	EXPECT_TRUE(state.MatchesOwner(context, thread));
	EXPECT_EQ(SkiaGlDeviceHealth::Healthy, state.Health());
	EXPECT_TRUE(state.BeginAccess(context, thread));
}

TEST(skia_gl_contract, context_identity_mismatch_is_sticky) {
	SkiaGlDeviceState state;
	auto const thread = std::this_thread::get_id();
	ASSERT_TRUE(state.BeginAccess(MakeContext(1, 4), thread));
	state.MarkHealthy();

	EXPECT_FALSE(state.BeginAccess(MakeContext(2, 4), thread));
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
	EXPECT_EQ(SkiaGlDeviceFailure::ContextIdentityMismatch, state.LastFailure());
	EXPECT_FALSE(state.BeginAccess(MakeContext(1, 4), thread));
	EXPECT_EQ(SkiaGlDeviceFailure::ContextIdentityMismatch, state.LastFailure());
}

TEST(skia_gl_contract, context_generation_mismatch_is_sticky) {
	SkiaGlDeviceState state;
	auto const thread = std::this_thread::get_id();
	ASSERT_TRUE(state.BeginAccess(MakeContext(1, 8), thread));
	state.MarkHealthy();

	EXPECT_FALSE(state.BeginAccess(MakeContext(1, 9), thread));
	EXPECT_EQ(SkiaGlDeviceFailure::ContextGenerationMismatch, state.LastFailure());
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
}

TEST(skia_gl_contract, wrong_thread_is_sticky) {
	SkiaGlDeviceState state;
	auto const context = MakeContext();
	ASSERT_TRUE(state.BeginAccess(context, std::this_thread::get_id()));
	state.MarkHealthy();

	bool other_thread_result = true;
	std::thread other([&] {
		other_thread_result = state.BeginAccess(context, std::this_thread::get_id());
	});
	other.join();

	EXPECT_FALSE(other_thread_result);
	EXPECT_EQ(SkiaGlDeviceFailure::WrongThread, state.LastFailure());
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
}

TEST(skia_gl_contract, first_failure_reason_is_preserved) {
	SkiaGlDeviceState state;
	state.Fail(SkiaGlDeviceFailure::FrameBeginInjected, "first");
	state.Fail(SkiaGlDeviceFailure::FlushInjected, "second");

	EXPECT_EQ(SkiaGlDeviceFailure::FrameBeginInjected, state.LastFailure());
	EXPECT_EQ("first", state.LastFailureDetail());
	EXPECT_EQ(SkiaGlDeviceHealth::Unhealthy, state.Health());
}

TEST(skia_gl_contract, abandoned_state_cannot_be_rebound) {
	SkiaGlDeviceState state;
	ASSERT_TRUE(state.BeginAccess(MakeContext(), std::this_thread::get_id()));
	state.MarkHealthy();
	state.MarkAbandoned();

	EXPECT_FALSE(state.BeginAccess(MakeContext(), std::this_thread::get_id()));
	EXPECT_EQ(SkiaGlDeviceHealth::Abandoned, state.Health());
}
