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
#ifdef WITH_LSMASNATIVE
#include "lsmas_native_api.h"
#endif
#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"
#endif
#ifdef WITH_AVISYNTH
#include "avisynth_wrap.h"
#endif

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>

#include <exception>
#include <utility>

std::unique_ptr<VideoProvider> CreateDummyVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
std::unique_ptr<VideoProvider> CreateYUV4MPEGVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
std::unique_ptr<VideoProvider> CreateLsmasNativeVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
std::unique_ptr<VideoProvider> CreateFFmpegSourceVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
std::unique_ptr<VideoProvider> CreateAvisynthVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider>);

namespace {
	thread_local aegisub::provider_selection_diagnostics::SelectionReport last_video_provider_selection_report;

	struct factory {
		const char *name;
		std::unique_ptr<VideoProvider> (*create)(agi::fs::path const&, std::string const&, agi::BackgroundRunner *, std::shared_ptr<agi::SingleChoiceInteractionSink>);
		bool (*is_available)();
		std::string (*availability_error)();
		bool hidden;
	};

	std::unique_ptr<VideoProvider> CreateDummyVideoProviderWithChoice(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink>) {
		return CreateDummyVideoProvider(filename, colormatrix, br);
	}

	std::unique_ptr<VideoProvider> CreateYUV4MPEGVideoProviderWithChoice(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink>) {
		return CreateYUV4MPEGVideoProvider(filename, colormatrix, br);
	}

#ifdef WITH_AVISYNTH
	std::unique_ptr<VideoProvider> CreateAvisynthVideoProviderWithChoice(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink>) {
		return CreateAvisynthVideoProvider(filename, colormatrix, br);
	}
#endif

#ifdef WITH_FFMS2
	bool IsFFmpegSourceAvailable() {
		return ffms::IsAvailable();
	}

	std::string GetFFmpegSourceAvailabilityError() {
		auto err = ffms::GetLoadError();
		return err.empty() ? "runtime library is unavailable." : err;
	}
#endif

#ifdef WITH_LSMASNATIVE
	bool IsLsmasNativeAvailable() {
		return lsmas::IsAvailable();
	}

