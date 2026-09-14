#include <main.h>

#include "../../src/audio_display_source.h"
#include "../../src/audio_renderer.h"
#include "../../src/audio_waveform_summary_cache.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/init.h>

namespace {
class PartiallyFailingAudioProvider final : public agi::AudioProvider {
	public:
	mutable std::array<int, 3> reads{};
	bool fail_middle = true;
	bool unexpected_error = false;

	PartiallyFailingAudioProvider() {
		channels = 1;
		num_samples = 96;
		decoded_samples = num_samples;
		sample_rate = 1000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		++reads.at(static_cast<size_t>(start / 32));
		auto *samples = static_cast<int16_t *>(buffer);
		std::fill_n(samples, static_cast<size_t>(count / 2), int16_t{32767});
		if (unexpected_error)
			throw std::runtime_error("unexpected renderer failure");
		if (fail_middle && start < 64 && start + count > 32)
			throw agi::AudioDecodeError("persistent middle block failure");
		for (int64_t i = 0; i < count; ++i)
			samples[i] = static_cast<int16_t>((1 + (start + i) / 32) * 8192);
	}
};

// Real checked display reads and waveform summaries feed a small, deterministic
// bitmap provider, so the renderer's failed-cache rollback is visible in pixels.
class SummaryBitmapProvider final : public AudioRendererBitmapProvider {
	std::unique_ptr<AudioDisplaySource> source;
	AudioWaveformSummaryCache summaries;

	void OnSetProvider() override {
		summaries.SetSource(nullptr);
		source = CreateInt16MonoAudioDisplaySource(provider);
		summaries.SetSource(source.get());
	}

	void OnSetMillisecondsPerPixel() override {
		summaries.SetMillisecondsPerPixel(pixel_ms);
	}

	public:
	void Render(wxBitmap& bitmap, int start, AudioRenderingStyle) override {
		wxMemoryDC dc(bitmap);
		dc.SetBackground(wxBrush(wxColour(255, 0, 255)));
		dc.Clear();
		auto block = summaries.Get(static_cast<size_t>(start / 32));
		if (!block || !block->has_exact_pcm16)
			throw std::runtime_error("missing waveform summary");
		for (int x = 0; x < bitmap.GetWidth(); ++x) {
			auto const value = static_cast<unsigned char>(block->pcm16_summaries[x].peak_max / 128);
			dc.SetPen(wxPen(wxColour(value, value, value)));
			dc.DrawLine(x, 0, x, bitmap.GetHeight());
		}
	}

	void RenderBlank(wxDC& dc, wxRect const& rect, AudioRenderingStyle) override {
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(*wxBLACK_BRUSH);
		dc.DrawRectangle(rect);
	}

	void AgeCache(size_t max_size) override { summaries.Age(max_size); }

	bool HasSummary(size_t block) { return !!summaries.GetIfReady(block); }
};

class RendererLogCapture final : public agi::log::Emitter {
	public:
	std::vector<std::string> messages;

	void log(agi::log::SinkMessage const& message) override {
		if (std::string_view(message.section) == "audio/renderer")
			messages.push_back(message.message);
	}
};

class AudioRendererFailureTest : public testing::Test {
	wxInitializer wx;
	RendererLogCapture *log_capture = nullptr;

	protected:
	PartiallyFailingAudioProvider source;
	SummaryBitmapProvider bitmap_provider;
	AudioRenderer renderer;

	void SetUp() override {
		ASSERT_TRUE(wx.IsOk());
		auto capture = std::make_unique<RendererLogCapture>();
		log_capture = capture.get();
		agi::log::log->Subscribe(std::move(capture));
		renderer.SetAudioProvider(&source);
		renderer.SetRenderer(&bitmap_provider);
		renderer.SetHeight(8);
		renderer.SetCacheMaxSize(4 * 1024 * 1024);
	}

	void TearDown() override {
		if (log_capture)
			agi::log::log->Unsubscribe(log_capture);
	}

