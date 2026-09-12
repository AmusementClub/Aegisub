#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_style.h"
#include "../../src/perspective_apply_plan.h"
#include "../../src/perspective_ass_state.h"
#include "../../src/perspective_tag_solver.h"
#include "../../vendor/csri/include/csri/csri.h"

#include <libaegisub/fs.h>
#include <libaegisub/native_library.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include "../../src/gdi_font_resolver.h"
#include <libaegisub/charset_conv_win.h>
#include <windows.h>

namespace {
using perspective::Quad;
using perspective::Vec2;

constexpr int Width = 640;
constexpr int Height = 360;
constexpr wchar_t WorkerArgument[] = L"--perspective-vsfilter-worker";

std::wstring Environment(wchar_t const *name) {
	auto const length = GetEnvironmentVariableW(name, nullptr, 0);
	if (!length)
		return {};
	std::wstring result(length, L'\0');
	result.resize(GetEnvironmentVariableW(name, result.data(), length));
	return result;
}

std::filesystem::path Executable() {
	std::wstring result(32768, L'\0');
	auto const length = GetModuleFileNameW(nullptr, result.data(),
										   static_cast<DWORD>(result.size()));
	if (!length || length == result.size())
		throw std::runtime_error("cannot resolve the renderer test executable");
	result.resize(length);
	return result;
}

std::filesystem::path RendererPath() {
	if (auto configured = Environment(L"AEGISUB_XY_VSF_DLL"); !configured.empty())
		return configured;
	for (auto const *name : {L"xy-VSFilter.dll", L"VSFilter.dll"}) {
		auto path = Executable().parent_path() / L"csri" / name;
		if (std::filesystem::is_regular_file(path))
			return path;
	}
	return {};
}

std::string FileVersion(std::filesystem::path const& path) {
	auto const size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
	if (!size)
		throw std::runtime_error("xy-VSFilter DLL has no file version");
	std::vector<unsigned char> data(size);
	VS_FIXEDFILEINFO *version = nullptr;
	UINT length = 0;
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&version), &length) || length < sizeof(*version) || version->dwSignature != 0xfeef04bd)
		throw std::runtime_error("xy-VSFilter DLL version is invalid");
	return std::to_string(HIWORD(version->dwFileVersionMS)) + "." + std::to_string(LOWORD(version->dwFileVersionMS)) + "." + std::to_string(HIWORD(version->dwFileVersionLS)) + "." + std::to_string(LOWORD(version->dwFileVersionLS));
}

struct Mask {
	std::vector<unsigned char> pixels;

	[[nodiscard]] unsigned char At(int x, int y) const {
		if (x < 0 || x >= Width || y < 0 || y >= Height)
			return 0;
		return pixels[static_cast<std::size_t>(y) * Width + x];
	}
};

struct RenderSettings {
	int play_width = Width;
	int play_height = Height;
	int scale_x = 100;
	int scale_y = 100;
	std::string font = "Arial";
	double spacing = 0.0;
	int angle = 0;
};

std::string StyleLine(RenderSettings const& settings) {
	return "Style: Default," + settings.font + ",40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
											   "0,0,0,0," +
		   std::to_string(settings.scale_x) + "," + std::to_string(settings.scale_y) + "," + std::to_string(settings.spacing) + "," + std::to_string(settings.angle) + ",1,0,0,7,0,0,0,1";
}

std::string MakeScript(std::string const& event, RenderSettings const& settings) {
	return "[Script Info]\nScriptType: v4.00+\nPlayResX: " + std::to_string(settings.play_width) + "\nPlayResY: " + std::to_string(settings.play_height) + "\n"
																																						   "WrapStyle: 2\nScaledBorderAndShadow: yes\n[V4+ Styles]\n"
																																						   "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
																																						   "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
																																						   "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
																																						   "MarginL, MarginR, MarginV, Encoding\n" +
		   StyleLine(settings) + "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, "
								 "MarginV, Effect, Text\nDialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,," +
		   event + "\n";
}

class Renderer {
	agi::native::Library library;
	csri_rend *renderer = nullptr;
	decltype(&csri_open_mem) open;
	decltype(&csri_close) close;
	decltype(&csri_request_fmt) request_format;
	decltype(&csri_render) render;

