# GUI automation protocol

The executable has one automation contract for every platform:

```text
Aegisub --headless run --scenario <scenario.json>
Aegisub --gui-test run --scenario <scenario.json>
Aegisub --gui-test host [--open <file> ...] [--artifacts <directory>]
```

Scenarios are versioned JSON. Paths in `resources` are relative to the scenario
file unless replaced by `--input name=value`; no machine-specific path belongs
in a checked-in scenario.

## Host capabilities (v1)

| Action | `--headless run` | `--gui-test run` |
| --- | --- | --- |
| `run_automation` | yes | yes |
| `command` | no (schema error) | yes (wx main thread) |
| `query` / `assert` | no (schema error) | yes, command registry state |
| `wait` | no (schema error) | yes, wx event pump |
| `capture` | no (schema error) | best-effort wx client PNG |

v1 headless supports only `run_automation`. Headless does not initialize the
command system and does not execute `command` steps. The `CMD_HEADLESS_SAFE` /
`CommandExecutionScope::HeadlessSafe` markers exist for a future headless
command path once a shared non-UI `agi::Context` exists; they are not active in
v1. `--gui-test` creates the normal wx frame and runs `command` actions on the
wx main thread so the same command IDs can be exercised on Windows, macOS, and
Linux.

`load_global_scripts` is an explicit scenario root option and defaults to
`false`; global scripts are loaded only when a GUI scenario opts in. `query`
and `assert` currently use `target: "command"` with the command `id`.
`capture` accepts a plain artifact file name and always writes beneath the
run's artifacts directory. It fails rather than publishing an all-black image
when the platform does not expose a printable wx client surface. Windows CI
uses the external UIA correctness driver for reliable full-window evidence.

## Timeouts

Supervisor budgets are separate:

- **startup** — fixed floor until the worker writes `runtime-ready`
- **step** — scenario `default_timeout_ms` (default **120000**) per step after
  `step-N-running`; pure-Lua fail-fast cases should set a shorter value
  explicitly
- **teardown** — fixed floor after `worker-scenario-complete` (or the last
  `step-N-done` as defense in depth) until `worker-shutdown-complete`. Covers
  worker runtime teardown (managed/CoreCLR unload, font cache shutdown,
  dispatch cleanup) and `result.json` persistence. It does **not** cover the
  parent process deleting the temporary profile after the supervisor returns.
- **exit** — short floor after `worker-shutdown-complete` until process exit

`worker-scenario-complete` is written after scenario execution returns — including
schema/runtime failures that never emit `step-N-done` — and **before** the
worker destroys `HeadlessRuntimeEnvironment`.

## GUI-test host seam

`--gui-test host` is the black-box attach seam. It writes `ready.json` after
the real frame exists and keeps the process alive. An external driver waits for
that file, attaches through its platform accessibility API, performs UIA/AX/
AT-SPI operations, and closes the process. `--gui-test run` writes both
`ready.json` and `result.json`, then exits with the scenario result code.

Each run owns an isolated profile containing `user`, `local`, and `artifacts`.
The default temporary profile is deleted after success and retained after
failure; `--keep-profile` or an explicit `--profile-dir` retains it for local
diagnostics. `startup.log`, `ready.json`, `result.json`, UI trees, screenshots,
and driver logs belong under the artifacts directory.

The Windows driver in `tests/gui-automation/skia-audio-uia.cs` is intentionally a separate
black-box adapter. Its reusable protocol, UIA discovery, fatal-dialog, and PNG
helpers live in `tests/gui-automation/driver`; the Skia entry retains only
audio/video-specific input scenarios and guarded `SendInput`. It must not reach
into Aegisub internals. The external UIA correctness driver covers ready/PID
validation, main-window discovery, focus, a UIA `TogglePattern` state change on
the non-document `Show Original` control, and font selection through standard
`ValuePattern`, `ExpandCollapsePattern`, and `SelectionItemPattern` operations
in both Style Editor and the custom Select Font dialog. The font phase exercises
both an `EM_REPLACESEL` retype and a `WM_CHAR` retype; the latter is dispatched
through the native EDIT wndproc and triggers the native `CBS_DROPDOWN`
auto-select path, specifically verifying that a backward highlight jump (new
match sorting above the previous match, e.g. `ahom` -> Tahoma then `nsol` ->
Consolas) lands on the correct row. It enables automatic font-list expansion,
requires typing to open the list without an explicit expand action, and commits
the highlighted match with Enter to verify a keyboard-only selection path.

