#include "../../src/avisynth_runtime_policy.h"

#include <gtest/gtest.h>

#include <libaegisub/path.h>

TEST(avisynth_runtime_policy, default_request_uses_app_local_runtime_name_only) {
	auto request = avisynth::BuildRuntimeLoadRequest("");
#ifdef _WIN32
	EXPECT_EQ("AviSynth.dll", request.library_name);
#else
	EXPECT_EQ("libavisynth.so", request.library_name);
#endif
	EXPECT_EQ(agi::native::DefaultAppLocalLoadOptions(false), request.load_options);
}

TEST(avisynth_runtime_policy, token_paths_are_resolved_before_loading) {
	agi::Path path;
#ifdef _WIN32
	path.SetToken("?user", "C:\\portable\\Aegisub");
	auto request = avisynth::BuildRuntimeLoadRequest("?user/runtimes/Avisynth.dll", [&path](std::string_view configured_runtime_path) {
		return path.Decode(std::string(configured_runtime_path)).string();
	});
	EXPECT_EQ("C:\\portable\\Aegisub\\runtimes\\Avisynth.dll", request.library_name);
#else
	path.SetToken("?user", "/portable/Aegisub");
	auto request = avisynth::BuildRuntimeLoadRequest("?user/runtimes/libavisynth.so", [&path](std::string_view configured_runtime_path) {
		return path.Decode(std::string(configured_runtime_path)).string();
	});
	EXPECT_EQ("/portable/Aegisub/runtimes/libavisynth.so", request.library_name);
#endif
	EXPECT_EQ(agi::native::DefaultAppLocalLoadOptions(false), request.load_options);
}

TEST(avisynth_runtime_policy, explicit_runtime_path_overrides_default_name_without_system_fallback) {
#ifdef _WIN32
	auto request = avisynth::BuildRuntimeLoadRequest("C:\\Program Files\\AviSynth+\\AviSynth.dll");
	EXPECT_EQ("C:\\Program Files\\AviSynth+\\AviSynth.dll", request.library_name);
#else
	auto request = avisynth::BuildRuntimeLoadRequest("/opt/avisynth/libavisynth.so");
	EXPECT_EQ("/opt/avisynth/libavisynth.so", request.library_name);
#endif
	EXPECT_EQ(agi::native::DefaultAppLocalLoadOptions(false), request.load_options);
}
