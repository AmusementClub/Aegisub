// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
// Copyright (c) 2026, MIR
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "ass_file_app.h"

#include "ass_dialogue.h"
#include "ass_style.h"
#include "ass_style_storage.h"
#include "options.h"

void LoadDefaultAssFileWithAppOptions(AssFile& file, bool include_dialogue_line, std::string const& style_catalog) {
	AssFileLoadDefaultOptions options;
	options.include_dialogue_line = false;
	if (!OPT_GET("Subtitle/Default Resolution/Auto")->GetBool()) {
		options.set_resolution = true;
		options.resolution_width = OPT_GET("Subtitle/Default Resolution/Width")->GetInt();
		options.resolution_height = OPT_GET("Subtitle/Default Resolution/Height")->GetInt();
	}

	file.LoadDefault(options);

	if (AssStyleStorage::CatalogExists(style_catalog)) {
		AssStyleStorage catalog;
		catalog.LoadCatalog(style_catalog);
		catalog.ReplaceIntoFile(file);
	}

	if (include_dialogue_line)
		file.Events.push_back(*new AssDialogue);
}

ScriptResolutionType GetAppScriptResolutionPreference() {
	// libass always uses PlayRes for \pos, \move, \fs, margins, and all coordinate mapping.
	// LayoutRes only affects \blur, \frx/\fry perspective, and border/shadow when SBAS=no.
	// Aegisub's internal coordinate system (visual tools, Lua API) MUST use PlayRes
	// to match what libass actually renders. See libass ass_render.c: init_font_scale(),
	// x2scr_pos(), y2scr_pos() — all use track->PlayResX/Y, never LayoutRes.
	return ScriptResolutionType::PlayRes;
}
