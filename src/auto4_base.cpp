// Copyright (c) 2006, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "auto4_base.h"

#include "ass_file.h"
#include "ass_style.h"
#include "automation/engine/automation_engine_registry.h"
#include "automation/automation_live_host.h"
#include "compat.h"
#include "dialog_progress.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "string_codec.h"
#include "subs_controller.h"
#include "ui_dispatch.h"
#include "ui_services.h"

#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/split.h>

#include <chrono>
#include <future>
#include <utility>

#include <wx/dcmemory.h>
#include <wx/log.h>
#include <wx/sizer.h>

#ifdef __WINDOWS__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <libaegisub/charset_conv_win.h>
#endif

namespace Automation4 {
	namespace {
		class ScriptCompatibilityWrapper final : public Script {
			std::unique_ptr<AutomationScriptInstance> impl;

		public:
			explicit ScriptCompatibilityWrapper(std::unique_ptr<AutomationScriptInstance> impl)
			: Script(impl ? impl->GetFilename() : agi::fs::path())
			, impl(std::move(impl))
			{
			}

			void Reload() override { impl->Reload(); }
			std::string GetName() const override { return impl->GetName(); }
			std::string GetDescription() const override { return impl->GetDescription(); }
			std::string GetAuthor() const override { return impl->GetAuthor(); }
			std::string GetVersion() const override { return impl->GetVersion(); }
			bool GetLoadedState() const override { return impl->GetLoadedState(); }
			std::vector<cmd::Command*> GetMacros() const override { return impl->GetMacros(); }
			std::vector<ExportFilter*> GetFilters() const override { return impl->GetFilters(); }
			std::string GetEngineName() const override { return impl->GetEngineName(); }
			std::optional<AutomationRuntimeStateSnapshot> TryGetRuntimeStateSnapshot() const override { return impl->TryGetRuntimeStateSnapshot(); }
			void SetRuntimeTraceSink(AutomationRuntimeTraceSink *sink) override { impl->SetRuntimeTraceSink(sink); }
			void SetAutomationHost(std::shared_ptr<AutomationHost> host) override { impl->SetAutomationHost(std::move(host)); }
			void SetDebugSession(AutomationDebugSession *session) override { impl->SetDebugSession(session); }
		};

		std::unique_ptr<Script> WrapScriptInstance(std::unique_ptr<AutomationScriptInstance> instance)
		{
			if (!instance)
				return nullptr;

			if (auto *script = dynamic_cast<Script *>(instance.get())) {
				instance.release();
				return std::unique_ptr<Script>(script);
			}

			return agi::make_unique<ScriptCompatibilityWrapper>(std::move(instance));
		}

		void AddFilterEntry(
			std::vector<std::pair<std::string, std::string>>& entries,
			std::string engine_name,
			std::string filename_pattern)
		{
			if (engine_name.empty() || filename_pattern.empty())
				return;

			auto const entry = std::make_pair(std::move(engine_name), std::move(filename_pattern));
			if (find(entries.begin(), entries.end(), entry) == entries.end())
				entries.emplace_back(entry);
		}

		struct ScriptLoadAttempt {
			std::unique_ptr<Script> script;
			bool recognised = false;
		};

		void ReportFailedAutomationScriptLoad(agi::fs::path const& filename, std::string const& description)
		{
			wxLogError(_("Failed to load Automation script '%s':\n%s"), filename.wstring(), to_wx(description));
		}

		void ReportUnrecognisedAutomationScript(agi::fs::path const& filename)
		{
			wxLogError(_("The file was not recognised as an Automation script: %s"), filename.wstring());
		}
	}

