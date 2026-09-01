#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_style.h"
#include "../../src/libass_runtime.h"
#include "../../src/perspective_apply_plan.h"
#include "../../src/perspective_ass_state.h"
#include "../../src/perspective_quad_edit_state.h"
#include "../../src/perspective_tag_solver.h"

#include <gtest/gtest.h>

#include <ass/ass.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using perspective::BoundsKind;
using perspective::ForwardInput;
using perspective::MakeHomography;
using perspective::Quad;
using perspective::Rect;
using perspective::Resolution;
using perspective::SolvePerspectiveTags;
using perspective::Vec2;

constexpr int RenderWidth = 640;
constexpr int RenderHeight = 360;

struct RenderedMask {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> alpha;

    unsigned char At(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return 0;
        return alpha[static_cast<std::size_t>(y) * width + x];
    }
};

std::string StyleLine(int scale_x = 100, int scale_y = 100) {
    return
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0," + std::to_string(scale_x) + "," + std::to_string(scale_y)
        + ",0,0,1,0,0,7,0,0,0,1";
}

std::string AssHeader(int scale_x = 100, int scale_y = 100) {
    return std::string(
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "WrapStyle: 2\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
            "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
            "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
            "Alignment, MarginL, MarginR, MarginV, Encoding\n")
        + StyleLine(scale_x, scale_y) + "\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
            "Effect, Text\n";
}

std::string MakeScript(
    std::string const& event_text,
    int scale_x = 100,
    int scale_y = 100) {
    return AssHeader(scale_x, scale_y)
        + "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,"
        + event_text + "\n";
}

std::string MakeTransformTags(
    perspective::SerializedTransformState const& serialized,
    std::string_view geometry_mode) {
    std::string result = "{\\an7\\pos" + serialized.position;
    if (serialized.origin)
        result += "\\org" + *serialized.origin;
    result += "\\fscx" + serialized.scale_x;
    result += "\\fscy" + serialized.scale_y;
    result += "\\fax" + serialized.shear_x;
    result += "\\fay" + serialized.shear_y;
    result += "\\frx" + serialized.rotation_x;
    result += "\\fry" + serialized.rotation_y;
    result += "\\frz" + serialized.rotation_z;
    result += geometry_mode;
    result += "}";
    return result;
}

RenderedMask Render(ASS_Library* library, ASS_Renderer* renderer, std::string script) {
    auto const& api = libass::runtime::GetApi();
    auto *mutable_data = script.data();
    auto *track = api.ass_read_memory(
        library, mutable_data, script.size(), nullptr);
    EXPECT_NE(nullptr, track);
    if (!track)
        return {};

    api.ass_set_frame_size(renderer, RenderWidth, RenderHeight);
    api.ass_set_storage_size(renderer, RenderWidth, RenderHeight);
    auto *images = api.ass_render_frame(renderer, track, 0, nullptr);

    RenderedMask result;
    result.width = RenderWidth;
    result.height = RenderHeight;
    result.alpha.assign(static_cast<std::size_t>(RenderWidth) * RenderHeight, 0);
    for (auto *image = images; image; image = image->next) {
        for (int y = 0; y < image->h; ++y) {
            for (int x = 0; x < image->w; ++x) {
                int const dst_x = image->dst_x + x;
                int const dst_y = image->dst_y + y;
                if (dst_x < 0 || dst_y < 0
                    || dst_x >= RenderWidth || dst_y >= RenderHeight)
                    continue;
                auto const source_alpha = image->bitmap[
                    static_cast<std::ptrdiff_t>(y) * image->stride + x];
                auto& destination = result.alpha[
                    static_cast<std::size_t>(dst_y) * RenderWidth + dst_x];
                destination = std::max(destination, source_alpha);
            }
        }
    }
    api.ass_free_track(track);
    return result;
}

struct MaskStats {
    std::size_t opaque_pixels = 0;
    double alpha_area = 0.0;
    Vec2 centroid;
};

struct MaskBounds {
    int left = RenderWidth;
    int top = RenderHeight;
    int right = 0;
    int bottom = 0;

    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Empty() const { return left >= right || top >= bottom; }
};

