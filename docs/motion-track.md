# Motion tracking

Motion Track follows one rectangular image patch through the selected subtitle
time range. Choose a clearly visible reference frame, draw the ROI, select a
model and direction, then Analyze. The overlay shows the tracked quadrilateral;
adjusting the ROI and analyzing again continues from the new reference.
Changing the tracking model or direction keeps the old trajectory available, but
Apply and Plan preview require Analyze with the new settings. Switching back to
the original settings restores access to the existing trajectory. A fresh analysis
uses a fresh ROI pose; continuing an existing session retains its accumulated pose.

## Models

| Model | Motion |
| --- | --- |
| Translation | Horizontal and vertical movement, with subpixel refinement |
| Similarity | Translation, in-plane rotation and uniform scale |
| Affine | Translation, rotation, shear and independent horizontal/vertical scale |
| Perspective | A planar homography: perspective deformation of all four ROI corners |

Affine and Perspective use direct grayscale registration against the reference
patch. They need texture and sufficiently small changes between adjacent frames.
They do not recognize object identities, estimate 3D depth, or follow independent
objects within one ROI. Partial occlusion is handled with robust residual weights;
severe occlusion, an invalid projection, or an excessive jump fails the frame.
Three consecutive failed frames stop that direction. Forward and backward passes
have independent motion priors.

Translation additionally supports template refresh, duplicate-frame handling and
fade detection/recovery. These options do not apply to the other backends.

## ASS output

Exact writes a static pose for each frame and merges adjacent identical output.
Compact fits motion in actual video time, including variable-frame-rate timecodes.
Its error setting is measured in video pixels, independent of PlayRes. Position
and pose interpolation are checked separately for Translation/Similarity. Full
geometry interpolation is checked by evaluating the generated ASS at every tracked
frame and measuring the projected geometry in video pixels.
Compact animation windows use ASS's serialized centisecond event times. If the
selected position precision cannot meet the error budget, Apply rejects the plan
and requests more position decimals or a larger Compact error.
Position decimals also controls full geometry position tags and Compact move
endpoints. Other pose channels keep the precision needed by the geometry solver.
Exact merges positions only when they serialize identically at the selected
precision. Changing any apply option clears the old Plan preview immediately.

Affine and Perspective compose the tracked map with the subtitle's existing
geometry. They can write position, scale, shear and all three rotation tags. The
existing perspective solver evaluates PlayRes, LayoutRes, alignment and the source
geometry before choosing a representation; a result outside the error budget
rejects the complete apply plan.
Vertical font faces retain the renderer's semantic base rotation while the
remaining transform tags follow the tracked plane.

Static `\org`, rectangular `\clip`/`\iclip`, and vector clips follow the motion.
Rotated, sheared or perspective rectangles become vector paths. Drawing scales
are respected. Affine transformations preserve cubic curves; perspective curves
are subdivided with a bounded approximation error. Absolute clip paths and moving
origins cannot generally be animated by ASS, so Compact retains static segments
where needed. It may therefore produce as many events as Exact.

Full geometry apply currently requires static source geometry. Geometry animations,
animated clips, unsupported mixed text/drawing runs and ambiguous clip conversions
are rejected with a diagnostic, rather than partially rewriting a subtitle. The
font measurement and drawing restrictions of the perspective tool also apply.
Similarity also checks rotation and scale across visible text runs: different
per-run geometry is rejected instead of replacing it with one uniform transform.
Other supported local styling and resets with matching geometry retain their scope.
Disable smoothing, trajectory stabilization and border/shadow/blur scaling when
using full geometry apply; the established Translation/Similarity path retains
those options for subtitles without additional absolute or perspective geometry.

Standard source `\fad`/`\fade` and color/opacity `\t` animations keep their original
event timeline when output is split into new events. Implicit transform durations
are made explicit before the split; supported acceleration values are preserved.
Nested or unsupported animations and karaoke must be made static before applying
tracking. Unsupported forms reject the complete plan with a diagnostic.

The same coverage rule applies to every model: a failed gap bounded by good samples
holds the previous good pose; missing coverage at either end prevents Apply. All
selected lines are planned before mutation, and more than 100 generated events
requires the existing event-count confirmation.
