#pragma once

#include "audio_provider_factory.h"
#include "video_provider_manager.h"

// Hosts call this after all built-in, platform, and plugin provider factories
// have been registered for the process.
inline void FinalizeProviderFactoryRegistries() {
	FreezeAudioProviderFactoryRegistry();
	FreezeVideoProviderFactoryRegistry();
}

inline bool AreProviderFactoryRegistriesFinalized() {
	return IsAudioProviderFactoryRegistryFrozen() && IsVideoProviderFactoryRegistryFrozen();
}

inline void FreezeProviderFactoryRegistries() {
	FinalizeProviderFactoryRegistries();
}

inline bool AreProviderFactoryRegistriesFrozen() {
	return AreProviderFactoryRegistriesFinalized();
}
