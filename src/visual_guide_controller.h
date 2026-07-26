#pragma once

#include "visual_guide_model.h"

#include <libaegisub/signal.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct VisualGuideSnapshot {
	std::uint64_t generation = 1;
	std::vector<VisualGuide> guides;
	std::optional<std::string> selected_id;
	/// Convenience: the most recently added measurement guide ID. Always equal
	/// to guides.back().id when guides is non-empty.
	std::optional<std::string> last_measurement_id;
};

/// A non-owning view for paint paths which run on the UI thread. Do not retain
/// it across a controller mutation.
struct VisualGuideSnapshotView {
	std::uint64_t const& generation;
	std::vector<VisualGuide> const& guides;
	std::optional<std::string> const& selected_id;
};

/// Session-only visual guides. This controller owns no ASS data, so mutations
/// do not create subtitle commits or dirty the subtitle document.
class VisualGuideController final {
public:
	static constexpr size_t MaximumGuideCount = 256;

	VisualGuideController() = default;
	VisualGuideController(VisualGuideController const&) = delete;
	VisualGuideController& operator=(VisualGuideController const&) = delete;

	/// Assign a stable session ID and add a valid guide, or return no ID when
	/// the data is invalid or the session guide limit has been reached.
	[[nodiscard]] std::optional<std::string> Add(VisualGuide guide);
	bool Update(std::string const& id, VisualGuide guide);
	bool Remove(std::string const& id);
	bool Select(std::string const& id);
	bool Select(std::optional<std::string> id);
	void Clear();
	void ResetForVideoChange();

	[[nodiscard]] VisualGuideSnapshot CaptureSnapshot() const;
	[[nodiscard]] VisualGuideSnapshotView CaptureView() const noexcept;

	DEFINE_SIGNAL_ADDERS(changed, AddChangedListener)

private:
	std::vector<VisualGuide> guides;
	std::optional<std::string> selected_id;
	std::uint64_t next_id = 1;
	std::uint64_t generation = 1;
	agi::signal::Signal<> changed;

	void NotifyChanged();
};