	bool CalculateTextExtents(AssStyle *style, std::string const& text, double &width, double &height, double &descent, double &extlead)
	{
		width = height = descent = extlead = 0;

		double fontsize = style->fontsize * 64;
		double spacing = style->spacing * 64;

#ifdef WIN32
		// This is almost copypasta from TextSub
		auto dc = CreateCompatibleDC(nullptr);
		if (!dc) return false;

		SetMapMode(dc, MM_TEXT);

		LOGFONTW lf = {0};
		lf.lfHeight = (LONG)fontsize;
		lf.lfWeight = style->bold ? FW_BOLD : FW_NORMAL;
		lf.lfItalic = style->italic;
		lf.lfUnderline = style->underline;
		lf.lfStrikeOut = style->strikeout;
		lf.lfCharSet = style->encoding;
		lf.lfOutPrecision = OUT_TT_PRECIS;
		lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
		lf.lfQuality = ANTIALIASED_QUALITY;
		lf.lfPitchAndFamily = DEFAULT_PITCH|FF_DONTCARE;
		wcsncpy(lf.lfFaceName, agi::charset::ConvertW(style->font).c_str(), 31);

		auto font = CreateFontIndirect(&lf);
		if (!font) return false;

		auto old_font = SelectObject(dc, font);

		std::wstring wtext(agi::charset::ConvertW(text));
		if (spacing != 0 ) {
			width = 0;
			for (auto c : wtext) {
				SIZE sz;
				GetTextExtentPoint32(dc, &c, 1, &sz);
				width += sz.cx + spacing;
				height = sz.cy;
			}
		}
		else {
			SIZE sz;
			GetTextExtentPoint32(dc, &wtext[0], (int)wtext.size(), &sz);
			width = sz.cx;
			height = sz.cy;
		}

		TEXTMETRIC tm;
		GetTextMetrics(dc, &tm);
		descent = tm.tmDescent;
		extlead = tm.tmExternalLeading;

		SelectObject(dc, old_font);
		DeleteObject(font);
		DeleteObject(dc);

#else // not WIN32
		wxMemoryDC thedc;

		// fix fontsize to be 72 DPI
		//fontsize = -FT_MulDiv((int)(fontsize+0.5), 72, thedc.GetPPI().y);

		// now try to get a font!
		// use the font list to get some caching... (chance is the script will need the same font very often)
		// USING wxTheFontList SEEMS TO CAUSE BAD LEAKS!
		//wxFont *thefont = wxTheFontList->FindOrCreateFont(
		wxFont thefont(
			(int)fontsize,
			wxFONTFAMILY_DEFAULT,
			style->italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
			style->bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL,
			style->underline,
			to_wx(style->font),
			wxFONTENCODING_SYSTEM); // FIXME! make sure to get the right encoding here, make some translation table between windows and wx encodings
		thedc.SetFont(thefont);

		wxString wtext(to_wx(text));
		if (spacing) {
			// If there's inter-character spacing, kerning info must not be used, so calculate width per character
			// NOTE: Is kerning actually done either way?!
			for (auto const& wc : wtext) {
				int a, b, c, d;
				thedc.GetTextExtent(wc, &a, &b, &c, &d);
				double scaling = fontsize / (double)(b > 0 ? b : 1); // semi-workaround for missing OS/2 table data for scaling
				width += (a + spacing)*scaling;
				height = b > height ? b*scaling : height;
				descent = c > descent ? c*scaling : descent;
				extlead = d > extlead ? d*scaling : extlead;
			}
		} else {
			// If the inter-character spacing should be zero, kerning info can (and must) be used, so calculate everything in one go
			wxCoord lwidth, lheight, ldescent, lextlead;
			thedc.GetTextExtent(wtext, &lwidth, &lheight, &ldescent, &lextlead);
			double scaling = fontsize / (double)(lheight > 0 ? lheight : 1); // semi-workaround for missing OS/2 table data for scaling
			width = lwidth*scaling; height = lheight*scaling; descent = ldescent*scaling; extlead = lextlead*scaling;
		}
#endif

		// Compensate for scaling
		width = style->scalex / 100 * width / 64;
		height = style->scaley / 100 * height / 64;
		descent = style->scaley / 100 * descent / 64;
		extlead = style->scaley / 100 * extlead / 64;

		return true;
	}

	ExportFilter::ExportFilter(std::string const& name, std::string const& description, int priority)
	: AssExportFilter(name, description, priority)
	{
	}

	std::string ExportFilter::GetScriptSettingsIdentifier()
	{
		return inline_string_encode(GetName());
	}

	wxWindow* ExportFilter::GetConfigDialogWindow(wxWindow *parent, agi::Context *c) {
		automation_host = c ? CreateAutomationLiveHost(c) : nullptr;
		config_dialog = GenerateConfigDialog(parent, c);

		if (config_dialog) {
			auto core = c->GetCore();
			std::string const& val = core.ass->Properties.automation_settings[GetScriptSettingsIdentifier()];
			if (!val.empty())
				config_dialog->Unserialise(val);
			return config_dialog->CreateWindow(parent);
		}

		return nullptr;
	}

	void ExportFilter::LoadSettings(bool is_default, agi::Context *c) {
		automation_host = c ? CreateAutomationLiveHost(c) : nullptr;
		if (config_dialog)
			c->GetCore().ass->Properties.automation_settings[GetScriptSettingsIdentifier()] = config_dialog->Serialise();
	}

