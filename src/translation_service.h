/// @file translation_service.h
/// @brief Framework-agnostic translation service interface for aegisub_core
/// @ingroup utility

#pragma once

#include <string>

/// Translation service interface for aegisub_core (wx-free).
/// The GUI host (Aegisub executable) provides a concrete implementation
/// backed by wxLocale/wxTranslations; headless/test environments can
/// provide a null implementation that returns the source string unchanged.
class TranslationService {
public:
	virtual ~TranslationService() = default;

	/// Translate a message ID to the current UI language.
	/// @param msgid The message ID (source string, typically English)
	/// @return Translated string, or msgid if translation not available
	virtual std::string Translate(std::string const& msgid) const = 0;

	/// Translate with plural form selection.
	/// @param msgid Singular form message ID
	/// @param msgid_plural Plural form message ID
	/// @param n Count determining which plural form to use
	/// @return Translated string appropriate for the count
	virtual std::string TranslatePlural(std::string const& msgid,
	                                    std::string const& msgid_plural,
	                                    unsigned long n) const = 0;
};

/// Null translation service that returns the source string unchanged.
/// Used in headless/test environments or when no translation is needed.
class NullTranslationService final : public TranslationService {
public:
	std::string Translate(std::string const& msgid) const override {
		return msgid;
	}

	std::string TranslatePlural(std::string const& msgid,
	                            std::string const& msgid_plural,
	                            unsigned long n) const override {
		// Simple English plural rule: n != 1
		return n == 1 ? msgid : msgid_plural;
	}
};

/// Thread-local translation service pointer for aegisub_core.
/// Set by the host (GUI or headless) before calling into core services.
/// Defaults to NullTranslationService if not set.
class TranslationContext {
	static thread_local TranslationService const* service;
	static NullTranslationService null_service;

public:
	/// Set the translation service for the current thread.
	/// @param svc Translation service pointer; must remain valid until Reset() or another Set()
	static void Set(TranslationService const* svc) {
		service = svc ? svc : &null_service;
	}

	/// Reset to the default null translation service.
	static void Reset() {
		service = &null_service;
	}

	/// Get the current translation service.
	static TranslationService const& Get() {
		return service ? *service : null_service;
	}

	static TranslationService const* CurrentService() {
		return service ? service : &null_service;
	}
};

// Convenience functions for aegisub_core.
// wx-based translation adapters define AEGISUB_TRANSLATION_SERVICE_NO_SHORTHANDS
// before including this header so they can use the platform translation macros.
#if !defined(AEGISUB_TRANSLATION_SERVICE_NO_SHORTHANDS)
	#ifdef _
		#undef _
	#endif

	/// Translate a message ID (aegisub_core only).
	inline std::string _(std::string const& msgid) {
		return TranslationContext::Get().Translate(msgid);
	}

	/// Translate with plural form selection (aegisub_core only).
	inline std::string _n(std::string const& msgid, std::string const& msgid_plural, unsigned long n) {
		return TranslationContext::Get().TranslatePlural(msgid, msgid_plural, n);
	}
#endif
