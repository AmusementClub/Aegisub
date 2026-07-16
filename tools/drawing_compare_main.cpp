#include <libaegisub/ass/drawing.h>
#include <libaegisub/fs.h>

#include <ass/ass.h>
#include <csri/csri.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using agi::ass::drawing::AssDrawingCompatMode;
using agi::ass::drawing::AssDrawingPathMode;
using agi::ass::drawing::DrawingBooleanOp;
using agi::ass::drawing::DrawingStrokeCap;
using agi::ass::drawing::DrawingStrokeJoin;
using agi::ass::drawing::Matrix3x2;
using agi::ass::drawing::PathCommand;
using agi::ass::drawing::PathData;
using agi::ass::drawing::PathVerb;
using agi::ass::drawing::Point;
using agi::ass::drawing::Rect;

constexpr double kPathTolerance = 1e-3;
constexpr double kScalarTolerance = 1e-3;
constexpr std::uint64_t kRenderAbsSumTolerance = 20000;
constexpr int kRenderPixelTolerance = 150;
constexpr int kRenderMaxDiffTolerance = 96;
constexpr std::string_view kDefaultDumpDirectory = "drawing-compare-diffs";

struct Options {
	std::string reference_dll;
	std::string csri_dll;
	std::string csri_renderer;
	std::string corpus_file;
	int width = 640;
	int height = 360;
	bool verbose = false;
	bool dump_failures = false;
	bool strict_reference = false;
	bool json = false;
};

struct ReportEvent {
	std::string status;
	std::string name;
	std::string detail;
};

struct Counters {
	int passed = 0;
	int failed = 0;
	int skipped = 0;
	int warned = 0;
	std::vector<ReportEvent> events;

	void Pass(std::string_view name, Options const& options, std::string const& detail = {}) {
		++passed;
		events.push_back({"pass", std::string(name), detail});
		if (options.verbose) {
			std::cout << "PASS " << name;
			if (!detail.empty())
				std::cout << " (" << detail << ")";
			std::cout << "\n";
		}
	}

	void Fail(std::string_view name, std::string const& detail) {
		++failed;
		events.push_back({"fail", std::string(name), detail});
		std::cout << "FAIL " << name << ": " << detail << "\n";
	}

	void Skip(std::string_view name, Options const& options, std::string const& detail) {
		++skipped;
		events.push_back({"skip", std::string(name), detail});
		if (options.verbose)
			std::cout << "SKIP " << name << ": " << detail << "\n";
	}

	void Warn(std::string_view name, std::string const& detail) {
		++warned;
		events.push_back({"warn", std::string(name), detail});
		std::cout << "WARN " << name << ": " << detail << "\n";
	}
};

bool ParseInt(std::string_view text, int& value) {
	char *end = nullptr;
	long parsed = std::strtol(std::string(text).c_str(), &end, 10);
	if (!end || *end != '\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max())
		return false;
	value = static_cast<int>(parsed);
	return true;
}

bool ParseOptions(int argc, char **argv, Options& options) {
	for (int index = 1; index < argc; ++index) {
		std::string_view arg(argv[index]);
		auto read_value = [&](std::string& value) {
			if (index + 1 >= argc)
				return false;
			value = argv[++index];
			return true;
		};

		if (arg == "--reference-dll") {
			if (!read_value(options.reference_dll))
				return false;
		} else if (arg == "--csri-dll") {
			if (!read_value(options.csri_dll))
				return false;
		} else if (arg == "--csri-renderer") {
			if (!read_value(options.csri_renderer))
				return false;
		} else if (arg == "--corpus-file") {
			if (!read_value(options.corpus_file))
				return false;
		} else if (arg == "--width") {
			if (index + 1 >= argc || !ParseInt(argv[++index], options.width))
				return false;
		} else if (arg == "--height") {
			if (index + 1 >= argc || !ParseInt(argv[++index], options.height))
				return false;
		} else if (arg == "--verbose") {
			options.verbose = true;
		} else if (arg == "--dump-failures") {
			options.dump_failures = true;
		} else if (arg == "--strict-reference") {
			options.strict_reference = true;
		} else if (arg == "--json") {
			options.json = true;
		} else if (arg == "--help" || arg == "-h") {
			std::cout << "usage: drawing-compare [--reference-dll path] [--csri-dll path] [--csri-renderer name] [--corpus-file path] [--width n] [--height n] [--verbose] [--dump-failures] [--strict-reference] [--json]\n";
			std::exit(0);
		} else {
			return false;
		}
	}

	return true;
}

std::string JsonEscape(std::string_view text) {
	std::ostringstream out;
	for (char c : text) {
		switch (c) {
			case '\\':
				out << "\\\\";
				break;
			case '"':
				out << "\\\"";
				break;
			case '\n':
				out << "\\n";
				break;
			case '\r':
				out << "\\r";
				break;
			case '\t':
				out << "\\t";
				break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					out << "\\u";
					out << std::hex << std::uppercase;
					out.width(4);
					out.fill('0');
					out << static_cast<int>(static_cast<unsigned char>(c));
					out << std::dec << std::nouppercase;
				} else {
					out << c;
				}
				break;
		}
	}
	return out.str();
}

class DynamicLibrary {
public:
	DynamicLibrary() = default;
	DynamicLibrary(DynamicLibrary const&) = delete;
	DynamicLibrary& operator=(DynamicLibrary const&) = delete;

	~DynamicLibrary() {
		Close();
	}

	bool Load(std::string const& path, std::string& error) {
		Close();
#if defined(_WIN32)
		handle_ = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!handle_) {
			error = "LoadLibraryEx failed with error " + std::to_string(GetLastError());
			return false;
		}
#else
		handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle_) {
			error = dlerror();
			return false;
		}
#endif
		return true;
	}

	template<typename Function>
	Function Resolve(char const *name) const {
#if defined(_WIN32)
		return reinterpret_cast<Function>(GetProcAddress(handle_, name));
#else
		return reinterpret_cast<Function>(dlsym(handle_, name));
#endif
	}

	bool IsLoaded() const {
		return handle_ != nullptr;
	}

private:
	void Close() {
		if (!handle_)
			return;
#if defined(_WIN32)
		FreeLibrary(handle_);
#else
		dlclose(handle_);
#endif
		handle_ = nullptr;
	}

#if defined(_WIN32)
	HMODULE handle_ = nullptr;
#else
	void *handle_ = nullptr;
#endif
};

