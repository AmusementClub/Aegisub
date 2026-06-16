/// @file translation_context_guard.h
/// @brief RAII guard for setting/resetting TranslationContext
/// @ingroup utility

#pragma once

#include "translation_service.h"

/// RAII guard that sets a translation service for the current thread
/// and resets it on destruction. Used by the GUI host to inject the
/// WxTranslationService when entering core service calls.
class TranslationContextGuard {
	TranslationService const* previous = nullptr;

public:
	explicit TranslationContextGuard(TranslationService const* service)
		: previous(TranslationContext::CurrentService()) {
		TranslationContext::Set(service);
	}

	~TranslationContextGuard() {
		TranslationContext::Set(previous);
	}

	TranslationContextGuard(TranslationContextGuard const&) = delete;
	TranslationContextGuard& operator=(TranslationContextGuard const&) = delete;
};