MaskBounds GetMaskBounds(RenderedMask const& mask, unsigned char threshold = 16) {
    MaskBounds result;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (mask.At(x, y) < threshold)
                continue;
            result.left = std::min(result.left, x);
            result.top = std::min(result.top, y);
            result.right = std::max(result.right, x + 1);
            result.bottom = std::max(result.bottom, y + 1);
        }
    }
    return result;
}

#ifdef _WIN32
std::pair<double, double> GetGdiExtent(
    std::string_view font, std::wstring_view text = L"\u4e2d") {
    auto *dc = CreateCompatibleDC(nullptr);
    EXPECT_NE(nullptr, dc);
    if (!dc)
        return {};
    LOGFONTW descriptor {};
    descriptor.lfHeight = 40 * 64;
    descriptor.lfWeight = FW_NORMAL;
    descriptor.lfOutPrecision = OUT_TT_PRECIS;
    descriptor.lfQuality = ANTIALIASED_QUALITY;
    auto const wide_font = std::wstring(font.begin(), font.end());
    wcsncpy_s(descriptor.lfFaceName, wide_font.c_str(), 31);
    auto *selected_font = CreateFontIndirectW(&descriptor);
    EXPECT_NE(nullptr, selected_font);
    if (!selected_font) {
        DeleteDC(dc);
        return {};
    }
    auto *old_font = SelectObject(dc, selected_font);
    SIZE size {};
    EXPECT_TRUE(GetTextExtentPoint32W(
        dc, text.data(), static_cast<int>(text.size()), &size));
    SelectObject(dc, old_font);
    DeleteObject(selected_font);
    DeleteDC(dc);
    return {size.cx / 64.0, size.cy / 64.0};
}

bool VerticalCjkTextExtents(
    AssStyle*, std::string const&,
    double& width, double& height,
    double& descent, double& external_leading) {
    auto const extent = GetGdiExtent(
        "@Microsoft YaHei",
        L"\u5929\u5730\u3001\u3002\uff01\uff1f\uff08\uff09\u300c\u300d");
    width = extent.first;
    height = extent.second;
    descent = 0.0;
    external_leading = 0.0;
    return width > 0.0 && height > 0.0;
}

bool ArialTextExtents(
    AssStyle*, std::string const& text,
    double& width, double& height,
    double& descent, double& external_leading) {
    auto const extent = GetGdiExtent(
        "Arial", std::wstring(text.begin(), text.end()));
    width = extent.first;
    height = extent.second;
    descent = 0.0;
    external_leading = 0.0;
    return width > 0.0 && height > 0.0;
}
#endif

MaskStats GetStats(RenderedMask const& mask, unsigned char threshold = 16) {
    MaskStats result;
    double weighted_x = 0.0;
    double weighted_y = 0.0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            auto const alpha = mask.At(x, y);
            if (alpha >= threshold)
                ++result.opaque_pixels;
            double const weight = static_cast<double>(alpha) / 255.0;
            result.alpha_area += weight;
            weighted_x += (x + 0.5) * weight;
            weighted_y += (y + 0.5) * weight;
        }
    }
    if (result.alpha_area > 0.0)
        result.centroid = {
            weighted_x / result.alpha_area,
            weighted_y / result.alpha_area};
    return result;
}

double SignedDistanceOutside(Vec2 point, Quad const& quad) {
    double minimum = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < quad.size(); ++index) {
        auto const start = quad[index];
        auto const end = quad[(index + 1) % quad.size()];
        auto const edge = end - start;
        auto const length = std::hypot(edge.x, edge.y);
        if (length <= 0.0)
            return std::numeric_limits<double>::infinity();
        minimum = std::min(
            minimum,
            edge.Cross(point - start) / length);
    }
    return std::max(0.0, -minimum);
}

double MaxMaskDistanceOutside(RenderedMask const& mask, Quad const& target) {
    double result = 0.0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (mask.At(x, y) < 16)
                continue;
            result = std::max(
                result,
                SignedDistanceOutside({x + 0.5, y + 0.5}, target));
        }
    }
    return result;
}

double PolygonArea(Quad const& quad) {
    double area = 0.0;
    for (std::size_t index = 0; index < quad.size(); ++index)
        area += quad[index].Cross(quad[(index + 1) % quad.size()]);
    return std::abs(area) / 2.0;
}