	// ProgressSink
	ProgressSink::ProgressSink(agi::ProgressSink *impl, BackgroundScriptRunner *bsr)
	: impl(impl)
	, bsr(bsr)
	, trace_level(OPT_GET("Automation/Trace Level")->GetInt())
	{
	}

	void ProgressSink::ShowDialog(ScriptDialog *config_dialog)
	{
		agi::ui::MainInvoke([=] {
			auto duration_ms = [](auto const& started) {
				return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			};

			wxDialog w; // container dialog box
			w.SetExtraStyle(wxWS_EX_VALIDATE_RECURSIVELY);
			w.Create(bsr->GetParentWindow(), -1, to_wx(bsr->GetTitle()));
			auto s = new wxBoxSizer(wxHORIZONTAL); // sizer for putting contents in
			wxWindow *ww = config_dialog->CreateWindow(&w); // generate actual dialog contents
			s->Add(ww, 0, wxALL, 5); // add contents to dialog

			auto const outer_fit_started = std::chrono::steady_clock::now();
			w.SetSizerAndFit(s);
			perf_trace::ObserveLuaDialogPhase("outer_dialog_fit", -1, -1, duration_ms(outer_fit_started));

			auto const center_started = std::chrono::steady_clock::now();
			w.CenterOnParent();
			perf_trace::ObserveLuaDialogPhase("outer_dialog_center", -1, -1, duration_ms(center_started));

			w.ShowModal();
		});
	}

	int ProgressSink::ShowDialog(wxDialog *dialog)
	{
		return agi::ui::MainInvoke([dialog] { return dialog->ShowModal(); });
	}

	std::vector<agi::fs::path> ProgressSink::RequestOpenFiles(AutomationOpenFileDialogRequest const& request)
	{
		return bsr->RequestOpenFiles(request);
	}

	agi::fs::path ProgressSink::RequestSaveFile(AutomationSaveFileDialogRequest const& request)
	{
		return bsr->RequestSaveFile(request);
	}

	BackgroundScriptRunner::BackgroundScriptRunner(wxWindow *parent, std::string const& title, std::shared_ptr<agi::FileDialogService> file_dialog_service)
	: impl(std::make_unique<DialogProgress>(parent, to_wx(title)))
	, parent(parent)
	, title(title)
	, file_dialog_service(std::move(file_dialog_service))
	{
	}

	BackgroundScriptRunner::BackgroundScriptRunner(
		std::unique_ptr<agi::BackgroundRunner> impl,
		wxWindow *parent,
		std::string title,
		std::shared_ptr<agi::FileDialogService> file_dialog_service)
	: impl(std::move(impl))
	, parent(parent)
	, title(std::move(title))
	, file_dialog_service(std::move(file_dialog_service))
	{
	}

	BackgroundScriptRunner::~BackgroundScriptRunner()
	{
	}

	void BackgroundScriptRunner::Run(std::function<void (ProgressSink*)> task)
	{
		impl->Run([&](agi::ProgressSink *ps) {
			ProgressSink aps(ps, this);
			task(&aps);
		});
	}

	wxWindow *BackgroundScriptRunner::GetParentWindow() const
	{
		if (auto* window = dynamic_cast<wxWindow *>(impl.get()))
			return window;
		return parent;
	}

	std::string BackgroundScriptRunner::GetTitle() const
	{
		if (auto* dialog = dynamic_cast<wxDialog *>(impl.get()))
			return from_wx(dialog->GetTitle());
		return title;
	}

	std::vector<agi::fs::path> BackgroundScriptRunner::RequestOpenFiles(AutomationOpenFileDialogRequest const& request) const
	{
		if (!file_dialog_service)
			return {};

		if (request.multiple) {
			return file_dialog_service->RequestOpenFiles({
				request.message,
				"",
				request.file,
				"",
				request.wildcard,
				request.dir,
				request.must_exist
			});
		}

		auto path = file_dialog_service->RequestOpenFile({
			request.message,
			"",
			request.file,
			"",
			request.wildcard,
			request.dir,
			request.must_exist
		});
		if (path.empty())
			return {};
		return {std::move(path)};
	}

	agi::fs::path BackgroundScriptRunner::RequestSaveFile(AutomationSaveFileDialogRequest const& request) const
	{
		if (!file_dialog_service)
			return {};

		return file_dialog_service->RequestSaveFile({
			request.message,
			"",
			request.file,
			"",
			request.wildcard,
			request.dir,
			request.prompt_overwrite
		});
	}

