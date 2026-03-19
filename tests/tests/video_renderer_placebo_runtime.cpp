#ifdef WITH_LIBPLACEBO

#include "../../src/video_renderer_placebo_runtime.h"

#include <gtest/gtest.h>

TEST(video_renderer_placebo_runtime, loads_runtime_when_available) {
	if (!placebo::runtime::IsAvailable())
		GTEST_SKIP() << placebo::runtime::GetLoadError();

	auto const& api = placebo::runtime::GetApi();
	EXPECT_NE(nullptr, api.log_create);
	EXPECT_NE(nullptr, api.opengl_create);
	EXPECT_NE(nullptr, api.renderer_create);
	EXPECT_NE(nullptr, api.opengl_wrap);
	EXPECT_NE(nullptr, api.plane_data_align);
	EXPECT_NE(nullptr, api.frame_set_chroma_location);
	EXPECT_NE(nullptr, api.upload_plane);
	EXPECT_FALSE(placebo::runtime::GetLoadedLibrary().empty());
	EXPECT_FALSE(placebo::runtime::GetLoadedVersion().empty());
}

#endif
