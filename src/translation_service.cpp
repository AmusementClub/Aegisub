#include "translation_service.h"

thread_local TranslationService const* TranslationContext::service = &TranslationContext::null_service;
NullTranslationService TranslationContext::null_service;