	// Script
	Script::Script(agi::fs::path const& filename)
	: filename(filename)
	{
		include_path.emplace_back(filename.parent_path());

		for (auto probe = filename.parent_path(); !probe.empty();) {
			auto candidate = probe / "include";
			if (agi::fs::DirectoryExists(candidate)) {
				include_path.emplace_back(std::move(candidate));
				break;
			}
			auto parent = probe.parent_path();
			if (parent == probe)
				break;
			probe = std::move(parent);
		}

		std::string include_paths = OPT_GET("Path/Automation/Include")->GetString();
		for (auto tok : agi::Split(include_paths, '|')) {
			auto path = config::path->Decode(agi::str(tok));
			if (path.is_absolute() && agi::fs::DirectoryExists(path))
				include_path.emplace_back(std::move(path));
		}
	}

	// ScriptManager
	void ScriptManager::Add(std::unique_ptr<Script> script)
	{
		if (!script)
			return;

		if (find(scripts.begin(), scripts.end(), script) == scripts.end())
			scripts.emplace_back(std::move(script));

		ScriptsChanged();
	}

	void ScriptManager::Remove(Script *script)
	{
		auto i = find_if(scripts.begin(), scripts.end(), [&](std::unique_ptr<Script> const& s) { return s.get() == script; });
		if (i != scripts.end())
			scripts.erase(i);

		ScriptsChanged();
	}

	void ScriptManager::RemoveAll()
	{
		scripts.clear();
		ScriptsChanged();
	}

	void ScriptManager::Reload(Script *script)
	{
		script->Reload();
		ScriptsChanged();
	}

	const std::vector<cmd::Command*>& ScriptManager::GetMacros()
	{
		macros.clear();
		for (auto& script : scripts) {
			std::vector<cmd::Command*> sfs = script->GetMacros();
			copy(sfs.begin(), sfs.end(), back_inserter(macros));
		}
		return macros;
	}

	// AutoloadScriptManager
	AutoloadScriptManager::AutoloadScriptManager(std::string path)
	: path(std::move(path))
	{
		Reload();
	}

	void AutoloadScriptManager::Reload()
	{
		scripts.clear();

		std::vector<std::future<ScriptLoadAttempt>> script_futures;

		for (auto tok : agi::Split(path, '|')) {
			auto dirname = config::path->Decode(agi::str(tok));
			if (!agi::fs::DirectoryExists(dirname)) continue;

			for (auto filename : agi::fs::DirectoryIterator(dirname, "*.*"))
				script_futures.emplace_back(std::async(std::launch::async, [=] {
					ScriptLoadAttempt attempt;
					attempt.script = ScriptFactory::CreateFromFile(
						dirname / agi::fs::PathFromString(filename),
						false,
						&attempt.recognised);
					return attempt;
				}));
		}

		int error_count = 0;
		for (auto& future : script_futures) {
			auto attempt = future.get();
			if (!attempt.script)
				continue;

			if (!attempt.script->GetLoadedState()) {
				++error_count;
				ReportFailedAutomationScriptLoad(attempt.script->GetFilename(), attempt.script->GetDescription());
			}
			scripts.emplace_back(std::move(attempt.script));
		}

		if (error_count == 1) {
			wxLogWarning(wxS("A script in the Automation autoload directory failed to load.\nPlease review the errors, fix them and use the Rescan Autoload Dir button in Automation Manager to load the scripts again."));
		}
		else if (error_count > 1) {
			wxLogWarning(wxS("Multiple scripts in the Automation autoload directory failed to load.\nPlease review the errors, fix them and use the Rescan Autoload Dir button in Automation Manager to load the scripts again."));
		}

		ScriptsChanged();
	}

	LocalScriptManager::LocalScriptManager(agi::Context *c)
	: context(c)
	, file_open_connection(c->GetCore().subsController->AddFileOpenListener(&LocalScriptManager::Reload, this))
	{
		AddScriptChangeListener(&LocalScriptManager::SaveLoadedList, this);
	}

