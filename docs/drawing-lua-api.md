# Drawing Lua API

The drawing module is available as:

```lua
local drawing = require "aegisub.drawing"
```

It parses and emits ASS drawing text. Emitted text is normalized for renderer
stability and compactness: repeated drawing commands are coalesced where ASS
syntax allows it, filled contours are implicitly closed, and coordinates are
written with no trailing zeroes and normally zero to three decimal places.
Coordinates outside the common 32-bit D6 renderer domain use an exact fallback
with at most six places. Each emitted coordinate is chosen so that both
xy-VSFilter's truncating parser and libass's rounding parser resolve it to the
same 1/64 drawing coordinate within that common domain.

Debug-only helpers are not part of this module. Public compatibility functions
use the `shape_*` names listed below.

## Path objects

Use a path object when applying more than one operation. It keeps coordinates
as doubles and only performs ASS compaction and D6 quantization in the final
`:ass()` call:

```lua
-- String API: every nested call parses and serializes again.
local old = drawing.scale(
  drawing.rotate(
    drawing.translate(shape, 20, 10),
    15),
  1.2, 0.8)

-- Path API: parse once, keep doubles, serialize once.
local path = drawing.path(shape)
path:translate(20, 10):rotate(15):scale(1.2, 0.8)
local ass = path:ass()

-- Constructors can avoid even the initial ASS string.
local constructed = drawing.rect(0, 0, 100, 40)
constructed:translate(20, 10):rotate(15):scale(1.2, 0.8)
local constructed_ass = constructed:ass()
```

Start with `path` for an open drawing and `filled_path` for a shape whose
contours should be treated as filled. Keep the object through transforms,
boolean operations, stroking, and queries; call `ass()` only when inserting the
result into an ASS tag or event. Path methods mutate the object, so use
`clone()` before branching one source into multiple results.

- `path([shape[, mode]]) -> path` parses an open drawing, or creates an empty path.
- `filled_path([shape[, mode]]) -> path` parses a filled drawing.
- `rect(...)`, `ellipse(...)`, and `rounded_rect(...)` construct filled paths
  without an intermediate ASS string.
- `clone()` returns an independent path. Other path operations mutate and
  return the same object for chaining.
- `transform`, `translate`, `scale`, `rotate`, `shear`, `arc_move_to`,
  `arc_to`, `flatten`, and `reverse` mirror the corresponding module operations.
- `unite`, `intersect`, `subtract`, `xor`, `outline`, and `pattern_outline`
  perform backend operations without an intermediate serialization. Boolean
  methods take another path object. `outline` accepts an optional fifth
  flattening-tolerance argument.
- Geometry queries are available as methods: `bounds`, `control_bounds`,
  `length`, `percent_at_length`, `point_at_percent`, `point_at_length`,
  `angle_at_percent`, `slope_at_percent`, `area`, `centroid`, `filled_area`,
  `filled_centroid`,
  `contains_point`, and `contains_rect`.
- Path length and position methods share a lazy segment-measurement cache.
  `filled_area` and `filled_centroid` share a separate cache for the requested
  tolerance and fill rule. Geometry mutations invalidate both automatically.
- `ass()` emits open ASS for an open path and checked compact filled ASS for a
  filled path. `filled_ass()` and `open_ass()` select the output explicitly;
  `fill()` and `open()` change the default.

Filled serialization rejects even-odd paths because ASS cannot encode their
fill rule. Backend boolean and stroke results are converted to winding fill.

## Compatibility modes

Most parsing functions accept an optional compatibility mode as either a number
or a string:

- `0`, `"vsfilter"`, `"xy-vsfilter"`, or `"xy"`: VSFilter-style coordinate
  handling.
- `1` or `"libass"`: libass-style coordinate handling.

When omitted, VSFilter-style handling is used.

## General helpers

- `normalize(shape[, mode]) -> shape`
  Parses a filled drawing and emits normalized ASS text.
- `normalize_open(shape[, mode]) -> shape`
  Parses an open drawing and preserves dangling anchors.