struct ReferenceApi {
	using ShapeRectFn = char *(*)(double, double, double, double);
	using ShapeRoundedRectFn = char *(*)(double, double, double, double, double, double);
	using ShapeArcMoveToFn = char *(*)(char const *, double, double, double, double, double);
	using ShapeArcToFn = char *(*)(char const *, double, double, double, double, double, double);
	using ShapeStringFn = char *(*)(char const *);
	using ShapeStringModeFn = char *(*)(char const *, int);
	using ShapeTransform2Fn = char *(*)(char const *, double, double);
	using ShapeRotateFn = char *(*)(char const *, double);
	using ShapeBinaryFn = char *(*)(char const *, char const *);
	using ShapeOutlineFn = char *(*)(char const *, double, char const *, char const *);
	using ShapeOutlineWithFlattenFn = char *(*)(char const *, double, char const *, char const *, double);
	using ShapePatternOutlineFn = char *(*)(char const *, double, char const *, char const *, double, double, double);
	using ShapeScalarFn = double (*)(char const *);
	using ShapeScalarAtFn = double (*)(char const *, double);
	using ShapePointAtFn = void (*)(char const *, double, double *, double *);
	using ShapeBoundsFn = void (*)(char const *, double *, double *, double *, double *);
	using ShapeContainsPointFn = bool (*)(char const *, double, double);
	using ShapeContainsRectFn = bool (*)(char const *, double, double, double, double);

	DynamicLibrary library;
	ShapeRectFn shape_rect = nullptr;
	ShapeRectFn shape_ellipse = nullptr;
	ShapeRoundedRectFn shape_rounded_rect = nullptr;
	ShapeArcMoveToFn shape_arc_move_to = nullptr;
	ShapeArcToFn shape_arc_to = nullptr;
	ShapeStringFn shape_normalize_ass = nullptr;
	ShapeStringModeFn shape_normalize_ass_with_mode = nullptr;
	ShapeTransform2Fn shape_translate = nullptr;
	ShapeRotateFn shape_rotate = nullptr;
	ShapeTransform2Fn shape_scale = nullptr;
	ShapeTransform2Fn shape_shear = nullptr;
	ShapeBoundsFn shape_bouding = nullptr;
	ShapeBoundsFn shape_bouding_coords = nullptr;
	ShapeScalarFn shape_length = nullptr;
	ShapeScalarAtFn shape_percent_at_length = nullptr;
	ShapePointAtFn shape_point_at_percent = nullptr;
	ShapeScalarAtFn shape_angle_at_percent = nullptr;
	ShapeScalarAtFn shape_slope_at_percent = nullptr;
	ShapeContainsPointFn shape_contains_point = nullptr;
	ShapeContainsRectFn shape_contains_rect = nullptr;
	ShapeBinaryFn shape_united = nullptr;
	ShapeBinaryFn shape_intersected = nullptr;
	ShapeBinaryFn shape_subtracted = nullptr;
	ShapeBinaryFn shape_xored = nullptr;
	ShapeOutlineFn shape_outline = nullptr;
	ShapeOutlineWithFlattenFn shape_outline_with_flatten = nullptr;
	ShapePatternOutlineFn shape_pattern_outline = nullptr;

	bool Load(std::string const& path, std::string& error) {
		if (!library.Load(path, error))
			return false;

		shape_rect = library.Resolve<ShapeRectFn>("shape_rect");
		shape_ellipse = library.Resolve<ShapeRectFn>("shape_ellipse");
		shape_rounded_rect = library.Resolve<ShapeRoundedRectFn>("shape_rounded_rect");
		shape_arc_move_to = library.Resolve<ShapeArcMoveToFn>("shape_arc_move_to");
		shape_arc_to = library.Resolve<ShapeArcToFn>("shape_arc_to");
		shape_normalize_ass = library.Resolve<ShapeStringFn>("shape_normalize_ass");
		shape_normalize_ass_with_mode = library.Resolve<ShapeStringModeFn>("shape_normalize_ass_with_mode");
		shape_translate = library.Resolve<ShapeTransform2Fn>("shape_translate");
		shape_rotate = library.Resolve<ShapeRotateFn>("shape_rotate");
		shape_scale = library.Resolve<ShapeTransform2Fn>("shape_scale");
		shape_shear = library.Resolve<ShapeTransform2Fn>("shape_shear");
		shape_bouding = library.Resolve<ShapeBoundsFn>("shape_bouding");
		shape_bouding_coords = library.Resolve<ShapeBoundsFn>("shape_bouding_coords");
		shape_length = library.Resolve<ShapeScalarFn>("shape_length");
		shape_percent_at_length = library.Resolve<ShapeScalarAtFn>("shape_percent_at_length");
		shape_point_at_percent = library.Resolve<ShapePointAtFn>("shape_point_at_percent");
		shape_angle_at_percent = library.Resolve<ShapeScalarAtFn>("shape_angle_at_percent");
		shape_slope_at_percent = library.Resolve<ShapeScalarAtFn>("shape_slope_at_percent");
		shape_contains_point = library.Resolve<ShapeContainsPointFn>("shape_contains_point");
		shape_contains_rect = library.Resolve<ShapeContainsRectFn>("shape_contains_rect");
		shape_united = library.Resolve<ShapeBinaryFn>("shape_united");
		shape_intersected = library.Resolve<ShapeBinaryFn>("shape_intersected");
		shape_subtracted = library.Resolve<ShapeBinaryFn>("shape_subtracted");
		shape_xored = library.Resolve<ShapeBinaryFn>("shape_xored");
		shape_outline = library.Resolve<ShapeOutlineFn>("shape_outline");
		shape_outline_with_flatten = library.Resolve<ShapeOutlineWithFlattenFn>("shape_outline_with_flatten");
		shape_pattern_outline = library.Resolve<ShapePatternOutlineFn>("shape_pattern_outline");
		return shape_rect || shape_normalize_ass || shape_length || shape_outline;
	}

	template<typename Function, typename... Args>
	std::optional<std::string> CallString(Function function, Args... args) const {
		if (!function)
			return std::nullopt;

		char *raw = function(args...);
		if (!raw)
			return std::nullopt;

		// Some reference providers expose allocated strings without a matching release hook.
		return std::string(raw);
	}
};

class AssRenderer {
public:
	AssRenderer() = default;
	AssRenderer(AssRenderer const&) = delete;
	AssRenderer& operator=(AssRenderer const&) = delete;

	~AssRenderer() {
		if (track_)
			ass_free_track(track_);
		if (renderer_)
			ass_renderer_done(renderer_);
		if (library_)
			ass_library_done(library_);
	}

