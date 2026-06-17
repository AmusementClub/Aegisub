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

#include "include/aegisub/video_provider.h"
#include "options.h"
#include "provider_catalog_builder.h"
#include "provider_factory_entry.h"
#include "provider_open_policy.h"
#ifdef WITH_LSMASNATIVE
#include "lsmas_native_api.h"
#endif
#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"
#endif

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

std::unique_ptr<VideoProvider> CreateDummyVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
std::unique_ptr<VideoProvider> CreateYUV4MPEGVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *);
std::unique_ptr<VideoProvider> CreateLsmasNativeVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
std::unique_ptr<VideoProvider> CreateFFmpegSourceVideoProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider>);

namespace {
	thread_local aegisub::provider_selection_diagnostics::SelectionReport last_video_provider_selection_report;

	using Factory = VideoProviderFactoryEntry;

	std::unique_ptr<VideoProvider> CreateDummyVideoProviderWithChoice(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink>) {
		return CreateDummyVideoProvider(filename, colormatrix, br);
	}

	std::unique_ptr<VideoProvider> CreateYUV4MPEGVideoProviderWithChoice(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink>) {
		return CreateYUV4MPEGVideoProvider(filename, colormatrix, br);
	}

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

	const Factory builtin_providers[] = {
		{"Dummy", CreateDummyVideoProviderWithChoice, nullptr, nullptr, true},
		{"YUV4MPEG", CreateYUV4MPEGVideoProviderWithChoice, nullptr, nullptr, true},
#ifdef WITH_FFMS2
		{"FFmpegSource", CreateFFmpegSourceVideoProvider, IsFFmpegSourceAvailable, GetFFmpegSourceAvailabilityError, false},
#endif
#ifdef WITH_LSMASNATIVE
		{"LsmasNative", CreateLsmasNativeVideoProvider, IsLsmasNativeAvailable, GetLsmasNativeAvailabilityError, false},
#endif
	};

	std::vector<Factory>& RegisteredProviders() {
		static std::vector<Factory> providers;
		return providers;
	}

	std::mutex& RegisteredProvidersMutex() {
		static std::mutex mutex;
		return mutex;
	}

	std::vector<Factory> ProviderFactories() {
		std::vector<Factory> factories(std::begin(builtin_providers), std::end(builtin_providers));
		std::lock_guard<std::mutex> lock(RegisteredProvidersMutex());
		auto const& registered = RegisteredProviders();
		factories.insert(factories.end(), registered.begin(), registered.end());
		return factories;
	}
}

void RegisterVideoProviderFactory(VideoProviderFactoryEntry factory) {
	std::lock_guard<std::mutex> lock(RegisteredProvidersMutex());
	auto& providers = RegisteredProviders();
	auto name = factory.name ? factory.name : "";
	auto existing = std::find_if(providers.begin(), providers.end(), [&](auto const& provider) {
		return name == std::string(provider.name ? provider.name : "");
	});
	if (existing == providers.end())
		providers.push_back(factory);
}

aegisub::provider_catalog::ProviderCatalog VideoProviderFactory::GetCatalog(std::string const& preferred_provider) {
	auto providers = ProviderFactories();
	return aegisub::provider_catalog::BuildCatalog(
		aegisub::provider_catalog::ProviderKind::Video,
		providers,
		preferred_provider,
		aegisub::provider_catalog::DescribeProviderFactoryEntry<VideoProviderCreate>);
}

std::vector<std::string> VideoProviderFactory::GetClasses() {
	auto providers = ProviderFactories();
	return aegisub::provider_catalog::VisibleFactoryNames(providers, aegisub::provider_catalog::DescribeProviderFactoryEntry<VideoProviderCreate>);
}

std::vector<std::pair<std::string, std::string>> VideoProviderFactory::GetChoices() {
	return aegisub::provider_catalog::VisibleProviderChoices(GetCatalog());
}