- `compact(shape[, mode]) -> shape`
  Parses a filled drawing and emits a shorter equivalent ASS form when possible.
- `transform(shape, m11, m12, m21, m22, dx, dy[, mode]) -> shape`
  Applies an affine transform to an open drawing.
- `translate(shape, dx, dy[, mode]) -> shape`
- `scale(shape, sx, sy[, mode]) -> shape`
- `rotate(shape, degrees[, mode]) -> shape`
- `shear(shape, horizontal, vertical[, mode]) -> shape`
- `bounds(shape[, mode]) -> x, y, width, height`
  Returns tight geometric bounds (curve extrema included), or `nil` for an empty drawing.
- `flatten(shape[, tolerance[, mode]]) -> shape`
  Non-positive or non-finite tolerances use the default `0.25` tolerance.
- `reverse(shape[, mode]) -> shape`
- `length(shape[, mode]) -> number`
- `percent_at_length(shape, distance[, mode]) -> number`
  Returns the distance divided by total path length, clamped to `[0, 1]`.
- `point_at_percent(shape, percent[, mode]) -> x, y`
  Uses normalized arc length across and within every segment. Percent values
  outside `[0, 1]` are clamped. Returns `nil` if no position is available.
- `point_at_length(shape, distance[, mode]) -> x, y`
  Returns `nil` if no position is available.
- `angle_at_percent(shape, percent[, mode]) -> degrees`
- `slope_at_percent(shape, percent[, mode]) -> number`
  These use the same normalized arc-length position as `point_at_percent`.
- `area(shape[, tolerance[, mode]]) -> number`
  Returns the absolute algebraic contour area after curve flattening, or `nil`
  if there is no measurable area. Overlapping and self-intersecting contours
  are not first resolved through a fill rule.
- `centroid(shape[, tolerance[, mode]]) -> x, y`
  Returns the centroid of the same algebraic contour area, or `nil` if none is
  available.
- `filled_area(shape[, tolerance[, mode]]) -> number`
- `filled_centroid(shape[, tolerance[, mode]]) -> x, y`
  These resolve overlaps and self-intersections according to the path's fill
  rule before measuring, so repeated coverage is counted once. They return
  `nil` when the resolved fill has no measurable area and require the optional
  drawing geometry backend.

## Public shape_* API

These names are exposed for script compatibility:

- `shape_rect(x, y, width, height) -> shape`
- `shape_ellipse(x, y, width, height) -> shape`
- `shape_rounded_rect(x, y, width, height, radius_x, radius_y) -> shape`
- `shape_arc_move_to(shape, x, y, width, height, angle[, mode]) -> shape`
- `shape_arc_to(shape, x, y, width, height, start_angle, sweep[, mode]) -> shape`
- `shape_normalize_ass(shape[, mode]) -> shape`
- `shape_normalize_ass_with_mode(shape[, mode]) -> shape`
- `shape_translate(shape, dx, dy[, mode]) -> shape`
- `shape_rotate(shape, degrees[, mode]) -> shape`
- `shape_scale(shape, sx, sy[, mode]) -> shape`
- `shape_shear(shape, horizontal, vertical[, mode]) -> shape`
- `shape_bouding(shape[, mode]) -> x, y, width, height`
  The misspelling is intentional for compatibility. Uses control-point
  rectangle semantics from the legacy API: curve control points are included,
  but curve extrema are not. Empty shapes return `0, 0, 0, 0`.
- `shape_bouding_coords(shape[, mode]) -> x1, y1, x2, y2`
  Same control-point semantics as `shape_bouding`. Empty shapes return
  `0, 0, 0, 0`.
- `shape_length(shape[, mode]) -> number`
- `shape_percent_at_length(shape, distance[, mode]) -> number`
- `shape_point_at_percent(shape, percent[, mode]) -> x, y`
- `shape_angle_at_percent(shape, percent[, mode]) -> degrees`
- `shape_slope_at_percent(shape, percent[, mode]) -> number`

