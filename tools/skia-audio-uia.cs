#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false

using System.Diagnostics;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.ComponentModel;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Automation;

var options = DriverOptions.Parse(args);
if (options.Help)
{
    DriverOptions.PrintHelp();
    return 0;
}

if (!File.Exists(options.Executable))
    throw new FileNotFoundException("Aegisub executable was not found", options.Executable);

using var app = new AegisubSession(options);
app.Start();

if (options.DumpTree)
{
    app.DumpWindowTree();
    if (options.DryRun)
        return 0;
}

app.WaitForAudioCanvas();
if (options.DryRun)
{
    Console.WriteLine("uia.dry_run=ok");
    return 0;
}

await app.RunScenarioAsync();
return 0;

sealed record DriverOptions(
    string Executable,
    string Scenario,
    int DurationSeconds,
    string? Project,
    string? Audio,
    int Width,
    int Height,
    bool DumpTree,
    bool DryRun,
    bool KeepOpen,
    int WarmupSeconds)
{
    public bool Help { get; init; }

    public static DriverOptions Parse(string[] args)
    {
        string? executable = null;
        string scenario = "audio-waveform-scroll";
        int duration = 30;
        string? project = null;
        string? audio = null;
        int width = 1280;
        int height = 900;
        bool dumpTree = false;
        bool dryRun = false;
        bool keepOpen = false;
        int warmup = 5;
        bool help = false;

        for (var i = 0; i < args.Length; ++i)
        {
            var arg = args[i];
            string Value()
            {
                if (++i >= args.Length)
                    throw new ArgumentException($"Missing value for {arg}");
                return args[i];
            }

            switch (arg)
            {
                case "--help":
                case "-h":
                    help = true;
                    break;
                case "--exe":
                    executable = Value();
                    break;
                case "--scenario":
                    scenario = Value();
                    break;
                case "--duration-seconds":
                    duration = ParsePositive(Value(), arg);
                    break;
                case "--warmup-seconds":
                    warmup = ParseNonNegative(Value(), arg);
                    break;
                case "--project":
                    project = Value();
                    break;
                case "--audio":
                    audio = Value();
                    break;
                case "--width":
                    width = ParsePositive(Value(), arg);
                    break;
                case "--height":
                    height = ParsePositive(Value(), arg);
                    break;
                case "--dump-tree":
                    dumpTree = true;
                    break;
                case "--dry-run":
                    dryRun = true;
                    break;
                case "--keep-open":
                    keepOpen = true;
                    break;
                default:
                    throw new ArgumentException($"Unknown option: {arg}");
            }
        }

        if (help)
            return new DriverOptions(executable ?? "Aegisub.exe", scenario, duration, project, audio,
                width, height, dumpTree, dryRun, keepOpen, warmup) { Help = true };
        if (string.IsNullOrWhiteSpace(executable))
            throw new ArgumentException("--exe is required");
        if (!dryRun && string.IsNullOrWhiteSpace(audio))
            throw new ArgumentException("--audio is required for an automated audio scenario");

        return new DriverOptions(Path.GetFullPath(executable), scenario, duration, project, audio,
            width, height, dumpTree, dryRun, keepOpen, warmup);
    }

    public static void PrintHelp()
    {
        Console.WriteLine("Aegisub Audio Display UIA + SendInput local driver");
        Console.WriteLine("  --exe PATH                         Aegisub.exe");
        Console.WriteLine("  --scenario NAME                    audio-waveform-scroll, audio-spectrum-scroll, audio-cursor-marker, audio-playback-cursor, all");
        Console.WriteLine("  --duration-seconds N               active scenario duration (default 30)");
        Console.WriteLine("  --warmup-seconds N                 warmup before input (default 5)");
        Console.WriteLine("  --project PATH                     optional ASS/project file passed at startup");
        Console.WriteLine("  --audio PATH                       optional audio file passed at startup");
        Console.WriteLine("  --width N --height N               fixed top-level window size (default 1280x900)");
        Console.WriteLine("  --dump-tree                        print UIA and Win32 child window inventory");
        Console.WriteLine("  --dry-run                          start, inspect and exit without input");
        Console.WriteLine("  --keep-open                        leave Aegisub open after the scenario");
    }

    private static int ParsePositive(string value, string option) =>
        int.TryParse(value, out var result) && result > 0
            ? result
            : throw new ArgumentException($"{option} must be a positive integer");

    private static int ParseNonNegative(string value, string option) =>
        int.TryParse(value, out var result) && result >= 0
            ? result
            : throw new ArgumentException($"{option} must be a non-negative integer");
}

