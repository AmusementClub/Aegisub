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

#include "audio_provider_factory.h"

#include "compat.h"
#include "factory_manager.h"
#include "options.h"
#include "ui_services.h"
#include "utils.h"
#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"
#endif
#ifdef WITH_AVISYNTH
#include "avisynth_wrap.h"
#endif

#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/string_utils.h>

using namespace agi;

std::unique_ptr<AudioProvider> CreateAvisynthAudioProvider(fs::path const& filename, BackgroundRunner *);
std::unique_ptr<AudioProvider> CreateFFmpegSourceAudioProvider(fs::path const& filename, BackgroundRunner *, std::shared_ptr<SingleChoiceInteractionSink> choice_sink);

namespace {
struct factory {
	const char *name;
	std::unique_ptr<AudioProvider> (*create)(fs::path const&, BackgroundRunner *, std::shared_ptr<SingleChoiceInteractionSink>);
	bool (*is_available)();
	std::string (*availability_error)();
	bool hidden;
};

std::unique_ptr<AudioProvider> CreateDummyAudioProviderWithChoice(fs::path const& filename, BackgroundRunner *br, std::shared_ptr<SingleChoiceInteractionSink>) {
	return CreateDummyAudioProvider(filename, br);
}

std::unique_ptr<AudioProvider> CreatePCMAudioProviderWithChoice(fs::path const& filename, BackgroundRunner *br, std::shared_ptr<SingleChoiceInteractionSink>) {
	return CreatePCMAudioProvider(filename, br);
}

#ifdef WITH_AVISYNTH
std::unique_ptr<AudioProvider> CreateAvisynthAudioProviderWithChoice(fs::path const& filename, BackgroundRunner *br, std::shared_ptr<SingleChoiceInteractionSink>) {
	return CreateAvisynthAudioProvider(filename, br);
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
	{"Dummy", CreateDummyAudioProviderWithChoice, nullptr, nullptr, true},
	{"PCM", CreatePCMAudioProviderWithChoice, nullptr, nullptr, true},
#ifdef WITH_FFMS2
	{"FFmpegSource", CreateFFmpegSourceAudioProvider, IsFFmpegSourceAvailable, GetFFmpegSourceAvailabilityError, false},
#endif
#ifdef WITH_AVISYNTH
	{"Avisynth", CreateAvisynthAudioProviderWithChoice, IsAvisynthAvailable, GetAvisynthAvailabilityError, false},
#endif
};
}

std::vector<std::string> GetAudioProviderNames() {
	return ::GetClasses(providers);
}

std::vector<std::pair<std::string, std::string>> GetAudioProviderChoices() {
	std::vector<std::pair<std::string, std::string>> choices;
	for (auto const& provider : providers) {
		if (!provider.hidden)
			choices.emplace_back(GetDisplayName(provider), provider.name);
	}
	return choices;
}

std::unique_ptr<agi::AudioProvider> GetAudioProvider(fs::path const& filename,
                                                     Path const& path_helper,
                                                     BackgroundRunner *br,
                                                     NotificationSink& notification_sink,
                                                     std::shared_ptr<SingleChoiceInteractionSink> choice_sink) {
	auto preferred = OPT_GET("Audio/Provider")->GetString();
	auto sorted = GetSorted(providers, preferred);

	std::unique_ptr<AudioProvider> provider;
	bool found_file = false;
	bool found_audio = false;
	std::string msg_all;     // error messages from all attempted providers
	std::string msg_partial; // error messages from providers that could partially load the file (knows container, missing codec)

	for (auto const& factory : sorted) {
		if (factory->is_available && !factory->is_available()) {
			std::string err;
			err.append(factory->name);
			err.append(": ");
			err.append(factory->availability_error ? factory->availability_error() : "runtime library is unavailable.");
			LOG_D("audio_provider") << err;
			msg_all.append(err);
			msg_all.push_back('\n');
			continue;
		}

		try {
			provider = factory->create(filename, br, choice_sink);
			if (!provider) continue;
			LOG_I("audio_provider") << "Using audio provider: " << factory->name;
			break;
		}
		catch (fs::FileNotFound const& err) {
			LOG_D("audio_provider") << err.GetMessage();
			msg_all.append(factory->name);
			msg_all.append(": ");
			msg_all.append(err.GetMessage());
			msg_all.append(" not found.\n");
		}
		catch (AudioDataNotFound const& err) {
			LOG_D("audio_provider") << err.GetMessage();
			found_file = true;
			msg_all.append(factory->name);
			msg_all.append(": ");
			msg_all.append(err.GetMessage());
			msg_all.push_back('\n');
		}
		catch (AudioProviderError const& err) {
			LOG_D("audio_provider") << err.GetMessage();
			found_audio = true;
			found_file = true;
			std::string thismsg;
			thismsg.append(factory->name);
			thismsg.append(": ");
			thismsg.append(err.GetMessage());
			thismsg.push_back('\n');
			msg_all.append(thismsg);
			msg_partial.append(thismsg);
		}
	}

	if (!provider) {
		if (found_audio)
			throw AudioProviderError(msg_partial);
		if (found_file)
			throw AudioDataNotFound(msg_all);
		throw fs::FileNotFound(filename);
	}

	bool needs_cache = provider->NeedsCache();

	// Give it a converter if needed
	if (provider->GetBytesPerSample() != 2 || provider->GetSampleRate() < 32000 || provider->GetChannels() != 1)
		provider = CreateConvertAudioProvider(std::move(provider));

	// Change provider to RAM/HD cache if needed
	int cache = OPT_GET("Audio/Cache/Type")->GetInt();
	if (!cache || !needs_cache)
		return CreateLockAudioProvider(std::move(provider));

	// Convert to RAM
	if (cache == 1) {
		if (sizeof(void*) == 4 && (provider->GetNumSamples() * provider->GetChannels() * provider->GetBytesPerSample() >= (1 << 30))) {
			auto message = from_wx(_(
				"Unable to create RAM audio cache: 32-bit memory limit exceeded. Fallback to hard disk cache.\n\n"
				"Possible solutions:\n"
				"- Use 64-bit version\n"
				"- Turn off cache or switch to hard disk cache in Preferences -> Advanced -> Audio -> Cache -> Cache type\n"
				"- Enable channel downmix in Preferences -> Advanced -> Audio"
			));
			notification_sink.ShowError(from_wx(_("Out of Memory")), message);
			cache = 2;
		}
		else
			return CreateRAMAudioProvider(std::move(provider));
	}

	// Convert to HD
	if (cache == 2) {
		auto path = OPT_GET("Audio/Cache/HD/Location")->GetString();
		if (path == "default")
			path = "?temp";
		auto cache_dir = path_helper.MakeAbsolute(path_helper.Decode(path), "?temp");
		return CreateHDAudioProvider(std::move(provider), cache_dir);
	}

	throw InternalError("Invalid audio caching method");
}