ForwardInput MakeSourceInput() {
    ForwardInput source;
    source.play_resolution = {640.0, 360.0};
    source.bounds = {
        Rect {0.0, 0.0, 400.0, 60.0},
        BoundsKind::Drawing,
        Resolution {400.0, 60.0},
        {0.0, 0.0},
        {}};
    source.state.alignment = 7;
    source.state.position = {80.0, 80.0};
    return source;
}

struct LibassSession {
    ASS_Library* library = nullptr;
    ASS_Renderer* renderer = nullptr;

    ~LibassSession() {
        if (!library && !renderer)
            return;
        auto const& api = libass::runtime::GetApi();
        if (renderer)
            api.ass_renderer_done(renderer);
        if (library)
            api.ass_library_done(library);
    }
};

LibassSession StartLibass() {
    LibassSession result;
    auto const& api = libass::runtime::GetApi();
    result.library = api.ass_library_init();
    if (!result.library)
        return result;
    api.ass_set_extract_fonts(result.library, 0);
    result.renderer = api.ass_renderer_init(result.library);
    if (!result.renderer)
        return result;
    api.ass_set_font_scale(result.renderer, 1.0);
    api.ass_set_fonts(result.renderer, nullptr, "Sans", 1, nullptr, true);
    return result;
}

}

TEST(perspective_libass_render, generated_tags_match_hand_drawn_quad_for_vector_and_text) {
    if (!libass::runtime::IsAvailable())
        GTEST_SKIP() << "libass runtime is unavailable: "
            << libass::runtime::GetLoadError();

    auto session = StartLibass();
    ASSERT_NE(nullptr, session.library);
    ASSERT_NE(nullptr, session.renderer);

    auto const source = MakeSourceInput();
    Quad const target {{{140.0, 110.0}, {500.0, 90.0},
        {470.0, 250.0}, {120.0, 235.0}}};
    auto const solved = SolvePerspectiveTags({
        source, target, {1.0, 1.0}, 0.1});
    ASSERT_TRUE(solved) << perspective::DescribeSolverError(solved.error);

    auto const marker_tags = MakeTransformTags(
        solved.candidate->serialized, "\\p1");
    auto const marker = Render(
        session.library,
        session.renderer,
        MakeScript(marker_tags + "m 0 0 l 400 0 l 400 60 l 0 60"));
    auto const marker_stats = GetStats(marker);
    ASSERT_GT(marker_stats.alpha_area, 1000.0);
    EXPECT_LT(MaxMaskDistanceOutside(marker, target), 3.0);
    EXPECT_NEAR(
        marker_stats.alpha_area,
        PolygonArea(target),
        1500.0);

    auto const source_text = Render(
        session.library,
        session.renderer,
        MakeScript("{\\an7\\pos(80,80)}Perspective"));
    auto const rewritten = perspective::RewritePerspectiveTags(
        "{\\an7\\pos(80,80)}Perspective",
        source.state,
        source.state,
        *solved.candidate);
    ASSERT_TRUE(rewritten) << perspective::DescribeRewriteError(rewritten.error);
    ASSERT_TRUE(rewritten.changed);
    auto const transformed_text = Render(
        session.library,
        session.renderer,
        MakeScript(rewritten.text));
    auto const source_stats = GetStats(source_text);
    auto const transformed_stats = GetStats(transformed_text);
    ASSERT_GT(source_stats.alpha_area, 10.0);
    ASSERT_GT(transformed_stats.alpha_area, 10.0);
    EXPECT_LT(MaxMaskDistanceOutside(transformed_text, target), 5.0);

    auto const expected_homography = MakeHomography(source.bounds.rectangle, target);
    ASSERT_TRUE(expected_homography);
    auto const source_local_centroid = source_stats.centroid - source.state.position;
    auto const expected_centroid = expected_homography.value.Map(source_local_centroid);
    ASSERT_TRUE(expected_centroid);
    // A projective warp changes the alpha weighting across a glyph, so its
    // raster centroid is not exactly the homography of the source centroid.
    EXPECT_NEAR(transformed_stats.centroid.x, expected_centroid->x, 8.0);
    EXPECT_NEAR(transformed_stats.centroid.y, expected_centroid->y, 8.0);
}

