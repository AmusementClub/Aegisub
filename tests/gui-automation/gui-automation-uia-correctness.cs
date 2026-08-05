#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

try
{
    const string workerArgument = "--internal-uia-worker";
    const string heartbeatArgument = "--internal-watchdog-heartbeat";
    var workerArguments = args.ToList();
    var isWorker = workerArguments.Remove(workerArgument);
    string? heartbeatPath = null;
    var heartbeatIndex = workerArguments.IndexOf(heartbeatArgument);
    if (heartbeatIndex >= 0)
    {
        if (heartbeatIndex + 1 >= workerArguments.Count)
            throw new ArgumentException($"Missing value for {heartbeatArgument}");
        heartbeatPath = workerArguments[heartbeatIndex + 1];
        workerArguments.RemoveRange(heartbeatIndex, 2);
    }

    if (!isWorker)
        return RunSupervised(args);

    WorkerWatchdog.Initialize(heartbeatPath);
    return Run(workerArguments.ToArray());
}
catch (Exception error)
{
    Console.Error.WriteLine($"uia.correctness.error={error.GetType().Name}:{error.Message}");
    return 1;
}

static int RunSupervised(string[] args)
{
    var operationTimeoutSeconds = ReadPositiveOption(args, "--timeout-seconds", 30);
    var stallTimeout = TimeSpan.FromSeconds(Math.Clamp(operationTimeoutSeconds, 10, 30));
    var totalTimeout = TimeSpan.FromSeconds(Math.Max(120, operationTimeoutSeconds * 6));
    var watchdogRoot = Path.Combine(
        Path.GetTempPath(),
        "aegisub-uia-watchdog-" + Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(watchdogRoot);
    var heartbeatPath = Path.Combine(watchdogRoot, "heartbeat.txt");
    File.WriteAllText(heartbeatPath, "supervisor-started");

    try
    {
        var startInfo = CreateWorkerStartInfo(args, heartbeatPath);
        using var worker = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Could not start the UIA worker");
        worker.OutputDataReceived += (_, eventArgs) =>
        {
            if (eventArgs.Data is not null)
                Console.Out.WriteLine(eventArgs.Data);
        };
        worker.ErrorDataReceived += (_, eventArgs) =>
        {
            if (eventArgs.Data is not null)
                Console.Error.WriteLine(eventArgs.Data);
        };
        worker.BeginOutputReadLine();
        worker.BeginErrorReadLine();

        var started = Stopwatch.StartNew();
        while (!worker.WaitForExit(250))
        {
            var lastHeartbeat = File.GetLastWriteTimeUtc(heartbeatPath);
            if (DateTime.UtcNow - lastHeartbeat > stallTimeout)
            {
                var stage = ReadWatchdogStage(heartbeatPath);
                KillWorkerTree(worker);
                throw new TimeoutException(
                    $"UIA worker stopped making progress for {stallTimeout.TotalSeconds:0} seconds " +
                    $"at stage '{stage}'");
            }
            if (started.Elapsed > totalTimeout)
            {
                var stage = ReadWatchdogStage(heartbeatPath);
                KillWorkerTree(worker);
                throw new TimeoutException(
                    $"UIA worker exceeded the {totalTimeout.TotalSeconds:0}-second total limit " +
                    $"at stage '{stage}'");
            }
        }

        worker.WaitForExit();
        return worker.ExitCode;
    }
    finally
    {
        try
        {
            Directory.Delete(watchdogRoot, true);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}

static ProcessStartInfo CreateWorkerStartInfo(string[] args, string heartbeatPath)
{
    var processPath = Environment.ProcessPath
        ?? throw new InvalidOperationException("Could not locate the current UIA driver process");
    var startInfo = new ProcessStartInfo
    {
        FileName = processPath,
        UseShellExecute = false,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
    };
    if (string.Equals(
            Path.GetFileNameWithoutExtension(processPath),
            "dotnet",
            StringComparison.OrdinalIgnoreCase))
    {
        var assemblyPath = Assembly.GetEntryAssembly()?.Location;
        if (string.IsNullOrWhiteSpace(assemblyPath))
            throw new InvalidOperationException("Could not locate the UIA driver assembly");
        startInfo.ArgumentList.Add(assemblyPath);
    }
    startInfo.ArgumentList.Add("--internal-uia-worker");
    startInfo.ArgumentList.Add("--internal-watchdog-heartbeat");
    startInfo.ArgumentList.Add(heartbeatPath);
    foreach (var argument in args)
        startInfo.ArgumentList.Add(argument);
    return startInfo;
}

static int ReadPositiveOption(string[] args, string option, int defaultValue)
{
    for (var index = 0; index < args.Length; ++index)
    {
        if (!string.Equals(args[index], option, StringComparison.Ordinal))
            continue;
        if (++index >= args.Length || !int.TryParse(args[index], out var value) || value <= 0)
            throw new ArgumentException($"{option} must be positive");
        return value;
    }
    return defaultValue;
}

static string ReadWatchdogStage(string heartbeatPath)
{
    try
    {
        return File.ReadAllText(heartbeatPath).Trim();
    }
    catch (IOException)
    {
        return "unknown";
    }
}

static void KillWorkerTree(Process worker)
{
    try
    {
        worker.Kill(entireProcessTree: true);
        worker.WaitForExit(5000);
    }
    catch (InvalidOperationException)
    {
    }
}

static int Run(string[] args)
{
    WorkerWatchdog.Stage("parse-arguments");
    string? executable = null;
    string? artifacts = null;
    var openFiles = new List<string>();
    var timeoutSeconds = 30;
    for (var index = 0; index < args.Length; ++index)
    {
        var argument = args[index];
        string Value()
        {
            if (++index >= args.Length)
                throw new ArgumentException($"Missing value for {argument}");
            return args[index];
        }
        switch (argument)
        {
            case "--help":
                Console.WriteLine("--exe PATH [--open FILE] [--artifacts PATH] [--timeout-seconds N]");
                return 0;
            case "--exe":
                executable = Path.GetFullPath(Value());
                break;
            case "--artifacts":
                artifacts = Path.GetFullPath(Value());
                break;
            case "--open":
                var openFile = Path.GetFullPath(Value());
                if (!File.Exists(openFile))
                    throw new FileNotFoundException("The UIA input subtitle was not found", openFile);
                openFiles.Add(openFile);
                break;
            case "--timeout-seconds":
                timeoutSeconds = int.Parse(Value());
                if (timeoutSeconds <= 0)
                    throw new ArgumentException("--timeout-seconds must be positive");
                break;
            default:
                throw new ArgumentException($"Unknown option: {argument}");
        }
    }
    if (string.IsNullOrWhiteSpace(executable))
        throw new ArgumentException("--exe is required");
    if (!File.Exists(executable))
        throw new FileNotFoundException("Aegisub executable was not found", executable);

    var root = Path.Combine(Path.GetTempPath(), "aegisub-uia-correctness-" + Guid.NewGuid().ToString("N"));
    var output = artifacts ?? Path.Combine(root, "artifacts");
    var profile = artifacts is null
        ? Path.Combine(root, "profile")
        : Path.Combine(output, "profile");
    PrepareRunArtifacts(output, profile);
    PrepareFontSelectorProfile(profile);
    Console.WriteLine($"uia.correctness.artifacts={output}");
    var startInfo = new ProcessStartInfo
    {
        FileName = executable,
        WorkingDirectory = Path.GetDirectoryName(executable) ?? Environment.CurrentDirectory,
        UseShellExecute = false,
    };
    startInfo.ArgumentList.Add("--gui-test");
    startInfo.ArgumentList.Add("host");
    startInfo.ArgumentList.Add("--profile-dir");
    startInfo.ArgumentList.Add(profile);
    startInfo.ArgumentList.Add("--artifacts");
    startInfo.ArgumentList.Add(output);
    foreach (var openFile in openFiles)
    {
        startInfo.ArgumentList.Add("--open");
        startInfo.ArgumentList.Add(openFile);
    }
    var processStartedAtUtc = DateTime.UtcNow;
    using var process = Process.Start(startInfo)
        ?? throw new InvalidOperationException("Could not start Aegisub");

    var passed = false;
    try
    {
        WorkerWatchdog.Stage("wait-for-ready");
        var readyPath = Path.Combine(output, "ready.json");
        var ready = AutomationProtocol.WaitForReadyArtifact(
            readyPath, process, TimeSpan.FromSeconds(timeoutSeconds));
        if (File.GetLastWriteTimeUtc(readyPath) < processStartedAtUtc.AddSeconds(-1))
            throw new InvalidOperationException("ready.json predates this UIA run");
        if (!PathsEqual(ready.Artifacts, output))
            throw new InvalidOperationException("ready.json points at a different artifacts directory");
        Console.WriteLine($"uia.correctness.ready_pid={ready.ProcessId}");
        WorkerWatchdog.Stage("discover-main-window");
        var window = UiaDriver.WaitForMainWindow(process, TimeSpan.FromSeconds(timeoutSeconds));
        NativeWindowSearch.ThrowIfFatalDialog(process);
        UiaDriver.FocusAndVerify(window, process, TimeSpan.FromSeconds(5));

        WorkerWatchdog.Stage("verify-toggle");
        var toggle = UiaDriver.FindEnabledToggle(
                window,
                ControlType.CheckBox,
                "Show Original",
                "显示原文")
            ?? throw new InvalidOperationException(
                "The UIA non-document state-changing checkbox was not found");
        var toggleBefore = UiaDriver.ReadToggleState(toggle);
        Console.WriteLine($"uia.correctness.toggle_name={toggle.Current.Name}");
        Console.WriteLine($"uia.correctness.toggle_id={toggle.Current.AutomationId}");
        Console.WriteLine($"uia.correctness.toggle_before={toggleBefore}");
        UiaDriver.Toggle(toggle);
        var toggleAfter = UiaDriver.WaitForToggleStateChange(
            toggle, toggleBefore, TimeSpan.FromSeconds(5));
        Console.WriteLine($"uia.correctness.toggle_after={toggleAfter}");
        if (toggleAfter == toggleBefore)
            throw new InvalidOperationException("UIA toggle state did not change");
        UiaDriver.Toggle(toggle);
        var toggleRestored = UiaDriver.WaitForToggleStateChange(
            toggle, toggleAfter, TimeSpan.FromSeconds(5));
        if (toggleRestored != toggleBefore)
            throw new InvalidOperationException(
                $"UIA toggle did not restore its initial state: {toggleRestored}");
        Console.WriteLine("uia.correctness.observable_state_change=true");
        NativeWindowSearch.ThrowIfFatalDialog(process);

        WorkerWatchdog.Stage("verify-font-selectors");
        VerifyFontSelectors(
            window,
            process,
            output,
            TimeSpan.FromSeconds(timeoutSeconds));
        NativeWindowSearch.ThrowIfFatalDialog(process);

        WorkerWatchdog.Stage("capture-main-window");
        var screenshot = Path.Combine(output, "correctness-main-window.png");
        var capture = ScreenCapture.SaveWindowPng(window, screenshot);
        Console.WriteLine($"uia.correctness.capture={screenshot}");
        Console.WriteLine(
            $"uia.correctness.capture_evidence={capture.Width}x{capture.Height};" +
            $"non_black_pixels={capture.NonBlackPixelCount};" +
            $"minimum={capture.MinimumNonBlackPixelCount}");
        WorkerWatchdog.Stage("close-aegisub");
        UiaDriver.RequestCleanClose(process, TimeSpan.FromSeconds(15));
        Console.WriteLine("uia.correctness.clean_exit=true");
        passed = true;
    }
    finally
    {
        if (!process.HasExited)
        {
            process.Kill(true);
            process.WaitForExit(5000);
        }
        if (passed && Directory.Exists(profile))
            Directory.Delete(profile, true);
        if (passed && artifacts is null && Directory.Exists(root))
            Directory.Delete(root, true);
    }
    return 0;
}

static void VerifyFontSelectors(
    AutomationElement mainWindow,
    Process process,
    string output,
    TimeSpan timeout)
{
    WorkerWatchdog.Stage("open-styles-manager");
    var stylesCommand = UiaDriver.FindEnabledInvokableButtonByAutomationId(
            mainWindow,
            "Item 5014",
            "Open the styles manager")
        ?? throw new InvalidOperationException("The Styles Manager command was not found");
    var stylesInvoke = Task.Run(() => UiaDriver.Invoke(stylesCommand));
    var stylesManager = WaitForWindow(process, timeout, "Styles Manager");

    WorkerWatchdog.Stage("open-style-editor");
    var currentStyle = FindSelectedListItem(stylesManager, "Default")
        ?? throw new InvalidOperationException(
            "The selected current-script Default style was not found");
    if (!currentStyle.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var currentPattern))
        throw new InvalidOperationException("The current style is not UIA-selectable");
    ((SelectionItemPattern)currentPattern).Select();

    var edit = UiaDriver.FindEnabledInvokableButton(stylesManager, "Edit")
        ?? throw new InvalidOperationException(
            "The current-script Style Editor command was not found");
    var editInvoke = Task.Run(() => UiaDriver.Invoke(edit));
    var styleEditor = WaitForDialogWithProviderCheck(
        process, timeout, "Style Editor");
    WorkerWatchdog.Stage("verify-style-editor-font-selector");
    VerifyFontSelector(
        styleEditor,
        process,
        output,
        "style_editor",
        timeout);
    CloseDialog(styleEditor, "Cancel");
    WaitForTask(editInvoke, timeout, "Style Editor did not close");

    CloseDialog(stylesManager, "Close");
    WaitForTask(stylesInvoke, timeout, "Styles Manager did not close");

    WorkerWatchdog.Stage("open-select-font-dialog");
    process.Refresh();
    if (process.MainWindowHandle == 0)
        throw new InvalidOperationException(
            "Aegisub lost its main window after Styles Manager closed");
    mainWindow = AutomationElement.FromHandle(process.MainWindowHandle);
    var fontFaceCommand = UiaDriver.FindEnabledInvokableButtonByAutomationId(
            mainWindow,
            "6000",
            "Font Face")
        ?? throw new InvalidOperationException("The Font Face command was not found");
    Console.WriteLine("uia.correctness.font_face_entry=button");
    var fontFaceInvoke = Task.Run(() => UiaDriver.Invoke(fontFaceCommand));
    var selectFont = WaitForDialogWithProviderCheck(
        process, timeout, "Select Font");
    WorkerWatchdog.Stage("verify-select-font-selector");
    VerifyFontSelector(
        selectFont,
        process,
        output,
        "select_font",
        timeout);
    CloseDialog(selectFont, "Cancel");
    WaitForTask(fontFaceInvoke, timeout, "Select Font did not close");
    Console.WriteLine("uia.correctness.font_selectors=true");
}

static void VerifyFontSelector(
    AutomationElement dialog,
    Process process,
    string output,
    string label,
    TimeSpan timeout)
{
    WorkerWatchdog.Stage($"{label}:discover-combo");
    var combo = FindEditableComboBox(dialog)
        ?? throw new InvalidOperationException(
            $"The editable font selector was not found in {dialog.Current.Name}");
    if (!combo.TryGetCurrentPattern(ValuePattern.Pattern, out var valueObject))
        throw new InvalidOperationException("The font selector has no ValuePattern");
    var value = (ValuePattern)valueObject;
    if (value.Current.IsReadOnly)
        throw new InvalidOperationException("The font selector is read-only");
    if (!combo.TryGetCurrentPattern(
            ExpandCollapsePattern.Pattern,
            out var expandObject))
        throw new InvalidOperationException(
            "The font selector has no ExpandCollapsePattern");
    var expand = (ExpandCollapsePattern)expandObject;

    UiaDriver.FocusAndVerify(dialog, process, TimeSpan.FromSeconds(5));
    combo.SetFocus();
    // Each pass uses a distinct contains-hit in a different list region so a
    // retype cannot pass by reusing leftover highlight state, and so native
    // autoselect anchored on the previous match is exposed if not reset.
    const string query1 = "rial";
    const string expected1 = "Arial";
    // pass2 (EM_REPLACESEL path): short, unambiguous rank-2 contains hit,
    // guaranteed installed on Windows, far from the Arial region.
    const string query2 = "nsol";
    const string expected2 = "Consolas";
    var comboHwnd = new IntPtr(combo.Current.NativeWindowHandle);

    // Cycle the user reported:
    //   type → open list → close list → clear → retype (other font) → open list
    // Edit must stay as the typed query unless the user explicitly selects;
    // list caret must jump to the contains match each time the list opens.
    ClearComboEdit(combo);
    WaitForValue(combo, string.Empty, timeout);

    // --- pass 1: type + automatic expand ---
    TypeIntoCombo(combo, query1, charByChar: true);
    ExpectEdit(combo, query1, label, "after_type", timeout);
    WaitForComboDropState(comboHwnd, expectedDropped: true, timeout);
    Console.WriteLine($"uia.correctness.{label}_auto_expanded=true");
    var samples1 = SampleHighlightTimeline(
        combo, query1, expected1, $"{label}_pass1");
    AssertHighlightSettled(dialog, samples1, query1, expected1, label, "pass1");
    Console.WriteLine(
        $"uia.correctness.{label}_pass1_timeline={FormatHighlightTimeline(samples1)}");

    // Close without selecting — edit must remain the typed query.
    CollapseCombo(expand);
    Thread.Sleep(200);
    ExpectEdit(combo, query1, label, "after_close", timeout);
    if (NativeKeyboard.IsComboDropped(comboHwnd))
        throw new InvalidOperationException(
            $"{dialog.Current.Name} drop-down still open after close");

    // --- clear, retype a different font, verify the expanded list again ---
    ClearComboEdit(combo);
    WaitForValue(combo, string.Empty, timeout);
    Console.WriteLine($"uia.correctness.{label}_after_clear=");

    TypeIntoCombo(combo, query2, charByChar: true);
    ExpectEdit(combo, query2, label, "after_retype", timeout);

    expand.Expand();
    var samples2 = SampleHighlightTimeline(
        combo, query2, expected2, $"{label}_pass2");
    AssertHighlightSettled(dialog, samples2, query2, expected2, label, "pass2");
    Console.WriteLine(
        $"uia.correctness.{label}_pass2_timeline={FormatHighlightTimeline(samples2)}");

    // Close and start fresh so pass2b begins from a closed state, matching the
    // user's "close → clear → retype" cycle.
    CollapseCombo(expand);
    Thread.Sleep(200);
    if (NativeKeyboard.IsComboDropped(comboHwnd))
        throw new InvalidOperationException(
            $"{dialog.Current.Name} drop-down still open after pass2 close");

    // --- pass 2b: WM_CHAR (native-key path) backward-jump retype ---
    // EM_REPLACESEL writes the edit directly and bypasses the native CBS_DROPDOWN
    // auto-select that fires on real WM_CHAR keystrokes. pass2b types via WM_CHAR
    // to the EDIT child, which IS dispatched through the native EDIT wndproc and
    // triggers auto-select — reproducing the user's real-keyboard path without
    // the foreground-focus flakiness of SendInput. The bug specifically needs a
    // BACKWARD jump: type a query whose match is far down the list (queryFar),
    // then type a query whose match is ABOVE it (queryNear). Native auto-select
    // only searches forward from the caret, so without a caret reset the
    // highlight stays stranded on queryFar and never reaches queryNear. This
    // mirrors the user's "skip → fot" cycle (fot's first match sorts before
    // skip's match) using robust core Windows fonts.
    const string queryFar = "ahom";   // -> Tahoma (T region)
    const string expectedFar = "Tahoma";
    const string queryNear = "nsol";  // -> Consolas (C region, before Tahoma)
    const string expectedNear = "Consolas";

    ClearComboEdit(combo);
    WaitForValue(combo, string.Empty, timeout);
    Console.WriteLine($"uia.correctness.{label}_pass2b_after_clear=");

    // Step 1: WM_CHAR type the far query, expand, verify it lands there.
    NativeKeyboard.TypeCharsViaComboKeys(comboHwnd, queryFar);
    ExpectEdit(combo, queryFar, label, "pass2b_after_far_retype", timeout);
    expand.Expand();
    var samplesFar = SampleHighlightTimeline(
        combo, queryFar, expectedFar, $"{label}_pass2b_far");
    AssertHighlightSettled(dialog, samplesFar, queryFar, expectedFar, label, "pass2b_far");
    Console.WriteLine(
        $"uia.correctness.{label}_pass2b_far_timeline={FormatHighlightTimeline(samplesFar)}");
    CollapseCombo(expand);
    Thread.Sleep(200);

    // Step 2: clear and type the near query (above the far match) via WM_CHAR,
    // then expand. Highlight MUST jump backward to the near match.
    ClearComboEdit(combo);
    WaitForValue(combo, string.Empty, timeout);
    NativeKeyboard.TypeCharsViaComboKeys(comboHwnd, queryNear);
    ExpectEdit(combo, queryNear, label, "pass2b_after_near_retype", timeout);
    expand.Expand();
    var samplesNear = SampleHighlightTimeline(
        combo, queryNear, expectedNear, $"{label}_pass2b_near");
    AssertHighlightSettled(dialog, samplesNear, queryNear, expectedNear, label, "pass2b_near");
    Console.WriteLine(
        $"uia.correctness.{label}_pass2b_near_timeline={FormatHighlightTimeline(samplesNear)}");
    Console.WriteLine(
        $"uia.correctness.{label}_pass2b_backward_jump_verified={expectedNear}");

    CollapseCombo(expand);

    // --- keyboard-only selection after automatic expansion ---
    ClearComboEdit(combo);
    WaitForValue(combo, string.Empty, timeout);
    NativeKeyboard.TypeCharsViaComboKeys(comboHwnd, query2);
    ExpectEdit(combo, query2, label, "before_keyboard_commit", timeout);
    WaitForComboDropState(comboHwnd, expectedDropped: true, timeout);
    WaitForHighlightedListItem(combo, expected2, timeout);
    NativeKeyboard.CommitHighlightedComboItem(comboHwnd);
    var keyboardCommittedValue = WaitForValue(combo, expected2, timeout);
    WaitForComboDropState(comboHwnd, expectedDropped: false, timeout);
    Console.WriteLine(
        $"uia.correctness.{label}_keyboard_selection={keyboardCommittedValue}");

    // --- explicit selection commit ---
    // Selecting a real item after contains matching must replace the typed
    // query with the chosen family. Use native list mouse messages because
    // wx/MSW exposes off-caret combo items through UIA but Select() can commit
    // the provisional caret instead of the requested item.
    ClearComboEdit(combo);
    WaitForValue(combo, string.Empty, timeout);
    TypeIntoCombo(combo, query1, charByChar: true);
    ExpectEdit(combo, query1, label, "before_explicit_select", timeout);
    expand.Expand();
    WaitForHighlightedListItem(combo, expected1, timeout);
    const string committedFont = "Arial Black";
    NativeKeyboard.ClickComboItem(comboHwnd, committedFont);
    Thread.Sleep(250);
    var postSelectValue = NativeKeyboard.GetComboEditText(comboHwnd);
    Console.WriteLine(
        $"uia.correctness.{label}_post_select=" +
        $"edit='{postSelectValue}';" +
        $"native='{NativeKeyboard.GetComboHighlightText(comboHwnd) ?? string.Empty}';" +
        $"dropped={NativeKeyboard.IsComboDropped(comboHwnd)}");
    var committedValue = WaitForValue(combo, committedFont, timeout);
    if (string.IsNullOrEmpty(committedValue))
        throw new InvalidOperationException(
            $"{dialog.Current.Name} cleared the explicitly selected font");
    Console.WriteLine(
        $"uia.correctness.{label}_explicit_selection={committedValue}");

    // A manual selection or copy can be followed by an unchanged edit
    // notification. It must not schedule another automatic expansion.
    CollapseCombo(expand);
    WaitForComboDropState(comboHwnd, expectedDropped: false, timeout);
    var committedEditHwnd = NativeKeyboard.GetComboEditHwnd(comboHwnd);
    if (committedEditHwnd == IntPtr.Zero)
        throw new InvalidOperationException("Font combo edit has no native HWND");
    NativeKeyboard.SetEditText(committedEditHwnd, committedValue);
    Thread.Sleep(400);
    if (NativeKeyboard.IsComboDropped(comboHwnd))
        throw new InvalidOperationException(
            $"{dialog.Current.Name} reopened after an unchanged edit notification");
    Console.WriteLine(
        $"uia.correctness.{label}_unchanged_edit_stayed_closed=true");

    var screenshot = Path.Combine(output, $"correctness-{label}.png");
    var capture = ScreenCapture.SaveWindowPng(dialog, screenshot);
    Console.WriteLine(
        $"uia.correctness.{label}_capture={capture.Width}x{capture.Height};" +
        $"non_black_pixels={capture.NonBlackPixelCount}");
}

static void ExpectEdit(
    AutomationElement combo,
    string expectedEdit,
    string label,
    string phase,
    TimeSpan timeout)
{
    var actual = WaitForValue(combo, expectedEdit, timeout);
    Console.WriteLine($"uia.correctness.{label}_{phase}={actual}");
    if (!string.Equals(actual, expectedEdit, StringComparison.Ordinal))
        throw new InvalidOperationException(
            $"Edit at {phase} is '{actual}', expected typed '{expectedEdit}' " +
            "(must not auto-complete / fill the match name until explicit select)");
}

static void AssertHighlightSettled(
    AutomationElement dialog,
    List<(int Ms, string Edit, string Sel, int ListCount, bool Match, bool Lost)> samples,
    string query,
    string expected,
    string label,
    string pass)
{
    var afterExpand = samples[^1].Edit;
    Console.WriteLine($"uia.correctness.{label}_{pass}_after_expand={afterExpand}");
    if (!string.Equals(afterExpand, query, StringComparison.Ordinal))
        throw new InvalidOperationException(
            $"{dialog.Current.Name} {pass}: edit on expand is '{afterExpand}', " +
            $"expected typed '{query}'");

    var listCount = samples.Max(s => s.ListCount);
    Console.WriteLine($"uia.correctness.{label}_{pass}_list_count={listCount}");
    if (listCount < 20)
        throw new InvalidOperationException(
            $"{dialog.Current.Name} {pass}: font list looks filtered ({listCount} items)");

    if (samples.Any(s => s.Lost))
        throw new InvalidOperationException(
            $"{dialog.Current.Name} {pass}: highlight jumped to '{expected}' then snapped away: " +
            FormatHighlightTimeline(samples));
    if (!samples.Any(s => s.Match))
        throw new InvalidOperationException(
            $"{dialog.Current.Name} {pass}: highlight never settled on '{expected}': " +
            FormatHighlightTimeline(samples));
    if (!samples[^1].Match)
        throw new InvalidOperationException(
            $"{dialog.Current.Name} {pass}: highlight not on '{expected}' after settle: " +
            FormatHighlightTimeline(samples));

    // Edit must stay typed for every sample in this open.
    foreach (var sample in samples)
    {
        if (!string.Equals(sample.Edit, query, StringComparison.Ordinal)
            && sample.ListCount > 0)
            throw new InvalidOperationException(
                $"{dialog.Current.Name} {pass}: edit became '{sample.Edit}' at {sample.Ms}ms " +
                $"(expected typed '{query}'); " + FormatHighlightTimeline(samples));
    }

    Console.WriteLine($"uia.correctness.{label}_{pass}_highlighted={expected}");
}

static void CollapseCombo(ExpandCollapsePattern expand)
{
    try
    {
        expand.Collapse();
    }
    catch (InvalidOperationException)
    {
    }
    catch (ElementNotAvailableException)
    {
    }
}

static void WaitForComboDropState(
    IntPtr comboHwnd,
    bool expectedDropped,
    TimeSpan timeout)
{
    var deadline = Stopwatch.GetTimestamp()
        + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
    while (Stopwatch.GetTimestamp() < deadline)
    {
        WorkerWatchdog.Pulse();
        if (NativeKeyboard.IsComboDropped(comboHwnd) == expectedDropped)
            return;
        Thread.Sleep(25);
    }
    throw new TimeoutException(
        $"Font combo drop state did not become {expectedDropped}");
}

static void ClearComboEdit(AutomationElement combo)
{
    var comboHwnd = new IntPtr(combo.Current.NativeWindowHandle);
    var editHwnd = NativeKeyboard.GetComboEditHwnd(comboHwnd);
    if (editHwnd == IntPtr.Zero)
        editHwnd = comboHwnd;
    combo.SetFocus();
    Thread.Sleep(50);
    NativeKeyboard.SetEditText(editHwnd, string.Empty);
    Thread.Sleep(100);
}

static void TypeIntoCombo(AutomationElement combo, string text, bool charByChar)
{
    var comboHwnd = new IntPtr(combo.Current.NativeWindowHandle);
    if (comboHwnd == IntPtr.Zero)
        throw new InvalidOperationException("Font combo has no native HWND");
    combo.SetFocus();
    Thread.Sleep(50);
    // Prefer the real EDIT child from COMBOBOXINFO; UIA child lookup can miss it.
    var editHwnd = NativeKeyboard.GetComboEditHwnd(comboHwnd);
    if (editHwnd == IntPtr.Zero)
    {
        var edit = combo.FindFirst(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Edit));
        if (edit is not null)
            editHwnd = new IntPtr(edit.Current.NativeWindowHandle);
    }
    if (editHwnd == IntPtr.Zero)
        editHwnd = comboHwnd;
    Console.WriteLine(
        $"uia.correctness.font_combo_hwnd=0x{comboHwnd.ToInt64():X};edit_hwnd=0x{editHwnd.ToInt64():X}");
    if (charByChar)
        NativeKeyboard.TypeCharsOneByOne(editHwnd, text);
    else
        NativeKeyboard.TypeCharsViaWindowMessage(editHwnd, text);
}

static AutomationElement? FindEditableComboBox(AutomationElement root)
{
    var combos = root.FindAll(
        TreeScope.Descendants,
        new PropertyCondition(
            AutomationElement.ControlTypeProperty,
            ControlType.ComboBox));
    foreach (AutomationElement combo in combos)
    {
        try
        {
            if (combo.Current.IsEnabled
                && combo.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern)
                && !((ValuePattern)pattern).Current.IsReadOnly)
                return combo;
        }
        catch (ElementNotAvailableException)
        {
        }
    }
    return null;
}

