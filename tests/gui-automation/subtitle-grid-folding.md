# Subtitle grid folding GUI regression

Run from the repository root after building the `Aegisub` target. The scenarios
use real GUI command dispatch and Lua validators which inspect the current
document's dialogue text and ExtraData. A validator returning false fails the
scenario; checking that a command merely exists is not sufficient.

The GUI scenario protocol does not support opening a document in `run` mode,
and `run_automation` uses its own session. Instead, the fixture is loaded as the
only global script in an isolated build-directory profile. The first command,
`am/reload/autoload`, loads it synchronously before any macro is invoked. This
avoids racing the normal asynchronous startup script loader. Its first macro
creates five deterministic subtitle lines in the main window.

```powershell
New-Item -ItemType Directory -Force -Path build-dir/folding-scenario-profile/user/automation/autoload | Out-Null
Copy-Item -LiteralPath tests/gui-automation/subtitle-grid-folding.lua -Destination build-dir/folding-scenario-profile/user/automation/autoload/subtitle-grid-folding.lua
'{"Path":{"Automation":{"Autoload":"?user/automation/autoload/"}}}' | Set-Content -LiteralPath build-dir/folding-scenario-profile/user/config.json
```

For routine regression, pass the following arguments to the newly built
Aegisub executable in a hidden process (PowerShell `Start-Process
-WindowStyle Hidden`). Use a process supervisor with a finite **250 second**
total timeout. Treat a timeout as failure and stop that test process. The
background scenario has a **5 second** per-step limit and a **200 second**
total scenario budget. It uses document duplicate commands and has no native
input, focus, capture, or clipboard steps.

```text
--gui-test run --scenario tests/gui-automation/subtitle-grid-folding-background.json --profile-dir build-dir/folding-scenario-profile --artifacts build-dir/folding-background-artifacts
```

The run checks persistent creation, expansion and collapse, real undo/redo,
Lua boundary replacement, complete-group duplication with a fresh ID,
partial-boundary duplication without a fold marker, and clear-all undo while
preserving unrelated ExtraData. It leaves `result.json` and runtime logs in
the explicit artifacts directory. Final undo steps restore the initial clean
document and assert that no earlier undo state remains, so normal shutdown
does not require a save prompt.

`subtitle-grid-folding-hidden-join.json` is a separate background regression
for a selection callback that occurs before its document commit. It selects
hidden group members plus the ungrouped following line, makes that following
line active, then invokes Join Lines / Keep First. Deleting the group end
while activating the hidden surviving member must clear the boundaries and
preserve the expected subtitle text. Undo restores the original group and
then the clean initial document. This scenario has **14 steps**, a **70 second**
scenario budget and a recommended **110 second** process limit. Use the same
Lua profile setup and change the scenario and artifacts arguments:

```text
--gui-test run --scenario tests/gui-automation/subtitle-grid-folding-hidden-join.json --profile-dir build-dir/folding-scenario-profile --artifacts build-dir/folding-hidden-join-artifacts
```

`subtitle-grid-folding.json` is a separate, explicit opt-in clipboard scenario.
It substitutes real Copy Lines/Paste Lines commands and checks the pasted
boundary IDs and preserved data. Its 44 steps have a **220 second** total
scenario budget; use a **270 second** process limit. Run it only when
clipboard use is acceptable. It must not run as part of background validation
during normal desktop use. The real copy command also follows the focused
control, so the Grid must own focus when manually exercising that scenario.

These assertions complement the C++ mapping and ASS save/reload tests. They do
not verify native arrow hit testing, exact scrollbar geometry, or painted
active-row borders; those remain separate GUI inspection checks.

The background scenario and hidden-join scenario were verified in
`RelWithDebInfo`: **40/40** and **14/14** steps passed, respectively, and both
owned test processes exited with code **0**. The clipboard scenario remains
unverified and opt-in.