TEST(perspective_libass_render, ordinary_text_minimal_style_tags_render_target) {
#ifndef _WIN32
    GTEST_SKIP() << "the renderer-compatible Arial extent provider is Windows-specific";
#else
    if (!libass::runtime::IsAvailable())
        GTEST_SKIP() << "libass runtime is unavailable: "
            << libass::runtime::GetLoadError();

    auto session = StartLibass();
    ASSERT_NE(nullptr, session.library);
    ASSERT_NE(nullptr, session.renderer);

    constexpr int StyleScaleX = 125;
    constexpr int StyleScaleY = 90;
    auto const original = std::string(
        "{\\an7\\pos(90,80)\\fscx100\\fscy100}Perspective");
    AssFile file;
    file.Info.emplace_back("PlayResX", "640");
    file.Info.emplace_back("PlayResY", "360");
    file.Info.emplace_back("WrapStyle", "2");
    file.Styles.push_back(*new AssStyle(StyleLine(StyleScaleX, StyleScaleY)));
    auto *line = new AssDialogue;
    line->Start = 0;
    line->End = 10000;
    line->Text = original;
    file.Events.push_back(*line);

    perspective::PerspectiveApplyContext context;
    context.frame_number = 0;
    context.capture_time_ms = 0;
    context.play_resolution = {640.0, 360.0};
    context.video_storage_resolution = Resolution {640.0, 360.0};
    context.output_mapping = {1.0, 1.0};
    auto const capture = perspective::CapturePerspectiveSource(
        file, *line, context, ArialTextExtents);
    ASSERT_TRUE(capture)
        << perspective::DescribePerspectivePlanError(capture.error);
    ASSERT_TRUE(capture.source->current_quad);
    EXPECT_DOUBLE_EQ(100.0, capture.source->state.transform.scale_x);
    EXPECT_DOUBLE_EQ(100.0, capture.source->state.transform.scale_y);
    EXPECT_DOUBLE_EQ(
        static_cast<double>(StyleScaleX),
        capture.source->state.event_style_transform.scale_x);
    EXPECT_DOUBLE_EQ(
        static_cast<double>(StyleScaleY),
        capture.source->state.event_style_transform.scale_y);

    auto target_input = capture.source->forward_input;
    target_input.state.position = {185.0, 105.0};
    target_input.state.origin.reset();
    target_input.state.scale_x = StyleScaleX;
    target_input.state.scale_y = StyleScaleY;
    target_input.state.shear_x = 0.16;
    target_input.state.shear_y = 0.0;
    target_input.state.rotation_x = 12.0;
    target_input.state.rotation_y = -18.0;
    target_input.state.rotation_z = 7.0;
    auto const target_forward = perspective::ForwardQuad(target_input);
    ASSERT_TRUE(target_forward)
        << perspective::DescribeForwardError(target_forward.error);
    auto const target = target_forward.quad;
    auto const top_edge = target[1] - target[0];
    auto const bottom_edge = target[2] - target[3];
    EXPECT_GT(std::abs(top_edge.Cross(bottom_edge)), 1.0);

    auto const planned = perspective::BuildPerspectiveMutationPlan(
        file, context, *capture.source, target, 0.1, ArialTextExtents);
    ASSERT_TRUE(planned)
        << perspective::DescribePerspectivePlanError(planned.error);
    EXPECT_FALSE(planned.plan->Candidate().state.origin);
    EXPECT_FALSE(planned.plan->Candidate().serialized.origin);
    EXPECT_NE(
        perspective::CandidateFamily::ProjectiveExplicitOrigin,
        planned.plan->Family());
    EXPECT_LE(planned.plan->MaxError(), 0.1);

    auto const& rewritten = planned.plan->ReplacementText();
    SCOPED_TRACE("rewritten ASS event: " + rewritten);
    EXPECT_EQ(std::string::npos, rewritten.find("\\org"));
    EXPECT_EQ(std::string::npos, rewritten.find("\\fscx"));
    EXPECT_EQ(std::string::npos, rewritten.find("\\fscy"));
    EXPECT_NE(std::string::npos, rewritten.find("\\frx"));
    EXPECT_NE(std::string::npos, rewritten.find("\\fry"));
    EXPECT_NE(std::string::npos, rewritten.find("Perspective"));

    auto const source_rendered = Render(
        session.library,
        session.renderer,
        MakeScript(original, StyleScaleX, StyleScaleY));
    auto const transformed_rendered = Render(
        session.library,
        session.renderer,
        MakeScript(rewritten, StyleScaleX, StyleScaleY));
    auto const source_stats = GetStats(source_rendered);
    auto const transformed_stats = GetStats(transformed_rendered);
    ASSERT_GT(source_stats.alpha_area, 100.0);
    ASSERT_GT(transformed_stats.alpha_area, 100.0);
    EXPECT_LT(MaxMaskDistanceOutside(transformed_rendered, target), 5.0);
    EXPECT_GT(std::hypot(
        transformed_stats.centroid.x - source_stats.centroid.x,
        transformed_stats.centroid.y - source_stats.centroid.y), 50.0);
#endif
}