static AutomationElement? FindSelectedListItem(
    AutomationElement root,
    string name)
{
    var items = root.FindAll(
        TreeScope.Descendants,
        new AndCondition(
            new PropertyCondition(
                AutomationElement.ControlTypeProperty,
                ControlType.ListItem),
            new PropertyCondition(AutomationElement.NameProperty, name)));
    foreach (AutomationElement item in items)
    {
        try
        {
            if (item.TryGetCurrentPattern(
                    SelectionItemPattern.Pattern,
                    out var pattern)
                && ((SelectionItemPattern)pattern).Current.IsSelected)
                return item;
        }
        catch (ElementNotAvailableException)
        {
        }
    }
    return null;
}

static AutomationElement WaitForDialogWithProviderCheck(
    Process process,
    TimeSpan timeout,
    string expectedTitle)
{
    var dialog = WaitForWindow(
        process,
        timeout,
        expectedTitle,
        "No subtitles provider");
    if (string.Equals(
            dialog.Current.Name,
            "No subtitles provider",
            StringComparison.OrdinalIgnoreCase))
        throw new InvalidOperationException(
            $"{expectedTitle} requires the staged subtitles-provider runtime");
    return dialog;
}

static AutomationElement WaitForWindow(
    Process process,
    TimeSpan timeout,
    params string[] titles)
{
    var deadline = Stopwatch.GetTimestamp()
        + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
    var observed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    while (Stopwatch.GetTimestamp() < deadline)
    {
        WorkerWatchdog.Pulse();
        NativeWindowSearch.ThrowIfFatalDialog(process);
        foreach (var window in NativeWindowSearch.GetTopLevelWindows(process.Id))
        {
            if (!string.IsNullOrWhiteSpace(window.Title))
                observed.Add(window.Title);
            if (titles.Any(candidate => string.Equals(
                    window.Title,
                    candidate,
                    StringComparison.OrdinalIgnoreCase)))
                return AutomationElement.FromHandle(window.Handle);
        }
        Thread.Sleep(50);
    }
    throw new TimeoutException(
        $"UIA window was not found: {string.Join(" or ", titles)}; " +
        $"observed={string.Join(" | ", observed)}");
}