	bool Init(std::string& error) {
		library_ = ass_library_init();
		if (!library_) {
			error = "ass_library_init failed";
			return false;
		}
		ass_set_message_cb(library_, QuietMessage, nullptr);

		renderer_ = ass_renderer_init(library_);
		if (!renderer_) {
			error = "ass_renderer_init failed";
			return false;
		}

		ass_set_fonts(renderer_, nullptr, "Sans", 1, nullptr, true);
		return true;
	}

	std::vector<std::uint8_t> Render(std::string const& shape, int width, int height, std::string& error) {
		std::string script = MakeScript(shape, width, height);
		if (track_) {
			ass_free_track(track_);
			track_ = nullptr;
		}

		track_ = ass_read_memory(library_, script.data(), script.size(), nullptr);
		if (!track_) {
			error = "ass_read_memory failed";
			return {};
		}

		ass_set_frame_size(renderer_, width, height);
		ass_set_storage_size(renderer_, width, height);

		int changed = 0;
		ASS_Image *images = ass_render_frame(renderer_, track_, 0, &changed);
		(void)changed;

		std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
		for (ASS_Image *image = images; image; image = image->next) {
			int opacity = 255 - static_cast<int>(image->color & 0xFF);
			for (int y = 0; y < image->h; ++y) {
				int target_y = image->dst_y + y;
				if (target_y < 0 || target_y >= height)
					continue;
				for (int x = 0; x < image->w; ++x) {
					int target_x = image->dst_x + x;
					if (target_x < 0 || target_x >= width)
						continue;

					int coverage = image->bitmap[y * image->stride + x];
					int alpha = coverage * opacity / 255;
					auto& dst = mask[static_cast<std::size_t>(target_y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(target_x)];
					dst = static_cast<std::uint8_t>(alpha + dst * (255 - alpha) / 255);
				}
			}
		}
		return mask;
	}

private:
	static void QuietMessage(int, char const *, std::va_list, void *) {
	}

	static std::string MakeScript(std::string const& shape, int width, int height) {
		std::ostringstream script;
		script << "[Script Info]\n"
			<< "ScriptType: v4.00+\n"
			<< "PlayResX: " << width << "\n"
			<< "PlayResY: " << height << "\n"
			<< "[V4+ Styles]\n"
			<< "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
			<< "Style: Default,Arial,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
			<< "[Events]\n"
			<< "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
			<< "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{\\an7\\p1\\pos(0,0)\\bord0\\shad0\\1c&HFFFFFF&\\alpha&H00&}" << shape << "\n";
		return script.str();
	}

	ASS_Library *library_ = nullptr;
	ASS_Renderer *renderer_ = nullptr;
	ASS_Track *track_ = nullptr;
};

struct CsriApi {
	using RendererDefaultFn = csri_rend *(*)();
	using RendererNextFn = csri_rend *(*)(csri_rend *);
	using RendererInfoFn = csri_info *(*)(csri_rend *);
	using OpenMemFn = csri_inst *(*)(csri_rend *, void const *, std::size_t, csri_openflag *);
	using CloseFn = void (*)(csri_inst *);
	using RequestFmtFn = int (*)(csri_inst *, csri_fmt const *);
	using RenderFn = void (*)(csri_inst *, csri_frame *, double);

	DynamicLibrary library;
	RendererDefaultFn renderer_default = nullptr;
	RendererNextFn renderer_next = nullptr;
	RendererInfoFn renderer_info = nullptr;
	OpenMemFn open_mem = nullptr;
	CloseFn close = nullptr;
	RequestFmtFn request_fmt = nullptr;
	RenderFn render = nullptr;

	bool Load(std::string const& path, std::string& error) {
		if (!library.Load(path, error))
			return false;

		renderer_default = library.Resolve<RendererDefaultFn>("csri_renderer_default");
		renderer_next = library.Resolve<RendererNextFn>("csri_renderer_next");
		renderer_info = library.Resolve<RendererInfoFn>("csri_renderer_info");
		open_mem = library.Resolve<OpenMemFn>("csri_open_mem");
		close = library.Resolve<CloseFn>("csri_close");
		request_fmt = library.Resolve<RequestFmtFn>("csri_request_fmt");
		render = library.Resolve<RenderFn>("csri_render");
		if (!renderer_default || !renderer_next || !renderer_info || !open_mem || !close || !request_fmt || !render) {
			error = "missing CSRI exports";
			return false;
		}
		return true;
	}

	csri_rend *FindRenderer(std::string const& requested_name, std::string& selected_name) const {
		for (csri_rend *renderer = renderer_default(); renderer; renderer = renderer_next(renderer)) {
			csri_info *info = renderer_info(renderer);
			std::string name = info && info->name ? info->name : "";
			if (requested_name.empty() || name == requested_name) {
				selected_name = name.empty() ? "(unnamed)" : name;
				return renderer;
			}
		}
		return nullptr;
	}

