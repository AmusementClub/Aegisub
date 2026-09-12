#include "video_color_zoom_preview.h"

#include "ass_compat.h"
#include "async_video_provider.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "video_color_pick.h"
#include "video_color_pick_preview.h"
#include "video_controller.h"
#include "video_frame.h"

#include <libaegisub/color.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/frame.h>

namespace {

/// Source pixels from the centre in each direction, and the on-screen size of
/// one source pixel. A 17x17 grid at 8 px is a ~136 px window: readable at any
/// video zoom, small enough to sit beside the pointer.
constexpr int kRadius = 8;
constexpr int kCell = 8;

/// How far from the pointer the window floats.
constexpr int kOffset = 24;

} // namespace

/// The magnifier window itself: always on top, never activated (the canvas
/// keeps the keyboard so Escape still cancels the pick, and the sampling
/// click always lands on the canvas).
class VideoColorZoomPreview::ZoomWindow final : public wxFrame {
	public:
	explicit ZoomWindow(wxWindow *parent) {
		Create(parent, wxID_ANY, wxString(), wxDefaultPosition, wxDefaultSize,
			   wxFRAME_TOOL_WINDOW | wxFRAME_NO_TASKBAR | wxSTAY_ON_TOP | wxBORDER_NONE);
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		SetFont(wxFont(wxFontInfo(GetFont().Smaller().GetFractionalPointSize())
						   .Family(wxFONTFAMILY_TELETYPE)));
		UpdateMetrics();
		Bind(wxEVT_PAINT, &ZoomWindow::OnPaint, this);
		Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
			UpdateMetrics();
			event.Skip();
		});
	}

	/// Never take the keyboard: the canvas owns it while the pick is armed
	/// (Escape cancels) and the sampling click always lands on the canvas.
	bool AcceptsFocus() const override { return false; }
	bool AcceptsFocusFromKeyboard() const override { return false; }

	void SetContent(std::vector<agi::Color> grid, std::optional<agi::Color> picked, bool frozen) {
		content = std::move(grid);
		this->picked = picked;
		this->frozen = frozen;
		RebuildLabel();
		Refresh(false);
	}

	void SetFrozen(bool frozen) {
		if (this->frozen == frozen)
			return;
		this->frozen = frozen;
		RebuildLabel();
		Refresh(false);
	}

	void PlaceNear(wxPoint screen_pos) {
		wxPoint pos = screen_pos + wxPoint(kOffset, kOffset);
		wxSize const size = GetSize();
		wxRect const area = wxDisplay(wxDisplay::GetFromPoint(screen_pos)).GetClientArea();

		if (pos.x + size.x > area.GetRight())
			pos.x = screen_pos.x - kOffset - size.x;
		if (pos.y + size.y > area.GetBottom())
			pos.y = screen_pos.y - kOffset - size.y;
		pos.x = std::clamp<int>(pos.x, area.GetLeft(), std::max(area.GetLeft(), area.GetRight() - size.x));
		pos.y = std::clamp<int>(pos.y, area.GetTop(), std::max(area.GetTop(), area.GetBottom() - size.y));

		if (GetPosition() != pos)
			Move(pos);
	}

	private:
	void OnPaint(wxPaintEvent&) {
		wxBufferedPaintDC dc(this);
		int const extent = 2 * kRadius + 1;
		int const grid_height = extent * kCell;
		wxRect const client(wxPoint(0, 0), GetClientSize());

		// Dark ground under everything, chiefly the badge strip, so white
		// label text is readable over any video content.
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(*wxBLACK_BRUSH);
		dc.DrawRectangle(client);

		if (!content.empty()) {
			// Columns stretch to the full client width so a label-widened
			// window shows colour everywhere instead of a dead black strip
			// beside the grid; rows stay at the fixed cell size.
			double const cell_w = static_cast<double>(client.GetWidth()) / extent;
			auto const column = [cell_w](int index) {
				return static_cast<int>(index * cell_w + 0.5);
			};
			for (int y = 0; y < extent; ++y) {
				int const top = y * kCell;
				for (int x = 0; x < extent; ++x) {
					agi::Color const& color = content[static_cast<size_t>(y) * extent + x];
					dc.SetBrush(wxBrush(wxColour(color.r, color.g, color.b)));
					dc.DrawRectangle(column(x), top, column(x + 1) - column(x), kCell);
				}
			}
			// Ring the centre cell: white outside, black inside, so it reads
			// on any content.
			dc.SetBrush(*wxTRANSPARENT_BRUSH);
			int const centre_left = column(kRadius);
			int const centre_width = column(kRadius + 1) - centre_left;
			int const centre_top = kRadius * kCell;
			dc.SetPen(*wxWHITE_PEN);
			dc.DrawRectangle(centre_left - 2, centre_top - 2, centre_width + 4, kCell + 4);
			dc.SetPen(*wxBLACK_PEN);
			dc.DrawRectangle(centre_left - 1, centre_top - 1, centre_width + 2, kCell + 2);
		}

		// The badge line carries both values, built in RebuildLabel: the raw
		// centre pixel and the colour a click would actually write (the pick
		// pipeline's region median, the same call the click itself makes), so
		// the averaging is visible as the difference between them. While
		// playing the grid is a frozen frame, and the label says so.
		dc.SetFont(GetFont());
		dc.SetTextForeground(*wxWHITE);
		if (!label.empty())
			dc.DrawText(label, 2, grid_height + 2);
	}

	void RebuildLabel() {
		int const extent = 2 * kRadius + 1;
		if (content.empty()) {
			label.clear();
			return;
		}
		agi::Color const& centre_colour =
			content[static_cast<size_t>(kRadius) * extent + kRadius];
		label = wxString::FromUTF8(AssCompat::FormatOverrideColor(centre_colour));
		if (picked)
			label += wxString::FromUTF8(" -> ") +
					 wxString::FromUTF8(AssCompat::FormatOverrideColor(*picked));
		if (frozen)
			label += wxString::FromUTF8(" (frozen)");
	}

	void UpdateMetrics() {
		// Reserve both colours and the playback badge in a monospace font.
		// Colour changes must not resize a moving native window.
		int const text_width = GetTextExtent(wxString::FromUTF8("&H000000& -> &H000000& (frozen)")).x;
		int const grid_px = (2 * kRadius + 1) * kCell;
		int const width = std::max(grid_px, text_width + 4);
		int const height = grid_px + GetCharHeight() + 4;
		SetClientSize(width, height);
	}

	std::vector<agi::Color> content;
	/// Pick-pipeline colour of the centre pixel (what a click writes);
	/// nullopt leaves the badge on the raw centre pixel.
	std::optional<agi::Color> picked;
	bool frozen = false;
	wxString label;
};