// Sample tuple: ms, edit, selectedName, listCount, matchHighlighted, sawMatchThenLost
static List<(int Ms, string Edit, string Sel, int ListCount, bool Match, bool Lost)>
    SampleHighlightTimeline(
        AutomationElement combo,
        string query,
        string expected,
        string label)
{
    // Delayed samples after expand. Prefer cheap named lookups so sampling
    // itself does not stall the desktop UIA tree for seconds per tick.
    int[] delaysMs = [0, 50, 100, 200, 350, 500, 750, 1000, 1500, 2000];
    var samples = new List<(int Ms, string Edit, string Sel, int ListCount, bool Match, bool Lost)>(
        delaysMs.Length);
    var started = Stopwatch.StartNew();
    var sawMatch = false;
    var lostAfterMatch = false;
    var listCount = 0;
    var comboHwnd = new IntPtr(combo.Current.NativeWindowHandle);

    foreach (var targetMs in delaysMs)
    {
        WorkerWatchdog.Pulse();
        var wait = targetMs - (int)started.ElapsedMilliseconds;
        if (wait > 0)
            Thread.Sleep(wait);

        var edit = NativeKeyboard.GetComboEditText(comboHwnd);

        // Read the native combo count once; desktop-wide UIA enumeration can
        // stall every provider on the interactive desktop.
        if (listCount < 20)
            listCount = NativeKeyboard.GetComboItemCount(comboHwnd);

        // Read the real Win32 list caret (UIA IsSelected is unreliable on
        // CBS_DROPDOWN listboxes after CB_SETCURSEL + edit restore).
        var selectedName = NativeKeyboard.GetComboHighlightText(comboHwnd) ?? "";
        var matchHighlighted = string.Equals(
            selectedName, expected, StringComparison.OrdinalIgnoreCase);
        var dropped = NativeKeyboard.IsComboDropped(comboHwnd);

        if (matchHighlighted)
            sawMatch = true;
        else if (sawMatch)
            lostAfterMatch = true;

        var sample = (
            Ms: (int)started.ElapsedMilliseconds,
            Edit: edit,
            Sel: selectedName,
            ListCount: listCount,
            Match: matchHighlighted,
            Lost: lostAfterMatch);
        samples.Add(sample);
        Console.WriteLine(
            $"uia.correctness.{label}_t{sample.Ms}ms=" +
            $"edit='{sample.Edit}';sel='{sample.Sel}';" +
            $"match={sample.Match};dropped={dropped};list={sample.ListCount}");
    }

    if (lostAfterMatch)
    {
        for (var i = 0; i < samples.Count; ++i)
        {
            var s = samples[i];
            samples[i] = (s.Ms, s.Edit, s.Sel, s.ListCount, s.Match, true);
        }
    }

    // Edit must stay on the typed query for every sample (no late auto-complete).
    foreach (var sample in samples)
    {
        if (!string.Equals(sample.Edit, query, StringComparison.Ordinal)
            && !string.IsNullOrEmpty(sample.Edit)
            && sample.ListCount > 0)
        {
            throw new InvalidOperationException(
                $"Edit left typed query during highlight settle at {sample.Ms}ms: " +
                $"'{sample.Edit}' (expected '{query}'); " +
                FormatHighlightTimeline(samples));
        }
    }

    return samples;
}