	std::vector<std::uint8_t> RenderShape(csri_rend *renderer, std::string const& shape, int width, int height, std::string& error) const {
		std::string script = AssRendererScript(shape, width, height);
		csri_inst *instance = open_mem(renderer, script.data(), script.size(), nullptr);
		if (!instance) {
			error = "csri_open_mem failed";
			return {};
		}

		std::vector<std::uint8_t> frame(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
		csri_frame csri_frame_data = {};
		csri_frame_data.pixfmt = CSRI_F_BGR_;
		csri_frame_data.planes[0] = frame.data();
		csri_frame_data.strides[0] = width * 4;

		csri_fmt format = {
			CSRI_F_BGR_,
			static_cast<unsigned>(width),
			static_cast<unsigned>(height),
		};

		if (request_fmt(instance, &format) != 0) {
			close(instance);
			error = "csri_request_fmt failed";
			return {};
		}

		render(instance, &csri_frame_data, 0.0);
		close(instance);

		std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
		for (std::size_t pixel = 0; pixel < mask.size(); ++pixel) {
			std::uint8_t b = frame[pixel * 4 + 0];
			std::uint8_t g = frame[pixel * 4 + 1];
			std::uint8_t r = frame[pixel * 4 + 2];
			mask[pixel] = std::max({b, g, r});
		}
		return mask;
	}

private:
	static std::string AssRendererScript(std::string const& shape, int width, int height) {
		std::ostringstream script;
		script << "[Script Info]\n"
			<< "ScriptType: v4.00+\n"
			<< "PlayResX: " << width << "\n"
			<< "PlayResY: " << height << "\n"
			<< "[V4+ Styles]\n"
			<< "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
			<< "Style: Default,Arial,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n"
			<< "[Events]\n"
			<< "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
			<< "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{\\an7\\p1\\pos(0,0)\\bord0\\shad0\\1c&HFFFFFF&\\alpha&H00&}" << shape << "\n";
		return script.str();
	}
};

struct MaskMetrics {
	std::uint64_t nonzero_a = 0;
	std::uint64_t nonzero_b = 0;
	std::uint64_t abs_sum = 0;
	int max_diff = 0;
	int pixels_over_16 = 0;
	int pixels_over_32 = 0;
};

MaskMetrics CompareMasks(std::vector<std::uint8_t> const& lhs, std::vector<std::uint8_t> const& rhs) {
	MaskMetrics metrics;
	std::size_t size = std::min(lhs.size(), rhs.size());
	for (std::size_t index = 0; index < size; ++index) {
		int a = lhs[index];
		int b = rhs[index];
		if (a)
			++metrics.nonzero_a;
		if (b)
			++metrics.nonzero_b;

		int diff = std::abs(a - b);
		metrics.abs_sum += static_cast<std::uint64_t>(diff);
		metrics.max_diff = std::max(metrics.max_diff, diff);
		if (diff > 16)
			++metrics.pixels_over_16;
		if (diff > 32)
			++metrics.pixels_over_32;
	}
	return metrics;
}

std::uint64_t CountNonzero(std::vector<std::uint8_t> const& mask) {
	return static_cast<std::uint64_t>(std::count_if(mask.begin(), mask.end(), [](std::uint8_t value) {
		return value != 0;
	}));
}

std::string RenderDetail(MaskMetrics const& metrics) {
	std::ostringstream out;
	out << "nonzero=" << metrics.nonzero_a << "/" << metrics.nonzero_b
		<< ", abs_sum=" << metrics.abs_sum
		<< ", max=" << metrics.max_diff
		<< ", px>16=" << metrics.pixels_over_16
		<< ", px>32=" << metrics.pixels_over_32;
	return out.str();
}

bool RenderCloseEnough(MaskMetrics const& metrics) {
	return metrics.abs_sum <= kRenderAbsSumTolerance &&
		metrics.pixels_over_32 <= kRenderPixelTolerance &&
		metrics.max_diff <= kRenderMaxDiffTolerance;
}

std::string CompactWhitespace(std::string_view text) {
	std::string compact;
	bool in_space = false;
	for (char c : text) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			in_space = true;
			continue;
		}
		if (in_space && !compact.empty())
			compact.push_back(' ');
		compact.push_back(c);
		in_space = false;
	}
	return compact;
}

std::string Truncate(std::string const& text) {
	constexpr std::size_t max_length = 1000;
	if (text.size() <= max_length)
		return text;
	return text.substr(0, max_length) + "...";
}

std::string SafeFileStem(std::string_view name) {
	std::string stem;
	stem.reserve(name.size());
	for (unsigned char c : name)
		stem.push_back(std::isalnum(c) || c == '-' || c == '_' ? static_cast<char>(c) : '_');
	return stem.empty() ? "unnamed" : stem;
}

bool WritePgm(std::filesystem::path const& path,
	std::vector<std::uint8_t> const& mask,
	int width,
	int height,
	std::string& error) {
	if (mask.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
		error = "mask size does not match image dimensions";
		return false;
	}

	std::ofstream file(path, std::ios::binary);
	if (!file) {
		error = "failed to open " + agi::fs::PathToString(path);
		return false;
	}

	file << "P5\n" << width << " " << height << "\n255\n";
	file.write(reinterpret_cast<char const *>(mask.data()), static_cast<std::streamsize>(mask.size()));
	if (!file) {
		error = "failed to write " + agi::fs::PathToString(path);
		return false;
	}
	return true;
}

bool DumpMaskComparison(Options const& options,
	std::string_view name,
	std::vector<std::uint8_t> const& current,
	std::vector<std::uint8_t> const& reference,
	std::string& detail) {
	std::error_code ec;
	auto directory = std::filesystem::absolute(kDefaultDumpDirectory, ec);
	if (ec) {
		detail = "failed to resolve dump directory: " + ec.message();
		return false;
	}
	std::filesystem::create_directories(directory, ec);
	if (ec) {
		detail = "failed to create " + agi::fs::PathToString(directory) + ": " + ec.message();
		return false;
	}

	std::vector<std::uint8_t> diff(std::min(current.size(), reference.size()));
	std::transform(current.begin(), current.begin() + diff.size(), reference.begin(), diff.begin(),
		[](std::uint8_t lhs, std::uint8_t rhs) {
			return static_cast<std::uint8_t>(std::abs(static_cast<int>(lhs) - static_cast<int>(rhs)));
		});

	auto base = directory / SafeFileStem(name);
	auto current_path = base;
	current_path += "-current.pgm";
	auto reference_path = base;
	reference_path += "-reference.pgm";
	auto diff_path = base;
	diff_path += "-diff.pgm";
	std::string error;
	if (!WritePgm(current_path, current, options.width, options.height, error) ||
		!WritePgm(reference_path, reference, options.width, options.height, error) ||
		!WritePgm(diff_path, diff, options.width, options.height, error)) {
		detail = std::move(error);
		return false;
	}

	detail = agi::fs::PathToString(base) + "-{current,reference,diff}.pgm";
	return true;
}

PathData ParseShapeForMode(std::string_view text, AssDrawingPathMode mode) {
	return agi::ass::drawing::ParseAss(text, AssDrawingCompatMode::VsFilter, mode);
}

double MaxPointDiff(Point const& lhs, Point const& rhs) {
	return std::max(std::abs(lhs.x - rhs.x), std::abs(lhs.y - rhs.y));
}

bool CommandsClose(PathCommand const& lhs, PathCommand const& rhs, double& max_delta) {
	if (lhs.verb != rhs.verb)
		return false;

	max_delta = std::max(max_delta, MaxPointDiff(lhs.p1, rhs.p1));
	max_delta = std::max(max_delta, MaxPointDiff(lhs.p2, rhs.p2));
	max_delta = std::max(max_delta, MaxPointDiff(lhs.p3, rhs.p3));
	max_delta = std::max(max_delta, std::abs(lhs.weight - rhs.weight));
	return true;
}

bool ParsedPathsClose(std::string const& lhs, std::string const& rhs, AssDrawingPathMode mode, std::string& detail) {
	PathData lhs_path = ParseShapeForMode(lhs, mode);
	PathData rhs_path = ParseShapeForMode(rhs, mode);

	if (lhs_path.winding_fill != rhs_path.winding_fill) {
		detail = "fill rule differs";
		return false;
	}

	if (lhs_path.commands.size() != rhs_path.commands.size()) {
		detail = "command count " + std::to_string(lhs_path.commands.size()) + " vs " + std::to_string(rhs_path.commands.size());
		return false;
	}

	double max_delta = 0.0;
	for (std::size_t index = 0; index < lhs_path.commands.size(); ++index) {
		if (!CommandsClose(lhs_path.commands[index], rhs_path.commands[index], max_delta)) {
			detail = "verb differs at command " + std::to_string(index);
			return false;
		}
	}

	if (max_delta > kPathTolerance) {
		detail = "max coordinate delta " + std::to_string(max_delta);
		return false;
	}

	detail = "max coordinate delta " + std::to_string(max_delta);
	return true;
}