	std::string GetLsmasNativeAvailabilityError() {
		auto err = lsmas::GetLoadError();
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

std::string GetAvailabilityError(factory const& provider) {
	if (!provider.availability_error)
		return "runtime library is unavailable.";

	try {
		return provider.availability_error();
	}
	catch (agi::Exception const& err) {
		return err.GetMessage();
	}
	catch (std::exception const& err) {
		return err.what();
	}
	catch (...) {
		return "unknown availability error";
	}
}

bool IsProviderAvailable(factory const& provider, std::string& availability_error) {
	if (!provider.is_available)
		return true;

	try {
		if (provider.is_available())
			return true;
	}
	catch (agi::Exception const& err) {
		availability_error = err.GetMessage();
		return false;
	}
	catch (std::exception const& err) {
		availability_error = err.what();
		return false;
	}
	catch (...) {
		availability_error = "unknown availability exception";
		return false;
	}

	availability_error = GetAvailabilityError(provider);
	return false;
}

	const factory providers[] = {
		{"Dummy", CreateDummyVideoProviderWithChoice, nullptr, nullptr, true},
		{"YUV4MPEG", CreateYUV4MPEGVideoProviderWithChoice, nullptr, nullptr, true},
#ifdef WITH_FFMS2
		{"FFmpegSource", CreateFFmpegSourceVideoProvider, IsFFmpegSourceAvailable, GetFFmpegSourceAvailabilityError, false},
#endif
#ifdef WITH_LSMASNATIVE
		{"LsmasNative", CreateLsmasNativeVideoProvider, IsLsmasNativeAvailable, GetLsmasNativeAvailabilityError, false},
#endif
#ifdef WITH_AVISYNTH
		{"Avisynth", CreateAvisynthVideoProviderWithChoice, IsAvisynthAvailable, GetAvisynthAvailabilityError, false},
#endif
	};

	void RecordAttempt(aegisub::provider_selection_diagnostics::SelectionReport& report,
	                   char const* provider_name,
	                   char const* outcome,
	                   std::string detail = {}) {
		report.attempts.push_back({provider_name ? provider_name : "", outcome ? outcome : "", std::move(detail)});
	}
}

aegisub::provider_catalog::ProviderCatalog VideoProviderFactory::GetCatalog(std::string const& preferred_provider) {
	auto preferred = aegisub::provider_selection_diagnostics::CanonicalizeProviderName(preferred_provider);
	auto sorted = GetSorted(providers, preferred);

	aegisub::provider_catalog::ProviderCatalog catalog;
	catalog.kind = aegisub::provider_catalog::ProviderKind::Video;
	catalog.preferred_provider = preferred;
	catalog.providers.reserve(sorted.size());

	for (auto const* provider : sorted) {
		std::string availability_error;
		bool available = IsProviderAvailable(*provider, availability_error);

		aegisub::provider_catalog::ProviderDescriptor descriptor;
		descriptor.kind = catalog.kind;
		descriptor.name = provider->name;
		descriptor.display_name = provider->name;
		descriptor.hidden = provider->hidden;
		descriptor.available = available;
		descriptor.unavailable_reason = std::move(availability_error);
		if (!descriptor.hidden && !descriptor.available)
			descriptor.display_name.append(" (Unavailable)");
		catalog.providers.push_back(std::move(descriptor));
	}

	return catalog;
}

std::vector<std::string> VideoProviderFactory::GetClasses() {
	return ::GetClasses(providers);
}

std::vector<std::pair<std::string, std::string>> VideoProviderFactory::GetChoices() {
	return aegisub::provider_catalog::VisibleProviderChoices(GetCatalog());
}

std::unique_ptr<VideoProvider> VideoProviderFactory::GetProvider(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
	auto preferred = aegisub::provider_selection_diagnostics::CanonicalizeProviderName(OPT_GET("Video/Provider")->GetString());
	auto sorted = GetSorted(providers, preferred);
	aegisub::provider_selection_diagnostics::SelectionReport diagnostics;
	diagnostics.preferred_provider = preferred;
	last_video_provider_selection_report = diagnostics;

	bool found = false;
	bool supported = false;
	std::string errors;
	errors.reserve(1024);

	for (auto factory : sorted) {
		std::string err;
		char const* attempt_outcome = "error";
		if (factory->is_available && !factory->is_available()) {
			err = factory->availability_error ? factory->availability_error() : "runtime library is unavailable.";
			errors.append(factory->name);
			errors.append(": ");
			errors.append(err);
			errors.push_back('\n');
			LOG_D("manager/video/provider") << factory->name << ": " << err;
			RecordAttempt(diagnostics, factory->name, "unavailable", err);
			continue;
		}

		try {
			auto provider = factory->create(filename, colormatrix, br, choice_sink);
			if (!provider) {
				RecordAttempt(diagnostics, factory->name, "returned_null", "provider factory returned null");
				continue;
			}
			diagnostics.selected_provider = factory->name;
			RecordAttempt(diagnostics, factory->name, "opened");
			last_video_provider_selection_report = diagnostics;
			LOG_I("manager/video/provider") << factory->name << ": opened " << filename;
			return provider->WantsCaching() ? CreateCacheVideoProvider(std::move(provider)) : std::move(provider);
		}
		catch (agi::fs::FileNotFound const&) {
			err = "file not found.";
			attempt_outcome = "file_not_found";
			// Keep trying other providers as this one may just not be able to
			// open a valid path
		}
		catch (VideoNotSupported const&) {
			found = true;
			err = "video is not in a supported format.";
			attempt_outcome = "not_supported";
		}
		catch (VideoOpenError const& ex) {
			supported = true;
			err = ex.GetMessage();
			attempt_outcome = "error";
		}
		catch (agi::vfr::Error const& ex) {
			supported = true;
			err = ex.GetMessage();
			attempt_outcome = "error";
		}

		errors.append(factory->name);
		errors.append(": ");
		errors.append(err);
		errors.push_back('\n');
		LOG_D("manager/video/provider") << factory->name << ": " << err;
		RecordAttempt(diagnostics, factory->name, attempt_outcome, err);
	}

	last_video_provider_selection_report = diagnostics;

	// No provider could open the file
	LOG_E("manager/video/provider") << "Could not open " << filename;
	std::string msg = "Could not open ";
	msg.append(agi::fs::PathToString(filename));
	msg.append(":\n");
	msg.append(errors);

	if (!found) throw agi::fs::FileNotFound(filename);
	if (!supported) throw VideoNotSupported(msg);
	throw VideoOpenError(msg);
}

aegisub::provider_selection_diagnostics::SelectionReport GetLastVideoProviderSelectionReport() {
	return last_video_provider_selection_report;
}

void ClearLastVideoProviderSelectionReport() {
	last_video_provider_selection_report = {};
}