static string FormatHighlightTimeline(
    IReadOnlyList<(int Ms, string Edit, string Sel, int ListCount, bool Match, bool Lost)> samples) =>
    string.Join(
        " | ",
        samples.Select(s => $"{s.Ms}ms:edit='{s.Edit}',sel='{s.Sel}',match={s.Match}"));

/// Wait until the named drop-down item is the current Win32 list caret.
static void WaitForHighlightedListItem(
    AutomationElement combo,
    string name,
    TimeSpan timeout)
{
    var comboHwnd = new IntPtr(combo.Current.NativeWindowHandle);
    var deadline = Stopwatch.GetTimestamp()
        + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
    while (Stopwatch.GetTimestamp() < deadline)
    {
        WorkerWatchdog.Pulse();
        var highlight = NativeKeyboard.GetComboHighlightText(comboHwnd);
        if (string.Equals(highlight, name, StringComparison.OrdinalIgnoreCase))
            return;
        Thread.Sleep(50);
    }
    throw new TimeoutException(
        $"Font list highlight did not jump to '{name}' after expand " +
        $"(native caret='{NativeKeyboard.GetComboHighlightText(comboHwnd) ?? ""}')");
}

static string WaitForValue(
    AutomationElement element,
    string expected,
    TimeSpan timeout)
{
    var comboHwnd = new IntPtr(element.Current.NativeWindowHandle);
    var deadline = Stopwatch.GetTimestamp()
        + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
    var current = string.Empty;
    while (Stopwatch.GetTimestamp() < deadline)
    {
        WorkerWatchdog.Pulse();
        current = NativeKeyboard.GetComboEditText(comboHwnd);
        if (string.Equals(current, expected, StringComparison.Ordinal))
            return current;
        Thread.Sleep(50);
    }
    throw new TimeoutException(
        $"Font selector value did not become '{expected}'; current='{current}'");
}

