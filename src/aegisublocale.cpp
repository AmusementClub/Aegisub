// Copyright (c) 2005, Rodrigo Braz Monteiro
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

/// @file aegisublocale.cpp
/// @brief Enumerate available locales for picking translation on Windows
/// @ingroup utility
///

#include "aegisublocale.h"
#include "aegisublocale_compat.h"

#include "compat.h"
#include "locale_choice.h"
#include "locale_pick.h"
#include "options.h"
#include "ui_services.h"

#include <libaegisub/path.h>

#include <algorithm>
#include <clocale>
#include <functional>
#include <wx/intl.h>

#ifndef AEGISUB_CATALOG
#define AEGISUB_CATALOG "aegisub"
#endif

wxTranslations *AegisubLocale::GetTranslations() {
	wxTranslations *translations = wxTranslations::Get();
	if (!translations) {
		wxTranslations::Set(translations = new wxTranslations);
		wxFileTranslationsLoader::AddCatalogLookupPathPrefix(config::path->Decode("?data/locale/").wstring());
	}
	return translations;
}

void AegisubLocale::Init(std::string const& language) {
	wxTranslations *translations = GetTranslations();
	translations->SetLanguage(to_wx(language));
	translations->AddCatalog(wxString::FromUTF8(AEGISUB_CATALOG));
	translations->AddStdCatalog();

	setlocale(LC_NUMERIC, "C");
	setlocale(LC_CTYPE, "C");
	active_language = language;
}

bool AegisubLocale::HasLanguage(std::string const& language) {
	auto langs = GetTranslations()->GetAvailableTranslations(wxString::FromUTF8(AEGISUB_CATALOG));
	return std::find(langs.begin(), langs.end(), to_wx(language)) != langs.end();
}

std::string AegisubLocale::PickLanguage(std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
	auto available = GetTranslations()->GetAvailableTranslations(wxString::FromUTF8(AEGISUB_CATALOG));
	std::vector<std::string> available_languages;
	available_languages.reserve(available.size());
	for (auto const& language : available)
		available_languages.push_back(from_wx(language));

	auto immediate_language = aegisub::locale_pick::ResolveImmediateLanguage(
		available_languages,
		active_language,
		from_wx(aegisub::locale::FindPreferredTranslation(available)));
	if (immediate_language) {
		return *immediate_language;
	}

	wxArrayString langs = available;
	if (std::find(langs.begin(), langs.end(), wxS("en_US")) == langs.end())
		langs.insert(langs.begin(), wxS("en_US"));

	std::string preferred_language;
	auto preferred = aegisub::locale::FindPreferredTranslation(langs);
	if (!preferred.empty()) {
		preferred_language = from_wx(preferred);
	}

	auto language_codes = aegisub::locale_pick::BuildSelectionLanguages(available_languages, preferred_language);

	if (!choice_sink)
		return "";

	auto new_lang = aegisub::locale_choice::ResolveSelection(
		language_codes,
		choice_sink->RequestSingleChoice(aegisub::locale_choice::BuildRequest(language_codes)));
	if (new_lang && *new_lang != active_language)
		return *new_lang;

	return "";
}
