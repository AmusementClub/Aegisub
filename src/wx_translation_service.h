/// @file wx_translation_service.h
/// @brief wxWidgets-backed translation service implementation
/// @ingroup utility

#pragma once

#define AEGISUB_TRANSLATION_SERVICE_NO_SHORTHANDS
#include "translation_service.h"
#undef AEGISUB_TRANSLATION_SERVICE_NO_SHORTHANDS
#include <wx/string.h>
#include <wx/translation.h>

/// Translation service backed by wxTranslations/wxLocale.
/// Bridges the aegisub_core translation interface to the wx i18n system.
class WxTranslationService final : public TranslationService {
public:
	std::string Translate(std::string const& msgid) const override {
		wxString wx_msgid = wxString::FromUTF8(msgid);
		wxString translated = wxGetTranslation(wx_msgid);
		return translated.utf8_str().data();
	}

	std::string TranslatePlural(std::string const& msgid,
	                            std::string const& msgid_plural,
	                            unsigned long n) const override {
		wxString wx_msgid = wxString::FromUTF8(msgid);
		wxString wx_msgid_plural = wxString::FromUTF8(msgid_plural);
		wxString translated = wxGetTranslation(wx_msgid, wx_msgid_plural, n);
		return translated.utf8_str().data();
	}
};
