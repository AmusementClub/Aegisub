// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "video_provider_manager.h"

#include "factory_manager.h"
#include "include/aegisub/video_provider.h"
#include "options.h"
#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"
#endif
#ifdef WITH_AVISYNTH
#include "avisynth_wrap.h"
#endif

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>

#include <boost/range/iterator_range.hpp>

std::unique_ptr<VideoProvider> CreateDummyVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
std::unique_ptr<VideoProvider> CreateYUV4MPEGVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
std::unique_ptr<VideoProvider> CreateFFmpegSourceVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
std::unique_ptr<VideoProvider> CreateAvisynthVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider>);

namespace {
	struct factory {
		const char *name;
		std::unique_ptr<VideoProvider> (*create)(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
		bool (*is_available)();
		std::string (*availability_error)();
		bool hidden;
	};

#ifdef WITH_FFMS2
	bool IsFFmpegSourceAvailable() {
		return ffms::IsAvailable();
	}

	std::string GetFFmpegSourceAvailabilityError() {
		auto err = ffms::GetLoadError();
		return err.empty() ? "runtime library is unavailable." : err;
	}
#endif

#ifdef WITH_AVISYNTH
	bool IsAvisynthAvailable() {
		return avisynth::IsAvailable();
	}

	std::string GetAvisynthAvailabilityError() {
		auto err = avisynth::GetLoadError();
		return err.empty() ? "runtime library is unavailable." : err;
	}
#endif

std::string GetDisplayName(factory const& provider) {
	std::string name = provider.name;
	if (!provider.hidden && provider.is_available && !provider.is_available())
		name.append(" (Unavailable)");
	return name;
}

	const factory providers[] = {
		{"Dummy", CreateDummyVideoProvider, nullptr, nullptr, true},
		{"YUV4MPEG", CreateYUV4MPEGVideoProvider, nullptr, nullptr, true},
#ifdef WITH_FFMS2
		{"FFmpegSource", CreateFFmpegSourceVideoProvider, IsFFmpegSourceAvailable, GetFFmpegSourceAvailabilityError, false},
#endif
#ifdef WITH_AVISYNTH
		{"Avisynth", CreateAvisynthVideoProvider, IsAvisynthAvailable, GetAvisynthAvailabilityError, false},
#endif
	};
}

std::vector<std::string> VideoProviderFactory::GetClasses() {
	return ::GetClasses(boost::make_iterator_range(std::begin(providers), std::end(providers)));
}

std::vector<std::pair<std::string, std::string>> VideoProviderFactory::GetChoices() {
	std::vector<std::pair<std::string, std::string>> choices;
	for (auto const& provider : providers) {
		if (!provider.hidden)
			choices.emplace_back(GetDisplayName(provider), provider.name);
	}
	return choices;
}

std::unique_ptr<VideoProvider> VideoProviderFactory::GetProvider(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br) {
	auto preferred = OPT_GET("Video/Provider")->GetString();
	auto sorted = GetSorted(boost::make_iterator_range(std::begin(providers), std::end(providers)), preferred);

	bool found = false;
	bool supported = false;
	std::string errors;
	errors.reserve(1024);

	for (auto factory : sorted) {
		std::string err;
		if (factory->is_available && !factory->is_available()) {
			err = factory->availability_error ? factory->availability_error() : "runtime library is unavailable.";
			errors.append(factory->name);
			errors.append(": ");
			errors.append(err);
			errors.push_back('\n');
			LOG_D("manager/video/provider") << factory->name << ": " << err;
			continue;
		}

		try {
			auto provider = factory->create(filename, colormatrix, br);
			if (!provider) continue;
			LOG_I("manager/video/provider") << factory->name << ": opened " << filename;
			return provider->WantsCaching() ? CreateCacheVideoProvider(std::move(provider)) : std::move(provider);
		}
		catch (agi::fs::FileNotFound const&) {
			err = "file not found.";
			// Keep trying other providers as this one may just not be able to
			// open a valid path
		}
		catch (VideoNotSupported const&) {
			found = true;
			err = "video is not in a supported format.";
		}
		catch (VideoOpenError const& ex) {
			supported = true;
			err = ex.GetMessage();
		}
		catch (agi::vfr::Error const& ex) {
			supported = true;
			err = ex.GetMessage();
		}

		errors.append(factory->name);
		errors.append(": ");
		errors.append(err);
		errors.push_back('\n');
		LOG_D("manager/video/provider") << factory->name << ": " << err;
	}

	// No provider could open the file
	LOG_E("manager/video/provider") << "Could not open " << filename;
	std::string msg = "Could not open ";
	msg.append(filename.string());
	msg.append(":\n");
	msg.append(errors);

	if (!found) throw agi::fs::FileNotFound(filename.string());
	if (!supported) throw VideoNotSupported(msg);
	throw VideoOpenError(msg);
}
