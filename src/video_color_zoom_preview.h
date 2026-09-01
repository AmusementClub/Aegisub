#pragma once

#include <memory>
#include <optional>

#include <wx/gdicmn.h>

namespace agi {
struct Context;
}
// struct, matching video_frame.h's definition: MSVC mangles the class-key, so
// a mismatched forward declaration changes the symbol and fails to link.
struct VideoFrame;
class wxWindow;

/// Floating nearest-neighbour magnifier for the video colour pick.
///
/// Owns one cached raw BGRA frame and a small always-on-top, non-activating
/// window showing the (2*radius+1)^2 storage pixels around the pixel under
/// the pointer, labelled with that pixel's colour and the pick pipeline's
/// predicted result. The cache is read when the pick arms and re-read
/// whenever a frame is presented while playback is stopped, so while paused
/// the grid is always the frame a click would sample. During playback the
/// grid stays on the cached frame on purpose and marks itself frozen in the
/// badge, rather than pay a synchronous frame readback per presented frame.
///
/// The grid is drawn in storage-pixel order; videos whose display output is
/// rotated or v-flipped see the grid in storage orientation.
class VideoColorZoomPreview final {
	public:
	VideoColorZoomPreview(agi::Context *context, wxWindow *anchor);
	~VideoColorZoomPreview();

	VideoColorZoomPreview(VideoColorZoomPreview&&) = delete;
	VideoColorZoomPreview& operator=(VideoColorZoomPreview&&) = delete;

	/// Show or move the magnified window. `storage_pixel` is the raw-frame
	/// pixel under the pointer; nullopt (pointer outside the video viewport)
	/// hides the window without dropping the cache. `anchor_client_pos` is the
	/// pointer position in the anchor window's client coordinates, used to
	/// place the window beside the pointer.
	void UpdateAt(std::optional<wxPoint> storage_pixel, wxPoint anchor_client_pos);

	/// A frame was presented: re-read the cache while stopped (repainting the
	/// visible window, so keyboard frame stepping with a parked pointer also
	/// steps the grid), keep the frozen cache (the badge already names its
	/// frame) while playing.
	void OnFramePresented(int frame_n);

	private:
	class ZoomWindow;
	void RefreshCache(int frame_n);
	bool CacheUsable() const;
	void RepaintAt(wxPoint anchor_client_pos);

	agi::Context *context;
	wxWindow *anchor;
	std::unique_ptr<ZoomWindow> window;
	std::shared_ptr<VideoFrame> cache;
	int cache_frame = -1;
	/// Frame number of the last readback attempt, successful or not: a frame
	/// whose decode keeps failing must not be retried on every mouse move.
	int last_read_attempt = -1;
	std::optional<wxPoint> pixel;
	/// Anchor client position of the last UpdateAt, for repainting on frame
	/// changes while the pointer itself is parked.
	wxPoint last_anchor;
	/// Storage pixel and cache frame the on-screen content was built from:
	/// pointer motion that maps to the same pixel on the same frame only
	/// moves the window, skipping the pick prediction's region walk.
	std::optional<wxPoint> painted_pixel;
	int painted_frame = -1;
};
