#include "gui_wx_subtitle_format_host.h"

#include "dialog_export_ebu3264.h"
#include "dialogs.h"
#include "subtitle_format.h"
#include "subtitle_format_ebu3264.h"
#include "subtitle_format_txt.h"

#include <libaegisub/make_unique.h>

#include <utility>

namespace {
class GuiWxEbu3264SubtitleFormat final : public Ebu3264SubtitleFormat {
protected:
	std::optional<EbuExportSettings> GetExportSettings() const override {
		EbuExportSettings settings("Subtitle Format/EBU STL");
		auto configured = PromptForEbuExportSettings(nullptr, settings);
		if (configured)
			configured->Save();
		return configured;
	}
};

class GuiWxTXTSubtitleFormat final : public TXTSubtitleFormat {
public:
	void ReadFile(
		AssFile *target,
		agi::fs::path const& filename,
		agi::vfr::Framerate const& fps,
		std::string const& encoding,
		std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink,
		std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory) const override {
		if (!ShowPlainTextImportDialog())
			return;
		TXTSubtitleFormat::ReadFile(target, filename, fps, encoding, std::move(choice_sink), std::move(background_runner_factory));
	}
};
}

void RegisterGuiWxSubtitleFormats() {
	SubtitleFormat::RegisterFormat(agi::make_unique<GuiWxEbu3264SubtitleFormat>());
	SubtitleFormat::RegisterFormat(agi::make_unique<GuiWxTXTSubtitleFormat>());
}
