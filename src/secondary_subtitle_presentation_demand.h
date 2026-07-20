#pragma once

#include <unordered_set>

enum class SecondarySubtitlePresentationDemandChange {
	None,
	BecameDemanded,
	BecameIdle,
};

/// Tracks visible secondary-subtitle presenters by identity. During a
/// detached-window transition both presenters may briefly report demand, so a
/// single shared bool cannot represent the state correctly.
class SecondarySubtitlePresentationDemand final {
	std::unordered_set<void const*> presenters;

public:
	SecondarySubtitlePresentationDemandChange Set(void const *presenter, bool demanded) {
		if (!presenter)
			return SecondarySubtitlePresentationDemandChange::None;

		bool const had_demand = !presenters.empty();
		if (demanded)
			presenters.insert(presenter);
		else
			presenters.erase(presenter);

		bool const has_demand = !presenters.empty();
		if (had_demand == has_demand)
			return SecondarySubtitlePresentationDemandChange::None;
		return has_demand
			? SecondarySubtitlePresentationDemandChange::BecameDemanded
			: SecondarySubtitlePresentationDemandChange::BecameIdle;
	}

	bool HasDemand() const noexcept { return !presenters.empty(); }
};
