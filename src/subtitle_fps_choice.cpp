#include "subtitle_fps_choice.h"

#include "compat.h"
#include "format.h"

#include <libaegisub/exception.h>

#include <wx/intl.h>

namespace {
std::vector<std::string> build_standard_fps_labels(bool show_smpte) {
	std::vector<std::string> choices;
	choices.reserve(show_smpte ? 12 : 11);
	choices.push_back(from_wx(_("15.000 FPS")));
	choices.push_back(from_wx(_("23.976 FPS (Decimated NTSC)")));
	choices.push_back(from_wx(_("24.000 FPS (FILM)")));
	choices.push_back(from_wx(_("25.000 FPS (PAL)")));
	choices.push_back(from_wx(_("29.970 FPS (NTSC)")));
	if (show_smpte)
		choices.push_back(from_wx(_("29.970 FPS (NTSC with SMPTE dropframe)")));
	choices.push_back(from_wx(_("30.000 FPS")));
	choices.push_back(from_wx(_("50.000 FPS (PAL x2)")));
	choices.push_back(from_wx(_("59.940 FPS (NTSC x2)")));
	choices.push_back(from_wx(_("60.000 FPS")));
	choices.push_back(from_wx(_("119.880 FPS (NTSC x4)")));
	choices.push_back(from_wx(_("120.000 FPS")));
	return choices;
}
}

SubtitleFpsChoiceModel BuildSubtitleFpsChoiceModel(bool allow_vfr, bool show_smpte, agi::vfr::Framerate const& fps) {
	SubtitleFpsChoiceModel model;
	model.show_smpte = show_smpte;

	if (fps.IsLoaded() && (!fps.IsVFR() || allow_vfr)) {
		model.includes_video_choice = true;
		model.choices.push_back(!fps.IsVFR()
			? from_wx(fmt_tl("From video (%g)", fps.FPS()))
			: from_wx(_("From video (VFR)")));
	}

	auto standard = build_standard_fps_labels(show_smpte);
	model.choices.insert(model.choices.end(), standard.begin(), standard.end());
	return model;
}

agi::vfr::Framerate ResolveSubtitleFpsChoiceSelection(SubtitleFpsChoiceModel const& model, int selection, agi::vfr::Framerate const& fps) {
	if (model.includes_video_choice && selection == 0)
		return fps;

	int offset = selection - (model.includes_video_choice ? 1 : 0);
	if (offset < 0)
		throw agi::InternalError("Out of bounds FPS choice index.");

	switch (offset) {
	case 0:  return agi::vfr::Framerate(15, 1);
	case 1:  return agi::vfr::Framerate(24000, 1001);
	case 2:  return agi::vfr::Framerate(24, 1);
	case 3:  return agi::vfr::Framerate(25, 1);
	case 4:  return agi::vfr::Framerate(30000, 1001);
	case 5:
		if (model.show_smpte)
			return agi::vfr::Framerate(30000, 1001, true);
		return agi::vfr::Framerate(30, 1);
	case 6:
		if (model.show_smpte)
			return agi::vfr::Framerate(30, 1);
		return agi::vfr::Framerate(50, 1);
	case 7:
		if (model.show_smpte)
			return agi::vfr::Framerate(50, 1);
		return agi::vfr::Framerate(60000, 1001);
	case 8:
		if (model.show_smpte)
			return agi::vfr::Framerate(60000, 1001);
		return agi::vfr::Framerate(60, 1);
	case 9:
		if (model.show_smpte)
			return agi::vfr::Framerate(60, 1);
		return agi::vfr::Framerate(120000, 1001);
	case 10:
		if (model.show_smpte)
			return agi::vfr::Framerate(120000, 1001);
		return agi::vfr::Framerate(120, 1);
	case 11:
		if (model.show_smpte)
			return agi::vfr::Framerate(120, 1);
		break;
	}

	throw agi::InternalError("Out of bounds FPS choice index.");
}