sealed class AegisubSession : IDisposable
{
    private readonly DriverOptions options;
    private Process? process;
    private AutomationElement? mainWindow;
    private nint audioCanvas;
    private WinRect audioCanvasRect;
    private readonly bool preferSkiaCanvas = IsRuntimeOptIn(
        Environment.GetEnvironmentVariable("AEGISUB_ENABLE_SKIA_AUDIO_DISPLAY"));

    public AegisubSession(DriverOptions options) => this.options = options;

    public void Start()
    {
        if (!string.IsNullOrWhiteSpace(options.Audio))
        {
            var audioPath = Path.GetFullPath(options.Audio);
            if (!File.Exists(audioPath))
                throw new FileNotFoundException("Audio file was not found", audioPath);
        }
        if (!string.IsNullOrWhiteSpace(options.Project))
        {
            var projectPath = Path.GetFullPath(options.Project);
            if (!File.Exists(projectPath))
                throw new FileNotFoundException("Project file was not found", projectPath);
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = options.Executable,
            WorkingDirectory = Path.GetDirectoryName(options.Executable) ?? Environment.CurrentDirectory,
            UseShellExecute = false,
        };
        if (!string.IsNullOrWhiteSpace(options.Project))
            startInfo.ArgumentList.Add(Path.GetFullPath(options.Project));
        if (!string.IsNullOrWhiteSpace(options.Audio))
            startInfo.ArgumentList.Add(Path.GetFullPath(options.Audio));

        process = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start Aegisub");
        Console.WriteLine($"uia.pid={process.Id}");
        WaitForMainWindow();
        SetWindowSize();
    }

    public void WaitForAudioCanvas()
    {
        if (mainWindow is null)
            throw new InvalidOperationException("Main window was not discovered");

        Console.WriteLine($"uia.prefer_skia_canvas={preferSkiaCanvas}");

        var root = mainWindow ?? throw new InvalidOperationException("Main window was not discovered");
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 30;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (root is null)
                break;
            audioCanvas = FindAudioCanvas(root, preferSkiaCanvas);
            if (audioCanvas != 0)
            {
                audioCanvasRect = GetWindowRect(audioCanvas);
                Console.WriteLine($"uia.audio_canvas_hwnd=0x{audioCanvas:X}");
                Console.WriteLine($"uia.audio_canvas_class={GetClassName(audioCanvas)}");
                Console.WriteLine($"uia.audio_canvas_rect={audioCanvasRect.Left},{audioCanvasRect.Top},{audioCanvasRect.Width}x{audioCanvasRect.Height}");
                return;
            }
            Thread.Sleep(250);
            root = RefreshMainWindow();
            mainWindow = root;
        }

