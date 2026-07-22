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

The Windows driver in `tools/skia-audio-uia.cs` is intentionally a separate
black-box adapter. Its reusable protocol, UIA discovery, fatal-dialog, and PNG
helpers live in `tools/gui-automation-driver`; the Skia entry retains only
audio/video-specific input scenarios and guarded `SendInput`. It must not reach
into Aegisub internals. `run-aegisub-uia-correctness-smoke` covers ready/PID
validation, main-window discovery, focus, a UIA `TogglePattern` state change on
the non-document `Show Original` control, fatal-dialog checks, PNG capture, and
clean exit. The smoke opens the checked-in subtitle fixture so the control is
deterministically available. Future AX and AT-SPI drivers consume the same
host/result protocol without changing scenario semantics.
