// Copyright (c) 2007, Rodrigo Braz Monteiro
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

/// @file subtitles_provider_csri.cpp
/// @brief Wrapper for CSRI-based subtitle renderers
/// @ingroup subtitle_rendering
///

#ifdef WITH_CSRI
#include "subtitles_provider_csri.h"

#include "include/aegisub/subtitles_provider.h"
#include "subtitle_format_ass.h"
#include "video_frame.h"

#include <libaegisub/make_unique.h>

#include <mutex>

#ifdef WIN32
#define CSRIAPI
#endif

#include <csri/csri.h>

namespace {
// CSRI renderers are not required to be thread safe (and VSFilter very much
// is not)
std::mutex csri_mutex;

struct closer {
	void operator()(csri_inst *inst) { if (inst) csri_close(inst); }
};

bool RenderCsriBgraOverlay(csri_inst *instance, SubtitleOverlay& overlay, double time) {
	if (!instance || !overlay.IsValid() || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8)
		return false;

	csri_frame frame = { };
	auto *data = overlay.planes[0].data;
	auto stride = overlay.planes[0].stride;
	if (!data || stride == 0)
		return false;

	if (overlay.flipped && stride > 0) {
		data += (overlay.height - 1) * stride;
		stride = -stride;
	}

	frame.planes[0] = data;
	frame.strides[0] = stride;
	frame.pixfmt = CSRI_F_BGR_;

	csri_fmt format = {
		frame.pixfmt,
		static_cast<unsigned>(overlay.width),
		static_cast<unsigned>(overlay.height)
	};

	std::lock_guard<std::mutex> lock(csri_mutex);
	if (!csri_request_fmt(instance, &format))
		csri_render(instance, &frame, time);
	return true;
}

class CSRISubtitlesProvider final : public SubtitlesProvider {
	std::unique_ptr<csri_inst, closer> instance;
	csri_rend *renderer = nullptr;

	void LoadSubtitles(const char *data, size_t len) override {
		std::lock_guard<std::mutex> lock(csri_mutex);
		instance.reset(csri_open_mem(renderer, data, len, nullptr));
	}

public:
	CSRISubtitlesProvider(std::string subType);

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double time) override;
	void DrawSubtitles(VideoFrame &dst, double time) override;
};

CSRISubtitlesProvider::CSRISubtitlesProvider(std::string type) {
	std::lock_guard<std::mutex> lock(csri_mutex);
	for (csri_rend *cur = csri_renderer_default(); cur; cur = csri_renderer_next(cur)) {
		if (type == csri_renderer_info(cur)->name) {
			renderer = cur;
			break;
		}
	}

	if (!renderer)
		throw agi::InternalError("CSRI renderer vanished between initial list and creation?");
}

bool CSRISubtitlesProvider::RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double time) {
	overlay.premultiplied_alpha = false;
	return RenderCsriBgraOverlay(instance.get(), overlay, time);
}

void CSRISubtitlesProvider::DrawSubtitles(VideoFrame &dst, double time) {
	auto overlay = MakeLegacyBgraSubtitleOverlayView(dst);
	RenderCsriBgraOverlay(instance.get(), overlay, time);
}
}

namespace csri {
std::vector<std::string> List() {
	std::vector<std::string> final;
	for (csri_rend *cur = csri_renderer_default(); cur; cur = csri_renderer_next(cur)) {
		std::string name(csri_renderer_info(cur)->name);
		if (name.find("aegisub") != name.npos)
			final.insert(final.begin(), name);
		else
			final.push_back(name);
	}
	return final;
}

std::unique_ptr<SubtitlesProvider> Create(std::string const& name, agi::BackgroundRunner *) {
	return agi::make_unique<CSRISubtitlesProvider>(name);
}
}
#endif // WITH_CSRI
