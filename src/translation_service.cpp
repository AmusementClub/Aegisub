#include "translation_service.h"

thread_local TranslationService const* TranslationContext::service = nullptr;
NullTranslationService TranslationContext::null_service;
std::atomic<TranslationService const*> TranslationContext::default_service{&TranslationContext::null_service};
