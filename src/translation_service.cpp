#include "translation_service.h"

TranslationService const* TranslationContext::service = &TranslationContext::null_service;
NullTranslationService TranslationContext::null_service;
