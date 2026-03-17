// Copyright (c) 2006-2007, Rodrigo Braz Monteiro, Evgeniy Stepanov
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file subtitles_provider_libass.cpp
/// @brief libass-based subtitle renderer
/// @ingroup subtitle_rendering
///

#include "subtitles_provider_libass.h"

#include "compat.h"
#include "include/aegisub/subtitles_provider.h"
#include "ready_flag.h"
#include "subtitle_overlay_blend.h"
#include "video_frame.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

#include <wx/intl.h>
#include <wx/thread.h>

extern "C" {
#include <ass/ass.h>
}

namespace {
std::unique_ptr<agi::dispatch::Queue> cache_queue;
ASS_Library *library;

void msg_callback(int level, const char *fmt, va_list args, void *) {
	if (level >= 7) return;
	char buf[1024];
#ifdef _WIN32
	vsprintf_s(buf, sizeof(buf), fmt, args);
#else
	vsnprintf(buf, sizeof(buf), fmt, args);
#endif

	if (level < 2) // warning/error
		LOG_I("subtitle/provider/libass") << buf;
	else // verbose
		LOG_D("subtitle/provider/libass") << buf;
}

// Stuff used on the cache thread, owned by a shared_ptr in case the provider
// gets deleted before the cache finishing updating
struct cache_thread_shared {
	ASS_Renderer *renderer = nullptr;
	std::mutex mutex;
	ReadyFlag ready;
	~cache_thread_shared() { if (renderer) ass_renderer_done(renderer); }
};

class LibassSubtitlesProvider final : public SubtitlesProvider {
	agi::BackgroundRunner *br;
	std::shared_ptr<cache_thread_shared> shared;
	ASS_Track* ass_track = nullptr;

	ASS_Renderer *renderer() {
		if (!shared->ready.IsReady()) {
			auto wait_for_ready = [&] {
				if (shared->ready.WaitFor(std::chrono::milliseconds(250)))
					return;

				if (!br) {
					shared->ready.Wait();
					return;
				}

				br->Run([=](agi::ProgressSink *ps) {
					ps->SetTitle(from_wx(_("Updating font index")));
					ps->SetMessage(from_wx(_("This may take several minutes")));
					ps->SetIndeterminate();
					shared->ready.Wait();
				});
			};

			if (wxThread::IsMain())
				wait_for_ready();
			else
				agi::dispatch::Main().Sync(wait_for_ready);
		}

		std::lock_guard<std::mutex> lock(shared->mutex);
		return shared->renderer;
	}

public:
	LibassSubtitlesProvider(agi::BackgroundRunner *br);
	~LibassSubtitlesProvider();

	void LoadSubtitles(const char *data, size_t len) override {
		if (ass_track) ass_free_track(ass_track);
		ass_track = ass_read_memory(library, const_cast<char *>(data), len, nullptr);
		if (!ass_track) throw agi::InternalError("libass failed to load subtitles.");
	}

	SubtitleRenderMode GetRenderMode() const override { return SubtitleRenderMode::PremultipliedOverlay; }
	bool RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double time) override;
	void DrawSubtitles(VideoFrame &dst, double time) override;

	void Reinitialize() override {
		// No need to reinit if we're not even done with the initial init
		if (!shared->ready.IsReady())
			return;

		std::lock_guard<std::mutex> lock(shared->mutex);
		ass_renderer_done(shared->renderer);
		shared->renderer = ass_renderer_init(library);
		ass_set_font_scale(shared->renderer, 1.);
		ass_set_fonts(shared->renderer, nullptr, "Sans", 1, nullptr, true);
	}
};

LibassSubtitlesProvider::LibassSubtitlesProvider(agi::BackgroundRunner *br)
: br(br)
, shared(std::make_shared<cache_thread_shared>())
{
	auto state = shared;
	cache_queue->Async([state] {
		auto ass_renderer = ass_renderer_init(library);
		if (ass_renderer) {
			ass_set_font_scale(ass_renderer, 1.);
			ass_set_fonts(ass_renderer, nullptr, "Sans", 1, nullptr, true);
		}
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->renderer = ass_renderer;
		}
		state->ready.Signal();
	});
}

LibassSubtitlesProvider::~LibassSubtitlesProvider() {
	if (ass_track) ass_free_track(ass_track);
}

bool LibassSubtitlesProvider::RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double time) {
	if (!overlay.IsValid() || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8)
		return false;

	int render_width = overlay.width;
	int render_height = overlay.height;
	if (source.IsValid()) {
		render_width = source.width;
		render_height = source.height;
	}

	ass_set_frame_size(renderer(), render_width, render_height);
	ass_set_storage_size(renderer(), render_width, render_height);

	ASS_Image* img = ass_render_frame(renderer(), ass_track, int(time * 1000), nullptr);
	BgraSubtitleTargetView target {
		overlay.planes[0].data,
		overlay.planes[0].stride,
		overlay.width,
		overlay.height,
		overlay.flipped
	};

	auto blend_mode = overlay.premultiplied_alpha
		? SubtitleOverlayBlendMode::PremultipliedOverlay
		: SubtitleOverlayBlendMode::LegacyBakeIn;
	overlay.composition_mode = overlay.premultiplied_alpha
		? SubtitleOverlayCompositionMode::PremultipliedAlpha
		: SubtitleOverlayCompositionMode::Unsupported;
	if (overlay.premultiplied_alpha)
		ClearBgraSubtitleTarget(target);

	for (; img; img = img->next) {
		BlendLibassMaskIntoBgraTarget(
			target,
			blend_mode,
			img->dst_x,
			img->dst_y,
			img->w,
			img->h,
			img->bitmap,
			img->stride,
			static_cast<std::uint32_t>(img->color));
	}

	return true;
}

void LibassSubtitlesProvider::DrawSubtitles(VideoFrame &frame,double time) {
	auto source = MakeSourceFrameView(frame);
	auto overlay = MakeLegacyBgraSubtitleOverlayView(frame);
	RenderOverlay(source, overlay, time);
}
}

namespace libass {
std::unique_ptr<SubtitlesProvider> Create(std::string const&, agi::BackgroundRunner *br) {
	return agi::make_unique<LibassSubtitlesProvider>(br);
}

void CacheFonts() {
	// Initialize the cache worker thread
	cache_queue = agi::dispatch::Create();

	// Initialize libass
	library = ass_library_init();
	ass_set_message_cb(library, msg_callback, nullptr);

	// Initialize a renderer to force fontconfig to update its cache
	cache_queue->Async([] {
		auto ass_renderer = ass_renderer_init(library);
		ass_set_fonts(ass_renderer, nullptr, "Sans", 1, nullptr, true);
		ass_renderer_done(ass_renderer);
	});
}
}