VideoColorZoomPreview::VideoColorZoomPreview(agi::Context *context, wxWindow *anchor)
	: context(context), anchor(anchor) {
	preview = std::make_unique<aegisub::color_pick::Preview>(
		agi::dispatch::BackgroundExecutor(), agi::dispatch::MainExecutor(),
		[this](aegisub::color_pick::Snapshot snapshot) {
			int const presented = this->context->videoController->GetPresentedFrameN();
			bool const frozen = this->context->videoController->IsPlaying() &&
								presented >= 0 && presented != cache_frame;
			window->SetContent(std::move(snapshot.grid),
							   snapshot.pick.pixels ? std::optional<agi::Color>(snapshot.pick.color) : std::nullopt,
							   frozen);
			window->PlaceNear(this->anchor->ClientToScreen(last_anchor));
			if (!window->IsShown())
				window->ShowWithoutActivating();
		},
		kRadius);
	int frame = context->videoController->GetPresentedFrameN();
	if (frame < 0)
		frame = context->videoController->GetFrameN();
	RefreshCache(frame);
}

VideoColorZoomPreview::~VideoColorZoomPreview() = default;

void VideoColorZoomPreview::UpdateAt(std::optional<wxPoint> storage_pixel, wxPoint anchor_client_pos) {
	pixel = storage_pixel;
	if (!storage_pixel) {
		preview->Clear();
		if (window)
			window->Hide();
		return;
	}

	if (!window)
		window = std::make_unique<ZoomWindow>(anchor);
	if (!CacheUsable()) {
		// The constructor read failed (no provider output yet); retry once per
		// frame number before deciding the magnifier cannot show anything.
		int const presented = context->videoController->GetPresentedFrameN();
		if (presented != last_read_attempt)
			RefreshCache(presented);
	}
	if (!CacheUsable()) {
		window->Hide();
		return;
	}

	last_anchor = anchor_client_pos;
	int const presented = context->videoController->GetPresentedFrameN();
	window->SetFrozen(context->videoController->IsPlaying() &&
					  presented >= 0 && presented != cache_frame);
	window->PlaceNear(anchor->ClientToScreen(anchor_client_pos));
	// Movement never waits for the region walk. The preview publishes complete
	// grid/colour snapshots while coalescing requests to the latest position.
	preview->Request(cache, pixel->x, pixel->y);
}

void VideoColorZoomPreview::OnFramePresented(int frame_n) {
	if (window)
		window->SetFrozen(context->videoController->IsPlaying() &&
						  frame_n >= 0 && frame_n != cache_frame);
	if (context->videoController->IsPlaying())
		return;
	int const previous = cache_frame;
	RefreshCache(frame_n);
	// A seek or step with the pointer parked (keyboard frame stepping during
	// an armed pick) changed the frame under a still magnifier: repaint it,
	// or the grid would keep showing the pre-step frame while a click samples
	// the new one.
	if (cache_frame != previous && window && pixel)
		preview->Request(cache, pixel->x, pixel->y);
}

void VideoColorZoomPreview::RefreshCache(int frame_n) {
	auto *provider = context ? context->project->VideoProvider() : nullptr;
	if (!provider || frame_n < 0)
		return;
	last_read_attempt = frame_n;
	try {
		auto frame = provider->GetFrameBgra(
			frame_n, context->videoController->TimeAtFrame(frame_n), /*raw=*/true);
		if (frame && !frame->data.empty()) {
			cache = std::move(frame);
			cache_frame = frame_n;
		}
	}
	catch (agi::Exception const&) {
		// Decode failure leaves the previous cache in place; the pick itself
		// reports its own error when clicked.
	}
}

bool VideoColorZoomPreview::CacheUsable() const {
	return cache && !cache->data.empty();
}
