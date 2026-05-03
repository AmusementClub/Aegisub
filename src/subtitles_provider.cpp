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

#include "include/aegisub/subtitles_provider.h"

#include "ass_dialogue.h"
#include "ass_time_projection.h"
#include "ass_attachment.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "factory_manager.h"
#include "options.h"
#include "subtitles_provider_csri.h"
#include "subtitles_provider_libass.h"
#include "subtitles_provider_plugin.h"

#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>

namespace {
	constexpr char kSubtitleProviderSelectLogTag[] = "subtitle/provider/select";

	struct factory {
		std::string name;
		std::string subtype;
		std::unique_ptr<SubtitlesProvider> (*create)(std::string const& subtype, SubtitleRenderEnvironment const& env);
		bool hidden;
		bool external_file;
	};

	std::vector<factory> subtitle_file_factories() {
		std::vector<factory> factories;
		for (auto const& provider : subtitle_plugin::List())
			factories.push_back(factory{provider, provider, subtitle_plugin::Create, false, true});
		return factories;
	}

	std::vector<factory> ass_renderer_factories() {
		std::vector<factory> factories;
#ifdef WITH_CSRI
		for (auto const& subtype : csri::List())
			factories.push_back(factory{"CSRI/" + subtype, subtype, csri::Create, false, false});
#endif
		factories.push_back(factory{"libass", "", libass::Create, false, false});
		return factories;
	}
}

std::vector<std::string> SubtitlesProviderFactory::GetClasses() {
	auto available_factories = ass_renderer_factories();
	return ::GetClasses(available_factories);
}

bool SubtitlesProviderFactory::HasExternalFileProviderFor(agi::fs::path const& filename) {
	return subtitle_plugin::HasExternalFileProviderFor(filename);
}

std::vector<std::string> SubtitlesProviderFactory::GetExternalFileProviderWildcards() {
	return subtitle_plugin::GetExternalFileProviderWildcards();
}

std::unique_ptr<SubtitlesProvider> SubtitlesProviderFactory::GetProvider(SubtitleRenderEnvironment const& env) {
	auto preferred = env.preferred_provider.empty()
		? OPT_GET("Subtitle/Provider")->GetString()
		: env.preferred_provider;
	auto available_factories = env.require_external_file_provider
		? subtitle_file_factories()
		: ass_renderer_factories();
	auto sorted = GetSorted(available_factories, preferred);
	LOG_I(kSubtitleProviderSelectLogTag) << "Selecting subtitles provider"
		<< (preferred.empty() ? "" : ": preferred=" + preferred);

	std::string error;
	for (auto factory : sorted) {
		try {
			auto provider = factory->create(factory->subtype, env);
			if (provider) {
				LOG_I(kSubtitleProviderSelectLogTag) << "Selected subtitles provider: " << factory->name;
				return provider;
			}
		}
		catch (agi::UserCancelException const&) { throw; }
		catch (agi::Exception const& err) {
			LOG_W(kSubtitleProviderSelectLogTag) << "Subtitle provider unavailable: "
				<< factory->name << ": " << err.GetMessage();
			error.append(factory->name);
			error.append(": ");
			error.append(err.GetMessage());
			error.push_back('\n');
		}
		catch (...) {
			LOG_W(kSubtitleProviderSelectLogTag) << "Subtitle provider unavailable: "
				<< factory->name << ": Unknown error";
			error.append(factory->name);
			error.append(": Unknown error\n");
		}
	}

	if (error.empty() && env.require_external_file_provider)
		throw std::string("No dynamic subtitle plugin provider is available for this subtitle file.");
	throw error;
}

void SubtitlesProvider::LoadSubtitles(AssFile *subs, int time, agi::vfr::Framerate const* fps) {
	buffer.clear();

	auto push_header = [&](const char *str) {
		buffer.insert(buffer.end(), str, str + strlen(str));
	};
	auto push_line = [&](std::string const& str) {
		buffer.insert(buffer.end(), &str[0], &str[0] + str.size());
		buffer.push_back('\n');
	};

	push_header("\xEF\xBB\xBF[Script Info]\n");
	for (auto const& line : subs->Info)
		push_line(line.GetEntryData());

	// libass requires PlayRes for all coordinate mapping (\pos, \move, \fs,
	// margins). If only LayoutRes is set, libass falls back to PlayRes=384x288
	// (Gabest default), which causes a preview mismatch with Aegisub's visual
	// tools that use LayoutRes as the effective resolution. Inject synthetic
	// PlayRes headers so libass renders with the correct coordinate space.
	// See libass ass_render.c: init_font_scale() — screen_scale always uses
	// PlayRes; x2scr_pos/y2scr_pos always use PlayRes.
	if (subs->GetScriptInfo("PlayResX").empty() && subs->GetScriptInfo("PlayResY").empty()) {
		int w = 0, h = 0;
		// Prefer LayoutRes if available, otherwise use effective resolution
		if (subs->GetScriptInfoAsInt("LayoutResX") > 0 && subs->GetScriptInfoAsInt("LayoutResY") > 0) {
			w = subs->GetScriptInfoAsInt("LayoutResX");
			h = subs->GetScriptInfoAsInt("LayoutResY");
		} else {
			subs->GetResolution(ScriptResolutionType::PlayRes, w, h);
		}
		if (w > 0 && h > 0) {
			push_line("PlayResX: " + std::to_string(w));
			push_line("PlayResY: " + std::to_string(h));
		}
	}

	push_header("[V4+ Styles]\n");
	for (auto const& line : subs->Styles)
		push_line(line.GetEntryData());

	if (!subs->Attachments.empty()) {
		// TODO: some scripts may have a lot of attachments, 
		// so ideally we'd want to write only those actually used on the requested video frame,
		// but this would require some pre-parsing of the attached font files with FreeType,
		// which isn't probably trivial.
		push_header("[Fonts]\n");
		for (auto const& attachment : subs->Attachments)
			if (attachment.Group() == AssEntryGroup::FONT)
				push_line(attachment.GetEntryData());
	}

	push_header("[Events]\n");
	for (auto const& line : subs->Events) {
		if (!line.Comment && (time < 0 || IsAssDialogueVisibleAtTimeForOutput(line.Start, line.End, time, AssTimeOutputMode::LegacyRounding, fps)))
			push_line(SerializeAssDialogueForOutput(line, AssTimeOutputMode::LegacyRounding, fps));
	}

	LoadSubtitles(&buffer[0], buffer.size());
}