        throw new TimeoutException(preferSkiaCanvas
            ? "Skia Audio runtime was requested, but no wxGLCanvas Audio Display appeared; the application may have fallen back to legacy wx"
            : "Could not locate the Audio Display canvas");
    }

    public async Task RunScenarioAsync()
    {
        if (options.WarmupSeconds > 0)
        {
            Console.WriteLine($"uia.warmup_seconds={options.WarmupSeconds}");
            await Task.Delay(TimeSpan.FromSeconds(options.WarmupSeconds));
        }

        var scenarios = options.Scenario.Equals("all", StringComparison.OrdinalIgnoreCase)
            ? new[] { "audio-waveform-scroll", "audio-spectrum-scroll", "audio-cursor-marker", "audio-playback-cursor" }
            : new[] { options.Scenario };

        foreach (var scenario in scenarios)
        {
            Console.WriteLine($"uia.scenario.begin={scenario}");
            switch (scenario)
            {
                case "audio-waveform-scroll":
                    await RunScrollAsync(false);
                    break;
                case "audio-spectrum-scroll":
                    if (!TryInvokeSpectrumToggle())
                        throw new InvalidOperationException("Could not locate the Audio > Spectrum Display command through UIA");
                    await Task.Delay(1000);
                    await RunScrollAsync(true);
                    break;
                case "audio-cursor-marker":
                    await RunCursorMarkerAsync();
                    break;
                case "audio-playback-cursor":
                    await RunPlaybackAsync();
                    break;
                default:
                    throw new ArgumentException($"Unknown scenario: {scenario}");
            }
            Console.WriteLine($"uia.scenario.end={scenario}");
        }

        if (!options.KeepOpen)
            Close();
    }

    public void DumpWindowTree()
    {
        if (mainWindow is null || process is null)
            return;
        Console.WriteLine("uia.tree.begin");
        DumpAutomation(mainWindow, 0, 4);
        Console.WriteLine("uia.win32.begin");
        foreach (var child in EnumerateChildWindows(process.MainWindowHandle))
        {
            var rect = GetWindowRect(child);
            Console.WriteLine($"uia.win32 hwnd=0x{child:X} parent=0x{GetParent(child):X} class={GetClassName(child)} rect={rect.Left},{rect.Top},{rect.Width}x{rect.Height} visible={IsWindowVisible(child)}");
        }
        Console.WriteLine("uia.tree.end");
    }

    private async Task RunScrollAsync(bool spectrum)
    {
        EnsureForeground();
        var center = audioCanvasRect.Center;
        var end = Stopwatch.GetTimestamp() + Stopwatch.Frequency * options.DurationSeconds;
        var direction = 1;
        var nextFlip = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 2;
        while (Stopwatch.GetTimestamp() < end)
        {
            MovePointer(center);
            SendMouseWheel(direction * -120);
            await Task.Delay(45);
            if (Stopwatch.GetTimestamp() >= nextFlip)
            {
                direction = -direction;
                nextFlip = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 2;
            }
        }
        Console.WriteLine($"uia.scroll.spectrum={spectrum}");
    }

    private async Task RunCursorMarkerAsync()
    {
        EnsureForeground();
        var centerY = audioCanvasRect.Top + Math.Max(10, audioCanvasRect.Height / 2);
        var left = audioCanvasRect.Left + audioCanvasRect.Width / 5;
        var right = audioCanvasRect.Right - audioCanvasRect.Width / 5;
        var end = Stopwatch.GetTimestamp() + Stopwatch.Frequency * options.DurationSeconds;
        while (Stopwatch.GetTimestamp() < end)
        {
            for (var i = 0; i <= 40; ++i)
            {
                var x = left + (right - left) * i / 40;
                MovePointer(new WinPoint(x, centerY));
                await Task.Delay(20);
            }
            Drag(new WinPoint(left, centerY), new WinPoint(right, centerY), 24, 12);
            for (var i = 40; i >= 0; --i)
            {
                var x = left + (right - left) * i / 40;
                MovePointer(new WinPoint(x, centerY));
                await Task.Delay(20);
            }
        }
    }

    private async Task RunPlaybackAsync()
    {
        EnsureForeground();
        SendKey((ushort)'B');
        await Task.Delay(TimeSpan.FromSeconds(options.DurationSeconds));
        SendKey((ushort)'H');
    }

    private bool TryInvokeSpectrumToggle()
    {
        if (mainWindow is null)
            return false;

        // wx menu children are materialized only after the top-level Audio menu
        // is expanded. Use UIA for the expansion so the operation remains
        // independent of localized accelerator keys.
        var audioMenu = mainWindow.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
            .Cast<AutomationElement>()
            .FirstOrDefault(element => string.Equals(element.Current.Name, "Audio", StringComparison.OrdinalIgnoreCase));
        if (audioMenu is not null && audioMenu.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var expandPattern))
        {
            try
            {
                ((ExpandCollapsePattern)expandPattern).Expand();
            }
            catch (ElementNotEnabledException)
            {
                // wx exposes the top-level menu with an ExpandCollapse pattern
                // even when that provider does not support Expand. The toolbar
                // button lookup below remains valid in that case.
            }
            catch (InvalidOperationException)
            {
                // Same provider quirk on older UIA implementations.
            }
            Thread.Sleep(150);
        }

        var buttons = mainWindow.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
        foreach (AutomationElement element in buttons)
        {
            var name = element.Current.Name ?? string.Empty;
            if (!ContainsAny(name, "spectrum", "analyzer", "频谱", "频谱分析"))
                continue;
            if (element.TryGetCurrentPattern(InvokePattern.Pattern, out var pattern))
            {
                ((InvokePattern)pattern).Invoke();
                Console.WriteLine($"uia.spectrum_toggle=button:{name}");
                return true;
            }
        }

        var menuItems = mainWindow.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem));
        foreach (AutomationElement element in menuItems)
        {
            var name = element.Current.Name ?? string.Empty;
            if (!ContainsAny(name, "Spectrum Display", "Spectrum analyzer", "频谱"))
                continue;
            if (element.TryGetCurrentPattern(InvokePattern.Pattern, out var pattern))
            {
                ((InvokePattern)pattern).Invoke();
                Console.WriteLine($"uia.spectrum_toggle=menu:{name}");
                return true;
            }
        }

        // wx may expose neither the menu popup nor the custom CH button to
        // UIA. The shipped English menu has a stable mnemonic: Alt+A opens
        // Audio and S selects Spectrum Display. Keep this fallback explicit.
        SendChord(0x12, (ushort)'A');
        Thread.Sleep(150);
        SendKey((ushort)'S');
        Thread.Sleep(250);
        var optionsButton = mainWindow.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
            .Cast<AutomationElement>()
            .FirstOrDefault(element => string.Equals((element.Current.Name ?? string.Empty).Trim(), "CH", StringComparison.OrdinalIgnoreCase));
        if (optionsButton is not null)
        {
            var enabled = optionsButton.Current.IsEnabled;
            Console.WriteLine($"uia.spectrum_toggle=keyboard:Alt+A,S enabled={enabled}");
            return enabled;
        }

        Console.WriteLine("uia.spectrum_toggle=keyboard:Alt+A,S unverified");
        return true;
    }

    private void WaitForMainWindow()
    {
        if (process is null)
            throw new InvalidOperationException("Process was not started");
        process.WaitForInputIdle(30000);
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 30;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            mainWindow = RefreshMainWindow();
            if (mainWindow is not null)
                return;
            Thread.Sleep(250);
        }
        throw new TimeoutException("Aegisub main window was not discovered");
    }

    private AutomationElement? RefreshMainWindow()
    {
        if (process is null || process.HasExited)
            return null;
        process.Refresh();
        if (process.MainWindowHandle == 0)
            return null;
        return AutomationElement.FromHandle(process.MainWindowHandle);
    }

    private void SetWindowSize()
    {
        if (process is null)
            return;
        SetWindowPos(process.MainWindowHandle, 0, 40, 40, options.Width, options.Height,
            SetWindowPosFlags.ShowWindow | SetWindowPosFlags.NoActivate);
        EnsureForeground();
    }

    private nint FindAudioCanvas(AutomationElement root, bool requireSkiaCanvas)
    {
        var rootHandle = new nint(root.Current.NativeWindowHandle);
        var mainRect = GetWindowRect(rootHandle);
        var candidates = new List<(nint Hwnd, WinRect Rect, string Class, nint Parent)>();
        foreach (var hwnd in EnumerateChildWindows(rootHandle))
        {
            if (!IsWindowVisible(hwnd))
                continue;
            var rect = GetWindowRect(hwnd);
            // Keep narrow horizontal panes in the inventory: the AudioBox
            // toolbar is used to distinguish the display from sibling panes.
            if (rect.Width < 160 || rect.Height < 20)
                continue;
            candidates.Add((hwnd, rect, GetClassName(hwnd), GetParent(hwnd)));
        }
        if (candidates.Count == 0)
            return 0;

        // The opt-in Skia Audio Display is itself a wxGLCanvas. Prefer that
        // concrete child over surrounding wxWindow panes; otherwise wheel
        // input can land on the lower AudioBox container instead of Skia.
        var toolbarCandidates = candidates
            .Where(item => item.Class.Equals("wxWindow", StringComparison.OrdinalIgnoreCase)
                && item.Rect.Width > 500
                && item.Rect.Height is >= 20 and <= 70)
            .ToArray();
        var skiaCanvas = candidates
            .Where(item => item.Class.Equals("wxGLCanvas", StringComparison.OrdinalIgnoreCase)
                && item.Rect.Width > 500
                && item.Rect.Height >= 70)
            .Select(canvas => new
            {
                Canvas = canvas,
                ToolbarGap = toolbarCandidates
                    .Where(toolbar => toolbar.Rect.Top >= canvas.Rect.Bottom
                        && toolbar.Rect.Top - canvas.Rect.Bottom <= 64
                        && toolbar.Rect.Width >= canvas.Rect.Width * 0.85)
                    .Select(toolbar => toolbar.Rect.Top - canvas.Rect.Bottom)
                    .DefaultIfEmpty(int.MaxValue)
                    .Min()
            })
            .OrderBy(item => item.ToolbarGap)
            .ThenByDescending(item => item.Canvas.Rect.Area)
            .Select(item => item.Canvas)
            .FirstOrDefault();
        if (skiaCanvas.Hwnd != 0)
            return skiaCanvas.Hwnd;
        if (requireSkiaCanvas)
            return 0;

        // AudioBox contains the canvas immediately above its narrow toolbar.
        // wx exposes both the AudioBox container and the actual wxGLCanvas as
        // generic wxWindow children, so the adjacent-toolbar relationship is
        // more stable than a class-name check across wx versions.
        var toolbars = candidates.Where(item => item.Class.Equals("wxWindow", StringComparison.OrdinalIgnoreCase)
            && item.Rect.Width > 500
            && item.Rect.Height is >= 20 and <= 70);
        var adjacent = candidates
            .Where(item => item.Rect.Width > 500
                && item.Rect.Height is >= 70 and <= 900
                && !candidates.Any(child => child.Parent == item.Hwnd
                    && child.Rect.Width > 500
                    && child.Rect.Height >= 70))
            .SelectMany(canvas => toolbars
                .Where(toolbar => toolbar.Rect.Top >= canvas.Rect.Bottom
                    && toolbar.Rect.Top - canvas.Rect.Bottom <= 64
                    && toolbar.Rect.Width >= canvas.Rect.Width * 0.85)
                .Select(toolbar => (canvas, toolbar)))
            .OrderBy(pair => pair.toolbar.Rect.Top - pair.canvas.Rect.Bottom)
            .ThenBy(pair => pair.canvas.Rect.Height)
            .FirstOrDefault();
        if (adjacent.canvas.Hwnd != 0)
            return adjacent.canvas.Hwnd;

        // The layout may still be settling while the audio file is opened.
        // Returning an arbitrary bottom pane here can direct input at the
        // subtitle grid, so let WaitForAudioCanvas retry instead.
        return 0;
    }

    private void EnsureForeground()
    {
        if (process is null)
            return;
        ShowWindow(process.MainWindowHandle, 9);
        SetForegroundWindow(process.MainWindowHandle);
        Thread.Sleep(100);
    }

    private void Close()
    {
        if (process is null || process.HasExited)
            return;
        process.CloseMainWindow();
        if (!process.WaitForExit(15000))
        {
            Console.WriteLine("uia.close=timeout");
            process.Kill(entireProcessTree: true);
        }
    }

    private void Drag(WinPoint start, WinPoint end, int steps, int delayMs)
    {
        MovePointer(start);
        SendMouseButton(true);
        for (var i = 1; i <= steps; ++i)
        {
            MovePointer(new WinPoint(start.X + (end.X - start.X) * i / steps,
                start.Y + (end.Y - start.Y) * i / steps));
            Thread.Sleep(delayMs);
        }
        SendMouseButton(false);
    }

    private static bool ContainsAny(string value, params string[] needles) =>
        needles.Any(needle => value.Contains(needle, StringComparison.OrdinalIgnoreCase));

    private static bool IsRuntimeOptIn(string? value) =>
        value is not null && (value.Equals("1", StringComparison.OrdinalIgnoreCase)
            || value.Equals("true", StringComparison.OrdinalIgnoreCase)
            || value.Equals("on", StringComparison.OrdinalIgnoreCase));

    private static void MovePointer(WinPoint point)
    {
        var left = GetSystemMetrics(SystemMetric.VirtualScreenLeft);
        var top = GetSystemMetrics(SystemMetric.VirtualScreenTop);
        var width = Math.Max(1, GetSystemMetrics(SystemMetric.VirtualScreenWidth) - 1);
        var height = Math.Max(1, GetSystemMetrics(SystemMetric.VirtualScreenHeight) - 1);
        var input = new Input
        {
            Type = InputMouse,
            Mouse = new MouseInput
            {
                Flags = MouseEventFlags.Move | MouseEventFlags.Absolute,
                X = (int)Math.Clamp((point.X - left) * 65535L / width, 0, 65535),
                Y = (int)Math.Clamp((point.Y - top) * 65535L / height, 0, 65535),
            }
        };
        SendInputChecked(new[] { input });
    }

    private static void SendMouseWheel(int delta)
    {
        SendMouseInput(MouseEventFlags.Wheel, unchecked((uint)delta));
    }

    private static void SendMouseButton(bool down)
    {
        SendMouseInput(down ? MouseEventFlags.LeftDown : MouseEventFlags.LeftUp, 0);
    }

    private static void SendMouseInput(MouseEventFlags flags, uint data)
    {
        var input = new Input
        {
            Type = InputMouse,
            Mouse = new MouseInput { Flags = flags, Data = data }
        };
        SendInputChecked(new[] { input });
    }

    private static void SendKey(ushort virtualKey)
    {
        var down = new Input
        {
            Type = InputKeyboard,
            Keyboard = new KeyboardInput { VirtualKey = virtualKey }
        };
        var up = new Input
        {
            Type = InputKeyboard,
            Keyboard = new KeyboardInput { VirtualKey = virtualKey, Flags = KeyboardEventFlags.KeyUp }
        };
        SendInputChecked(new[] { down, up });
    }

    private static void SendChord(ushort modifier, ushort key)
    {
        var inputs = new[]
        {
            new Input
            {
                Type = InputKeyboard,
                Keyboard = new KeyboardInput { VirtualKey = modifier }
            },
            new Input
            {
                Type = InputKeyboard,
                Keyboard = new KeyboardInput { VirtualKey = key }
            },
            new Input
            {
                Type = InputKeyboard,
                Keyboard = new KeyboardInput { VirtualKey = key, Flags = KeyboardEventFlags.KeyUp }
            },
            new Input
            {
                Type = InputKeyboard,
                Keyboard = new KeyboardInput { VirtualKey = modifier, Flags = KeyboardEventFlags.KeyUp }
            }
        };
        SendInputChecked(inputs);
    }

    private static void SendInputChecked(Input[] inputs)
    {
        var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>());
        if (sent != inputs.Length)
        {
            var error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, $"SendInput sent {sent}/{inputs.Length} input events");
        }
    }

    private static void DumpAutomation(AutomationElement element, int depth, int maxDepth)
    {
        if (depth > maxDepth)
            return;
        var current = element.Current;
        var indent = new string(' ', depth * 2);
        Console.WriteLine($"uia.node depth={depth} type={current.ControlType.ProgrammaticName} name={JsonEscape(current.Name)} class={JsonEscape(current.ClassName)} hwnd=0x{current.NativeWindowHandle:X}");
        foreach (AutomationElement child in element.FindAll(TreeScope.Children, Condition.TrueCondition))
            DumpAutomation(child, depth + 1, maxDepth);
    }

    private static string JsonEscape(string? value) =>
        (value ?? string.Empty).Replace("\\", "\\\\").Replace("\"", "\\\"");

    private static IEnumerable<nint> EnumerateChildWindows(nint parent)
    {
        var result = new List<nint>();
        EnumChildWindows(parent, (hwnd, _) =>
        {
            result.Add(hwnd);
            return true;
        }, 0);
        return result;
    }

    private static string GetClassName(nint hwnd)
    {
        var buffer = new StringBuilder(256);
        return GetClassName(hwnd, buffer, buffer.Capacity) == 0 ? string.Empty : buffer.ToString();
    }

    private static WinRect GetWindowRect(nint hwnd)
    {
        GetWindowRect(hwnd, out var rect);
        return new WinRect(rect.Left, rect.Top, rect.Right - rect.Left, rect.Bottom - rect.Top);
    }

    public void Dispose()
    {
        if (!options.KeepOpen && process is not null && !process.HasExited)
            Close();
        process?.Dispose();
    }

    private const uint InputMouse = 0;
    private const uint InputKeyboard = 1;

    [Flags]
    private enum SetWindowPosFlags : uint { NoActivate = 0x0010, ShowWindow = 0x0040 }
    [Flags]
    private enum MouseEventFlags : uint { Move = 0x0001, LeftDown = 0x0002, LeftUp = 0x0004, Wheel = 0x0800, Absolute = 0x8000 }
    [Flags]
    private enum KeyboardEventFlags : uint { KeyUp = 0x0002 }
    private enum SystemMetric { VirtualScreenLeft = 76, VirtualScreenTop = 77, VirtualScreenWidth = 78, VirtualScreenHeight = 79 }

    private readonly record struct WinPoint(int X, int Y);
    private readonly record struct WinRect(int Left, int Top, int Width, int Height)
    {
        public int Right => Left + Width;
        public int Bottom => Top + Height;
        public long Area => (long)Width * Height;
        public WinPoint Center => new(Left + Width / 2, Top + Height / 2);
    }

    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public InputUnion Union; public MouseInput Mouse { get => Union.Mouse; set => Union.Mouse = value; } public KeyboardInput Keyboard { get => Union.Keyboard; set => Union.Keyboard = value; } }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public MouseInput Mouse; [FieldOffset(0)] public KeyboardInput Keyboard; }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput { public int X, Y; public uint Data; public MouseEventFlags Flags; public uint Time; public nint ExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] private struct KeyboardInput { public ushort VirtualKey, ScanCode; public KeyboardEventFlags Flags; public uint Time; public nint ExtraInfo; }

    private delegate bool EnumWindowsProc(nint hwnd, nint lParam);

    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint inputCount, Input[] inputs, int size);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(nint parent, EnumWindowsProc callback, nint lParam);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")] private static extern nint GetParent(nint hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(nint hwnd, StringBuilder className, int maxCount);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint hwnd, out NativeRect rect);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint hwnd);
    [DllImport("user32.dll")] private static extern bool ShowWindow(nint hwnd, int command);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint hwnd, nint insertAfter, int x, int y, int width, int height, SetWindowPosFlags flags);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(SystemMetric index);
}
