#include <main.h>

#include "../../src/audio_display_source.h"
#include "../../src/avisynth_wrap.h"
#include "../../src/options.h"

#include <avisynth.h>

#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

namespace agi {
class BackgroundRunner;
}
std::unique_ptr<agi::AudioProvider> CreateAvisynthAudioProvider(agi::fs::path const&, agi::BackgroundRunner *);

namespace {
struct ReadState {
	int failures_remaining = 0;
	std::vector<std::pair<int64_t, int64_t>> requests;
};

class FailingAudioClip final : public GenericVideoFilter {
	ReadState& state;

	public:
	FailingAudioClip(PClip const& source, ReadState& state) : GenericVideoFilter(source), state(state) {}

	void __stdcall GetAudio(void *buffer, int64_t start, int64_t count, IScriptEnvironment *env) override {
		state.requests.emplace_back(start, count);
		if (state.failures_remaining > 0) {
			--state.failures_remaining;
			env->ThrowError("audio read regression failure");
		}
		std::fill_n(static_cast<int16_t *>(buffer), count, int16_t{16384});
	}
};

AVSValue __cdecl CreateFailingAudioClip(AVSValue args, void *user_data, IScriptEnvironment *) {
	return new FailingAudioClip(args[0].AsClip(), *static_cast<ReadState *>(user_data));
}

class AvisynthAudioProviderTest : public ::testing::Test {
	agi::Options options{"", "{}", agi::Options::FLUSH_SKIP};
	agi::Path paths;
	agi::Options *previous_options = nullptr;
	agi::Path *previous_path = nullptr;
	agi::fs::path root;
	std::unique_ptr<AviSynthWrapper> runtime;

	protected:
	ReadState state;
	std::unique_ptr<agi::AudioProvider> provider;

	void SetUp() override {
		root = agi::fs::UniquePath(std::filesystem::temp_directory_path() / "aegisub-avs-audio-%%%%%%%%");
		agi::fs::CreateDirectory(root);
		paths.SetToken("?user", root);
		paths.SetToken("?data", root);
		previous_options = config::opt;
		previous_path = config::path;
		config::opt = &options;
		config::path = &paths;
		if (!avisynth::IsAvailable())
			GTEST_SKIP() << "AviSynth runtime unavailable: " << avisynth::GetLoadError();
		runtime = std::make_unique<AviSynthWrapper>();
		runtime->GetEnv()->AddFunction("AegisubAudioReadRegression", "c", CreateFailingAudioClip, &state);
		auto const script = root / "audio-read.avs";
		{
			std::ofstream output(script);
			output << "AegisubAudioReadRegression(BlankClip(length=1, width=16, height=16, fps=1, "
					  "audio_rate=48000, channels=1, sample_type=\"16bit\"))\n";
			ASSERT_TRUE(output.good());
		}
		provider = CreateAvisynthAudioProvider(script, nullptr);
		ASSERT_EQ(48000, provider->GetNumSamples());
		ASSERT_EQ(1, provider->GetChannels());
		ASSERT_EQ(2, provider->GetBytesPerSample());
	}

	void TearDown() override {
		provider.reset();
		runtime.reset();
		config::opt = previous_options;
		config::path = previous_path;
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
};

TEST_F(AvisynthAudioProviderTest, checked_read_preserves_decoder_error_type_and_message) {
	state.failures_remaining = 1;
	std::array<int16_t, 32> samples{};
	try {
		provider->GetAudioChecked(samples.data(), 100, samples.size());
		FAIL() << "Expected AudioDecodeError";
	}
	catch (agi::AudioDecodeError const& error) {
		EXPECT_EQ("Avisynth error: audio read regression failure", error.GetMessage());
	}
	ASSERT_EQ(1u, state.requests.size());
	EXPECT_EQ((std::pair<int64_t, int64_t>{100, 32}), state.requests[0]);
}

TEST_F(AvisynthAudioProviderTest, display_retries_transient_error_before_returning_samples) {
	state.failures_remaining = 1;
	auto display = CreateAudioDisplaySource(provider.get());
	std::array<float, 32> samples{};
	display->GetFloatAudio(samples.data(), 100, samples.size());
	ASSERT_EQ(2u, state.requests.size());
	EXPECT_EQ((std::pair<int64_t, int64_t>{100, 32}), state.requests[0]);
	EXPECT_EQ(state.requests[0], state.requests[1]);
	for (auto sample : samples)
		EXPECT_FLOAT_EQ(0.5f, sample);
}

TEST_F(AvisynthAudioProviderTest, display_stops_after_two_failed_reads_and_can_recover) {
	state.failures_remaining = 2;
	auto display = CreateAudioDisplaySource(provider.get());
	std::array<float, 32> samples{};
	try {
		display->GetFloatAudio(samples.data(), 100, samples.size());
		FAIL() << "Expected AudioDecodeError";
	}
	catch (agi::AudioDecodeError const& error) {
		EXPECT_EQ("Avisynth error: audio read regression failure", error.GetMessage());
	}
	ASSERT_EQ(2u, state.requests.size());
	EXPECT_EQ((std::pair<int64_t, int64_t>{100, 32}), state.requests[0]);
	EXPECT_EQ(state.requests[0], state.requests[1]);
	display->GetFloatAudio(samples.data(), 100, samples.size());
	EXPECT_EQ(3u, state.requests.size());
	for (auto sample : samples)
		EXPECT_FLOAT_EQ(0.5f, sample);
}
}
