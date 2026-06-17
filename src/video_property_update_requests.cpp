#include "video_property_update_requests.h"

#include "translation_service.h"

#include <libaegisub/format.h>

#include <algorithm>

agi::SingleChoiceInteractionRequest BuildVideoResolutionMismatchRequest(VideoPropertyUpdateInput const& input,
                                                                        bool aspect_ratio_changed,
                                                                        int last_choice) {
	agi::SingleChoiceInteractionRequest request;
	request.title = _("Resolution mismatch");
	request.message = agi::format(_("The resolution of the loaded video and the resolution specified for the subtitles don't match.\n\nVideo resolution:\t%d x %d\nScript resolution:\t%d x %d\n\nChange subtitles resolution to match video?").c_str(),
		input.video_width, input.video_height, input.script_width, input.script_height);
	request.help_page = "Resolution mismatch";
	request.choices.push_back(_("Set to video resolution"));
	request.choices.push_back(aspect_ratio_changed
		? _("Resample script (stretch to new aspect ratio)")
		: _("Resample script"));
	if (aspect_ratio_changed) {
		request.choices.push_back(_("Resample script (add borders)"));
		request.choices.push_back(_("Resample script (remove borders)"));
	}

	request.default_choice = std::clamp(last_choice - 1, 0, static_cast<int>(request.choices.size() - 1));
	return request;
}

agi::SingleChoiceInteractionRequest BuildLayoutResRequest(VideoPropertyUpdateInput const& input) {
	agi::SingleChoiceInteractionRequest request;
	request.title = _("LayoutRes not set");
	request.message = agi::format(_("The script does not have LayoutRes headers set. LayoutRes tells the renderer which video resolution the subtitles were originally authored for, ensuring \\blur, \\frx/\\fry perspective, and borders (when ScaledBorderAndShadow=no) scale correctly.\n\nSet LayoutRes to the current video resolution?\nVideo resolution:\t%d x %d").c_str(),
		input.video_width, input.video_height);
	request.help_page = "Properties";
	request.choices.push_back(_("Set to video resolution"));
	request.choices.push_back(_("Not now"));
	request.default_choice = 0;
	return request;
}
