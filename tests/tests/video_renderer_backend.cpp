#include "../../src/video_renderer_backend.h"

#include <gtest/gtest.h>

TEST(video_renderer_backend, defaults_unknown_values_to_opengl) {
	EXPECT_EQ(ParseVideoRendererBackend(""), VideoRendererBackend::OpenGL);
	EXPECT_EQ(ParseVideoRendererBackend("unexpected"), VideoRendererBackend::OpenGL);
}

TEST(video_renderer_backend, parses_opengl_aliases) {
	EXPECT_EQ(ParseVideoRendererBackend("modern"), VideoRendererBackend::OpenGL);
	EXPECT_EQ(ParseVideoRendererBackend("modern-gl"), VideoRendererBackend::OpenGL);
	EXPECT_EQ(ParseVideoRendererBackend("opengl"), VideoRendererBackend::OpenGL);
}

TEST(video_renderer_backend, parses_placebo_aliases) {
	EXPECT_EQ(ParseVideoRendererBackend("libplacebo"), VideoRendererBackend::PlaceboOpenGL);
	EXPECT_EQ(ParseVideoRendererBackend("placebo"), VideoRendererBackend::PlaceboOpenGL);
	EXPECT_EQ(ParseVideoRendererBackend("libplacebo-gl"), VideoRendererBackend::PlaceboOpenGL);
}

TEST(video_renderer_backend, retains_placebo_when_available) {
	auto decision = ResolveVideoRendererBackend("libplacebo", true);
	EXPECT_EQ(decision.requested, VideoRendererBackend::PlaceboOpenGL);
	EXPECT_EQ(decision.actual, VideoRendererBackend::PlaceboOpenGL);
	EXPECT_FALSE(decision.fell_back);
}

TEST(video_renderer_backend, falls_back_to_opengl_when_placebo_unavailable) {
	auto decision = ResolveVideoRendererBackend("libplacebo", false);
	EXPECT_EQ(decision.requested, VideoRendererBackend::PlaceboOpenGL);
	EXPECT_EQ(decision.actual, VideoRendererBackend::OpenGL);
	EXPECT_TRUE(decision.fell_back);
}