	public:
	explicit Renderer(std::filesystem::path const& path)
		: library(agi::native::Library::Load(agi::fs::PathToString(path))) {
		auto const info = library.ResolveSymbol<decltype(&csri_renderer_info)>("csri_renderer_info");
		auto const first = library.ResolveSymbol<decltype(&csri_renderer_default)>("csri_renderer_default");
		renderer = first();
		auto const *identity = renderer ? info(renderer) : nullptr;
		if (!identity || !identity->name || !identity->longname || !identity->specific)
			throw std::runtime_error("DLL does not identify a CSRI renderer");
		std::string_view const name(identity->name);
		if ((name != "pf-xy-vsfilter_textsub" && name != "xy-vsfilter_textsub" && name != "xy-vsfilter_aegisub") || !std::string_view(identity->longname).starts_with("xy-VSFilter") || !*identity->specific)
			throw std::runtime_error("configured DLL is not an xy-VSFilter CSRI renderer");
		auto const version = FileVersion(path);
		auto const expected = Environment(L"AEGISUB_XY_VSF_VERSION");
		if (!expected.empty() && expected != std::wstring(version.begin(), version.end()))
			throw std::runtime_error("xy-VSFilter file version differs from AEGISUB_XY_VSF_VERSION");
		std::cout << "[ xy-VSFilter ] " << name << " CSRI " << identity->specific
				  << ", file " << version << '\n';
		open = library.ResolveSymbol<decltype(open)>("csri_open_mem");
		close = library.ResolveSymbol<decltype(close)>("csri_close");
		request_format = library.ResolveSymbol<decltype(request_format)>("csri_request_fmt");
		render = library.ResolveSymbol<decltype(render)>("csri_render");
	}

	[[nodiscard]] Mask Render(std::string const& event, RenderSettings const& settings = {}) const {
		auto const script = MakeScript(event, settings);
		std::unique_ptr<csri_inst, decltype(close)> instance(
			open(renderer, script.data(), script.size(), nullptr), close);
		if (!instance)
			throw std::runtime_error("xy-VSFilter rejected the test script");
		csri_fmt const format{.pixfmt = CSRI_F_BGR_, .width = Width, .height = Height};
		if (request_format(instance.get(), &format))
			throw std::runtime_error("xy-VSFilter rejected BGR32 rendering");
		std::vector<unsigned char> frame_pixels(Width * Height * 4);
		csri_frame frame{};
		frame.pixfmt = format.pixfmt;
		frame.planes[0] = frame_pixels.data();
		frame.strides[0] = Width * 4;
		render(instance.get(), &frame, 0.0);
		Mask mask{std::vector<unsigned char>(Width * Height)};
		for (std::size_t index = 0; index < mask.pixels.size(); ++index)
			mask.pixels[index] = frame_pixels[index * 4];
		return mask;
	}
};

class perspective_vsfilter_render : public ::testing::Test {
	protected:
	std::unique_ptr<Renderer> renderer;

	void SetUp() override {
		auto const path = RendererPath();
		if (path.empty()) {
			if (Environment(L"AEGISUB_XY_VSF_REQUIRED") == L"1")
				FAIL() << "xy-VSFilter is required; set AEGISUB_XY_VSF_DLL";
			GTEST_SKIP() << "xy-VSFilter unavailable; set AEGISUB_XY_VSF_DLL "
							"and AEGISUB_XY_VSF_REQUIRED=1 for required renderer verification";
		}
		if (std::wstring_view(GetCommandLineW()).find(WorkerArgument) != std::wstring_view::npos)
			renderer = std::make_unique<Renderer>(path);
	}

	bool RunParent() {
		if (renderer)
			return false;
		auto const *test = ::testing::UnitTest::GetInstance()->current_test_info();
		std::string const filter = std::string(test->test_suite_name()) + "." + test->name();
		auto command = L"\"" + Executable().wstring() + L"\" " + WorkerArgument + L" --gtest_color=no --gtest_filter=" + std::wstring(filter.begin(), filter.end());
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
							CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
			ADD_FAILURE() << "cannot start the isolated xy-VSFilter test worker";
			return true;
		}
		CloseHandle(process.hThread);
		auto const wait = WaitForSingleObject(process.hProcess, 30000);
		if (wait != WAIT_OBJECT_0) {
			TerminateProcess(process.hProcess, 1);
			WaitForSingleObject(process.hProcess, 5000);
			ADD_FAILURE() << "xy-VSFilter test worker failed to finish within 30 seconds";
		}
		else {
			DWORD code = 1;
			EXPECT_TRUE(GetExitCodeProcess(process.hProcess, &code));
			EXPECT_EQ(0u, code) << "isolated xy-VSFilter test worker failed";
		}
		CloseHandle(process.hProcess);
		return true;
	}
};