static void CloseDialog(AutomationElement dialog, string buttonName)
{
    var button = UiaDriver.FindEnabledInvokableButtonByAutomationId(
            dialog,
            "5101",
            buttonName)
        ?? throw new InvalidOperationException(
            $"{dialog.Current.Name} has no {buttonName} button");
    UiaDriver.Invoke(button);
}

static void WaitForTask(Task task, TimeSpan timeout, string message)
{
    var deadline = Stopwatch.GetTimestamp()
        + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
    while (!task.Wait(250))
    {
        WorkerWatchdog.Pulse();
        if (Stopwatch.GetTimestamp() >= deadline)
            throw new TimeoutException(message);
    }
    task.GetAwaiter().GetResult();
}

static void PrepareFontSelectorProfile(string profile)
{
    var user = Path.Combine(profile, "user");
    Directory.CreateDirectory(user);
    File.WriteAllText(
        Path.Combine(user, "config.json"),
        """
        {
          "Subtitle": {
            "Font": {
              "Prefer Localized Family Names": false,
              "Use Contains Matching": true,
              "Auto Expand List On Input": true
            }
          }
        }
        """);
}

static void PrepareRunArtifacts(string output, string profile)
{
    Directory.CreateDirectory(output);
    foreach (var name in new[]
    {
        "ready.json",
        "result.json",
        "correctness-main-window.png",
        "correctness-style_editor.png",
        "correctness-select_font.png",
        "startup.log",
    })
    {
        var path = Path.Combine(output, name);
        if (File.Exists(path))
            File.Delete(path);
    }
    foreach (var path in Directory.EnumerateFiles(output, "*.tmp-*", SearchOption.TopDirectoryOnly))
        File.Delete(path);
    if (Directory.Exists(profile))
        Directory.Delete(profile, true);
}