bool AlmostSame(double lhs, double rhs, double tolerance = kScalarTolerance) {
	if (std::isinf(lhs) || std::isinf(rhs))
		return std::isinf(lhs) && std::isinf(rhs) && std::signbit(lhs) == std::signbit(rhs);
	if (std::isnan(lhs) || std::isnan(rhs))
		return std::isnan(lhs) && std::isnan(rhs);
	double scale = std::max({1.0, std::abs(lhs), std::abs(rhs)});
	return std::abs(lhs - rhs) <= tolerance * scale;
}

double AngleFromTangent(Point const& tangent) {
	if (std::hypot(tangent.x, tangent.y) <= agi::ass::drawing::kPointEpsilon)
		return 0.0;

	constexpr double radians_to_degrees = 180.0 / 3.14159265358979323846;
	double degrees = std::atan2(-tangent.y, tangent.x) * radians_to_degrees;
	return std::fmod(degrees + 360.0, 360.0);
}

double SlopeFromTangent(Point const& tangent) {
	if (std::abs(tangent.x) <= agi::ass::drawing::kPointEpsilon) {
		if (std::abs(tangent.y) <= agi::ass::drawing::kPointEpsilon)
			return 0.0;
		return tangent.y >= 0.0
			? std::numeric_limits<double>::infinity()
			: -std::numeric_limits<double>::infinity();
	}

	return tangent.y / tangent.x;
}

Matrix3x2 Rotation(double degrees) {
	double radians = degrees * 3.14159265358979323846 / 180.0;
	double c = std::cos(radians);
	double s = std::sin(radians);
	return {c, s, -s, c, 0.0, 0.0};
}

void CompareDouble(Counters& counters, Options const& options, std::string_view name, double current, std::optional<double> reference) {
	if (!reference) {
		counters.Skip(name, options, "reference function unavailable");
		return;
	}

	if (AlmostSame(current, *reference))
		counters.Pass(name, options, "current=" + std::to_string(current));
	else {
		std::string detail = "current=" + std::to_string(current) + ", reference=" + std::to_string(*reference);
		if (options.strict_reference)
			counters.Fail(name, detail);
		else
			counters.Warn(name, detail);
	}
}

void CompareBool(Counters& counters, Options const& options, std::string_view name, bool current, std::optional<bool> reference) {
	if (!reference) {
		counters.Skip(name, options, "reference function unavailable");
		return;
	}

	if (current == *reference)
		counters.Pass(name, options, current ? "true" : "false");
	else {
		std::string detail = std::string("current=") + (current ? "true" : "false") + ", reference=" + (*reference ? "true" : "false");
		if (options.strict_reference)
			counters.Fail(name, detail);
		else
			counters.Warn(name, detail);
	}
}

void CompareShape(Counters& counters,
	Options const& options,
	AssRenderer& renderer,
	std::string_view name,
	std::string const& current,
	std::optional<std::string> reference,
	AssDrawingPathMode path_mode,
	bool expect_visible) {
	std::string render_error;
	auto current_mask = renderer.Render(current, options.width, options.height, render_error);
	if (current_mask.empty() && options.width > 0 && options.height > 0) {
		counters.Fail(name, "libass render failed: " + render_error);
		return;
	}

	if (expect_visible && CountNonzero(current_mask) == 0) {
		counters.Fail(name, "current output rendered empty");
		return;
	}

	if (!reference) {
		counters.Pass(name, options, "libass render smoke; reference unavailable");
		return;
	}

	if (CompactWhitespace(current) == CompactWhitespace(*reference)) {
		counters.Pass(name, options, "text equal");
		return;
	}

	std::string path_detail;
	if (ParsedPathsClose(current, *reference, path_mode, path_detail)) {
		counters.Pass(name, options, "parsed path equal enough, " + path_detail);
		return;
	}

	std::string reference_error;
	auto reference_mask = renderer.Render(*reference, options.width, options.height, reference_error);
	if (reference_mask.empty()) {
		std::string detail = "reference libass render failed after path mismatch: " + reference_error + "; " + path_detail;
		if (options.strict_reference)
			counters.Fail(name, detail);
		else
			counters.Warn(name, detail);
		if (options.dump_failures) {
			std::cout << "  current: " << Truncate(current) << "\n";
			std::cout << "  reference: " << Truncate(*reference) << "\n";
		}
		return;
	}

	auto metrics = CompareMasks(current_mask, reference_mask);
	if (RenderCloseEnough(metrics))
		counters.Pass(name, options, "render close, " + RenderDetail(metrics));
	else {
		std::string detail = path_detail + "; render diff " + RenderDetail(metrics);
		if (options.dump_failures) {
			std::string dump_detail;
			if (DumpMaskComparison(options, name, current_mask, reference_mask, dump_detail))
				detail += "; artifacts=" + dump_detail;
			else
				detail += "; artifact error=" + dump_detail;
		}
		if (options.strict_reference)
			counters.Fail(name, detail);
		else
			counters.Warn(name, detail);
		if (options.dump_failures) {
			std::cout << "  current: " << Truncate(current) << "\n";
			std::cout << "  reference: " << Truncate(*reference) << "\n";
		}
	}
}

struct ShapeCase {
	std::string name;
	std::string ass;
	bool expect_visible = true;
};

std::string SerializeFilled(PathData const& path) {
	return agi::ass::drawing::SerializeAssFilled(path);
}

std::string SerializeOpen(PathData const& path) {
	return agi::ass::drawing::SerializeAss(path);
}

std::string CurrentNormalize(std::string_view shape, AssDrawingCompatMode mode = AssDrawingCompatMode::VsFilter) {
	return SerializeFilled(agi::ass::drawing::ParseAss(shape, mode));
}

