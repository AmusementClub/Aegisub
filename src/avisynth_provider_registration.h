#pragma once

#ifdef WITH_AVISYNTH
void RegisterAvisynthAudioProviderFactory();
void RegisterAvisynthVideoProviderFactory();
#endif

inline void RegisterAvisynthProviderFactories() {
#ifdef WITH_AVISYNTH
	RegisterAvisynthAudioProviderFactory();
	RegisterAvisynthVideoProviderFactory();
#endif
}