Rectangle-based constructors and arc helpers intentionally use different
signed-dimension conventions. `rect`, `ellipse`, and `rounded_rect` normalize a
negative width or height by moving the rectangle origin. `arc_move_to` and
`arc_to` preserve signed dimensions, so a negative width or height mirrors the
ellipse around the corresponding axis.

The four percent-based `shape_*` aliases reproduce the legacy QPainterPath
semantics. Segment ranges are allocated according to measured arc length, but
the fraction inside the selected curve is used directly as its Bézier
parameter `t`. `shape_percent_at_length` performs the inverse mapping by first
solving the local curve `t`; distances are clamped to `0` or `1` at the path
ends. For a percent outside `[0, 1]`, `shape_point_at_percent` returns `0, 0`
and the angle/slope functions return `0`. The aliases without `shape_` retain
the modern normalized arc-length behavior described above.

## Legacy shape.lua compatibility

`require "shape"` and `require "aegisub.shape"` expose the original friendly
`shape.lua` names and set `_G.shape`. They are implemented by the built-in
module and do not load an external native library:

`ellipse`, `rect`, `rounded_rect`, `arc_move_to`, `arc_to`,
`angle_at_percent`, `length`, `percent_at_length`, `point_at_percent`,
`slope_at_percent`, `bounding`, `bounding_coords`, `contains_point`,
`contains_rect`, `translate`, `rotate`, `scale`, `shear`, `united`,
`intersected`, `subtracted`, `outline`, and `pattern_outline`.

The additive `xored`, `outline_with_flatten`, and `normalize` helpers are also
available. The compatibility percent methods use legacy QPainterPath parameter
semantics; path-object and unprefixed `aegisub.drawing` methods use normalized
arc length.

This replaces the original module-level `require "shape"` workflow. It does not
export the retired native C ABI: scripts which directly load the old DLL via
FFI, or execute a private copy of `shape.lua` with `dofile`, must switch to
`require "shape"`.

The following functions use the optional drawing geometry backend. Its current
implementation is the narrow Skia adapter enabled by `WITH_DRAWING_SKIA`;
otherwise they raise a Lua error. Parser, transforms, measurements, arcs, and
ASS emission do not depend on this backend.

- `shape_contains_point(shape, x, y[, mode]) -> boolean`
- `shape_contains_rect(shape, x, y, width, height[, mode]) -> boolean`
- `shape_united(lhs, rhs[, mode]) -> shape`
- `shape_intersected(lhs, rhs[, mode]) -> shape`
- `shape_subtracted(lhs, rhs[, mode]) -> shape`
- `shape_xored(lhs, rhs[, mode]) -> shape`
- `shape_outline(shape, width, cap, join[, mode]) -> shape`
- `shape_outline_with_flatten(shape, width, cap, join, tolerance[, mode]) -> shape`
- `shape_pattern_outline(shape, width, cap, join, pattern_length, space_length, dash_offset[, mode]) -> shape`

The unprefixed `filled_area` and `filled_centroid` functions and the matching
Path methods also use this backend. They are modern additions rather than
`shape_*` compatibility aliases.

`cap` accepts `"flat"`, `"butt"`, `"round"`, or `"square"`.
`join` accepts `"miter"`, `"svgmiter"`, `"round"`, or `"bevel"`.

Outline parameters have explicit boundary behavior. A zero width, or a zero
`pattern_length`, succeeds with an empty result. Negative widths and negative
pattern or space lengths are errors. A zero `space_length` is a continuous
outline, and `dash_offset` may be negative. Non-finite values are errors. An
empty source with otherwise valid parameters succeeds with an empty result.
The modern `aegisub.drawing` API raises these errors; the `require "shape"`
compatibility functions return an empty string instead.

`contains_rect` treats a rectangle with a zero or negative width or height as
empty and returns `false`. Non-finite coordinates or dimensions are errors in
the modern API and return `false` through the compatibility module.

## Example

```lua
local drawing = require "aegisub.drawing"

local rect = drawing.shape_rect(0, 0, 100, 40)
local moved = drawing.shape_translate(rect, 20, 10)
local x, y, width, height = drawing.shape_bouding(moved)
```