void AddBaseShapeChecks(std::vector<std::function<void()>>& checks,
	Counters& counters,
	Options const& options,
	AssRenderer& renderer,
	ReferenceApi const *reference) {
	auto add = [&](std::string name,
		std::string current,
		std::optional<std::string> ref,
		AssDrawingPathMode mode,
		bool expect_visible = true) {
		checks.push_back([&, name = std::move(name), current = std::move(current), ref = std::move(ref), mode, expect_visible] {
			CompareShape(counters, options, renderer, name, current, ref, mode, expect_visible);
		});
	};

	add("shape_rect",
		SerializeFilled(agi::ass::drawing::MakeRect(10, 20, 80, 40)),
		reference ? reference->CallString(reference->shape_rect, 10.0, 20.0, 80.0, 40.0) : std::nullopt,
		AssDrawingPathMode::FilledContours);
	add("shape_ellipse",
		SerializeFilled(agi::ass::drawing::MakeEllipse(140, 40, 90, 55)),
		reference ? reference->CallString(reference->shape_ellipse, 140.0, 40.0, 90.0, 55.0) : std::nullopt,
		AssDrawingPathMode::FilledContours);
	add("shape_rounded_rect",
		SerializeFilled(agi::ass::drawing::MakeRoundedRect(260, 35, 120, 70, 18, 12)),
		reference ? reference->CallString(reference->shape_rounded_rect, 260.0, 35.0, 120.0, 70.0, 18.0, 12.0) : std::nullopt,
		AssDrawingPathMode::FilledContours);

	PathData arc;
	agi::ass::drawing::AppendArcMoveTo(arc, 70, 150, 90, 70, 15);
	agi::ass::drawing::AppendArcTo(arc, 70, 150, 90, 70, 15, 240);
	std::optional<std::string> ref_arc;
	if (reference) {
		auto first = reference->CallString(reference->shape_arc_move_to, "", 70.0, 150.0, 90.0, 70.0, 15.0);
		if (first)
			ref_arc = reference->CallString(reference->shape_arc_to, first->c_str(), 70.0, 150.0, 90.0, 70.0, 15.0, 240.0);
	}
	add("shape_arc_move_to/shape_arc_to", SerializeOpen(arc), ref_arc, AssDrawingPathMode::PreserveOpenContours);
}

std::vector<ShapeCase> Corpus() {
	return {
		{"rect", "m 10 10 l 110 10 110 70 10 70"},
		{"cubic", "m 30 150 b 80 40 160 240 220 120 l 260 170 b 210 230 80 230 30 150"},
		{"donut", "m 300 40 l 420 40 420 160 300 160 m 390 70 l 390 130 330 130 330 70"},
		{"self_intersect", "m 60 230 l 160 330 60 330 160 230"},
		{"separate_contours", "m 240 220 l 310 220 310 280 240 280 m 340 230 l 420 230 420 310 340 310"},
		{"large_coords", "m 500 40 l 620 40 620 150 500 150"},
		{"degenerate", "m 500 240 l 500 240 560 280 500 320", true},
	};
}

std::vector<ShapeCase> LoadCorpusFile(std::string const& path, Counters& counters) {
	std::vector<ShapeCase> cases;
	if (path.empty())
		return cases;

	std::ifstream file(path);
	if (!file) {
		counters.Warn("corpus-file", "failed to open " + path);
		return cases;
	}

	std::string line;
	int line_number = 0;
	while (std::getline(file, line)) {
		++line_number;
		if (line.empty() || line[0] == '#')
			continue;

		std::size_t separator = line.find('\t');
		if (separator == std::string::npos)
			separator = line.find('=');
		if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size()) {
			counters.Warn("corpus-file", "ignored malformed line " + std::to_string(line_number));
			continue;
		}

		cases.push_back({line.substr(0, separator), line.substr(separator + 1)});
	}
	return cases;
}

void AddCorpusShapeChecks(std::vector<std::function<void()>>& checks,
	Counters& counters,
	Options const& options,
	AssRenderer& renderer,
	ReferenceApi const *reference) {
	auto corpus = Corpus();
	auto extra = LoadCorpusFile(options.corpus_file, counters);
	corpus.insert(corpus.end(), extra.begin(), extra.end());

	for (auto const& shape : corpus) {
		checks.push_back([&, shape] {
			std::string current = CurrentNormalize(shape.ass);
			std::optional<std::string> ref = reference ? reference->CallString(reference->shape_normalize_ass, shape.ass.c_str()) : std::nullopt;
			CompareShape(counters, options, renderer, "shape_normalize_ass/" + shape.name, current, ref, AssDrawingPathMode::FilledContours, shape.expect_visible);
		});

		checks.push_back([&, shape] {
			std::string current = CurrentNormalize(shape.ass, AssDrawingCompatMode::Libass);
			std::optional<std::string> ref = reference ? reference->CallString(reference->shape_normalize_ass_with_mode, shape.ass.c_str(), static_cast<int>(AssDrawingCompatMode::Libass)) : std::nullopt;
			CompareShape(counters, options, renderer, "shape_normalize_ass_with_mode/libass/" + shape.name, current, ref, AssDrawingPathMode::FilledContours, shape.expect_visible);
		});
	}
}

void AddTransformChecks(std::vector<std::function<void()>>& checks,
	Counters& counters,
	Options const& options,
	AssRenderer& renderer,
	ReferenceApi const *reference) {
	std::string open_shape = "m 25 25 b 60 5 90 75 120 45 l 150 75";
	PathData path = agi::ass::drawing::ParseAssOpen(open_shape);

	auto add = [&](std::string name, Matrix3x2 matrix, std::optional<std::string> ref) {
		std::string current = SerializeOpen(agi::ass::drawing::TransformPath(path, matrix));
		checks.push_back([&, name = std::move(name), current = std::move(current), ref = std::move(ref)] {
			CompareShape(counters, options, renderer, name, current, ref, AssDrawingPathMode::PreserveOpenContours, true);
		});
	};

	add("shape_translate", {1.0, 0.0, 0.0, 1.0, 15.0, -8.0},
		reference ? reference->CallString(reference->shape_translate, open_shape.c_str(), 15.0, -8.0) : std::nullopt);
	add("shape_rotate", Rotation(37.0),
		reference ? reference->CallString(reference->shape_rotate, open_shape.c_str(), 37.0) : std::nullopt);
	add("shape_scale", {1.25, 0.0, 0.0, 0.75, 0.0, 0.0},
		reference ? reference->CallString(reference->shape_scale, open_shape.c_str(), 1.25, 0.75) : std::nullopt);
	add("shape_shear", {1.0, -0.15, 0.20, 1.0, 0.0, 0.0},
		reference ? reference->CallString(reference->shape_shear, open_shape.c_str(), 0.20, -0.15) : std::nullopt);
}