static bool PathsEqual(string left, string right)
{
    try
    {
        return string.Equals(
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(left)),
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(right)),
            StringComparison.OrdinalIgnoreCase);
    }
    catch (Exception error) when (error is ArgumentException or NotSupportedException)
    {
        return false;
    }
}

file static class WorkerWatchdog
{
    private static readonly Stopwatch SinceLastPulse = Stopwatch.StartNew();
    private static string? heartbeatPath;
    private static string stage = "worker-started";

    public static void Initialize(string? path)
    {
        heartbeatPath = path;
        Stage(stage);
    }

    public static void Stage(string value)
    {
        stage = value;
        WriteHeartbeat();
    }

    public static void Pulse()
    {
        if (SinceLastPulse.ElapsedMilliseconds < 250)
            return;
        WriteHeartbeat();
    }

    private static void WriteHeartbeat()
    {
        if (string.IsNullOrWhiteSpace(heartbeatPath))
            return;
        using var stream = new FileStream(
            heartbeatPath,
            FileMode.Create,
            FileAccess.Write,
            FileShare.ReadWrite);
        using var writer = new StreamWriter(stream, new UTF8Encoding(false));
        writer.Write(stage);
        SinceLastPulse.Restart();
    }
}

file static class NativeWindowSearch
{
    public readonly record struct Window(IntPtr Handle, string Title);

    public static IReadOnlyList<Window> GetTopLevelWindows(int processId)
    {
        var windows = new List<Window>();
        EnumWindows((handle, _) =>
        {
            GetWindowThreadProcessId(handle, out var windowProcessId);
            if (windowProcessId != processId || !IsWindowVisible(handle))
                return true;
            var length = GetWindowTextLength(handle);
            var title = new StringBuilder(Math.Max(1, length + 1));
            GetWindowText(handle, title, title.Capacity);
            windows.Add(new Window(handle, title.ToString()));
            return true;
        }, IntPtr.Zero);
        return windows;
    }

    public static void ThrowIfFatalDialog(Process process)
    {
        process.Refresh();
        if (process.HasExited)
            return;
        foreach (var window in GetTopLevelWindows(process.Id))
        {
            var name = window.Title.Trim();
            if (name.Contains("Program error", StringComparison.OrdinalIgnoreCase)
                || name.Contains("Aegisub has crashed", StringComparison.OrdinalIgnoreCase)
                || name.Contains("Aegisub crashed", StringComparison.OrdinalIgnoreCase)
                || name.Contains("程序错误", StringComparison.OrdinalIgnoreCase)
                || name.Contains("Aegisub 已崩溃", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(
                    $"Aegisub reported a fatal error dialog: {name}");
        }
    }

    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window,
        out int processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextLength(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowText(
        IntPtr window,
        StringBuilder text,
        int maximumCount);
}

file static class NativeKeyboard
{
    private const int EmReplaceSel = 0x00C2;
    private const int EmSetSel = 0x00B1;
    private const int WmKeyDown = 0x0100;
    private const int WmKeyUp = 0x0101;
    private const int WmChar = 0x0102;
    private const int WmCommand = 0x0111;
    private const int WmLButtonDown = 0x0201;
    private const int WmLButtonUp = 0x0202;
    private const int MkLButton = 0x0001;
    private const int VkReturn = 0x0D;
    private const int EnChange = 0x0300;
    private const int WmGetText = 0x000D;
    private const int WmGetTextLength = 0x000E;
    private const int CbGetCount = 0x0146;
    private const int CbGetCurSel = 0x0147;
    private const int CbGetLbText = 0x0148;
    private const int CbGetLbTextLen = 0x0149;
    private const int CbFindStringExact = 0x0158;
    private const int CbGetDroppedState = 0x0157;
    private const int LbGetCurSel = 0x0188;
    private const int LbGetText = 0x0189;
    private const int LbGetTextLen = 0x018A;
    private const int LbSetTopIndex = 0x0197;
    private const int LbGetItemRect = 0x0198;
    private const uint SmtoBlock = 0x0001;
    private const uint SmtoAbortIfHung = 0x0002;
    private const uint SmtoErrorOnExit = 0x0020;
    private const uint MessageTimeoutMilliseconds = 2000;

    public static IntPtr GetComboEditHwnd(IntPtr comboHwnd)
    {
        var info = new ComboBoxInfo { cbSize = Marshal.SizeOf<ComboBoxInfo>() };
        if (!GetComboBoxInfo(comboHwnd, ref info))
            return IntPtr.Zero;
        return info.hwndItem;
    }

    public static IntPtr GetComboListHwnd(IntPtr comboHwnd)
    {
        var info = new ComboBoxInfo { cbSize = Marshal.SizeOf<ComboBoxInfo>() };
        if (!GetComboBoxInfo(comboHwnd, ref info))
            return IntPtr.Zero;
        return info.hwndList;
    }

    public static bool IsComboDropped(IntPtr comboHwnd)
    {
        if (comboHwnd == IntPtr.Zero)
            return false;
        return SendMessageChecked(
            comboHwnd, CbGetDroppedState, IntPtr.Zero, IntPtr.Zero) != IntPtr.Zero;
    }

    public static int GetComboItemCount(IntPtr comboHwnd)
    {
        if (comboHwnd == IntPtr.Zero)
            return 0;
        return SendMessageChecked(
            comboHwnd, CbGetCount, IntPtr.Zero, IntPtr.Zero).ToInt32();
    }

    public static string GetComboEditText(IntPtr comboHwnd)
    {
        if (comboHwnd == IntPtr.Zero)
            return string.Empty;
        var editHwnd = GetComboEditHwnd(comboHwnd);
        if (editHwnd == IntPtr.Zero)
            editHwnd = comboHwnd;
        var length = SendMessageChecked(
            editHwnd, WmGetTextLength, IntPtr.Zero, IntPtr.Zero).ToInt32();
        if (length <= 0)
            return string.Empty;
        var text = new StringBuilder(length + 1);
        SendMessageChecked(editHwnd, WmGetText, (IntPtr)text.Capacity, text);
        return text.ToString();
    }

    public static void ClickComboItem(IntPtr comboHwnd, string text)
    {
        if (comboHwnd == IntPtr.Zero)
            throw new InvalidOperationException("Font combo has no native HWND");
        if (!IsComboDropped(comboHwnd))
            throw new InvalidOperationException("Font combo list is not open");
        var index = SendMessageChecked(
            comboHwnd, CbFindStringExact, (IntPtr)(-1), text).ToInt32();
        if (index < 0)
            throw new InvalidOperationException($"Font combo item '{text}' was not found");
        var listHwnd = GetComboListHwnd(comboHwnd);
        if (listHwnd == IntPtr.Zero)
            throw new InvalidOperationException("Font combo list has no native HWND");

        SendMessageChecked(
            listHwnd, LbSetTopIndex, (IntPtr)Math.Max(0, index - 2), IntPtr.Zero);
        var itemRect = new Rect();
        var rectResult = SendMessageChecked(
            listHwnd, LbGetItemRect, (IntPtr)index, ref itemRect).ToInt32();
        if (rectResult < 0 || itemRect.bottom <= itemRect.top)
            throw new InvalidOperationException($"Could not locate font combo item '{text}'");
        var x = Math.Max(1, itemRect.left + 8);
        var y = itemRect.top + Math.Max(1, (itemRect.bottom - itemRect.top) / 2);
        var point = (IntPtr)((y << 16) | (x & 0xFFFF));
        SendMessageChecked(listHwnd, WmLButtonDown, (IntPtr)MkLButton, point);
        SendMessageChecked(listHwnd, WmLButtonUp, IntPtr.Zero, point);
    }

    /// Current list caret text: prefer open listbox LB_GETCURSEL, else CB_GETCURSEL.
    public static string? GetComboHighlightText(IntPtr comboHwnd)
    {
        if (comboHwnd == IntPtr.Zero)
            return null;
        var info = new ComboBoxInfo { cbSize = Marshal.SizeOf<ComboBoxInfo>() };
        if (GetComboBoxInfo(comboHwnd, ref info) && info.hwndList != IntPtr.Zero)
        {
            var listIndex = SendMessageChecked(
                info.hwndList, LbGetCurSel, IntPtr.Zero, IntPtr.Zero).ToInt32();
            if (listIndex >= 0)
            {
                var listText = GetListBoxText(info.hwndList, listIndex);
                if (!string.IsNullOrEmpty(listText))
                    return listText;
            }
        }

        var comboIndex = SendMessageChecked(
            comboHwnd, CbGetCurSel, IntPtr.Zero, IntPtr.Zero).ToInt32();
        if (comboIndex < 0)
            return null;
        return GetComboBoxListText(comboHwnd, comboIndex);
    }

    public static void SetEditText(IntPtr editHwnd, string text)
    {
        var parent = GetParent(editHwnd);
        var editId = GetDlgCtrlID(editHwnd);
        SendMessageChecked(editHwnd, EmSetSel, IntPtr.Zero, (IntPtr)(-1));
        SendMessageChecked(editHwnd, EmReplaceSel, (IntPtr)1, text ?? string.Empty);
        if (parent != IntPtr.Zero && editId != 0)
        {
            var wParam = (IntPtr)((EnChange << 16) | (editId & 0xFFFF));
            SendMessageChecked(parent, WmCommand, wParam, editHwnd);
        }
        Thread.Sleep(100);
    }

    public static void TypeCharsViaWindowMessage(IntPtr editHwnd, string text)
    {
        // Replace the whole edit contents in one shot.
        SetEditText(editHwnd, text);
        // Allow deferred CallAfter match application on the UI thread.
        Thread.Sleep(400);
    }

    /// Type one character at a time (user-like). Each char allows the app to
    /// strip native auto-complete and update list highlight.
    public static void TypeCharsOneByOne(IntPtr editHwnd, string text)
    {
        SetEditText(editHwnd, string.Empty);
        Thread.Sleep(100);
        foreach (var ch in text)
        {
            // Append one character at the end.
            SendMessageChecked(editHwnd, EmSetSel, (IntPtr)(-1), (IntPtr)(-1));
            SendMessageChecked(editHwnd, EmReplaceSel, (IntPtr)1, ch.ToString());
            var parent = GetParent(editHwnd);
            var editId = GetDlgCtrlID(editHwnd);
            if (parent != IntPtr.Zero && editId != 0)
            {
                var wParam = (IntPtr)((EnChange << 16) | (editId & 0xFFFF));
                SendMessageChecked(parent, WmCommand, wParam, editHwnd);
            }
            // Let CallAfter / hold-timer strip auto-complete and jump the list.
            Thread.Sleep(120);
        }
        Thread.Sleep(300);
    }

    /// Type into the combo edit by sending WM_CHAR directly to the EDIT child.
    /// Unlike EM_REPLACESEL (which replaces the selection and only fires
    /// EN_CHANGE), WM_CHAR is dispatched through the native EDIT wndproc that
    /// wx subclasses, so it triggers the native CBS_DROPDOWN auto-select /
    /// prefix-completion path — exactly what real keystrokes do. This is
    /// focus-immune (synchronous cross-process SendMessage) and reproduces the
    /// stale-list-caret bug without the flakiness of foreground SendInput.
    public static void TypeCharsViaComboKeys(IntPtr comboHwnd, string text)
    {
        if (comboHwnd == IntPtr.Zero)
            throw new InvalidOperationException("Font combo has no native HWND");
        var editHwnd = GetComboEditHwnd(comboHwnd);
        if (editHwnd == IntPtr.Zero)
            throw new InvalidOperationException("Font combo edit has no native HWND");

        // Ensure the drop-down is closed so typing behaves like a fresh query.
        if (IsComboDropped(comboHwnd))
        {
            SendMessageChecked(
                comboHwnd, 0x014D, IntPtr.Zero, IntPtr.Zero); // CB_SHOWDROPDOWN(FALSE)
            Thread.Sleep(100);
        }

        // Start from an empty edit so each appended WM_CHAR is unambiguous.
        SetEditText(editHwnd, string.Empty);
        Thread.Sleep(100);

        foreach (var ch in text)
        {
            // Move the caret to the end and inject one character via WM_CHAR,
            // which the native EDIT wndproc turns into text + auto-select.
            SendMessageChecked(editHwnd, EmSetSel, (IntPtr)(-1), (IntPtr)(-1));
            SendMessageChecked(editHwnd, WmChar, (IntPtr)ch, IntPtr.Zero);
            Thread.Sleep(150);
        }
        Thread.Sleep(350);
    }

    public static void CommitHighlightedComboItem(IntPtr comboHwnd)
    {
        if (comboHwnd == IntPtr.Zero)
            throw new InvalidOperationException("Font combo has no native HWND");
        var editHwnd = GetComboEditHwnd(comboHwnd);
        if (editHwnd == IntPtr.Zero)
            throw new InvalidOperationException("Font combo edit has no native HWND");
        SendMessageChecked(editHwnd, WmKeyDown, (IntPtr)VkReturn, IntPtr.Zero);
        // A real key press produces WM_CHAR between key-down and key-up. This
        // must not escape to the dialog's default OK button after the combo
        // has already committed the highlighted font.
        SendMessageChecked(editHwnd, WmChar, (IntPtr)'\r', IntPtr.Zero);
        SendMessageChecked(editHwnd, WmKeyUp, (IntPtr)VkReturn, IntPtr.Zero);
        Thread.Sleep(250);
    }

    private static string? GetComboBoxListText(IntPtr comboHwnd, int index)
    {
        var len = SendMessageChecked(
            comboHwnd, CbGetLbTextLen, (IntPtr)index, IntPtr.Zero).ToInt32();
        if (len < 0)
            return null;
        var buffer = new StringBuilder(len + 1);
        SendMessageChecked(comboHwnd, CbGetLbText, (IntPtr)index, buffer);
        return buffer.ToString();
    }

    private static string? GetListBoxText(IntPtr listHwnd, int index)
    {
        var len = SendMessageChecked(
            listHwnd, LbGetTextLen, (IntPtr)index, IntPtr.Zero).ToInt32();
        if (len < 0)
            return null;
        var buffer = new StringBuilder(len + 1);
        SendMessageChecked(listHwnd, LbGetText, (IntPtr)index, buffer);
        return buffer.ToString();
    }

    private static IntPtr SendMessageChecked(
        IntPtr window,
        int message,
        IntPtr wParam,
        IntPtr lParam)
    {
        if (SendMessageTimeout(
                window,
                message,
                wParam,
                lParam,
                SmtoBlock | SmtoAbortIfHung | SmtoErrorOnExit,
                MessageTimeoutMilliseconds,
                out var result) == IntPtr.Zero)
            throw new TimeoutException(
                $"Win32 message 0x{message:X} timed out for HWND 0x{window.ToInt64():X}");
        return result;
    }

    private static IntPtr SendMessageChecked(
        IntPtr window,
        int message,
        IntPtr wParam,
        string lParam)
    {
        if (SendMessageTimeout(
                window,
                message,
                wParam,
                lParam,
                SmtoBlock | SmtoAbortIfHung | SmtoErrorOnExit,
                MessageTimeoutMilliseconds,
                out var result) == IntPtr.Zero)
            throw new TimeoutException(
                $"Win32 message 0x{message:X} timed out for HWND 0x{window.ToInt64():X}");
        return result;
    }

    private static IntPtr SendMessageChecked(
        IntPtr window,
        int message,
        IntPtr wParam,
        StringBuilder lParam)
    {
        if (SendMessageTimeout(
                window,
                message,
                wParam,
                lParam,
                SmtoBlock | SmtoAbortIfHung | SmtoErrorOnExit,
                MessageTimeoutMilliseconds,
                out var result) == IntPtr.Zero)
            throw new TimeoutException(
                $"Win32 message 0x{message:X} timed out for HWND 0x{window.ToInt64():X}");
        return result;
    }

    private static IntPtr SendMessageChecked(
        IntPtr window,
        int message,
        IntPtr wParam,
        ref Rect lParam)
    {
        if (SendMessageTimeout(
                window,
                message,
                wParam,
                ref lParam,
                SmtoBlock | SmtoAbortIfHung | SmtoErrorOnExit,
                MessageTimeoutMilliseconds,
                out var result) == IntPtr.Zero)
            throw new TimeoutException(
                $"Win32 message 0x{message:X} timed out for HWND 0x{window.ToInt64():X}");
        return result;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int left, top, right, bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ComboBoxInfo
    {
        public int cbSize;
        public Rect rcItem;
        public Rect rcButton;
        public int stateButton;
        public IntPtr hwndCombo;
        public IntPtr hwndItem;
        public IntPtr hwndList;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetComboBoxInfo(IntPtr hwndCombo, ref ComboBoxInfo pcbi);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window,
        int message,
        IntPtr wParam,
        IntPtr lParam,
        uint flags,
        uint timeout,
        out IntPtr result);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window,
        int message,
        IntPtr wParam,
        ref Rect lParam,
        uint flags,
        uint timeout,
        out IntPtr result);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window,
        int message,
        IntPtr wParam,
        string lParam,
        uint flags,
        uint timeout,
        out IntPtr result);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window,
        int message,
        IntPtr wParam,
        StringBuilder lParam,
        uint flags,
        uint timeout,
        out IntPtr result);

    // EM_SETSEL uses LPARAM as character index pair via wParam/lParam.
    // Overload already covers (hwnd, msg, wParam, lParam).

    [DllImport("user32.dll")]
    private static extern IntPtr GetParent(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern int GetDlgCtrlID(IntPtr hWnd);
}