std::unique_ptr<VideoProvider> VideoProviderFactory::GetProvider(agi::fs::path const& filename, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
	auto preferred = aegisub::provider_selection_diagnostics::CanonicalizeProviderName(OPT_GET("Video/Provider")->GetString());
	auto providers = ProviderFactories();
	auto sorted = aegisub::provider_catalog::SortFactories(providers, preferred, aegisub::provider_catalog::DescribeProviderFactoryEntry<VideoProviderCreate>);
	aegisub::provider_selection_diagnostics::SelectionReport diagnostics;
	diagnostics.preferred_provider = preferred;
	last_video_provider_selection_report = diagnostics;

	aegisub::provider_catalog::VideoProviderOpenFailureReport open_failures;
	open_failures.errors.reserve(1024);

	for (auto factory : sorted) {
		std::string err;

		try {
			auto attempt = aegisub::provider_catalog::TryOpenProviderFactory(
				*factory,
				aegisub::provider_catalog::DescribeProviderFactoryEntry<VideoProviderCreate>,
				diagnostics,
				[&](Factory const& provider_factory) {
					return provider_factory.create(filename, colormatrix, br, choice_sink);
				});

			if (attempt.state == aegisub::provider_catalog::ProviderOpenAttemptState::Unavailable) {
				aegisub::provider_catalog::RecordVideoProviderOpenFailureAttempt(
					open_failures,
					diagnostics,
					factory->name,
					attempt.unavailable_reason,
					aegisub::provider_catalog::VideoProviderOpenAttemptFailure::Unavailable);
				LOG_D("manager/video/provider") << factory->name << ": " << attempt.unavailable_reason;
				continue;
			}
			if (attempt.state == aegisub::provider_catalog::ProviderOpenAttemptState::ReturnedNull)
				continue;

			auto provider = std::move(attempt.provider);
			last_video_provider_selection_report = diagnostics;
			LOG_I("manager/video/provider") << factory->name << ": opened " << filename;
			return provider->WantsCaching() ? CreateCacheVideoProvider(std::move(provider)) : std::move(provider);
		}
		catch (agi::fs::FileNotFound const&) {
			err = "file not found.";
			// Keep trying other providers as this one may just not be able to
			// open a valid path
			aegisub::provider_catalog::RecordVideoProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				err,
				aegisub::provider_catalog::VideoProviderOpenAttemptFailure::FileNotFound);
		}
		catch (VideoNotSupported const&) {
			err = "video is not in a supported format.";
			aegisub::provider_catalog::RecordVideoProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				err,
				aegisub::provider_catalog::VideoProviderOpenAttemptFailure::NotSupported);
		}
		catch (VideoOpenError const& ex) {
			err = ex.GetMessage();
			aegisub::provider_catalog::RecordVideoProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				err,
				aegisub::provider_catalog::VideoProviderOpenAttemptFailure::OpenError);
		}
		catch (agi::vfr::Error const& ex) {
			err = ex.GetMessage();
			aegisub::provider_catalog::RecordVideoProviderOpenFailureAttempt(
				open_failures,
				diagnostics,
				factory->name,
				err,
				aegisub::provider_catalog::VideoProviderOpenAttemptFailure::OpenError);
		}

		LOG_D("manager/video/provider") << factory->name << ": " << err;
	}

	last_video_provider_selection_report = diagnostics;

	// No provider could open the file
	LOG_E("manager/video/provider") << "Could not open " << filename;
	std::string msg = "Could not open ";
	msg.append(agi::fs::PathToString(filename));
	msg.append(":\n");
	msg.append(open_failures.errors);

	switch (aegisub::provider_catalog::FinalVideoProviderOpenFailure(open_failures)) {
	case aegisub::provider_catalog::VideoProviderOpenFailure::FileNotFound:
		throw agi::fs::FileNotFound(filename);
	case aegisub::provider_catalog::VideoProviderOpenFailure::NotSupported:
		throw VideoNotSupported(msg);
	case aegisub::provider_catalog::VideoProviderOpenFailure::OpenError:
		throw VideoOpenError(msg);
	}
	throw VideoOpenError(msg);
}

aegisub::provider_selection_diagnostics::SelectionReport GetLastVideoProviderSelectionReport() {
	return last_video_provider_selection_report;
}

void ClearLastVideoProviderSelectionReport() {
	last_video_provider_selection_report = {};
}