void AddGeometryChecks(std::vector<std::function<void()>>& checks,
	Counters& counters,
	Options const& options,
	ReferenceApi const *reference) {
	std::string open_shape = "m 15 45 b 80 5 110 115 175 60 l 250 110";
	std::string filled_shape = "m 20 20 l 160 20 160 130 20 130";

	checks.push_back([&, open_shape] {
		CompareDouble(counters, options, "shape_length",
			agi::ass::drawing::PathLength(agi::ass::drawing::ParseAssOpen(open_shape)),
			reference && reference->shape_length ? std::optional<double>(reference->shape_length(open_shape.c_str())) : std::nullopt);
	});

	checks.push_back([&, open_shape] {
		CompareDouble(counters, options, "shape_percent_at_length",
			agi::ass::drawing::LegacyPercentAtLength(agi::ass::drawing::ParseAssOpen(open_shape), 95.0),
			reference && reference->shape_percent_at_length ? std::optional<double>(reference->shape_percent_at_length(open_shape.c_str(), 95.0)) : std::nullopt);
	});

	checks.push_back([&, open_shape] {
		Point point;
		Point tangent;
		agi::ass::drawing::TryGetLegacyPositionAtPercent(agi::ass::drawing::ParseAssOpen(open_shape), 0.35, point, tangent);
		std::optional<double> ref_x;
		std::optional<double> ref_y;
		if (reference && reference->shape_point_at_percent) {
			double x = 0.0;
			double y = 0.0;
			reference->shape_point_at_percent(open_shape.c_str(), 0.35, &x, &y);
			ref_x = x;
			ref_y = y;
		}
		CompareDouble(counters, options, "shape_point_at_percent/x", point.x, ref_x);
		CompareDouble(counters, options, "shape_point_at_percent/y", point.y, ref_y);
	});

	checks.push_back([&, open_shape] {
		Point point;
		Point tangent;
		agi::ass::drawing::TryGetLegacyPositionAtPercent(agi::ass::drawing::ParseAssOpen(open_shape), 0.65, point, tangent);
		CompareDouble(counters, options, "shape_angle_at_percent",
			AngleFromTangent(tangent),
			reference && reference->shape_angle_at_percent ? std::optional<double>(reference->shape_angle_at_percent(open_shape.c_str(), 0.65)) : std::nullopt);
		CompareDouble(counters, options, "shape_slope_at_percent",
			SlopeFromTangent(tangent),
			reference && reference->shape_slope_at_percent ? std::optional<double>(reference->shape_slope_at_percent(open_shape.c_str(), 0.65)) : std::nullopt);
	});

	checks.push_back([&, filled_shape] {
		Rect bounds;
		agi::ass::drawing::TryGetControlPointBounds(agi::ass::drawing::ParseAss(filled_shape), bounds);
		std::optional<Rect> ref_bounds;
		if (reference && reference->shape_bouding) {
			Rect rect;
			reference->shape_bouding(filled_shape.c_str(), &rect.x, &rect.y, &rect.width, &rect.height);
			ref_bounds = rect;
		}
		CompareDouble(counters, options, "shape_bouding/x", bounds.x, ref_bounds ? std::optional<double>(ref_bounds->x) : std::nullopt);
		CompareDouble(counters, options, "shape_bouding/y", bounds.y, ref_bounds ? std::optional<double>(ref_bounds->y) : std::nullopt);
		CompareDouble(counters, options, "shape_bouding/w", bounds.width, ref_bounds ? std::optional<double>(ref_bounds->width) : std::nullopt);
		CompareDouble(counters, options, "shape_bouding/h", bounds.height, ref_bounds ? std::optional<double>(ref_bounds->height) : std::nullopt);
	});

	if (agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		checks.push_back([&, filled_shape] {
			bool contains = false;
			agi::ass::drawing::TryDrawingContainsPoint(agi::ass::drawing::ParseAss(filled_shape), 40, 50, contains);
			CompareBool(counters, options, "shape_contains_point", contains,
				reference && reference->shape_contains_point ? std::optional<bool>(reference->shape_contains_point(filled_shape.c_str(), 40.0, 50.0)) : std::nullopt);
		});

		checks.push_back([&, filled_shape] {
			bool contains = false;
			agi::ass::drawing::TryDrawingContainsRect(agi::ass::drawing::ParseAss(filled_shape), 35, 35, 25, 25, contains);
			CompareBool(counters, options, "shape_contains_rect", contains,
				reference && reference->shape_contains_rect ? std::optional<bool>(reference->shape_contains_rect(filled_shape.c_str(), 35.0, 35.0, 25.0, 25.0)) : std::nullopt);
		});
	}
}

void AddSkiaShapeChecks(std::vector<std::function<void()>>& checks,
	Counters& counters,
	Options const& options,
	AssRenderer& renderer,
	ReferenceApi const *reference) {
	if (!agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		checks.push_back([&] {
			counters.Skip("skia shape operations", options, "drawing Skia backend is off");
		});
		return;
	}

	std::string lhs = "m 40 40 l 150 40 150 150 40 150";
	std::string rhs = "m 95 85 l 210 85 210 190 95 190";
	auto lhs_path = agi::ass::drawing::ParseAss(lhs);
	auto rhs_path = agi::ass::drawing::ParseAss(rhs);

	auto add_binary = [&](std::string name, DrawingBooleanOp op, ReferenceApi::ShapeBinaryFn ref_fn) {
		PathData result;
		agi::ass::drawing::TryDrawingBoolean(lhs_path, rhs_path, op, result);
		std::string current = SerializeFilled(result);
		std::optional<std::string> ref = reference ? reference->CallString(ref_fn, lhs.c_str(), rhs.c_str()) : std::nullopt;
		checks.push_back([&, name = std::move(name), current = std::move(current), ref = std::move(ref)] {
			CompareShape(counters, options, renderer, name, current, ref, AssDrawingPathMode::FilledContours, true);
		});
	};

	add_binary("shape_united", DrawingBooleanOp::Union, reference ? reference->shape_united : nullptr);
	add_binary("shape_intersected", DrawingBooleanOp::Intersect, reference ? reference->shape_intersected : nullptr);
	add_binary("shape_subtracted", DrawingBooleanOp::Subtract, reference ? reference->shape_subtracted : nullptr);
	add_binary("shape_xored", DrawingBooleanOp::Xor, reference ? reference->shape_xored : nullptr);

	std::string open_shape = "m 35 235 b 100 160 175 315 250 235 l 330 300";
	PathData open_path = agi::ass::drawing::ParseAssOpen(open_shape);

	auto add_outline = [&](std::string name,
		std::string current,
		std::optional<std::string> ref) {
		checks.push_back([&, name = std::move(name), current = std::move(current), ref = std::move(ref)] {
			CompareShape(counters, options, renderer, name, current, ref, AssDrawingPathMode::FilledContours, true);
		});
	};

	PathData outline;
	agi::ass::drawing::TryDrawingOutline(open_path, 6.0, DrawingStrokeCap::Round, DrawingStrokeJoin::Round, outline);
	add_outline("shape_outline",
		SerializeFilled(outline),
		reference ? reference->CallString(reference->shape_outline, open_shape.c_str(), 6.0, "round", "round") : std::nullopt);

	PathData outline_flat;
	agi::ass::drawing::TryDrawingOutline(open_path, 6.0, DrawingStrokeCap::Round, DrawingStrokeJoin::Round, outline_flat);
	outline_flat = agi::ass::drawing::FlattenPath(outline_flat, 0.5);
	add_outline("shape_outline_with_flatten",
		SerializeFilled(outline_flat),
		reference ? reference->CallString(reference->shape_outline_with_flatten, open_shape.c_str(), 6.0, "round", "round", 0.5) : std::nullopt);

	PathData pattern;
	agi::ass::drawing::TryDrawingPatternOutline(open_path, 5.0, DrawingStrokeCap::Round, DrawingStrokeJoin::Bevel, 2.0, 1.25, 0.5, pattern);
	add_outline("shape_pattern_outline",
		SerializeFilled(pattern),
		reference ? reference->CallString(reference->shape_pattern_outline, open_shape.c_str(), 5.0, "round", "bevel", 2.0, 1.25, 0.5) : std::nullopt);
}