double Area(Mask const& mask) {
	double result = 0.0;
	for (auto pixel : mask.pixels)
		result += pixel / 255.0;
	return result;
}

std::vector<Vec2> Boundary(Mask const& mask) {
	std::vector<Vec2> result;
	for (int y = 0; y < Height; ++y) {
		for (int x = 0; x < Width; ++x) {
			if (mask.At(x, y) >= 128 && (mask.At(x - 1, y) < 128 || mask.At(x + 1, y) < 128 || mask.At(x, y - 1) < 128 || mask.At(x, y + 1) < 128))
				result.push_back({.x = x + 0.5, .y = y + 0.5});
		}
	}
	return result;
}

double DirectedBoundaryDistance(std::vector<Vec2> const& from,
								std::vector<Vec2> const& to) {
	double distance = 0.0;
	for (auto point : from) {
		double nearest = std::numeric_limits<double>::infinity();
		for (auto other : to)
			nearest = std::min(nearest, std::hypot(point.x - other.x, point.y - other.y));
		distance = std::max(distance, nearest);
	}
	return distance;
}

void ExpectMatchingBoundaries(Mask const& actual, Mask const& reference) {
	auto const actual_boundary = Boundary(actual);
	auto const reference_boundary = Boundary(reference);
	ASSERT_FALSE(actual_boundary.empty());
	ASSERT_FALSE(reference_boundary.empty());
	EXPECT_LE(DirectedBoundaryDistance(actual_boundary, reference_boundary), 1.5);
	EXPECT_LE(DirectedBoundaryDistance(reference_boundary, actual_boundary), 1.5);
	EXPECT_NEAR(Area(actual), Area(reference), Area(reference) * 0.02);
}

void ExpectFilledQuad(Mask const& mask, Quad const& quad) {
	Mask reference{std::vector<unsigned char>(Width * Height)};
	std::size_t expected_inside = 0;
	std::size_t missing_inside = 0;
	for (int y = 0; y < Height; ++y) {
		for (int x = 0; x < Width; ++x) {
			Vec2 const point{.x = x + 0.5, .y = y + 0.5};
			double inside_distance = std::numeric_limits<double>::infinity();
			for (std::size_t edge = 0; edge < quad.size(); ++edge) {
				auto const direction = quad[(edge + 1) % quad.size()] - quad[edge];
				inside_distance = std::min(inside_distance,
										   direction.Cross(point - quad[edge]) / std::hypot(direction.x, direction.y));
			}
			if (inside_distance >= 0.0)
				reference.pixels[static_cast<std::size_t>(y) * Width + x] = 255;
			if (inside_distance > 2.0) {
				++expected_inside;
				if (mask.At(x, y) < 128)
					++missing_inside;
			}
		}
	}
	ASSERT_GT(expected_inside, 1000u);
	EXPECT_EQ(0u, missing_inside);
	ExpectMatchingBoundaries(mask, reference);
}

perspective::PerspectiveApplyContext Context(RenderSettings const& settings = {}) {
	perspective::PerspectiveApplyContext result;
	result.frame_number = 0;
	result.play_resolution = {.width = static_cast<double>(settings.play_width),
							  .height = static_cast<double>(settings.play_height)};
	result.video_storage_resolution = perspective::Resolution{.width = Width, .height = Height};
	result.output_mapping = {.scale_x = static_cast<double>(Width) / settings.play_width,
							 .scale_y = static_cast<double>(Height) / settings.play_height};
	return result;
}

AssDialogue *AddLine(AssFile& file, std::string text, RenderSettings const& settings = {}) {
	file.Info.emplace_back("PlayResX", std::to_string(settings.play_width));
	file.Info.emplace_back("PlayResY", std::to_string(settings.play_height));
	file.Info.emplace_back("WrapStyle", "2");
	file.Styles.push_back(*new AssStyle(StyleLine(settings)));
	auto *line = new AssDialogue;
	line->End = 10000;
	line->Text = std::move(text);
	file.Events.push_back(*line);
	return line;
}