	wxImage Paint(AudioRenderingStyle style = AudioStyle_Normal) {
		wxBitmap output(96, 8, 24);
		{
			wxMemoryDC dc(output);
			dc.SetBackground(wxBrush(wxColour(0, 255, 255)));
			dc.Clear();
			renderer.Render(dc, wxPoint(0, 0), 0, 96, style);
		}
		return output.ConvertToImage();
	}

	void ExpectColumns(wxImage const& image, int first, int last, unsigned char value) {
		ASSERT_TRUE(image.IsOk());
		for (int x = first; x < last; ++x) {
			SCOPED_TRACE(x);
			EXPECT_EQ(value, image.GetRed(x, 4));
			EXPECT_EQ(value, image.GetGreen(x, 4));
			EXPECT_EQ(value, image.GetBlue(x, 4));
		}
	}

	std::vector<std::string> const& ErrorMessages() const { return log_capture->messages; }
};
}

TEST_F(AudioRendererFailureTest, failed_block_is_blank_and_later_blocks_still_render) {
	wxImage image;
	ASSERT_NO_THROW(image = Paint());
	ExpectColumns(image, 0, 32, 64);
	ExpectColumns(image, 32, 64, 0);
	ExpectColumns(image, 64, 96, 192);
	EXPECT_EQ((std::array<int, 3>{1, 2, 1}), source.reads);
	EXPECT_TRUE(bitmap_provider.HasSummary(0));
	EXPECT_FALSE(bitmap_provider.HasSummary(1));
	EXPECT_TRUE(bitmap_provider.HasSummary(2));
}

TEST_F(AudioRendererFailureTest, failed_bitmap_and_analysis_recover_on_next_paint) {
	ASSERT_NO_THROW(Paint());
	source.fail_middle = false;
	wxImage image;
	ASSERT_NO_THROW(image = Paint());
	ExpectColumns(image, 0, 32, 64);
	ExpectColumns(image, 32, 64, 128);
	ExpectColumns(image, 64, 96, 192);
	EXPECT_TRUE(bitmap_provider.HasSummary(1));
	EXPECT_EQ((std::array<int, 3>{1, 3, 1}), source.reads);
	ASSERT_NO_THROW(image = Paint());
	ExpectColumns(image, 32, 64, 128);
	EXPECT_EQ((std::array<int, 3>{1, 3, 1}), source.reads);
}

TEST_F(AudioRendererFailureTest, failure_reports_once_until_provider_changes_and_detaches) {
	ASSERT_NO_THROW(Paint());
	renderer.Invalidate();
	ASSERT_NO_THROW(Paint());
	ASSERT_NO_THROW(Paint(AudioStyle_Selected));
	ASSERT_EQ(1u, ErrorMessages().size());
	EXPECT_EQ("Audio display read failed: persistent middle block failure", ErrorMessages()[0]);
	EXPECT_EQ(6, source.reads[1]);

	PartiallyFailingAudioProvider replacement;
	renderer.SetAudioProvider(&replacement);
	ASSERT_NO_THROW(Paint());
	ASSERT_EQ(2u, ErrorMessages().size());
	EXPECT_EQ(ErrorMessages()[0], ErrorMessages()[1]);
	EXPECT_EQ(2, replacement.reads[1]);
	renderer.SetAudioProvider(nullptr);
	ASSERT_NO_THROW(Paint());
	EXPECT_EQ(2u, ErrorMessages().size());
}

TEST_F(AudioRendererFailureTest, unexpected_render_errors_still_propagate_without_caching) {
	source.unexpected_error = true;
	EXPECT_THROW(Paint(), std::runtime_error);
	EXPECT_TRUE(ErrorMessages().empty());
	EXPECT_FALSE(bitmap_provider.HasSummary(0));
	source.unexpected_error = false;
	source.fail_middle = false;
	wxImage image;
	ASSERT_NO_THROW(image = Paint());
	ExpectColumns(image, 0, 32, 64);
	EXPECT_EQ(2, source.reads[0]);
}