void RunCsriComparison(Counters& counters, Options const& options, AssRenderer& libass_renderer) {
	if (options.csri_dll.empty()) {
		counters.Skip("csri render comparison", options, "--csri-dll not provided");
		return;
	}

	CsriApi csri;
	std::string error;
	if (!csri.Load(options.csri_dll, error)) {
		counters.Warn("csri render comparison", "failed to load renderer DLL: " + error);
		return;
	}

	std::string selected_name;
	csri_rend *renderer = csri.FindRenderer(options.csri_renderer, selected_name);
	if (!renderer) {
		counters.Warn("csri render comparison", "requested renderer not found");
		return;
	}

	std::cout << "CSRI renderer: " << selected_name << "\n";
	std::vector<ShapeCase> cases = {
		{"rect", CurrentNormalize("m 30 30 l 160 30 160 110 30 110")},
		{"donut", CurrentNormalize("m 220 30 l 360 30 360 170 220 170 m 330 60 l 330 140 250 140 250 60")},
		{"cubic", CurrentNormalize("m 40 210 b 120 120 210 310 300 210 l 360 280 b 260 335 120 335 40 210")},
	};

	if (agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		PathData outline;
		agi::ass::drawing::TryDrawingOutline(agi::ass::drawing::ParseAssOpen("m 420 70 b 490 10 570 150 620 80"), 7.0, DrawingStrokeCap::Round, DrawingStrokeJoin::Round, outline);
		cases.push_back({"outline", SerializeFilled(outline)});
	}

	for (auto const& shape : cases) {
		std::string libass_error;
		auto libass_mask = libass_renderer.Render(shape.ass, options.width, options.height, libass_error);
		std::string csri_error;
		auto csri_mask = csri.RenderShape(renderer, shape.ass, options.width, options.height, csri_error);
		if (libass_mask.empty() || csri_mask.empty()) {
			counters.Warn("render/" + shape.name, libass_mask.empty() ? libass_error : csri_error);
			continue;
		}

		auto metrics = CompareMasks(libass_mask, csri_mask);
		std::cout << "RENDER " << shape.name << ": " << RenderDetail(metrics) << "\n";
		if (metrics.nonzero_a == 0 || metrics.nonzero_b == 0)
			counters.Warn("render/" + shape.name, "one renderer produced an empty mask");
	}
}

void PrintJsonSummary(Counters const& counters, Options const& options) {
	std::cout << "json: {"
		<< "\"passed\":" << counters.passed
		<< ",\"failed\":" << counters.failed
		<< ",\"skipped\":" << counters.skipped
		<< ",\"warned\":" << counters.warned
		<< ",\"drawing_skia_backend\":" << (agi::ass::drawing::DrawingSkiaBackendAvailable() ? "true" : "false")
		<< ",\"frame\":{\"width\":" << options.width << ",\"height\":" << options.height << "}"
		<< ",\"events\":[";
	for (std::size_t index = 0; index < counters.events.size(); ++index) {
		auto const& event = counters.events[index];
		if (index)
			std::cout << ',';
		std::cout << "{\"status\":\"" << JsonEscape(event.status)
			<< "\",\"name\":\"" << JsonEscape(event.name)
			<< "\",\"detail\":\"" << JsonEscape(event.detail) << "\"}";
	}
	std::cout << "]}\n";
}

} // namespace

int main(int argc, char **argv) {
	Options options;
	if (!ParseOptions(argc, argv, options)) {
		std::cerr << "Invalid arguments. Use --help for usage.\n";
		return 2;
	}

	std::cout << "Drawing comparison\n";
	std::cout << "drawing_skia_backend: " << (agi::ass::drawing::DrawingSkiaBackendAvailable() ? "on" : "off") << "\n";
	std::cout << "frame: " << options.width << "x" << options.height << "\n";

	AssRenderer renderer;
	std::string renderer_error;
	if (!renderer.Init(renderer_error)) {
		std::cerr << "libass init failed: " << renderer_error << "\n";
		return 2;
	}

	std::optional<ReferenceApi> reference_storage;
	if (!options.reference_dll.empty()) {
		reference_storage.emplace();
		std::string error;
		if (!reference_storage->Load(options.reference_dll, error)) {
			std::cerr << "reference DLL load failed: " << error << "\n";
			return 2;
		}
		std::cout << "reference DLL: loaded\n";
	} else {
		std::cout << "reference DLL: skipped\n";
	}

	Counters counters;
	ReferenceApi const *reference = reference_storage ? &*reference_storage : nullptr;
	std::vector<std::function<void()>> checks;

	AddBaseShapeChecks(checks, counters, options, renderer, reference);
	AddCorpusShapeChecks(checks, counters, options, renderer, reference);
	AddTransformChecks(checks, counters, options, renderer, reference);
	AddGeometryChecks(checks, counters, options, reference);
	AddSkiaShapeChecks(checks, counters, options, renderer, reference);

	for (auto& check : checks)
		check();

	RunCsriComparison(counters, options, renderer);

	std::cout << "summary: passed=" << counters.passed
		<< ", failed=" << counters.failed
		<< ", skipped=" << counters.skipped
		<< ", warned=" << counters.warned << "\n";
	if (options.json)
		PrintJsonSummary(counters, options);

	return counters.failed == 0 ? 0 : 1;
}