bool GdiExtents(AssStyle *style, std::string const& text,
				double& width, double& height, double& descent, double& external_leading) {
	auto const wide = agi::charset::ConvertW(text);
	GdiFontResolver resolver;
	bool measured = false;
	auto const probe = resolver.ProbeWithSelection(style->font,
												   style->bold ? FW_BOLD : FW_NORMAL, style->italic, style->encoding,
												   static_cast<int>(style->fontsize * 64), [&](GdiFontSelectionView const& selected) {
													   // The resolver validates localized aliases against this exact face.
													   // Vertical tests also require GDI to have selected a vertical face.
													   if (!selected.probe->matches_requested_family || selected.probe->selected_family_name.starts_with('@') != style->font.starts_with('@'))
														   return;
													   SIZE size{};
													   TEXTMETRICW metrics{};
													   bool success = true;
													   width = 0.0;
													   if (style->spacing != 0.0) {
														   for (auto character : wide) {
															   success = GetTextExtentPoint32W(selected.dc, &character, 1, &size) != 0 && success;
															   width += size.cx / 64.0 + style->spacing;
														   }
													   }
													   else {
														   success = GetTextExtentPoint32W(selected.dc, wide.data(),
																						   static_cast<int>(wide.size()), &size) != 0;
														   width = size.cx / 64.0;
													   }
													   measured = GetTextMetricsW(selected.dc, &metrics) != 0 && success;
													   height = size.cy / 64.0;
													   descent = metrics.tmDescent / 64.0;
													   external_leading = metrics.tmExternalLeading / 64.0;
												   });
	return probe.success && measured;
}

TEST_F(perspective_vsfilter_render, fractional_drawing_bounds_match_truncated_pixels) {
	if (RunParent())
		return;
	std::string const prefix = R"({\an7\pos(100,100)\fscx6400\fscy6400\p1})";
	std::string const drawing = "m 0.01 0.01 l 1.01 0.01 1.01 1.01 0.01 1.01";
	auto const actual = renderer->Render(prefix + drawing);
	auto const truncated = renderer->Render(prefix + "m 0 0 l 1 0 1 1 0 1");
	auto const rounded = renderer->Render(prefix + "m 0.015625 0.015625 l 1.015625 0.015625 1.015625 1.015625 0.015625 1.015625");
	ASSERT_EQ(4096.0, Area(actual));
	EXPECT_EQ(actual.pixels, truncated.pixels);
	EXPECT_NE(actual.pixels, rounded.pixels);
	AssFile file;
	auto *line = AddLine(file, prefix + drawing);
	auto const captured = perspective::CapturePerspectiveSource(file, *line, Context());
	ASSERT_TRUE(captured) << perspective::DescribePerspectivePlanError(captured.error);
	ASSERT_TRUE(captured.source->current_quad);
	ExpectFilledQuad(actual, *captured.source->current_quad);
}

TEST_F(perspective_vsfilter_render, nonclosing_move_matches_explicit_contour_bridge) {
	if (RunParent())
		return;
	std::string const prefix = R"({\an7\pos(100,100)\p1})";
	auto const actual = renderer->Render(prefix + "m 0 0 l 80 0 80 40 n 0 80 l 80 80 80 120");
	auto const bridged = renderer->Render(prefix + "m 0 0 l 80 0 80 40 0 80 80 80 80 120");
	auto const separate = renderer->Render(prefix + "m 0 0 l 80 0 80 40 m 0 80 l 80 80 80 120");
	ASSERT_GT(Area(actual), 4000.0);
	EXPECT_EQ(actual.pixels, bridged.pixels);
	EXPECT_NE(actual.pixels, separate.pixels);
}

TEST_F(perspective_vsfilter_render, inherits_equivalent_style_rotation_across_resets) {
	if (RunParent())
		return;
	RenderSettings const settings{.angle = 350};
	std::string const original = R"({\an7\pos(100,100)\frz0}Per{\r\frz0}spective)";
	AssFile file;
	auto *line = AddLine(file, original, settings);
	auto const capture = perspective::CapturePerspectiveSource(file, *line, Context(), GdiExtents);
	ASSERT_TRUE(capture) << perspective::DescribePerspectivePlanError(capture.error);
	perspective::SolverCandidate candidate;
	candidate.state = capture.source->state.transform;
	candidate.state.position = {.x = 140.0, .y = 100.0};
	candidate.state.rotation_z = -10.0;
	candidate.serialized.position = "(140,100)";
	candidate.serialized.rotation_z = "-10";
	auto const rewritten = perspective::RewritePerspectiveTags(original,
															   capture.source->state.transform, capture.source->state.event_style_transform, candidate);
	ASSERT_TRUE(rewritten);
	std::string const expected = R"({\pos(140,100)}Per{\r}spective)";
	EXPECT_EQ(expected, rewritten.text);
	EXPECT_EQ(1U, rewritten.cost.tag_count);
	EXPECT_EQ(expected.size(), rewritten.cost.byte_count);
	auto const actual = renderer->Render(rewritten.text, settings);
	auto const reference = renderer->Render(
		R"({\an7\pos(140,100)\frz-10}Per{\r\frz-10}spective)", settings);
	ASSERT_GT(Area(reference), 500.0);
	EXPECT_EQ(reference.pixels, actual.pixels);
}