	void LocalScriptManager::Reload()
	{
		bool was_empty = scripts.empty();
		scripts.clear();

		auto core = context->GetCore();
		auto const& local_scripts = core.ass->Properties.automation_scripts;
		if (local_scripts.empty()) {
			if (!was_empty)
				ScriptsChanged();
			return;
		}

		auto autobasefn(OPT_GET("Path/Automation/Base")->GetString());

		for (auto tok : agi::Split(local_scripts, '|')) {
			auto token = agi::util::strings::trim_copy(agi::str(tok));
			if (token.empty()) continue;
			char first_char = token[0];
			std::string trimmed(token.begin() + 1, token.end());

			agi::fs::path basepath;
			if (first_char == '~') {
				basepath = core.subsController->Filename().parent_path();
			} else if (first_char == '$') {
				basepath = autobasefn;
			} else if (first_char == '/') {
			} else {
				wxLogWarning(wxS("Automation Script referenced with unknown location specifier character.\nLocation specifier found: %c\nFilename specified: %s"),
					first_char, to_wx(trimmed));
				continue;
			}
			auto sfname = basepath / agi::fs::PathFromString(trimmed);
			if (agi::fs::FileExists(sfname)) {
				bool recognised = false;
				auto script = Automation4::ScriptFactory::CreateFromFile(sfname, true, &recognised);
				if (!recognised)
					ReportUnrecognisedAutomationScript(sfname);
				else if (script && !script->GetLoadedState())
					ReportFailedAutomationScriptLoad(sfname, script->GetDescription());
				scripts.emplace_back(std::move(script));
			}
			else {
				wxLogWarning(wxS("Automation Script referenced could not be found.\nFilename specified: %c%s\nSearched relative to: %s\nResolved filename: %s"),
					first_char, to_wx(trimmed), basepath.wstring(), sfname.wstring());
			}
		}

		ScriptsChanged();
	}

	void LocalScriptManager::SaveLoadedList()
	{
		// Store Automation script data
		// Algorithm:
		// 1. If script filename has Automation Base Path as a prefix, the path is relative to that (ie. "$")
		// 2. Otherwise try making it relative to the ass filename
		// 3. If step 2 failed, or absolute path is shorter than path relative to ass, use absolute path ("/")
		// 4. Otherwise, use path relative to ass ("~")
		std::string scripts_string;
		agi::fs::path autobasefn(OPT_GET("Path/Automation/Base")->GetString());
		auto core = context->GetCore();

		for (auto& script : GetScripts()) {
			if (!scripts_string.empty())
				scripts_string += "|";

			auto const scriptfn = script->GetFilename();
			auto const scriptfn_str = agi::fs::PathToString(scriptfn);
			auto const autobase_rel = core.path->MakeRelative(scriptfn, autobasefn);
			auto const assfile_rel = core.path->MakeRelative(scriptfn, "?script");
			auto const autobase_rel_str = agi::fs::PathToGenericString(autobase_rel);
			auto const assfile_rel_str = agi::fs::PathToGenericString(assfile_rel);

			if (autobase_rel_str.size() <= scriptfn_str.size() && autobase_rel_str.size() <= assfile_rel_str.size()) {
				scripts_string += "$" + autobase_rel_str;
			} else if (assfile_rel_str.size() <= scriptfn_str.size() && assfile_rel_str.size() <= autobase_rel_str.size()) {
				scripts_string += "~" + assfile_rel_str;
			} else {
				scripts_string += "/" + agi::fs::PathToGenericString(scriptfn);
			}
		}
		core.ass->Properties.automation_scripts = std::move(scripts_string);
	}

	std::unique_ptr<Script> ScriptFactory::CreateFromFile(
		agi::fs::path const& filename,
		bool create_unknown,
		bool *recognised_out)
	{
		if (recognised_out)
			*recognised_out = false;

		if (auto script = WrapScriptInstance(AutomationEngineRegistry::CreateFromFile(filename))) {
			if (recognised_out)
				*recognised_out = true;
			return script;
		}

		return create_unknown ? agi::make_unique<UnknownScript>(filename) : nullptr;
	}

	std::string ScriptFactory::GetWildcardStr()
	{
		std::string fnfilter, catchall;
		std::vector<std::pair<std::string, std::string>> entries;
		for (auto const& engine : AutomationEngineRegistry::GetEngines()) {
			if (!engine)
				continue;
			AddFilterEntry(entries, engine->EngineName(), engine->FilenamePattern());
		}

		for (auto const& entry : entries) {
			std::string filter(entry.second);
			agi::util::strings::replace_all_inplace(filter, ",", ";");
			fnfilter += agi::format("%s scripts (%s)|%s|", entry.first, entry.second, filter);
			catchall += filter + ";";
		}
		fnfilter += from_wx(_("All Files")) + " (*.*)|*.*";

		if (!catchall.empty())
			catchall.pop_back();

		if (entries.size() > 1)
			fnfilter = from_wx(_("All Supported Formats")) + "|" + catchall + "|" + fnfilter;

		return fnfilter;
	}

	std::string UnknownScript::GetDescription() const {
		return from_wx(_("File was not recognized as a script"));
	}
}
