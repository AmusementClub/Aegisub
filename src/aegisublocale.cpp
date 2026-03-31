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

#include "locale_choice.h"
#include "locale_pick.h"
#include "ui_services.h"

#include <clocale>

void AegisubLocale::SetHost(RuntimeLocaleHost host_hooks) {
	host = std::move(host_hooks);
}

void AegisubLocale::Init(std::string const& language) {
	if (host.initialize_language)
		host.initialize_language(language);

	setlocale(LC_NUMERIC, "C");
	setlocale(LC_CTYPE, "C");
	active_language = language;
}

bool AegisubLocale::HasLanguage(std::string const& language) {
	if (language == "en_US")
		return true;
	return host.has_language ? host.has_language(language) : false;
}

std::string AegisubLocale::PickLanguage(std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
	auto immediate_language = aegisub::locale_pick::ResolveImmediateLanguage(
		host.get_available_languages ? host.get_available_languages() : std::vector<std::string>(),
		active_language,
		host.find_preferred_language && host.get_available_languages
			? host.find_preferred_language(host.get_available_languages())
			: std::string());
	if (immediate_language) {
		return *immediate_language;
	}

	auto available_languages = host.get_available_languages ? host.get_available_languages() : std::vector<std::string>();
	auto preferred_language = host.find_preferred_language
		? host.find_preferred_language(aegisub::locale_pick::BuildSelectionLanguages(available_languages, {}))
		: std::string();

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