struct DrawingCase {
	std::string name;
	std::string prefix;
	RenderSettings settings;
};

void PrintTo(DrawingCase const& test, std::ostream *stream) {
	*stream << test.name;
}

class perspective_vsfilter_drawing : public perspective_vsfilter_render,
									 public ::testing::WithParamInterface<DrawingCase> {};

TEST_P(perspective_vsfilter_drawing, generated_tags_cover_the_drawn_quad) {
	if (RunParent())
		return;
	auto const& test = GetParam();
	AssFile file;
	auto *line = AddLine(file, test.prefix + "m 0 0 l 400 0 400 60 0 60", test.settings);
	auto const context = Context(test.settings);
	auto const captured = perspective::CapturePerspectiveSource(file, *line, context);
	ASSERT_TRUE(captured) << perspective::DescribePerspectivePlanError(captured.error);
	Quad const output_target{{{.x = 140.0, .y = 110.0}, {.x = 500.0, .y = 90.0}, {.x = 470.0, .y = 250.0}, {.x = 120.0, .y = 235.0}}};
	auto target = output_target;
	for (auto& point : target) {
		point.x /= context.output_mapping.scale_x;
		point.y /= context.output_mapping.scale_y;
	}
	auto const planned = perspective::BuildPerspectiveMutationPlan(
		file, context, *captured.source, target, 0.1);
	ASSERT_TRUE(planned) << perspective::DescribePerspectivePlanError(planned.error);
	SCOPED_TRACE(planned.plan->ReplacementText());
	ExpectFilledQuad(renderer->Render(planned.plan->ReplacementText(), test.settings), output_target);
}

INSTANTIATE_TEST_SUITE_P(xy, perspective_vsfilter_drawing,
						 ::testing::Values(
							 DrawingCase{.name = "P1TopLeft", .prefix = R"({\an7\pos(80,80)\p1})"},
							 DrawingCase{.name = "P2CenterBaseline", .prefix = R"({\an5\pos(80,80)\p2\pbo100})"},
							 DrawingCase{.name = "P2BottomRightNegativeBaseline", .prefix = R"({\an3\pos(80,80)\p2\pbo-12})"},
							 DrawingCase{.name = "HalfPlayRes", .prefix = R"({\an7\pos(40,40)\p1})", .settings = {.play_width = Width / 2, .play_height = Height / 2}},
							 DrawingCase{.name = "NonuniformOutputMapping", .prefix = R"({\an7\pos(40,80)\p1})", .settings = {.play_width = Width / 2}}),
						 [](testing::TestParamInfo<DrawingCase> const& info) { return info.param.name; });

struct TextCase {
	std::string name;
	std::string text;
	std::string font = "Arial";
	double spacing = 0.0;
	bool vertical = false;
	bool multiline = false;
	int play_width = Width;
	bool unrotated = false;
	bool quarter_turn = false;
};

void PrintTo(TextCase const& test, std::ostream *stream) {
	*stream << test.name;
}

class perspective_vsfilter_text : public perspective_vsfilter_render,
								  public ::testing::WithParamInterface<TextCase> {};