TEST(perspective_libass_render, vertical_cjk_perspective_tags_render_inside_target) {
#ifndef _WIN32
    GTEST_SKIP() << "the @ CJK vertical-face contract is Windows-specific";
#else
    if (!libass::runtime::IsAvailable())
        GTEST_SKIP() << "libass runtime is unavailable: "
            << libass::runtime::GetLoadError();

    auto session = StartLibass();
    ASSERT_NE(nullptr, session.library);
    ASSERT_NE(nullptr, session.renderer);

    auto const text = "\xE5\xA4\xA9\xE5\x9C\xB0\xE3\x80\x81\xE3\x80\x82"
        "\xEF\xBC\x81\xEF\xBC\x9F\xEF\xBC\x88\xEF\xBC\x89"
        "\xE3\x80\x8C\xE3\x80\x8D";
    auto const original = std::string(
        "{\\an7\\pos(100,30)\\frz270\\fn@Microsoft YaHei\\fs40}") + text;
    AssFile file;
    file.Info.emplace_back("PlayResX", "640");
    file.Info.emplace_back("PlayResY", "360");
    file.Info.emplace_back("WrapStyle", "2");
    file.Styles.push_back(*new AssStyle(
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,"
        "&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1"));
    auto *line = new AssDialogue;
    line->Start = 0;
    line->End = 10000;
    line->Text = original;
    file.Events.push_back(*line);

    perspective::PerspectiveApplyContext context;
    context.frame_number = 0;
    context.capture_time_ms = 0;
    context.play_resolution = {640.0, 360.0};
    context.video_storage_resolution = Resolution {640.0, 360.0};
    context.output_mapping = {1.0, 1.0};
    auto const capture = perspective::CapturePerspectiveSource(
        file, *line, context, VerticalCjkTextExtents);
    ASSERT_TRUE(capture)
        << perspective::DescribePerspectivePlanError(capture.error);
    ASSERT_TRUE(capture.source->current_quad);

    auto target = perspective::PerspectiveQuadFromOppositeCorners(
        {165.0, 25.0}, {270.0, 325.0},
        perspective::PerspectiveFirstEdgeDirection(*capture.source));
    target[1].x -= 20.0;
    target[2].y -= 25.0;
    target[3] = target[3] + Vec2 {15.0, 20.0};
    auto const planned = perspective::BuildPerspectiveMutationPlan(
        file, context, *capture.source, target, 0.1, VerticalCjkTextExtents);
    ASSERT_TRUE(planned)
        << perspective::DescribePerspectivePlanError(planned.error);
    EXPECT_EQ("270", planned.plan->Candidate().serialized.rotation_z);

    auto const source_rendered = Render(
        session.library, session.renderer, MakeScript(original));
    auto const source_bounds = GetMaskBounds(source_rendered);
    ASSERT_GT(GetStats(source_rendered).alpha_area, 100.0);
    EXPECT_LT(source_bounds.Width(), source_bounds.Height());

    auto const& rewritten = planned.plan->ReplacementText();
    EXPECT_NE(std::string::npos, rewritten.find("\\frz270"));

    auto const rendered = Render(
        session.library, session.renderer, MakeScript(rewritten));
    auto const stats = GetStats(rendered);
    ASSERT_GT(stats.alpha_area, 100.0);
    EXPECT_LT(MaxMaskDistanceOutside(rendered, target), 10.0);
    auto const rendered_bounds = GetMaskBounds(rendered);
    EXPECT_LT(rendered_bounds.Width(), rendered_bounds.Height());
#endif
}
