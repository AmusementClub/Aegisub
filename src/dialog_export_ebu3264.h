// Copyright (c) 2011 Niels Martin Hansen <nielsm@aegisub.org>
// Copyright (c) 2012 Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file dialog_export_ebu3264.h
/// @see dialog_export_ebu3264.cpp
/// @ingroup subtitle_io export

#include "ebu_export_settings.h"

#include <optional>

class wxWindow;

/// Show a dialog box for getting an export configuration for EBU Tech 3264-1991
/// @param owner Parent window of the dialog
/// @param s Struct with initial values and to fill with the chosen settings
int ShowEbuExportConfigurationDialog(wxWindow *owner, EbuExportSettings &s);
std::optional<EbuExportSettings> PromptForEbuExportSettings(wxWindow *owner, EbuExportSettings settings);