TEST_P(perspective_vsfilter_text, generated_tags_match_known_transform_pixels) {
	auto const& test = GetParam();
	RenderSettings const settings{.play_width = test.play_width, .scale_x = 125, .scale_y = 90, .font = test.font, .spacing = test.spacing};
	AssStyle measurement_style(StyleLine(settings));
	double width = 0.0;
	double height = 0.0;
	double descent = 0.0;
	double leading = 0.0;
	if (!GdiExtents(&measurement_style, "Test", width, height, descent, leading))
		GTEST_SKIP() << "required font unavailable: " << test.font;
	if (RunParent())
		return;
	AssFile file;
	bool const rotated = test.vertical || test.quarter_turn;
	int const target_x = test.play_width == Width ? 185 : 140;
	int const target_y = rotated ? 35 : 105;
	auto *line = AddLine(file, rotated ? R"({\an7\pos(90,80)\fscx100\fscy100\frz270})" + test.text : R"({\an7\pos(90,80)\fscx100\fscy100})" + test.text, settings);
	auto const context = Context(settings);
	auto const captured = perspective::CapturePerspectiveSource(file, *line, context, GdiExtents);
	ASSERT_TRUE(captured) << perspective::DescribePerspectivePlanError(captured.error);
	auto expected = captured.source->forward_input;
	expected.state.position = {.x = static_cast<double>(target_x), .y = static_cast<double>(target_y)};
	expected.state.scale_x = 125.0;
	expected.state.scale_y = 90.0;
	expected.state.shear_x = test.multiline || test.unrotated ? 0.0 : 0.16;
	expected.state.rotation_x = test.unrotated ? 0.0 : 12.0;
	expected.state.rotation_y = test.unrotated ? 0.0 : -18.0;
	expected.state.rotation_z = test.unrotated ? 0.0 : rotated ? 270.0
															   : 7.0;
	auto const forward = perspective::ForwardQuad(expected);
	ASSERT_TRUE(forward);
	auto const planned = perspective::BuildPerspectiveMutationPlan(
		file, context, *captured.source, forward.quad, 0.1, GdiExtents);
	ASSERT_TRUE(planned) << perspective::DescribePerspectivePlanError(planned.error);
	auto const& text = planned.plan->ReplacementText();
	SCOPED_TRACE(text);
	EXPECT_EQ(std::string::npos, text.find("\\org"));
	EXPECT_EQ(std::string::npos, text.find("\\fscx"));
	EXPECT_EQ(std::string::npos, text.find("\\fscy"));
	auto const actual = renderer->Render(text, settings);
	auto const reference_angles = test.unrotated   ? ""
								  : rotated        ? R"(\fax0.16\frx12\fry-18\frz270)"
								  : test.multiline ? R"(\frx12\fry-18\frz7)"
												   : R"(\fax0.16\frx12\fry-18\frz7)";
	auto const reference_tags = R"({\an7\pos()" + std::to_string(target_x) + "," + std::to_string(target_y) + ")" + reference_angles + "}";
	auto const reference = renderer->Render(reference_tags + test.text, settings);
	ASSERT_GT(Area(reference), 500.0);
	ExpectMatchingBoundaries(actual, reference);
}

INSTANTIATE_TEST_SUITE_P(xy, perspective_vsfilter_text,
						 ::testing::Values(
							 TextCase{.name = "Latin", .text = "Perspective"},
							 TextCase{.name = "LatinSpacing", .text = "Perspective", .spacing = 2.0},
							 TextCase{.name = "ExplicitMultiline", .text = R"(AB\NXYZ)", .multiline = true},
							 TextCase{.name = "Cjk", .text = "\xE5\xA4\xA9\xE5\x9C\xB0\xE4\xB8\xAD\xE6\x96\x87", .font = "Microsoft YaHei"},
							 TextCase{.name = "VerticalCjk", .text = "\xE5\xA4\xA9\xE5\x9C\xB0\xE4\xB8\xAD\xE6\x96\x87", .font = "@Microsoft YaHei", .vertical = true},
							 TextCase{.name = "NonuniformPlain", .text = "Perspective", .play_width = Width / 2, .unrotated = true},
							 TextCase{.name = "NonuniformSpacing", .text = "Perspective", .spacing = 2.0, .play_width = Width / 2, .unrotated = true},
							 TextCase{.name = "NonuniformRotation", .text = "Perspective", .play_width = Width / 2, .quarter_turn = true},
							 TextCase{.name = "NonuniformRotatedSpacing", .text = "Perspective", .spacing = 2.0, .play_width = Width / 2, .quarter_turn = true}),
						 [](testing::TestParamInfo<TextCase> const& info) { return info.param.name; });
}
#else
TEST(perspective_vsfilter_render, requires_windows_csri_runtime) {
	if (auto const *required = std::getenv("AEGISUB_XY_VSF_REQUIRED");
		required && std::string_view(required) == "1")
		FAIL() << "required xy-VSFilter CSRI verification requires Windows";
	GTEST_SKIP() << "xy-VSFilter CSRI verification requires Windows";
}
#endif
