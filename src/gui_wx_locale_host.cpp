// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "gui_wx_locale_host.h"

#include "aegisublocale_compat.h"
#include "compat.h"
#include "options.h"

#include <libaegisub/path.h>

#include <algorithm>
#include <wx/intl.h>

#ifndef AEGISUB_CATALOG
#define AEGISUB_CATALOG "aegisub"
#endif

namespace {

wxTranslations *GetTranslations() {
	wxTranslations *translations = wxTranslations::Get();
	if (!translations) {
		wxTranslations::Set(translations = new wxTranslations);
		wxFileTranslationsLoader::AddCatalogLookupPathPrefix(config::path->Decode("?data/locale/").wstring());
	}
	return translations;
}

std::vector<std::string> ToUtf8Languages(wxArrayString const& languages) {
	std::vector<std::string> values;
	values.reserve(languages.size());
	for (auto const& language : languages)
		values.push_back(from_wx(language));
	return values;
}

wxArrayString ToWxLanguages(std::vector<std::string> const& languages) {
	wxArrayString values;
	values.reserve(languages.size());
	for (auto const& language : languages)
		values.push_back(to_wx(language));
	return values;
}

}

RuntimeLocaleHost BuildGuiWxRuntimeLocaleHost() {
	return {
		[] {
			return ToUtf8Languages(GetTranslations()->GetAvailableTranslations(wxString::FromUTF8(AEGISUB_CATALOG)));
		},
		[](std::vector<std::string> const& languages) {
			auto preferred = aegisub::locale::FindPreferredTranslation(ToWxLanguages(languages));
			return preferred.empty() ? std::string() : from_wx(preferred);
		},
		[](std::string const& language) {
			auto *translations = GetTranslations();
			translations->SetLanguage(to_wx(language));
			translations->AddCatalog(wxString::FromUTF8(AEGISUB_CATALOG));
			translations->AddStdCatalog();
		},
		[](std::string const& language) {
			if (language == "en_US")
				return true;
			auto langs = GetTranslations()->GetAvailableTranslations(wxString::FromUTF8(AEGISUB_CATALOG));
			return std::find(langs.begin(), langs.end(), to_wx(language)) != langs.end();
		}
	};
}