Audio performance runs may explicitly isolate `Audio/Provider`,
`Audio/Cache/Type`, `Audio/Player`, and waveform/spectrum selection with the
driver's `--audio-provider`, `--audio-cache-type`, `--audio-player`, and
`--audio-view` options. `--cursor-time on|off` isolates the mouse time-label
setting. The explicit `audio-cursor-state-matrix` scenario uses guarded
foreground input to exercise native enter/leave behavior and requires
`--allow-global-input`; its trace assertion checks mouse -> playback -> restored
mouse -> leave -> re-enter -> resize ordering for both waveform and spectrum.
It also rejects any visible-content request, lookup, tile draw, or upload on a
cursor-only frame. `audio_display_snapshot` records `cursor_source`,
`cursor_position_ms`, `cursor_device_x`, and `cursor_label_visible` so the
assertion checks the state actually presented rather than only the input sent.
The `audio-playback-marker-drag` and `audio-spectrum-playback-marker-drag`
scenarios also require guarded foreground input. They create a deterministic
selection, keep real playback active while repeatedly dragging its marker, and
use the snapshot `marker_revision` to prove the presented marker state changed
while the cursor source remained playback. Their trace gate also requires the
cursor-only frames between marker updates to remain free of content work and
rejects new worker payload builds or GPU tile uploads caused by the marker drag.
The `audio-middle-seek-cursor` and `audio-spectrum-middle-seek-cursor`
scenarios exercise native middle-button state, an inside release, an
outside-canvas release detected by the widget timer, and pointer re-entry. They
require a real video provider and `--allow-global-input`; headless fake clocks
cannot reproduce Windows button state, capture/release delivery, or the
presented mouse/video-position-marker ordering. Their trace gate requires two
preview/commit intervals, immediate mouse-cursor restoration after an inside
release, `none` after an outside release, mouse restoration on re-entry, and no
content request, lookup, tile draw, upload, or worker build on retained
cursor/marker overlay frames. Absolute pointer input is normalized against the
virtual desktop and uses `MOUSEEVENTF_VIRTUALDESK`; the re-entry phase also
checks that the actual system cursor lands on the Audio Display HWND so
multi-monitor coordinate virtualization cannot produce a false result.
The `audio-waveform-focus-colour` and `audio-spectrum-focus-colour` scenarios
exercise native child-window focus and a running Preferences transaction. They
move focus Audio Display -> subtitle edit -> Audio Display -> subtitle edit,
open the real Preferences dialog through UIA, select the deterministic
waveform/spectrum scheme through the visible wxPropertyGrid owner-drawn choice,
and invoke Apply. Headless mode cannot reproduce native focus events, the modal
dialog, pending option values, or the Apply-time option subscriptions. The
trace gate requires every focus transition to advance `chrome_revision`, the
Apply operation to advance `presentation_revision`, and analysis generation,
CPU/FFT misses, raw/payload worker builds, and GPU content-tile uploads to stay
unchanged. Waveform permits no palette upload; spectrum permits only the small
bounded set of newly used palette textures. These scenarios use background
native window messages plus UIA and do not require `--allow-global-input`.
The `audio-waveform-dpi-transition` and `audio-spectrum-dpi-transition`
scenarios enumerate the attached displays, move the real Aegisub main window
to a display with a different effective DPI, wait for the child Audio Display
to settle, and then restore the original display. They require the Skia Audio
Display runtime opt-in but do not use global pointer input. The trace gate uses
the `audio_display_snapshot.content_scale` field to require an
initial -> target -> restored sequence with complete content at every scale;
analysis, presentation, and Chrome revisions must advance at both transitions,
and replacement worker payloads and GPU content tiles must be published instead
of reusing stale device-scale content. If all attached displays have the same
effective DPI, the driver reports
`uia.dpi_transition.trace_validation=not-run:no-different-dpi-monitor` and does
not emit a passing trace-validation result. It never changes the user's system
display scaling to manufacture this condition.
The `audio-waveform-runtime-fallback` and
`audio-spectrum-runtime-fallback` scenarios exercise failure after a real Skia
widget has already presented content. They require
`AEGISUB_SKIA_AUDIO_FAILURE_INJECTION=flush-submit` together with a bounded
positive `AEGISUB_SKIA_AUDIO_FAILURE_AFTER_CONTENT_FRAMES` value. The latter
keeps startup frames healthy and arms the existing injection only after the
configured number of successful content frames. The driver first performs a
background timeline drag so the Skia widget owns a non-zero viewport which
bypasses the slot's ordinary wheel-delta bookkeeping. It then generates
background mouse-cursor frames, discovers the native runtime-error dialog,
invokes its real `Switch to wx` action through UIA, and waits for the Skia
`wxGLCanvas` to be replaced by the compatibility widget. Its trace gate requires
at least the configured number of successful Skia snapshots followed by wx
frame `1`, rejects any later Skia snapshot, and requires that first wx frame to
render the same waveform/spectrum mode with real bitmap-cache activity and the
same logical left viewport after normalizing the Skia device-column value by
`content_scale`. This is
the post-startup fallback contract; the existing immediate injection remains
the startup automatic-fallback seam.
The playback-scroll scenarios keep real audio output active
while sending background wheel input to the Audio Display. Headless playback
uses a fake audio clock and therefore cannot replace this GUI seam for
provider-read, output-fill, or Audio Display worker contention measurements.
It also verifies explicit selection commit, fatal dialogs, PNG capture, and
clean exit. The smoke opens the checked-in subtitle
fixture so the controls are deterministically available; the font-dialog phase
also requires the configured subtitles-provider runtime to be staged beside the
built executable. Future AX and AT-SPI drivers consume the same host/result
protocol without changing scenario semantics.
