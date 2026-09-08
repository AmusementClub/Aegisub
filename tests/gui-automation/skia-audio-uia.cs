#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.ComponentModel;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

try
{
    return await RunAsync(args);
}
catch (Exception error)
{
    Console.Error.WriteLine($"uia.error={error.GetType().Name}:{error.Message}");
    Console.Error.WriteLine(error);
    return 1;
}

static async Task<int> RunAsync(string[] arguments)
{
    var options = DriverOptions.Parse(arguments);
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
        {
            app.MarkScenarioSucceeded();
            return 0;
        }
    }

    app.WaitForRequiredCanvas();
    if (options.DryRun)
    {
        app.MarkScenarioSucceeded();
        Console.WriteLine("uia.dry_run=ok");
        return 0;
    }

    await app.RunScenarioAsync();
    return 0;
}

sealed record DriverOptions(
    string Executable,
    string Scenario,
    int DurationSeconds,
    string? Project,
    string? Audio,
    string? AudioProvider,
    int? AudioCacheType,
    string? AudioPlayer,
    bool? AudioSpectrum,
    bool? CursorTime,
    string? Video,
    string? Artifacts,
    int Width,
    int Height,
    bool DumpTree,
    bool DryRun,
    bool KeepOpen,
    int WarmupSeconds,
    int ScrollDelta,
    int ScrollIntervalMilliseconds,
    bool AllowGlobalInput)
{
    public bool Help { get; init; }

    public bool RequiresAudioCanvas =>
        !Scenario.Equals("video-crosshair-sweep", StringComparison.OrdinalIgnoreCase);

    public bool RequiresVideoCanvas =>
        Scenario.Equals("video-crosshair-sweep", StringComparison.OrdinalIgnoreCase)
        || Scenario.Equals("video-playback-audio-scroll", StringComparison.OrdinalIgnoreCase)
        || Scenario.Equals("video-playback-audio-scrollbar-drag", StringComparison.OrdinalIgnoreCase)
        || Scenario.Equals("audio-middle-seek-cursor", StringComparison.OrdinalIgnoreCase)
        || Scenario.Equals("audio-spectrum-middle-seek-cursor", StringComparison.OrdinalIgnoreCase)
        || Scenario.Equals("audio-spectrum-playback-middle-seek", StringComparison.OrdinalIgnoreCase);

    public bool IsFocusColourScenario =>
        Scenario.Equals("audio-waveform-focus-colour", StringComparison.OrdinalIgnoreCase)
        || Scenario.Equals("audio-spectrum-focus-colour", StringComparison.OrdinalIgnoreCase);

    public static DriverOptions Parse(string[] args)
    {
        string? executable = null;
        string scenario = "audio-waveform-scroll";
        int duration = 30;
        string? project = null;
        string? audio = null;
        string? audioProvider = null;
        int? audioCacheType = null;
        string? audioPlayer = null;
        bool? audioSpectrum = null;
        bool? cursorTime = null;
        string? video = null;
        string? artifacts = null;
        int width = 1280;
        int height = 900;
        bool dumpTree = false;
        bool dryRun = false;
        bool keepOpen = false;
        bool allowGlobalInput = false;
        int warmup = 5;
        int scrollDelta = 120;
        int scrollIntervalMilliseconds = 45;
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
                case "--scroll-delta":
                    scrollDelta = ParsePositive(Value(), arg);
                    break;
                case "--scroll-interval-ms":
                    scrollIntervalMilliseconds = ParsePositive(Value(), arg);
                    break;
                case "--project":
                    project = Value();
                    break;
                case "--audio":
                    audio = Value();
                    break;
                case "--audio-provider":
                    audioProvider = ParseNonEmpty(Value(), arg);
                    break;
                case "--audio-cache-type":
                    audioCacheType = ParseAudioCacheType(Value(), arg);
                    break;
                case "--audio-player":
                    audioPlayer = ParseNonEmpty(Value(), arg);
                    break;
                case "--audio-view":
                    audioSpectrum = ParseAudioView(Value(), arg);
                    break;
                case "--cursor-time":
                    cursorTime = ParseOnOff(Value(), arg);
                    break;
                case "--video":
                    video = Value();
                    break;
                case "--artifacts":
                    artifacts = Value();
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
                case "--allow-global-input":
                    allowGlobalInput = true;
                    break;
                default:
                    throw new ArgumentException($"Unknown option: {arg}");
            }
        }

        if (help)
            return new DriverOptions(executable ?? "Aegisub.exe", scenario, duration, project, audio,
                audioProvider, audioCacheType, audioPlayer, audioSpectrum, cursorTime, video,
                artifacts,
                width, height, dumpTree, dryRun, keepOpen, warmup, scrollDelta, scrollIntervalMilliseconds,
                allowGlobalInput) { Help = true };
        if (string.IsNullOrWhiteSpace(executable))
            throw new ArgumentException("--exe is required");
        if (!dryRun && ScenarioRequiresAudioCanvas(scenario) && string.IsNullOrWhiteSpace(audio))
            throw new ArgumentException("--audio is required for an automated audio scenario");
        if (!dryRun && ScenarioRequiresVideoCanvas(scenario) && string.IsNullOrWhiteSpace(video))
            throw new ArgumentException("--video is required for an automated video scenario");

        return new DriverOptions(Path.GetFullPath(executable), scenario, duration, project, audio,
            audioProvider, audioCacheType, audioPlayer, audioSpectrum, cursorTime, video,
            string.IsNullOrWhiteSpace(artifacts) ? null : Path.GetFullPath(artifacts),
            width, height, dumpTree, dryRun, keepOpen, warmup, scrollDelta, scrollIntervalMilliseconds,
            allowGlobalInput);
    }

    private static bool ScenarioRequiresAudioCanvas(string scenario) =>
        !scenario.Equals("video-crosshair-sweep", StringComparison.OrdinalIgnoreCase);

    private static bool ScenarioRequiresVideoCanvas(string scenario) =>
        scenario.Equals("video-crosshair-sweep", StringComparison.OrdinalIgnoreCase)
        || scenario.Equals("video-playback-audio-scroll", StringComparison.OrdinalIgnoreCase)
        || scenario.Equals("video-playback-audio-scrollbar-drag", StringComparison.OrdinalIgnoreCase)
        || scenario.Equals("audio-middle-seek-cursor", StringComparison.OrdinalIgnoreCase)
        || scenario.Equals("audio-spectrum-middle-seek-cursor", StringComparison.OrdinalIgnoreCase)
        || scenario.Equals("audio-spectrum-playback-middle-seek", StringComparison.OrdinalIgnoreCase);

    public static void PrintHelp()
    {
        Console.WriteLine("Aegisub background UIA driver");
        Console.WriteLine("  --exe PATH                         Aegisub.exe");
        Console.WriteLine("  --scenario NAME                    audio-waveform-scroll, audio-scrollbar-drag, video-crosshair-sweep, video-playback-audio-scroll, video-playback-audio-scrollbar-drag, audio-spectrum-scroll, audio-spectrum-scrollbar-drag, audio-cursor-marker, audio-spectrum-cursor-marker, audio-playback-cursor, audio-spectrum-playback-cursor, audio-playback-marker-drag, audio-spectrum-playback-marker-drag, audio-middle-seek-cursor, audio-spectrum-middle-seek-cursor, audio-spectrum-playback-middle-seek, audio-cursor-state-matrix, audio-waveform-focus-colour, audio-spectrum-focus-colour, audio-waveform-dpi-transition, audio-spectrum-dpi-transition, audio-waveform-runtime-fallback, audio-spectrum-runtime-fallback, audio-waveform-playback-scroll, audio-spectrum-playback-scroll, all");
        Console.WriteLine("  --duration-seconds N               active scenario duration (default 30)");
        Console.WriteLine("  --warmup-seconds N                 warmup before input (default 5)");
        Console.WriteLine("  --scroll-delta N                   wheel delta magnitude for scroll scenarios (default 120)");
        Console.WriteLine("  --scroll-interval-ms N             delay between wheel messages (default 45)");
        Console.WriteLine("  --project PATH                     optional ASS/project file passed at startup");
        Console.WriteLine("  --audio PATH                       optional audio file passed at startup");
        Console.WriteLine("  --audio-provider NAME              optional Audio/Provider profile override");
        Console.WriteLine("  --audio-cache-type N               optional Audio/Cache/Type override (0-2)");
        Console.WriteLine("  --audio-player NAME                optional Audio/Player profile override");
        Console.WriteLine("  --audio-view waveform|spectrum     optional Audio/Spectrum profile override");
        Console.WriteLine("  --cursor-time on|off               optional mouse cursor time-label override");
        Console.WriteLine("  --video PATH                       optional video file passed at startup");
        Console.WriteLine("  --artifacts PATH                   persistent artifacts directory for perf sessions");
        Console.WriteLine("  --width N --height N               fixed top-level window size (default 1280x900)");
        Console.WriteLine("  --dump-tree                        print UIA and Win32 child window inventory");
        Console.WriteLine("  --dry-run                          start, inspect and exit without input");
        Console.WriteLine("  --keep-open                        leave Aegisub open after the scenario");
        Console.WriteLine("  --allow-global-input               allow foreground SendInput for pointer cursor scenarios");
    }

    private static int ParsePositive(string value, string option) =>
        int.TryParse(value, out var result) && result > 0
            ? result
            : throw new ArgumentException($"{option} must be a positive integer");

    private static int ParseNonNegative(string value, string option) =>
        int.TryParse(value, out var result) && result >= 0
            ? result
            : throw new ArgumentException($"{option} must be a non-negative integer");

    private static int ParseAudioCacheType(string value, string option) =>
        int.TryParse(value, out var result) && result is >= 0 and <= 2
            ? result
            : throw new ArgumentException($"{option} must be 0, 1, or 2");

    private static string ParseNonEmpty(string value, string option) =>
        !string.IsNullOrWhiteSpace(value)
            ? value
            : throw new ArgumentException($"{option} must not be empty");

    private static bool ParseAudioView(string value, string option) =>
        value.Equals("spectrum", StringComparison.OrdinalIgnoreCase)
            ? true
            : value.Equals("waveform", StringComparison.OrdinalIgnoreCase)
                ? false
                : throw new ArgumentException($"{option} must be waveform or spectrum");

    private static bool ParseOnOff(string value, string option) =>
        value.Equals("on", StringComparison.OrdinalIgnoreCase)
            || value.Equals("true", StringComparison.OrdinalIgnoreCase)
            || value.Equals("1", StringComparison.OrdinalIgnoreCase)
            ? true
            : value.Equals("off", StringComparison.OrdinalIgnoreCase)
                || value.Equals("false", StringComparison.OrdinalIgnoreCase)
                || value.Equals("0", StringComparison.OrdinalIgnoreCase)
                ? false
                : throw new ArgumentException($"{option} must be on or off");
}

sealed class AegisubSession : IDisposable
{
    private readonly DriverOptions options;
    private Process? process;
    private AutomationElement? mainWindow;
    private string? automationProjectDirectory;
    private string? automationProjectPath;
    private string? automationProfileDirectory;
    private string? automationArtifactsDirectory;
    private nint audioCanvas;
    private WinRect audioCanvasRect;
    private readonly bool preferSkiaCanvas = IsRuntimeOptIn(
        Environment.GetEnvironmentVariable("AEGISUB_ENABLE_SKIA_AUDIO_DISPLAY"));
    private bool scenarioSucceeded;
    private bool cleanShutdown;
    private double? dpiTransitionInitialScale;
    private double? dpiTransitionTargetScale;
    private int runtimeFallbackRequiredSkiaFrames;

    public AegisubSession(DriverOptions options) => this.options = options;

    public void MarkScenarioSucceeded() => scenarioSucceeded = true;

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
        if (!string.IsNullOrWhiteSpace(options.Video))
        {
            var videoPath = Path.GetFullPath(options.Video);
            if (!File.Exists(videoPath))
                throw new FileNotFoundException("Video file was not found", videoPath);
        }

        automationProjectPath = PrepareAutomationProject();
        automationProfileDirectory = Path.Combine(automationProjectDirectory!, "profile");
        automationArtifactsDirectory = string.IsNullOrWhiteSpace(options.Artifacts)
            ? Path.Combine(automationProjectDirectory!, "artifacts")
            : Path.GetFullPath(options.Artifacts);
        Directory.CreateDirectory(automationArtifactsDirectory);
        WriteAutomationConfig(automationProfileDirectory, options);
        Console.WriteLine($"uia.project={automationProjectPath}");

        var startInfo = new ProcessStartInfo
        {
            FileName = options.Executable,
            WorkingDirectory = Path.GetDirectoryName(options.Executable) ?? Environment.CurrentDirectory,
            UseShellExecute = false,
            WindowStyle = ProcessWindowStyle.Minimized,
        };
        startInfo.ArgumentList.Add("--gui-test");
        startInfo.ArgumentList.Add("host");
        startInfo.ArgumentList.Add("--profile-dir");
        startInfo.ArgumentList.Add(automationProfileDirectory);
        startInfo.ArgumentList.Add("--artifacts");
        startInfo.ArgumentList.Add(automationArtifactsDirectory);
        startInfo.ArgumentList.Add("--open");
        startInfo.ArgumentList.Add(automationProjectPath);
        if (!string.IsNullOrWhiteSpace(options.Audio))
        {
            startInfo.ArgumentList.Add("--open");
            startInfo.ArgumentList.Add(Path.GetFullPath(options.Audio));
        }
        if (!string.IsNullOrWhiteSpace(options.Video))
        {
            startInfo.ArgumentList.Add("--open");
            startInfo.ArgumentList.Add(Path.GetFullPath(options.Video));
        }

        process = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start Aegisub");
        Console.WriteLine($"uia.pid={process.Id}");
        var readyPath = Path.Combine(automationArtifactsDirectory, "ready.json");
        AutomationProtocol.WaitForReadyArtifact(
            readyPath,
            process,
            TimeSpan.FromSeconds(30));
        Console.WriteLine($"uia.ready={readyPath}");
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
            if (process is not null && process.HasExited)
                throw new InvalidOperationException($"Aegisub exited before an audio canvas was ready (exit code {process.ExitCode})");
            ThrowIfFatalDialog();
            if (root is null)
                break;
            audioCanvas = FindAudioCanvas(root, preferSkiaCanvas);
            if (audioCanvas != 0)
            {
                var mainWindowHandle = new nint(root.Current.NativeWindowHandle);
                if (mainWindowHandle == 0 || !IsWindowEnabled(mainWindowHandle))
                {
                    Thread.Sleep(250);
                    root = RefreshMainWindow();
                    mainWindow = root;
                    continue;
                }
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

    public void WaitForRequiredCanvas()
    {
        if (options.RequiresAudioCanvas)
        {
            WaitForAudioCanvas();
            return;
        }

        var videoCanvas = WaitForVideoCanvas();
        var rect = GetWindowRect(videoCanvas);
        Console.WriteLine($"uia.video_canvas_hwnd=0x{videoCanvas:X}");
        Console.WriteLine($"uia.video_canvas_class={GetClassName(videoCanvas)}");
        Console.WriteLine($"uia.video_canvas_rect={rect.Left},{rect.Top},{rect.Width}x{rect.Height}");
    }

    public async Task RunScenarioAsync()
    {
        if (options.WarmupSeconds > 0)
        {
            Console.WriteLine($"uia.warmup_seconds={options.WarmupSeconds}");
            await Task.Delay(TimeSpan.FromSeconds(options.WarmupSeconds));
        }

        // AudioDisplaySlot may replace a failed startup Skia canvas with the
        // legacy widget while the audio provider is still settling. Resolve
        // the target again after warmup so input never uses the stale HWND.
        if (options.RequiresAudioCanvas)
            WaitForAudioCanvas();

        var scenarios = options.Scenario.Equals("all", StringComparison.OrdinalIgnoreCase)
            ? new[] { "audio-waveform-scroll", "audio-scrollbar-drag", "audio-spectrum-scroll", "audio-cursor-marker", "audio-playback-cursor" }
            : new[] { options.Scenario };
        var cursorStateMatrixRan = false;
        var playbackMarkerDragRan = false;
        var middleSeekCursorRan = false;
        var focusColourRan = false;
        var focusColourSpectrum = false;
        var dpiTransitionRan = false;
        var runtimeFallbackRan = false;
        var runtimeFallbackSpectrum = false;

        foreach (var scenario in scenarios)
        {
            Console.WriteLine($"uia.scenario.begin={scenario}");
            switch (scenario)
            {
                case "audio-waveform-scroll":
                    await RunScrollAsync(false);
                    break;
                case "audio-scrollbar-drag":
                    await RunScrollbarDragAsync();
                    break;
                case "video-playback-audio-scroll":
                    await RunVideoPlaybackAudioScrollAsync();
                    break;
                case "video-playback-audio-scrollbar-drag":
                    await RunVideoPlaybackAudioScrollbarDragAsync();
                    break;
                case "video-crosshair-sweep":
                    await RunVideoCrosshairSweepAsync();
                    break;
                case "audio-spectrum-scroll":
                    await EnableSpectrumAsync();
                    await RunScrollAsync(true);
                    break;
                case "audio-spectrum-scrollbar-drag":
                    await EnableSpectrumAsync();
                    await RunScrollbarDragAsync();
                    break;
                case "audio-cursor-marker":
                    await RunCursorMarkerAsync();
                    break;
                case "audio-spectrum-cursor-marker":
                    await EnableSpectrumAsync();
                    await RunCursorMarkerAsync();
                    break;
                case "audio-playback-cursor":
                    await RunPlaybackAsync();
                    break;
                case "audio-spectrum-playback-cursor":
                    await EnableSpectrumAsync();
                    await RunPlaybackAsync();
                    break;
                case "audio-playback-marker-drag":
                    await RunPlaybackMarkerDragAsync();
                    playbackMarkerDragRan = true;
                    break;
                case "audio-spectrum-playback-marker-drag":
                    await EnableSpectrumAsync();
                    await RunPlaybackMarkerDragAsync();
                    playbackMarkerDragRan = true;
                    break;
                case "audio-middle-seek-cursor":
                    await RunMiddleSeekCursorAsync();
                    middleSeekCursorRan = true;
                    break;
                case "audio-spectrum-middle-seek-cursor":
                    await EnableSpectrumAsync();
                    await RunMiddleSeekCursorAsync();
                    middleSeekCursorRan = true;
                    break;
                case "audio-spectrum-playback-middle-seek":
                    await EnableSpectrumAsync();
                    await RunPlaybackMiddleSeekAsync();
                    break;
                case "audio-cursor-state-matrix":
                    await RunCursorStateMatrixAsync();
                    cursorStateMatrixRan = true;
                    break;
                case "audio-waveform-focus-colour":
                    await RunFocusColourAsync(false);
                    focusColourRan = true;
                    break;
                case "audio-spectrum-focus-colour":
                    await EnableSpectrumAsync();
                    await RunFocusColourAsync(true);
                    focusColourRan = true;
                    focusColourSpectrum = true;
                    break;
                case "audio-waveform-dpi-transition":
                    dpiTransitionRan = await RunDpiTransitionAsync(false);
                    break;
                case "audio-spectrum-dpi-transition":
                    await EnableSpectrumAsync();
                    dpiTransitionRan = await RunDpiTransitionAsync(true);
                    break;
                case "audio-waveform-runtime-fallback":
                    await RunRuntimeFallbackAsync(false);
                    runtimeFallbackRan = true;
                    break;
                case "audio-spectrum-runtime-fallback":
                    await EnableSpectrumAsync();
                    await RunRuntimeFallbackAsync(true);
                    runtimeFallbackRan = true;
                    runtimeFallbackSpectrum = true;
                    break;
                case "audio-waveform-playback-scroll":
                    await RunPlaybackScrollAsync(false);
                    break;
                case "audio-spectrum-playback-scroll":
                    await EnableSpectrumAsync();
                    await RunPlaybackScrollAsync(true);
                    break;
                default:
                    throw new ArgumentException($"Unknown scenario: {scenario}");
            }
            Console.WriteLine($"uia.scenario.end={scenario}");
        }

        if (!options.KeepOpen)
        {
            Close();
            if (cursorStateMatrixRan)
                ValidateCursorStateMatrixTrace();
            if (playbackMarkerDragRan)
                ValidatePlaybackMarkerDragTrace();
            if (middleSeekCursorRan)
                ValidateMiddleSeekCursorTrace();
            if (focusColourRan)
                ValidateFocusColourTrace(focusColourSpectrum);
            if (dpiTransitionRan)
                ValidateDpiTransitionTrace();
            if (runtimeFallbackRan)
                ValidateRuntimeFallbackTrace(runtimeFallbackSpectrum);
        }
        scenarioSucceeded = true;
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
        var center = audioCanvasRect.Center;
        var end = Stopwatch.GetTimestamp() + Stopwatch.Frequency * options.DurationSeconds;
        var direction = 1;
        var nextFlip = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 2;
        while (Stopwatch.GetTimestamp() < end)
        {
            SendMouseWheelToWindow(audioCanvas, center, direction * -options.ScrollDelta);
            await Task.Delay(options.ScrollIntervalMilliseconds);
            if (Stopwatch.GetTimestamp() >= nextFlip)
            {
                direction = -direction;
                nextFlip = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 2;
            }
        }
        Console.WriteLine($"uia.scroll.spectrum={spectrum} delta={options.ScrollDelta} interval_ms={options.ScrollIntervalMilliseconds}");
    }

    private async Task RunScrollbarDragAsync()
    {
        Console.WriteLine("uia.scrollbar_drag.input=background-window-messages");
        var y = Math.Max(1, audioCanvasRect.Height - 8);
        var left = Math.Max(10, audioCanvasRect.Width / 8);
        var right = Math.Max(left + 1, audioCanvasRect.Width - audioCanvasRect.Width / 8);
        var end = Stopwatch.GetTimestamp() + Stopwatch.Frequency * options.DurationSeconds;
        var forward = true;
        while (Stopwatch.GetTimestamp() < end)
        {
            var startX = forward ? left : right;
            var endX = forward ? right : left;
            SendMouseToWindow(audioCanvas, WindowMessage.MouseMove, startX, y, 0);
            SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonDown, startX, y, 1);
            for (var step = 1; step <= 64; ++step)
            {
                var x = startX + (endX - startX) * step / 64;
                SendMouseToWindow(audioCanvas, WindowMessage.MouseMove, x, y, 1);
                await Task.Delay(8);
            }
            SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonUp, endX, y, 0);
            forward = !forward;
            await Task.Delay(40);
        }
    }

    private async Task RunCursorMarkerAsync()
    {
        if (!options.AllowGlobalInput)
        {
            Console.WriteLine("uia.cursor_marker.input=background-window-messages");
            await RunBackgroundCursorMarkerAsync();
            return;
        }

        Console.WriteLine("uia.cursor_marker.input=foreground-sendinput");
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

    private async Task RunBackgroundCursorMarkerAsync()
    {
        var centerY = Math.Max(10, audioCanvasRect.Height / 2);
        var left = Math.Max(10, audioCanvasRect.Width / 5);
        var right = Math.Max(left + 1, audioCanvasRect.Width - audioCanvasRect.Width / 5);
        var end = Stopwatch.GetTimestamp() + Stopwatch.Frequency * options.DurationSeconds;
        var forward = true;
        while (Stopwatch.GetTimestamp() < end)
        {
            var startX = forward ? left : right;
            var endX = forward ? right : left;
            SendMouseToWindow(audioCanvas, WindowMessage.MouseMove, startX, centerY, 0);
            SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonDown, startX, centerY, 1);
            for (var step = 1; step <= 32; ++step)
            {
                var x = startX + (endX - startX) * step / 32;
                SendMouseToWindow(audioCanvas, WindowMessage.MouseMove, x, centerY, 1);
                await Task.Delay(8);
            }
            SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonUp, endX, centerY, 0);
            forward = !forward;
            await Task.Delay(40);
        }
    }

    private async Task RunPlaybackAsync()
    {
        SendKeyToWindow(audioCanvas, (ushort)'B');
        await Task.Delay(TimeSpan.FromSeconds(options.DurationSeconds));
        SendKeyToWindow(audioCanvas, (ushort)'H');
    }

    private async Task RunPlaybackMarkerDragAsync()
    {
        if (!options.AllowGlobalInput)
            throw new InvalidOperationException(
                "audio playback marker drag requires --allow-global-input so native mouse capture is real");

        EnsureForeground();
        WaitForAudioCanvas();
        var centerY = audioCanvasRect.Top + Math.Max(20, audioCanvasRect.Height / 2);
        var selectionStartX = audioCanvasRect.Left + Math.Max(12, audioCanvasRect.Width / 12);
        var outerMarkerX = audioCanvasRect.Right - Math.Max(12, audioCanvasRect.Width / 12);
        var innerMarkerX = audioCanvasRect.Left + audioCanvasRect.Width * 3 / 4;

        Console.WriteLine("uia.playback_marker_drag.phase=prepare-selection");
        Drag(new WinPoint(selectionStartX, centerY), new WinPoint(outerMarkerX, centerY), 32, 10);
        await Task.Delay(350);

        Console.WriteLine("uia.playback_marker_drag.phase=playback");
        // Play to the end of the audio so changing the selection marker cannot
        // terminate playback and hide a cursor/marker ordering regression.
        SendKeyToWindow(audioCanvas, (ushort)'T');
        await Task.Delay(500);

        var currentMarkerX = outerMarkerX;
        var markerMoves = 0;
        var end = Stopwatch.GetTimestamp() + Stopwatch.Frequency * options.DurationSeconds;
        try
        {
            Console.WriteLine("uia.playback_marker_drag.phase=drag");
            while (Stopwatch.GetTimestamp() < end)
            {
                var nextMarkerX = currentMarkerX == outerMarkerX ? innerMarkerX : outerMarkerX;
                Drag(new WinPoint(currentMarkerX, centerY), new WinPoint(nextMarkerX, centerY), 20, 10);
                currentMarkerX = nextMarkerX;
                ++markerMoves;
                await Task.Delay(60);
            }
        }
        finally
        {
            SendKeyToWindow(audioCanvas, (ushort)'H');
        }
        await Task.Delay(350);
        Console.WriteLine($"uia.playback_marker_drag.moves={markerMoves}");
    }

    private async Task RunMiddleSeekCursorAsync()
    {
        if (!options.AllowGlobalInput)
            throw new InvalidOperationException(
                "audio middle seek cursor requires --allow-global-input so native middle-button state is real");

        EnsureForeground();
        WaitForVideoCanvas();
        WaitForAudioCanvas();
        var centerY = audioCanvasRect.Top + Math.Max(20, audioCanvasRect.Height / 2);
        var left = audioCanvasRect.Left + audioCanvasRect.Width / 5;
        var right = audioCanvasRect.Right - audioCanvasRect.Width / 5;
        var outsideRight = audioCanvasRect.Right + 40;

        Console.WriteLine("uia.middle_seek.phase=mouse-before");
        MovePointer(new WinPoint(left, centerY));
        await Task.Delay(350);

        Console.WriteLine("uia.middle_seek.phase=inside-drag");
        MiddleDrag(new WinPoint(left, centerY), new WinPoint(right, centerY), 36, 10);
        await Task.Delay(500);

        Console.WriteLine("uia.middle_seek.phase=outside-release");
        MovePointer(new WinPoint(right, centerY));
        SendMiddleMouseButton(true);
        for (var step = 1; step <= 36; ++step)
        {
            var x = right + (outsideRight - right) * step / 36;
            MovePointer(new WinPoint(x, centerY));
            await Task.Delay(10);
        }
        SendMiddleMouseButton(false);
        await Task.Delay(750);

        Console.WriteLine("uia.middle_seek.phase=reenter");
        WaitForAudioCanvas();
        centerY = audioCanvasRect.Top + Math.Max(20, audioCanvasRect.Height / 2);
        var reenterX = audioCanvasRect.Left + audioCanvasRect.Width / 2;
        MovePointer(new WinPoint(reenterX, centerY));
        await Task.Delay(100);
        if (!GetCursorPos(out var firstCursorPosition))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetCursorPos failed after middle-seek re-entry");
        var firstReenterTarget = WindowFromPoint(firstCursorPosition);
        Console.WriteLine($"uia.middle_seek.reenter_position={firstCursorPosition.X},{firstCursorPosition.Y}");
        Console.WriteLine($"uia.middle_seek.reenter_target=0x{firstReenterTarget:X}:{GetClassName(firstReenterTarget)}");
        MovePointer(new WinPoint(reenterX + Math.Min(20, audioCanvasRect.Width / 10), centerY));
        await Task.Delay(100);
        if (!GetCursorPos(out var secondCursorPosition))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetCursorPos failed after middle-seek re-entry motion");
        var secondReenterTarget = WindowFromPoint(secondCursorPosition);
        Console.WriteLine($"uia.middle_seek.reenter_motion_position={secondCursorPosition.X},{secondCursorPosition.Y}");
        Console.WriteLine($"uia.middle_seek.reenter_motion_target=0x{secondReenterTarget:X}:{GetClassName(secondReenterTarget)}");
        if (firstReenterTarget != audioCanvas || secondReenterTarget != audioCanvas)
            throw new InvalidDataException("Middle-seek re-entry pointer did not land on the Audio Display canvas");
        await Task.Delay(300);
    }

    private async Task RunPlaybackMiddleSeekAsync()
    {
        if (!options.AllowGlobalInput)
            throw new InvalidOperationException(
                "audio playback middle seek requires --allow-global-input so native middle-button state is real");

        EnsureForeground();
        WaitForVideoCanvas();
        WaitForAudioCanvas();
        var centerY = audioCanvasRect.Top + Math.Max(20, audioCanvasRect.Height / 2);
        var playbackStartX = audioCanvasRect.Left + Math.Max(10, audioCanvasRect.Width / 5);
        var lookbackX = playbackStartX - Math.Max(4, audioCanvasRect.Width / 50);

        Console.WriteLine("uia.playback_middle_seek.phase=playback");
        if (!TryInvokeVideoPlayback())
            throw new InvalidOperationException("Could not invoke the Video Play command through background UIA");
        await Task.Delay(1500);

        Console.WriteLine("uia.playback_middle_seek.phase=lookback");
        MovePointer(new WinPoint(playbackStartX, centerY));
        SendMiddleMouseButton(true);
        await Task.Delay(40);
        MovePointer(new WinPoint(lookbackX, centerY));
        await Task.Delay(120);
        SendMiddleMouseButton(false);
        await Task.Delay(2000);
        if (!TryInvokeVideoStop())
            Console.WriteLine("uia.playback_middle_seek.stop=unverified");
    }

    private async Task RunCursorStateMatrixAsync()
    {
        if (!options.AllowGlobalInput)
            throw new InvalidOperationException(
                "audio-cursor-state-matrix requires --allow-global-input so native enter/leave state is real");
        EnsureForeground();
        var centerY = audioCanvasRect.Top + Math.Max(10, audioCanvasRect.Height / 2);
        var firstX = audioCanvasRect.Left + Math.Max(10, audioCanvasRect.Width / 4);
        var secondX = audioCanvasRect.Left + Math.Max(30, audioCanvasRect.Width * 3 / 4);
        var thirdX = audioCanvasRect.Left + Math.Max(10, audioCanvasRect.Width / 2);

        Console.WriteLine("uia.cursor_matrix.phase=mouse-reset-outside");
        MovePointer(new WinPoint(
            GetSystemMetrics(SystemMetric.VirtualScreenLeft) + 5,
            GetSystemMetrics(SystemMetric.VirtualScreenTop) + 5));
        await Task.Delay(250);

        Console.WriteLine("uia.cursor_matrix.phase=mouse-before-playback");
        MovePointer(new WinPoint(firstX, centerY));
        await Task.Delay(350);

        Console.WriteLine("uia.cursor_matrix.phase=playback");
        SendKeyToWindow(audioCanvas, (ushort)'B');
        await Task.Delay(500);
        MovePointer(new WinPoint(secondX, centerY));
        await Task.Delay(TimeSpan.FromSeconds(options.DurationSeconds));

        Console.WriteLine("uia.cursor_matrix.phase=playback-stop");
        SendKeyToWindow(audioCanvas, (ushort)'H');
        await Task.Delay(350);

        Console.WriteLine("uia.cursor_matrix.phase=mouse-leave");
        MovePointer(new WinPoint(
            GetSystemMetrics(SystemMetric.VirtualScreenLeft) + 5,
            GetSystemMetrics(SystemMetric.VirtualScreenTop) + 5));
        await Task.Delay(350);

        Console.WriteLine("uia.cursor_matrix.phase=mouse-reenter");
        MovePointer(new WinPoint(thirdX, centerY));
        await Task.Delay(350);

        if (process is null || process.HasExited || process.MainWindowHandle == 0)
            throw new InvalidOperationException("Aegisub main window disappeared before cursor resize validation");
        Console.WriteLine("uia.cursor_matrix.phase=resize");
        SetWindowPos(process.MainWindowHandle, 0, 40, 40, options.Width + 160, options.Height,
            SetWindowPosFlags.ShowWindow | SetWindowPosFlags.NoActivate);
        await Task.Delay(750);
        WaitForAudioCanvas();
        centerY = audioCanvasRect.Top + Math.Max(10, audioCanvasRect.Height / 2);
        thirdX = audioCanvasRect.Left + Math.Max(10, audioCanvasRect.Width / 2);
        MovePointer(new WinPoint(thirdX, centerY));
        await Task.Delay(350);
    }

    private async Task RunPlaybackScrollAsync(bool spectrum)
    {
        SendKeyToWindow(audioCanvas, (ushort)'B');
        await Task.Delay(500);
        try
        {
            await RunScrollAsync(spectrum);
        }
        finally
        {
            SendKeyToWindow(audioCanvas, (ushort)'H');
        }
    }

    private async Task RunFocusColourAsync(bool spectrum)
    {
        if (process is null || process.HasExited || mainWindow is null)
            throw new InvalidOperationException("Aegisub was not ready for the focus/colour scenario");

        var focusSink = FindNativeFocusSink();
        FocusAudioCanvas();
        await Task.Delay(300);

        FocusNativeWindow(focusSink);
        await Task.Delay(300);
        FocusAudioCanvas();
        await Task.Delay(300);
        FocusNativeWindow(focusSink);
        await Task.Delay(300);

        mainWindow = RefreshMainWindow() ?? mainWindow;
        var configure = UiaDriver.FindEnabledInvokableButton(
            mainWindow,
            "Configure Aegisub", "Options", "Preferences", "设置", "选项");
        if (configure is null)
            throw new InvalidOperationException("Could not locate the Preferences command through UIA");

        UiaDriver.Invoke(configure);
        var preferences = WaitForPreferencesWindow();
        Console.WriteLine($"uia.focus_colour.preferences={preferences.Current.Name}");
        Console.WriteLine($"uia.focus_colour.spectrum={spectrum}");
        if (options.DumpTree)
        {
            Console.WriteLine("uia.preferences_tree.begin");
            DumpAutomation(preferences, 0, 8);
            Console.WriteLine("uia.preferences_tree.end");
        }

        var selection = ChangeAudioColourScheme(preferences, spectrum);
        Console.WriteLine($"uia.focus_colour.scheme={selection.Before}->{selection.After}");
        var apply = WaitForEnabledPreferencesButton(
            preferences,
            TimeSpan.FromSeconds(5),
            "Apply", "应用", "套用");
        UiaDriver.Invoke(apply);
        Console.WriteLine("uia.focus_colour.apply=invoke");
        await Task.Delay(1000);

        if (!preferences.TryGetCurrentPattern(WindowPattern.Pattern, out var windowPattern))
            throw new InvalidOperationException("Preferences window did not expose WindowPattern");
        ((WindowPattern)windowPattern).Close();
        await Task.Delay(300);
    }

    private async Task<bool> RunDpiTransitionAsync(bool spectrum)
    {
        if (process is null || process.HasExited || process.MainWindowHandle == 0)
            throw new InvalidOperationException("Aegisub main window was not ready for the DPI transition scenario");
        if (!preferSkiaCanvas)
            throw new InvalidOperationException(
                "The DPI transition scenario requires AEGISUB_ENABLE_SKIA_AUDIO_DISPLAY so it validates the Skia production handler");

        var hwnd = process.MainWindowHandle;
        var originalRect = GetWindowRect(hwnd);
        var originalDpi = GetDpiForWindow(hwnd);
        if (originalDpi == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetDpiForWindow failed for the Aegisub main window");

        var currentMonitor = MonitorFromWindow(hwnd, MonitorDefaultToNearest);
        var monitors = EnumerateMonitors();
        foreach (var monitor in monitors)
        {
            Console.WriteLine(
                $"uia.dpi_transition.monitor=0x{monitor.Handle:X} dpi={monitor.DpiX}x{monitor.DpiY} work={monitor.Work.Left},{monitor.Work.Top},{monitor.Work.Width}x{monitor.Work.Height}");
        }

        var target = monitors
            .Where(monitor => monitor.Handle != currentMonitor
                && monitor.DpiX != 0
                && monitor.DpiY != 0
                && (monitor.DpiX != originalDpi || monitor.DpiY != originalDpi))
            .OrderBy(monitor => Math.Abs((long)monitor.DpiX - originalDpi))
            .FirstOrDefault();
        if (target.Handle == 0)
        {
            Console.WriteLine($"uia.dpi_transition.renderer={(preferSkiaCanvas ? "skia" : "legacy")}");
            Console.WriteLine($"uia.dpi_transition.spectrum={spectrum}");
            Console.WriteLine($"uia.dpi_transition.initial_dpi={originalDpi}");
            Console.WriteLine("uia.dpi_transition.available=false");
            Console.WriteLine("uia.dpi_transition.trace_validation=not-run:no-different-dpi-monitor");
            return false;
        }

        dpiTransitionInitialScale = originalDpi / 96.0;
        dpiTransitionTargetScale = target.DpiX / 96.0;
        var maximumWidth = Math.Max(200, target.Work.Width - 40);
        var maximumHeight = Math.Max(160, target.Work.Height - 40);
        var targetWidth = Math.Clamp(
            checked((int)Math.Round(originalRect.Width * target.DpiX / (double)originalDpi)),
            Math.Min(320, maximumWidth),
            maximumWidth);
        var targetHeight = Math.Clamp(
            checked((int)Math.Round(originalRect.Height * target.DpiY / (double)originalDpi)),
            Math.Min(240, maximumHeight),
            maximumHeight);
        var targetX = target.Work.Left + Math.Max(0, (target.Work.Width - targetWidth) / 2);
        var targetY = target.Work.Top + Math.Max(0, (target.Work.Height - targetHeight) / 2);
        var settleDelay = TimeSpan.FromSeconds(Math.Clamp(options.DurationSeconds, 2, 5));

        Console.WriteLine($"uia.dpi_transition.renderer={(preferSkiaCanvas ? "skia" : "legacy")}");
        Console.WriteLine($"uia.dpi_transition.spectrum={spectrum}");
        Console.WriteLine($"uia.dpi_transition.initial_dpi={originalDpi}");
        Console.WriteLine($"uia.dpi_transition.target_dpi={target.DpiX}");
        Console.WriteLine("uia.dpi_transition.available=true");
        await Task.Delay(500);

        try
        {
            Console.WriteLine("uia.dpi_transition.phase=move-target");
            SetWindowPosChecked(hwnd, targetX, targetY, targetWidth, targetHeight);
            WaitForWindowDpi(hwnd, target.DpiX, TimeSpan.FromSeconds(10));
            await Task.Delay(settleDelay);
            WaitForAudioCanvas();
        }
        finally
        {
            Console.WriteLine("uia.dpi_transition.phase=restore");
            SetWindowPosChecked(
                hwnd,
                originalRect.Left,
                originalRect.Top,
                originalRect.Width,
                originalRect.Height);
            WaitForWindowDpi(hwnd, originalDpi, TimeSpan.FromSeconds(10));
            await Task.Delay(settleDelay);
            WaitForAudioCanvas();
        }

        Console.WriteLine("uia.dpi_transition.sequence=initial,target,restored");
        return true;
    }

    private static void SetWindowPosChecked(nint hwnd, int x, int y, int width, int height)
    {
        if (!SetWindowPos(
            hwnd,
            0,
            x,
            y,
            width,
            height,
            SetWindowPosFlags.ShowWindow | SetWindowPosFlags.NoActivate))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SetWindowPos failed during DPI transition");
        }
    }

    private static void WaitForWindowDpi(nint hwnd, uint expectedDpi, TimeSpan timeout)
    {
        var deadline = Stopwatch.GetTimestamp()
            + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        uint observedDpi = 0;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            observedDpi = GetDpiForWindow(hwnd);
            if (observedDpi == expectedDpi)
                return;
            Thread.Sleep(50);
        }
        throw new TimeoutException(
            $"Window DPI did not reach {expectedDpi}; last observed value was {observedDpi}");
    }

    private static IReadOnlyList<MonitorDescriptor> EnumerateMonitors()
    {
        var monitors = new List<MonitorDescriptor>();
        bool AddMonitor(nint monitor, nint _, ref NativeRect bounds, nint __)
        {
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
            if (!GetMonitorInfo(monitor, ref info))
                return true;
            var result = GetDpiForMonitor(
                monitor,
                MonitorDpiType.Effective,
                out var dpiX,
                out var dpiY);
            if (result != 0)
            {
                dpiX = 0;
                dpiY = 0;
            }
            monitors.Add(new MonitorDescriptor(
                monitor,
                new WinRect(
                    info.Work.Left,
                    info.Work.Top,
                    info.Work.Right - info.Work.Left,
                    info.Work.Bottom - info.Work.Top),
                dpiX,
                dpiY));
            return true;
        }

        MonitorEnumProc callback = AddMonitor;
        if (!EnumDisplayMonitors(0, 0, callback, 0))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "EnumDisplayMonitors failed");
        GC.KeepAlive(callback);
        return monitors;
    }

    private async Task RunRuntimeFallbackAsync(bool spectrum)
    {
        if (process is null || process.HasExited || process.MainWindowHandle == 0)
            throw new InvalidOperationException("Aegisub main window was not ready for the runtime fallback scenario");
        if (!preferSkiaCanvas)
            throw new InvalidOperationException(
                "The runtime fallback scenario requires AEGISUB_ENABLE_SKIA_AUDIO_DISPLAY");
        var injection = Environment.GetEnvironmentVariable("AEGISUB_SKIA_AUDIO_FAILURE_INJECTION");
        if (!string.Equals(injection, "flush-submit", StringComparison.Ordinal))
            throw new InvalidOperationException(
                "The runtime fallback scenario requires AEGISUB_SKIA_AUDIO_FAILURE_INJECTION=flush-submit");
        var deferredFramesValue = Environment.GetEnvironmentVariable(
            "AEGISUB_SKIA_AUDIO_FAILURE_AFTER_CONTENT_FRAMES");
        if (!int.TryParse(deferredFramesValue, out runtimeFallbackRequiredSkiaFrames)
            || runtimeFallbackRequiredSkiaFrames is < 4 or > 256)
        {
            throw new InvalidOperationException(
                "The runtime fallback scenario requires AEGISUB_SKIA_AUDIO_FAILURE_AFTER_CONTENT_FRAMES in the range 4-256");
        }

        WaitForAudioCanvas();
        if (!GetClassName(audioCanvas).Equals("wxGLCanvas", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Runtime fallback did not start from a Skia wxGLCanvas");
        Console.WriteLine($"uia.runtime_fallback.spectrum={spectrum}");
        Console.WriteLine($"uia.runtime_fallback.deferred_frames={runtimeFallbackRequiredSkiaFrames}");
        Console.WriteLine("uia.runtime_fallback.phase=successful-skia-content");

        var timelineY = Math.Max(1, Math.Min(audioCanvasRect.Height - 20, 6));
        var dragStartX = Math.Max(96, audioCanvasRect.Width * 2 / 3);
        var dragDistance = Math.Max(48, Math.Min(160, audioCanvasRect.Width / 8));
        var dragEndX = Math.Max(1, dragStartX - dragDistance);
        SendMouseToWindow(audioCanvas, WindowMessage.MouseMove, dragStartX, timelineY, 0);
        SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonDown, dragStartX, timelineY, 1);
        SendMouseToWindow(audioCanvas, WindowMessage.MouseMove, dragEndX, timelineY, 1);
        SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonUp, dragEndX, timelineY, 0);
        await Task.Delay(750);
        Console.WriteLine($"uia.runtime_fallback.direct_timeline_scroll={dragDistance}");

        AutomationElement? dialog = FindRuntimeFallbackDialog();
        var centerY = Math.Max(10, audioCanvasRect.Height / 2);
        var left = Math.Max(10, audioCanvasRect.Width / 4);
        var right = Math.Max(left + 1, audioCanvasRect.Width * 3 / 4);
        var maximumMoves = runtimeFallbackRequiredSkiaFrames * 4 + 96;
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 20;
        for (var move = 0;
            dialog is null && move < maximumMoves && Stopwatch.GetTimestamp() < deadline;
            ++move)
        {
            var x = move % 2 == 0 ? left : right;
            try
            {
                SendMouseToWindow(audioCanvas, WindowMessage.MouseMove, x, centerY, 0);
            }
            catch (Win32Exception)
            {
                for (var retry = 0; retry < 20 && dialog is null; ++retry)
                {
                    dialog = FindRuntimeFallbackDialog();
                    if (dialog is null)
                        Thread.Sleep(50);
                }
                if (dialog is null)
                    throw;
                break;
            }
            await Task.Delay(25);
            if (move % 4 == 3)
            {
                dialog = FindRuntimeFallbackDialog();
                if (dialog is not null)
                    break;
            }
        }
        dialog ??= WaitForRuntimeFallbackDialog(TimeSpan.FromSeconds(10));
        Console.WriteLine($"uia.runtime_fallback.dialog={dialog.Current.Name}");

        var switchButton = UiaDriver.FindEnabledInvokableButton(
            dialog,
            "Switch to wx", "切换到 wx", "切換到 wx");
        if (switchButton is null)
            throw new InvalidOperationException("Runtime fallback dialog did not expose the Switch to wx action");
        UiaDriver.Invoke(switchButton);
        Console.WriteLine("uia.runtime_fallback.confirm=switch-to-wx");

        audioCanvas = WaitForLegacyAudioCanvas(TimeSpan.FromSeconds(15));
        audioCanvasRect = GetWindowRect(audioCanvas);
        Console.WriteLine($"uia.runtime_fallback.legacy_canvas_hwnd=0x{audioCanvas:X}");
        Console.WriteLine($"uia.runtime_fallback.legacy_canvas_class={GetClassName(audioCanvas)}");
        Console.WriteLine($"uia.runtime_fallback.legacy_canvas_rect={audioCanvasRect.Left},{audioCanvasRect.Top},{audioCanvasRect.Width}x{audioCanvasRect.Height}");
        await Task.Delay(1000);
        Console.WriteLine("uia.runtime_fallback.phase=legacy-first-frame");
    }

    private AutomationElement? FindRuntimeFallbackDialog()
    {
        if (process is null || process.HasExited)
            return null;
        foreach (var hwnd in EnumerateTopLevelWindows(process.Id))
        {
            if (GetWindowText(hwnd).Contains(
                "Skia Audio Display runtime error",
                StringComparison.OrdinalIgnoreCase))
                return AutomationElement.FromHandle(hwnd);
        }
        return null;
    }

    private AutomationElement WaitForRuntimeFallbackDialog(TimeSpan timeout)
    {
        var deadline = Stopwatch.GetTimestamp()
            + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (process is not null && process.HasExited)
                throw new InvalidOperationException(
                    $"Aegisub exited before the runtime fallback dialog appeared (exit code {process.ExitCode})");
            if (FindRuntimeFallbackDialog() is AutomationElement dialog)
                return dialog;
            Thread.Sleep(50);
        }
        throw new TimeoutException("Skia runtime failure did not open the fallback confirmation dialog");
    }

    private nint WaitForLegacyAudioCanvas(TimeSpan timeout)
    {
        if (process is null)
            throw new InvalidOperationException("Aegisub process was not started");
        var deadline = Stopwatch.GetTimestamp()
            + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (process.HasExited)
                throw new InvalidOperationException(
                    $"Aegisub exited before the legacy Audio Display appeared (exit code {process.ExitCode})");
            mainWindow = RefreshMainWindow() ?? mainWindow;
            if (mainWindow is not null)
            {
                var mainHandle = new nint(mainWindow.Current.NativeWindowHandle);
                var candidate = FindAudioCanvas(mainWindow, false);
                if (candidate != 0
                    && !GetClassName(candidate).Equals("wxGLCanvas", StringComparison.OrdinalIgnoreCase)
                    && mainHandle != 0
                    && IsWindowEnabled(mainHandle))
                {
                    return candidate;
                }
            }
            Thread.Sleep(100);
        }
        throw new TimeoutException("The wx compatibility Audio Display did not replace the failed Skia canvas");
    }

    private ColourSchemeSelection ChangeAudioColourScheme(
        AutomationElement preferences,
        bool spectrum)
    {
        var grids = preferences.FindAll(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Pane));
        var grid = grids.Cast<AutomationElement>().FirstOrDefault(element =>
        {
            try
            {
                return element.Current.NativeWindowHandle != 0
                    && string.Equals(element.Current.Name, "wxPropertyGrid", StringComparison.Ordinal);
            }
            catch (ElementNotAvailableException)
            {
                return false;
            }
        });
        if (grid is null)
            throw new InvalidOperationException("Preferences Colors page did not expose its wxPropertyGrid window");

        var gridHwnd = new nint(grid.Current.NativeWindowHandle);
        SendMouseToWindow(gridHwnd, WindowMessage.LeftButtonDown, 20, 10, 1);
        SendMouseToWindow(gridHwnd, WindowMessage.LeftButtonUp, 20, 10, 0);
        SendKeyToWindow(gridHwnd, VirtualKeyHome);
        for (var step = 0; step < 160; ++step)
            SendKeyToWindow(gridHwnd, VirtualKeyDown);

        var schemeChoice = 0;
        double? lastChoiceTop = null;
        var gridRect = GetWindowRect(gridHwnd);
        var valueX = Math.Clamp(gridRect.Width * 3 / 4, 1, Math.Max(1, gridRect.Width - 2));
        for (var y = 2; y < gridRect.Height - 2; y += 3)
        {
            SendMouseToWindow(gridHwnd, WindowMessage.LeftButtonDown, valueX, y, 1);
            SendMouseToWindow(gridHwnd, WindowMessage.LeftButtonUp, valueX, y, 0);
            Thread.Sleep(10);

            var editors = preferences.FindAll(
                TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Pane));
            var editor = editors.Cast<AutomationElement>().FirstOrDefault(element =>
            {
                try
                {
                    return element.Current.NativeWindowHandle != 0
                        && string.Equals(
                            element.Current.Name,
                            "wxOwnerDrawnComboBox",
                            StringComparison.Ordinal);
                }
                catch (ElementNotAvailableException)
                {
                    return false;
                }
            });
            if (editor is null)
                continue;

            var choiceTop = editor.Current.BoundingRectangle.Top;
            if (lastChoiceTop is double previousTop && Math.Abs(previousTop - choiceTop) < 1.0)
                continue;
            lastChoiceTop = choiceTop;

            ++schemeChoice;
            var expectedChoice = spectrum ? 1 : 2;
            if (schemeChoice != expectedChoice)
                continue;

            var editorHwnd = new nint(editor.Current.NativeWindowHandle);
            SendKeyToWindow(editorHwnd, spectrum ? VirtualKeyUp : VirtualKeyDown);
            return spectrum
                ? new ColourSchemeSelection("Icy Blue", "Green")
                : new ColourSchemeSelection("Green", "Icy Blue");
        }

        throw new InvalidOperationException(
            $"Could not locate the {(spectrum ? "Spectrum" : "Waveform")} colour scheme property in wxPropertyGrid");
    }

    private static AutomationElement WaitForEnabledPreferencesButton(
        AutomationElement preferences,
        TimeSpan timeout,
        params string[] names)
    {
        var deadline = Stopwatch.GetTimestamp()
            + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var button = UiaDriver.FindEnabledInvokableButton(preferences, names);
            if (button is not null)
                return button;
            Thread.Sleep(50);
        }
        throw new TimeoutException(
            $"Preferences button did not become enabled: {string.Join(',', names)}");
    }

    private nint FindNativeFocusSink()
    {
        if (process is null)
            throw new InvalidOperationException("Aegisub process was not started");
        var sink = EnumerateChildWindows(process.MainWindowHandle)
            .FirstOrDefault(hwnd => hwnd != audioCanvas
                && IsWindowVisible(hwnd)
                && GetClassName(hwnd).Equals("Edit", StringComparison.OrdinalIgnoreCase));
        return sink != 0
            ? sink
            : throw new InvalidOperationException("Could not locate a native focus sink outside Audio Display");
    }

    private void FocusAudioCanvas()
    {
        var x = Math.Clamp(audioCanvasRect.Width / 2, 1, Math.Max(1, audioCanvasRect.Width - 2));
        SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonDown, x, 2, 1);
        SendMouseToWindow(audioCanvas, WindowMessage.LeftButtonUp, x, 2, 0);
        WaitForAudioCanvasFocus(true);
        Console.WriteLine("uia.focus_colour.focus=audio");
    }

    private void FocusNativeWindow(nint hwnd)
    {
        var rect = GetWindowRect(hwnd);
        var x = Math.Clamp(rect.Width / 2, 1, Math.Max(1, rect.Width - 2));
        var y = Math.Clamp(rect.Height / 2, 1, Math.Max(1, rect.Height - 2));
        SendMouseToWindow(hwnd, WindowMessage.LeftButtonDown, x, y, 1);
        SendMouseToWindow(hwnd, WindowMessage.LeftButtonUp, x, y, 0);
        WaitForAudioCanvasFocus(false);
        Console.WriteLine($"uia.focus_colour.focus=other:{GetClassName(hwnd)}");
    }

    private void WaitForAudioCanvasFocus(bool expected)
    {
        var threadId = GetWindowThreadProcessId(audioCanvas, out _);
        if (threadId == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not resolve the Audio Display UI thread");
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 5;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var info = new GuiThreadInfo { Size = Marshal.SizeOf<GuiThreadInfo>() };
            if (!GetGUIThreadInfo(threadId, ref info))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "GetGUIThreadInfo failed");
            if ((info.Focus == audioCanvas) == expected)
                return;
            Thread.Sleep(25);
        }
        throw new TimeoutException(expected
            ? "Audio Display did not receive native keyboard focus"
            : "Audio Display did not release native keyboard focus");
    }

    private AutomationElement WaitForPreferencesWindow()
    {
        if (process is null)
            throw new InvalidOperationException("Aegisub process was not started");
        var mainHandle = process.MainWindowHandle.ToInt64();
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 10;
        var lastCandidates = Array.Empty<string>();
        while (Stopwatch.GetTimestamp() < deadline)
        {
            ThrowIfFatalDialog();
            var windows = AutomationElement.RootElement.FindAll(
                TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id));
            var candidates = new List<string>();
            foreach (AutomationElement window in windows)
            {
                try
                {
                    var current = window.Current;
                    if (current.NativeWindowHandle != 0)
                    {
                        candidates.Add($"{current.ControlType.ProgrammaticName}:{current.Name}:{current.ClassName}:0x{current.NativeWindowHandle:X}");
                    }
                    if (current.NativeWindowHandle != 0
                        && current.NativeWindowHandle != mainHandle
                        && (current.ControlType == ControlType.Window
                            || current.ClassName.Equals("wxWindowNR", StringComparison.OrdinalIgnoreCase))
                        && ContainsAny(current.Name ?? string.Empty,
                            "Preferences", "Options", "设置", "选项"))
                        return window;
                }
                catch (ElementNotAvailableException)
                {
                }
            }
            lastCandidates = candidates.ToArray();
            Thread.Sleep(100);
        }
        throw new TimeoutException(
            $"Preferences window was not discovered through UIA; candidates={string.Join('|', lastCandidates)}");
    }

    private Task RunVideoPlaybackAudioScrollAsync() =>
        RunVideoPlaybackAudioActionAsync(() => RunScrollAsync(false));

    private Task RunVideoPlaybackAudioScrollbarDragAsync() =>
        RunVideoPlaybackAudioActionAsync(RunScrollbarDragAsync);

    private async Task RunVideoCrosshairSweepAsync()
    {
        var videoCanvas = WaitForVideoCanvas();
        var rect = GetWindowRect(videoCanvas);
        Console.WriteLine("uia.video_crosshair.input=background-window-messages");
        Console.WriteLine($"uia.video_canvas_hwnd=0x{videoCanvas:X}");
        Console.WriteLine($"uia.video_canvas_class={GetClassName(videoCanvas)}");
        Console.WriteLine($"uia.video_canvas_rect={rect.Left},{rect.Top},{rect.Width}x{rect.Height}");

        // The standard tool draws the crosshair and coordinate label for every
        // mouse position. Sending the key directly to the canvas keeps the
        // scenario isolated from foreground/global input.
        SendKeyToWindow(videoCanvas, (ushort)'A');
        Console.WriteLine("uia.video_tool=standard");
        await Task.Delay(100);

        var marginX = Math.Min(24, Math.Max(4, rect.Width / 10));
        var marginY = Math.Min(24, Math.Max(4, rect.Height / 10));
        var left = marginX;
        var right = Math.Max(left + 1, rect.Width - marginX);
        var top = marginY;
        var bottom = Math.Max(top + 1, rect.Height - marginY);
        var end = Stopwatch.GetTimestamp() + Stopwatch.Frequency * options.DurationSeconds;
        var forward = true;
        var moves = 0;

        while (Stopwatch.GetTimestamp() < end)
        {
            var startX = forward ? left : right;
            var endX = forward ? right : left;
            var startY = forward ? top : bottom;
            var endY = forward ? bottom : top;
            for (var step = 0; step <= 64 && Stopwatch.GetTimestamp() < end; ++step)
            {
                var x = startX + (endX - startX) * step / 64;
                var y = startY + (endY - startY) * step / 64;
                SendMouseToWindow(videoCanvas, WindowMessage.MouseMove, x, y, 0);
                ++moves;
                await Task.Delay(options.ScrollIntervalMilliseconds);
            }
            forward = !forward;
        }

        Console.WriteLine($"uia.video_crosshair.moves={moves} interval_ms={options.ScrollIntervalMilliseconds}");
    }

    private async Task RunVideoPlaybackAudioActionAsync(Func<Task> action)
    {
        var videoCanvas = WaitForVideoCanvas();
        var rect = GetWindowRect(videoCanvas);
        Console.WriteLine($"uia.video_canvas_hwnd=0x{videoCanvas:X}");
        Console.WriteLine($"uia.video_canvas_class={GetClassName(videoCanvas)}");
        Console.WriteLine($"uia.video_canvas_rect={rect.Left},{rect.Top},{rect.Width}x{rect.Height}");
        if (!TryInvokeVideoPlayback())
            throw new InvalidOperationException("Could not invoke the Video Play command through background UIA");
        Console.WriteLine("uia.video_playback=start");
        await Task.Delay(2000);
        try
        {
            await action();
        }
        finally
        {
            if (!TryInvokeVideoStop())
                Console.WriteLine("uia.video_playback=stop-unverified");
            else
                Console.WriteLine("uia.video_playback=stop");
        }
    }

    private nint WaitForVideoCanvas()
    {
        if (process is null)
            throw new InvalidOperationException("Aegisub process was not started");
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 45;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (process.HasExited)
                throw new InvalidOperationException($"Aegisub exited before a video canvas was ready (exit code {process.ExitCode})");
            ThrowIfFatalDialog();
            var candidate = EnumerateChildWindows(process.MainWindowHandle)
                .Where(hwnd => hwnd != audioCanvas && IsWindowVisible(hwnd))
                .Select(hwnd => (Hwnd: hwnd, Rect: GetWindowRect(hwnd), Class: GetClassName(hwnd)))
                .Where(item => item.Class.Equals("wxGLCanvas", StringComparison.OrdinalIgnoreCase)
                    && item.Rect.Width >= 160
                    && item.Rect.Height >= 90)
                .OrderByDescending(item => item.Rect.Area)
                .FirstOrDefault();
            mainWindow = RefreshMainWindow();
            var mainWindowHandle = mainWindow is null ? 0 : new nint(mainWindow.Current.NativeWindowHandle);
            if (candidate.Hwnd != 0 && mainWindowHandle != 0 && IsWindowEnabled(mainWindowHandle))
                return candidate.Hwnd;
            Thread.Sleep(250);
        }
        throw new TimeoutException(
            "Could not locate a ready Video Display wxGLCanvas; video loading may be blocked by a modal dialog");
    }

    private bool TryInvokeVideoPlayback()
    {
        mainWindow = RefreshMainWindow();
        if (mainWindow is null)
            return false;

        var buttons = mainWindow.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
        foreach (AutomationElement element in buttons)
        {
            var name = (element.Current.Name ?? string.Empty).Trim();
            if (!ContainsAny(name, "Play video starting", "播放视频", "播放视讯", "ビデオを再生"))
                continue;
            if (!element.Current.IsEnabled || !TryClickButtonInBackground(element))
                continue;
            Console.WriteLine($"uia.video_playback_command=background-button:{name}");
            return true;
        }
        return false;
    }

    private bool TryInvokeVideoStop()
    {
        mainWindow = RefreshMainWindow();
        if (mainWindow is null)
            return false;

        var buttons = mainWindow.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
        foreach (AutomationElement element in buttons)
        {
            var name = (element.Current.Name ?? string.Empty).Trim();
            if (!ContainsAny(name,
                "Stop video playback", "Stop video",
                "停止视频播放", "停止影片播放", "停止視訊播放", "ビデオ再生を停止"))
                continue;
            if (!element.Current.IsEnabled || !TryClickButtonInBackground(element))
                continue;
            Console.WriteLine($"uia.video_stop_command=background-button:{name}");
            return true;
        }
        return false;
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

        if (!options.AllowGlobalInput) {
            return false;
        }

        // Explicit foreground fallback for old wx UIA providers. This is never
        // used unless --allow-global-input was requested by the caller.
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

    private async Task EnableSpectrumAsync()
    {
        if (options.AudioSpectrum is true)
        {
            Console.WriteLine("uia.spectrum_toggle=profile:spectrum");
            await Task.Delay(250);
            return;
        }
        for (var attempt = 1; attempt <= 4; ++attempt)
        {
            mainWindow = RefreshMainWindow() ?? mainWindow;
            if (TryInvokeSpectrumToggle())
            {
                if (attempt > 1)
                    Console.WriteLine($"uia.spectrum_toggle.retry_count={attempt - 1}");
                await Task.Delay(1000);
                return;
            }
            await Task.Delay(250);
        }
        Console.WriteLine("uia.spectrum_toggle=background-unavailable");
        throw new InvalidOperationException("Could not locate the Audio > Spectrum Display command through UIA");
    }

    private void WaitForMainWindow()
    {
        if (process is null)
            throw new InvalidOperationException("Process was not started");
        mainWindow = UiaDriver.WaitForMainWindow(process, TimeSpan.FromSeconds(30));
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

    private void ThrowIfFatalDialog()
    {
        if (process is null)
            return;
        UiaDriver.ThrowIfFatalDialog(process);
    }

    private void SetWindowSize()
    {
        if (process is null)
            return;
		process.Refresh();
		if (process.MainWindowHandle == 0)
			throw new InvalidOperationException("Aegisub main window handle was not available");
        ShowWindow(process.MainWindowHandle, 4); // SW_SHOWNOACTIVATE
        SetWindowPos(process.MainWindowHandle, 0, 40, 40, options.Width, options.Height,
            SetWindowPosFlags.ShowWindow | SetWindowPosFlags.NoActivate);
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
            .Where(item => item.Class.Equals("ToolbarWindow32", StringComparison.OrdinalIgnoreCase)
                && item.Rect.Width > 300
                && item.Rect.Height is >= 20 and <= 70)
            .ToArray();
        var skiaCanvas = candidates
            .Where(item => item.Class.Equals("wxGLCanvas", StringComparison.OrdinalIgnoreCase)
                && item.Rect.Width > 300
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
        if (requireSkiaCanvas)
            return skiaCanvas.Hwnd;

        // AudioBox contains the canvas immediately above its narrow toolbar.
        // wx exposes both the AudioBox container and the actual wxGLCanvas as
        // generic wxWindow children, so the adjacent-toolbar relationship is
        // more stable than a class-name check across wx versions.
        var toolbars = toolbarCandidates;
        var adjacent = candidates
            .Where(item => item.Rect.Width > 300
                && item.Rect.Height is >= 70 and <= 900
                && !candidates.Any(child => child.Parent == item.Hwnd
                    && child.Rect.Width > 300
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

        mainWindow = RefreshMainWindow();
        var mainWindowHandle = mainWindow is null ? 0 : new nint(mainWindow.Current.NativeWindowHandle);
        if (mainWindowHandle == 0)
        {
            TerminateAutomationProcess("close-message-failed");
            throw new InvalidOperationException("Could not request a clean Aegisub shutdown");
        }
        if (!IsWindowEnabled(mainWindowHandle))
        {
            TerminateAutomationProcess("modal-window-active");
            throw new InvalidOperationException(
                "Aegisub remained blocked by a modal dialog; the automation scenario was not ready");
        }

        EnsureAutomationProjectSaved();
        if (!PostMessage(mainWindowHandle, (uint)WindowMessage.Close, 0, 0))
        {
            TerminateAutomationProcess("close-message-failed");
            throw new InvalidOperationException("Could not request a clean Aegisub shutdown");
        }

        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 15;
        while (!process.HasExited && Stopwatch.GetTimestamp() < deadline && !process.WaitForExit(250)) { }
        if (!process.HasExited)
        {
            TerminateAutomationProcess("timeout");
            throw new TimeoutException("Aegisub did not complete a clean shutdown within 15 seconds");
        }
        Console.WriteLine($"uia.exit_code={process.ExitCode}");
        if (process.ExitCode != 0)
            throw new InvalidOperationException($"Aegisub exited abnormally with code {process.ExitCode}");
        cleanShutdown = true;
        Console.WriteLine("uia.close=clean");
    }

    private string PrepareAutomationProject()
    {
        automationProjectDirectory = Path.Combine(
            Path.GetTempPath(),
            "aegisub-skia-audio-uia",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(automationProjectDirectory);

        if (!string.IsNullOrWhiteSpace(options.Project))
        {
            var source = Path.GetFullPath(options.Project);
            var extension = Path.GetExtension(source);
            var destination = Path.Combine(automationProjectDirectory,
                "scenario" + (string.IsNullOrWhiteSpace(extension) ? ".ass" : extension));
            var content = BuildAutomationProjectContent(File.ReadAllText(source));
            File.WriteAllText(destination, content, new UTF8Encoding(false));
            return destination;
        }

        var project = Path.Combine(automationProjectDirectory, "scenario.ass");
        File.WriteAllText(project, BuildAutomationProjectContent(BlankAutomationProject), new UTF8Encoding(false));
        return project;
    }

    private static void WriteAutomationConfig(string profileDirectory, DriverOptions options)
    {
        var userDirectory = Path.Combine(profileDirectory, "user");
        Directory.CreateDirectory(userDirectory);
        var configPath = Path.Combine(userDirectory, "config.json");
        var config = new Dictionary<string, object>
        {
            ["Subtitle"] = new Dictionary<string, object>
            {
                ["Provider"] = "libass"
            }
        };
        var configSummary = new List<string> { "subtitle-provider:libass" };
        if (options.IsFocusColourScenario)
        {
            config["Tool"] = new Dictionary<string, object>
            {
                ["Preferences"] = new Dictionary<string, object> { ["Page"] = 7 }
            };
            config["Colour"] = new Dictionary<string, object>
            {
                ["Audio Display"] = new Dictionary<string, object>
                {
                    ["Spectrum"] = "Icy Blue",
                    ["Waveform"] = "Green"
                }
            };
            configSummary.Add("preferences-page:colors");
            configSummary.Add("audio-colours:deterministic");
        }
        var audioConfig = new Dictionary<string, object>();
        if (!string.IsNullOrWhiteSpace(options.AudioProvider))
        {
            audioConfig["Provider"] = options.AudioProvider;
            configSummary.Add($"audio-provider:{options.AudioProvider}");
        }
        if (options.AudioCacheType is int audioCacheType)
        {
            audioConfig["Cache"] = new Dictionary<string, object> { ["Type"] = audioCacheType };
            configSummary.Add($"audio-cache-type:{audioCacheType}");
        }
        if (!string.IsNullOrWhiteSpace(options.AudioPlayer))
        {
            audioConfig["Player"] = options.AudioPlayer;
            configSummary.Add($"audio-player:{options.AudioPlayer}");
        }
        if (options.AudioSpectrum is bool audioSpectrum)
        {
            audioConfig["Spectrum"] = audioSpectrum;
            configSummary.Add($"audio-view:{(audioSpectrum ? "spectrum" : "waveform")}");
        }
        if (options.CursorTime is bool cursorTime)
        {
            audioConfig["Display"] = new Dictionary<string, object>
            {
                ["Draw"] = new Dictionary<string, object> { ["Cursor Time"] = cursorTime }
            };
            configSummary.Add($"cursor-time:{(cursorTime ? "on" : "off")}");
        }
        if (audioConfig.Count > 0)
            config["Audio"] = audioConfig;
        File.WriteAllText(
            configPath,
            JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true }),
            new UTF8Encoding(false));
        Console.WriteLine($"uia.config={string.Join(',', configSummary)}");
    }

    private string BuildAutomationProjectContent(string source)
    {
        var lines = source.Replace("\r\n", "\n", StringComparison.Ordinal)
            .Replace('\r', '\n')
            .Split('\n')
            .ToList();
        if (!string.IsNullOrWhiteSpace(options.Video))
        {
            RemoveSectionKeys(lines, "[Script Info]",
                "PlayResX:", "PlayResY:", "LayoutResX:", "LayoutResY:");
        }
        var sectionStart = lines.FindIndex(line =>
            line.Trim().Equals("[Aegisub Project Garbage]", StringComparison.OrdinalIgnoreCase));
        if (sectionStart < 0)
        {
            while (lines.Count > 0 && string.IsNullOrWhiteSpace(lines[^1]))
                lines.RemoveAt(lines.Count - 1);
            lines.Add(string.Empty);
            lines.Add("[Aegisub Project Garbage]");
            sectionStart = lines.Count - 1;
        }

        var sectionEnd = lines.FindIndex(sectionStart + 1, line =>
        {
            var value = line.Trim();
            return value.StartsWith("[", StringComparison.Ordinal)
                && value.EndsWith("]", StringComparison.Ordinal);
        });
        if (sectionEnd < 0)
            sectionEnd = lines.Count;

        var linkedFileKeys = new[]
        {
            "Audio URI:", "Audio File:", "Video File:", "Timecodes File:", "Keyframes File:"
        };
        for (var i = sectionEnd - 1; i > sectionStart; --i)
        {
            var value = lines[i].TrimStart();
            if (linkedFileKeys.Any(key => value.StartsWith(key, StringComparison.OrdinalIgnoreCase)))
                lines.RemoveAt(i);
        }

        var insertAt = sectionStart + 1;
        if (!string.IsNullOrWhiteSpace(options.Audio))
            lines.Insert(insertAt++, $"Audio File: {Path.GetFullPath(options.Audio)}");
        if (!string.IsNullOrWhiteSpace(options.Video))
            lines.Insert(insertAt, $"Video File: {Path.GetFullPath(options.Video)}");
        return string.Join(Environment.NewLine, lines);
    }

    private static void RemoveSectionKeys(List<string> lines, string section, params string[] keys)
    {
        var sectionStart = lines.FindIndex(line =>
            line.Trim().Equals(section, StringComparison.OrdinalIgnoreCase));
        if (sectionStart < 0)
            return;
        var sectionEnd = lines.FindIndex(sectionStart + 1, line =>
        {
            var value = line.Trim();
            return value.StartsWith("[", StringComparison.Ordinal)
                && value.EndsWith("]", StringComparison.Ordinal);
        });
        if (sectionEnd < 0)
            sectionEnd = lines.Count;
        for (var i = sectionEnd - 1; i > sectionStart; --i)
        {
            var value = lines[i].TrimStart();
            if (keys.Any(key => value.StartsWith(key, StringComparison.OrdinalIgnoreCase)))
                lines.RemoveAt(i);
        }
    }

    private void EnsureAutomationProjectSaved()
    {
        if (!IsAutomationProjectModified())
            return;

        mainWindow = RefreshMainWindow();
        if (mainWindow is null)
        {
            TerminateAutomationProcess("save-window-unavailable");
            throw new InvalidOperationException("Could not save the temporary automation project because the main window disappeared");
        }

        var buttons = mainWindow.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
            .Cast<AutomationElement>()
            .ToArray();
        var save = buttons.FirstOrDefault(button =>
        {
            var name = (button.Current.Name ?? string.Empty).Trim();
            return name.Equals("Save", StringComparison.OrdinalIgnoreCase)
                || ContainsAny(name,
                    "Save Subtitles", "Save the current subtitles",
                    "保存字幕", "保存当前字幕", "儲存字幕", "儲存目前字幕",
                    "字幕を保存", "現在の字幕を保存");
        });
        if (save is null || !save.Current.IsEnabled
            || !save.TryGetCurrentPattern(InvokePattern.Pattern, out var pattern))
        {
            var names = string.Join(", ", buttons
                .Select(button => (button.Current.Name ?? string.Empty).Trim())
                .Where(name => !string.IsNullOrEmpty(name)));
            TerminateAutomationProcess("save-command-unavailable");
            throw new InvalidOperationException(
                $"Could not invoke the background Save Subtitles command. Available buttons: {names}");
        }

        ((InvokePattern)pattern).Invoke();
        Console.WriteLine($"uia.save=invoke:{save.Current.Name}");

        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 10;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (process is null || process.HasExited)
                return;
            if (!IsAutomationProjectModified())
            {
                Console.WriteLine("uia.save=clean");
                return;
            }
            Thread.Sleep(100);
        }

        TerminateAutomationProcess("save-not-confirmed");
        throw new TimeoutException("The temporary automation project remained modified after Save Subtitles");
    }

    private bool IsAutomationProjectModified()
    {
        if (process is null || process.HasExited)
            return false;
        process.Refresh();
        return process.MainWindowTitle.TrimStart().StartsWith("* ", StringComparison.Ordinal);
    }

    private void TerminateAutomationProcess(string reason)
    {
        if (process is null || process.HasExited)
            return;
        Console.WriteLine($"uia.close=terminate:{reason}");
        process.Kill(entireProcessTree: true);
        process.WaitForExit(5000);
    }

    private void DeleteAutomationProject()
    {
        if (string.IsNullOrWhiteSpace(automationProjectDirectory)
            || options.KeepOpen
            || !scenarioSucceeded
            || !cleanShutdown
            || (process is not null && !process.HasExited))
            return;
        try
        {
            Directory.Delete(automationProjectDirectory, recursive: true);
        }
        catch (IOException error)
        {
            Console.WriteLine($"uia.project_cleanup=deferred:{error.Message}");
        }
        catch (UnauthorizedAccessException error)
        {
            Console.WriteLine($"uia.project_cleanup=deferred:{error.Message}");
        }
    }

    private void ValidateCursorStateMatrixTrace()
    {
        var traceSelection = Environment.GetEnvironmentVariable("AEGISUB_PERF_TRACE");
        if (string.IsNullOrWhiteSpace(traceSelection)
            || traceSelection.Equals("0", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("false", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("uia.cursor_matrix.trace_validation=skipped:not-enabled");
            return;
        }
        if (string.IsNullOrWhiteSpace(automationProfileDirectory))
            throw new InvalidOperationException("Cursor matrix profile directory was not initialized");

        var sessionRoot = Path.Combine(automationProfileDirectory, "user", "perf-sessions");
        var traces = Directory.Exists(sessionRoot)
            ? Directory.EnumerateFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories).ToArray()
            : Array.Empty<string>();
        if (traces.Length != 1)
            throw new InvalidDataException($"Cursor matrix expected exactly one perf trace, found {traces.Length}");

        var snapshots = new List<CursorTraceSnapshot>();
        foreach (var line in File.ReadLines(traces[0]))
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("name", out var name)
                || !name.ValueEquals("audio_display_snapshot")
                || !root.TryGetProperty("payload", out var payload)
                || !payload.TryGetProperty("renderer", out var renderer)
                || !renderer.ValueEquals("skia"))
            {
                continue;
            }

            snapshots.Add(new CursorTraceSnapshot(
                payload.GetProperty("cursor_source").GetString() ?? "none",
                payload.GetProperty("cursor_position_ms").GetInt32(),
                payload.GetProperty("cursor_device_x").GetDouble(),
                payload.GetProperty("cursor_label_visible").GetBoolean(),
                payload.GetProperty("cursor_only").GetBoolean(),
                payload.GetProperty("target_width").GetInt32(),
                payload.GetProperty("visible_content_request_called").GetBoolean(),
                payload.GetProperty("content_lookup_performed").GetBoolean(),
                payload.GetProperty("content_tiles_drawn_this_frame").GetInt64(),
                payload.GetProperty("gpu_tile_uploads_this_frame").GetInt64()));
        }
        if (snapshots.Count == 0)
            throw new InvalidDataException("Cursor matrix trace did not contain Skia Audio Display snapshots");

        var mouseLabelVisible = options.CursorTime ?? true;
        var initialMouse = FindCursorSnapshot(snapshots, 0,
            snapshot => snapshot.Source == "mouse" && snapshot.LabelVisible == mouseLabelVisible);
        var playback = FindCursorSnapshot(snapshots, initialMouse + 1,
            snapshot => snapshot.Source == "playback" && !snapshot.LabelVisible);
        var restoredMouse = FindCursorSnapshot(snapshots, playback + 1,
            snapshot => snapshot.Source == "mouse"
                && snapshot.LabelVisible == mouseLabelVisible
                && snapshot.PositionMs > snapshots[initialMouse].PositionMs);
        var mouseLeft = FindCursorSnapshot(snapshots, restoredMouse + 1,
            snapshot => snapshot.Source == "none" && snapshot.PositionMs == -1);
        var reenteredMouse = FindCursorSnapshot(snapshots, mouseLeft + 1,
            snapshot => snapshot.Source == "mouse" && snapshot.LabelVisible == mouseLabelVisible);
        var resizedMouse = FindCursorSnapshot(snapshots, reenteredMouse + 1,
            snapshot => snapshot.Source == "mouse"
                && snapshot.LabelVisible == mouseLabelVisible
                && snapshot.TargetWidth != snapshots[initialMouse].TargetWidth);

        var invalidCursorOnly = snapshots.Where(snapshot => snapshot.CursorOnly
            && (snapshot.VisibleRequest
                || snapshot.ContentLookup
                || snapshot.TilesDrawn != 0
                || snapshot.GpuUploads != 0)).ToArray();
        if (invalidCursorOnly.Length != 0)
            throw new InvalidDataException("Cursor matrix found content work on a cursor-only frame");

        Console.WriteLine($"uia.cursor_matrix.sequence={initialMouse},{playback},{restoredMouse},{mouseLeft},{reenteredMouse},{resizedMouse}");
        Console.WriteLine($"uia.cursor_matrix.restored_mouse_ms={snapshots[restoredMouse].PositionMs}");
        Console.WriteLine($"uia.cursor_matrix.resize_width={snapshots[initialMouse].TargetWidth}->{snapshots[resizedMouse].TargetWidth}");
        Console.WriteLine("uia.cursor_matrix.trace_validation=ok");
    }

    private static int FindCursorSnapshot(
        IReadOnlyList<CursorTraceSnapshot> snapshots,
        int start,
        Func<CursorTraceSnapshot, bool> predicate)
    {
        for (var index = Math.Max(0, start); index < snapshots.Count; ++index)
        {
            if (predicate(snapshots[index]))
                return index;
        }
        throw new InvalidDataException($"Cursor matrix trace sequence was incomplete after snapshot {start}");
    }

    private void ValidatePlaybackMarkerDragTrace()
    {
        var traceSelection = Environment.GetEnvironmentVariable("AEGISUB_PERF_TRACE");
        if (string.IsNullOrWhiteSpace(traceSelection)
            || traceSelection.Equals("0", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("false", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("uia.playback_marker_drag.trace_validation=skipped:not-enabled");
            return;
        }
        if (string.IsNullOrWhiteSpace(automationProfileDirectory))
            throw new InvalidOperationException("Playback marker drag profile directory was not initialized");

        var sessionRoot = Path.Combine(automationProfileDirectory, "user", "perf-sessions");
        var traces = Directory.Exists(sessionRoot)
            ? Directory.EnumerateFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories).ToArray()
            : Array.Empty<string>();
        if (traces.Length != 1)
            throw new InvalidDataException($"Playback marker drag expected exactly one perf trace, found {traces.Length}");

        var snapshots = new List<PlaybackMarkerTraceSnapshot>();
        foreach (var line in File.ReadLines(traces[0]))
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("name", out var name)
                || !name.ValueEquals("audio_display_snapshot")
                || !root.TryGetProperty("payload", out var payload)
                || !payload.TryGetProperty("renderer", out var renderer)
                || !renderer.ValueEquals("skia"))
            {
                continue;
            }

            snapshots.Add(new PlaybackMarkerTraceSnapshot(
                payload.GetProperty("marker_revision").GetInt64(),
                payload.GetProperty("cursor_source").GetString() ?? "none",
                payload.GetProperty("cursor_position_ms").GetInt32(),
                payload.GetProperty("cursor_label_visible").GetBoolean(),
                payload.GetProperty("cursor_only").GetBoolean(),
                payload.GetProperty("visible_content_request_called").GetBoolean(),
                payload.GetProperty("content_lookup_performed").GetBoolean(),
                payload.GetProperty("content_tiles_drawn_this_frame").GetInt64(),
                payload.GetProperty("gpu_tile_uploads_this_frame").GetInt64(),
                payload.GetProperty("worker_builds_started").GetInt64(),
                payload.GetProperty("worker_payload_builds_started").GetInt64()));
        }

        var playback = snapshots.Where(snapshot => snapshot.Source == "playback").ToArray();
        if (playback.Length < 3)
            throw new InvalidDataException("Playback marker drag did not retain enough playback cursor snapshots");
        if (playback.Any(snapshot => snapshot.LabelVisible))
            throw new InvalidDataException("Playback marker drag displayed a mouse cursor label during playback");
        if (playback.Max(snapshot => snapshot.PositionMs) <= playback.Min(snapshot => snapshot.PositionMs))
            throw new InvalidDataException("Playback cursor did not advance while markers were dragged");

        var initialMarkerRevision = playback[0].MarkerRevision;
        var markerFrames = playback
            .Where(snapshot => snapshot.MarkerRevision > initialMarkerRevision && !snapshot.CursorOnly)
            .ToArray();
        var markerRevisions = playback.Select(snapshot => snapshot.MarkerRevision).Distinct().ToArray();
        if (markerFrames.Length < 2 || markerRevisions.Length < 3)
            throw new InvalidDataException("Marker revision did not advance repeatedly while playback remained active");
        if (!playback.Any(snapshot => snapshot.CursorOnly))
            throw new InvalidDataException("Playback marker drag did not resume the cursor-only path between marker updates");

        var invalidCursorOnly = playback.Where(snapshot => snapshot.CursorOnly
            && (snapshot.VisibleRequest
                || snapshot.ContentLookup
                || snapshot.TilesDrawn != 0
                || snapshot.GpuUploads != 0)).ToArray();
        if (invalidCursorOnly.Length != 0)
            throw new InvalidDataException("Playback marker drag found content work on a cursor-only frame");

        var firstMarkerFrame = markerFrames[0];
        var lastMarkerFrame = markerFrames[^1];
        if (lastMarkerFrame.WorkerBuildsStarted != firstMarkerFrame.WorkerBuildsStarted
            || lastMarkerFrame.WorkerPayloadBuildsStarted != firstMarkerFrame.WorkerPayloadBuildsStarted)
        {
            throw new InvalidDataException("Marker drag started new audio content or upload-payload worker builds");
        }
        if (markerFrames.Any(snapshot => snapshot.GpuUploads != 0))
            throw new InvalidDataException("Marker drag uploaded new GPU content tiles after warmup");

        Console.WriteLine($"uia.playback_marker_drag.playback_snapshots={playback.Length}");
        Console.WriteLine($"uia.playback_marker_drag.marker_frames={markerFrames.Length}");
        Console.WriteLine($"uia.playback_marker_drag.marker_revisions={markerRevisions.Length}");
        Console.WriteLine($"uia.playback_marker_drag.full_frame_content_requests={markerFrames.Count(snapshot => snapshot.VisibleRequest)}");
        Console.WriteLine($"uia.playback_marker_drag.full_frame_content_lookups={markerFrames.Count(snapshot => snapshot.ContentLookup)}");
        Console.WriteLine("uia.playback_marker_drag.trace_validation=ok");
    }

    private void ValidateMiddleSeekCursorTrace()
    {
        var traceSelection = Environment.GetEnvironmentVariable("AEGISUB_PERF_TRACE");
        if (string.IsNullOrWhiteSpace(traceSelection)
            || traceSelection.Equals("0", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("false", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("uia.middle_seek.trace_validation=skipped:not-enabled");
            return;
        }
        if (string.IsNullOrWhiteSpace(automationProfileDirectory))
            throw new InvalidOperationException("Middle seek profile directory was not initialized");

        var sessionRoot = Path.Combine(automationProfileDirectory, "user", "perf-sessions");
        var traces = Directory.Exists(sessionRoot)
            ? Directory.EnumerateFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories).ToArray()
            : Array.Empty<string>();
        if (traces.Length != 1)
            throw new InvalidDataException($"Middle seek expected exactly one perf trace, found {traces.Length}");

        var snapshots = new List<MiddleSeekTraceSnapshot>();
        var seekEvents = new List<MiddleSeekTraceEvent>();
        foreach (var line in File.ReadLines(traces[0]))
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("name", out var name)
                || !root.TryGetProperty("payload", out var payload))
            {
                continue;
            }

            if (name.ValueEquals("audio_display_snapshot")
                && payload.TryGetProperty("renderer", out var renderer)
                && renderer.ValueEquals("skia"))
            {
                snapshots.Add(new MiddleSeekTraceSnapshot(
                    payload.GetProperty("middle_seek_active").GetBoolean(),
                    payload.GetProperty("cursor_source").GetString() ?? "none",
                    payload.GetProperty("cursor_position_ms").GetInt32(),
                    payload.GetProperty("cursor_label_visible").GetBoolean(),
                    payload.GetProperty("cursor_only").GetBoolean(),
                    payload.GetProperty("retained_layers_reused").GetBoolean(),
                    payload.GetProperty("visible_content_request_called").GetBoolean(),
                    payload.GetProperty("content_lookup_performed").GetBoolean(),
                    payload.GetProperty("content_tiles_drawn_this_frame").GetInt64(),
                    payload.GetProperty("gpu_tile_uploads_this_frame").GetInt64(),
                    payload.GetProperty("gpu_tile_uploads").GetInt64(),
                    payload.GetProperty("worker_builds_started").GetInt64(),
                    payload.GetProperty("worker_payload_builds_started").GetInt64()));
            }
            else if (name.ValueEquals("audio_middle_seek"))
            {
                seekEvents.Add(new MiddleSeekTraceEvent(
                    payload.GetProperty("phase").GetString() ?? string.Empty,
                    payload.GetProperty("time_ms").GetInt32(),
                    payload.GetProperty("frame").GetInt32()));
            }
        }
        if (snapshots.Count == 0)
            throw new InvalidDataException("Middle seek trace did not contain Skia Audio Display snapshots");

        var activeGroups = new List<List<int>>();
        List<int>? activeGroup = null;
        for (var index = 0; index < snapshots.Count; ++index)
        {
            if (snapshots[index].Active)
            {
                if (activeGroup is null)
                {
                    activeGroup = new List<int>();
                    activeGroups.Add(activeGroup);
                }
                activeGroup.Add(index);
            }
            else
            {
                activeGroup = null;
            }
        }
        if (activeGroups.Count < 2)
            throw new InvalidDataException("Middle seek did not produce both inside-release and outside-release active intervals");

        var expectedLabelVisible = options.CursorTime ?? true;
        var activeIndices = activeGroups.Take(2).SelectMany(group => group).ToArray();
        var activeSnapshots = activeIndices.Select(index => snapshots[index]).ToArray();
        if (activeSnapshots.Any(snapshot => snapshot.Source != "mouse"
            || snapshot.LabelVisible != expectedLabelVisible))
        {
            throw new InvalidDataException("Middle seek cursor source or time-label state was incorrect while dragging");
        }
        foreach (var group in activeGroups.Take(2))
        {
            var positions = group.Select(index => snapshots[index].PositionMs).ToArray();
            if (positions.Length < 3 || positions.Max() <= positions.Min())
                throw new InvalidDataException("Middle seek cursor did not follow the dragged pointer");
        }

        var activeRetained = activeSnapshots.Where(snapshot => snapshot.RetainedLayersReused).ToArray();
        var activeCursorOnly = activeSnapshots.Where(snapshot => snapshot.CursorOnly).ToArray();
        if (activeRetained.Length * 5 < activeSnapshots.Length * 4)
            throw new InvalidDataException("Middle seek did not predominantly use retained cursor/marker overlays");
        if (activeCursorOnly.Length < 2)
            throw new InvalidDataException("Middle seek did not produce pure cursor-only frames between marker previews");
        if (activeRetained.Any(snapshot => snapshot.VisibleRequest
            || snapshot.ContentLookup
            || snapshot.TilesDrawn != 0
            || snapshot.GpuUploadsThisFrame != 0))
        {
            throw new InvalidDataException("Middle seek found content work on a retained overlay frame");
        }

        var firstGroup = activeGroups[0];
        var secondGroup = activeGroups[1];
        var restoredMouse = FindMiddleSeekSnapshot(snapshots, firstGroup[^1] + 1,
            snapshot => !snapshot.Active
                && snapshot.Source == "mouse"
                && snapshot.LabelVisible == expectedLabelVisible);
        var outsideRelease = FindMiddleSeekSnapshot(snapshots, secondGroup[^1] + 1,
            snapshot => !snapshot.Active && snapshot.Source == "none" && snapshot.PositionMs == -1);
        var reenteredMouse = FindMiddleSeekSnapshot(snapshots, outsideRelease + 1,
            snapshot => !snapshot.Active
                && snapshot.Source == "mouse"
                && snapshot.LabelVisible == expectedLabelVisible);

        var firstActive = snapshots[firstGroup[0]];
        var finalMouse = snapshots[reenteredMouse];
        if (finalMouse.GpuTileUploads != firstActive.GpuTileUploads
            || finalMouse.WorkerBuildsStarted != firstActive.WorkerBuildsStarted
            || finalMouse.WorkerPayloadBuildsStarted != firstActive.WorkerPayloadBuildsStarted)
        {
            throw new InvalidDataException("Middle seek started audio worker work or uploaded content tiles");
        }

        var commits = seekEvents.Where(entry => entry.Phase == "commit").ToArray();
        var previews = seekEvents.Where(entry => entry.Phase == "preview").ToArray();
        if (commits.Length != 2 || previews.Length < 2)
            throw new InvalidDataException("Middle seek did not emit the expected preview/commit trace sequence");
        if (commits.Any(entry => entry.TimeMs < 0 || entry.Frame < 0))
            throw new InvalidDataException("Middle seek committed an invalid time or video frame");

        Console.WriteLine($"uia.middle_seek.active_groups={activeGroups.Count}");
        Console.WriteLine($"uia.middle_seek.active_snapshots={activeSnapshots.Length}");
        Console.WriteLine($"uia.middle_seek.retained_overlay_snapshots={activeRetained.Length}");
        Console.WriteLine($"uia.middle_seek.cursor_only_snapshots={activeCursorOnly.Length}");
        Console.WriteLine($"uia.middle_seek.preview_events={previews.Length}");
        Console.WriteLine($"uia.middle_seek.commit_times_ms={string.Join(",", commits.Select(entry => entry.TimeMs))}");
        Console.WriteLine($"uia.middle_seek.sequence={firstGroup[0]},{restoredMouse},{secondGroup[0]},{outsideRelease},{reenteredMouse}");
        Console.WriteLine("uia.middle_seek.trace_validation=ok");
    }

    private static int FindMiddleSeekSnapshot(
        IReadOnlyList<MiddleSeekTraceSnapshot> snapshots,
        int start,
        Func<MiddleSeekTraceSnapshot, bool> predicate)
    {
        for (var index = Math.Max(0, start); index < snapshots.Count; ++index)
        {
            if (predicate(snapshots[index]))
                return index;
        }
        throw new InvalidDataException($"Middle seek trace sequence was incomplete after snapshot {start}");
    }

    private void ValidateFocusColourTrace(bool spectrum)
    {
        var traceSelection = Environment.GetEnvironmentVariable("AEGISUB_PERF_TRACE");
        if (string.IsNullOrWhiteSpace(traceSelection)
            || traceSelection.Equals("0", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("false", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("uia.focus_colour.trace_validation=skipped:not-enabled");
            return;
        }
        if (string.IsNullOrWhiteSpace(automationProfileDirectory))
            throw new InvalidOperationException("Focus/colour profile directory was not initialized");

        var sessionRoot = Path.Combine(automationProfileDirectory, "user", "perf-sessions");
        var traces = Directory.Exists(sessionRoot)
            ? Directory.EnumerateFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories).ToArray()
            : Array.Empty<string>();
        if (traces.Length != 1)
            throw new InvalidDataException($"Focus/colour expected exactly one perf trace, found {traces.Length}");

        var snapshots = new List<FocusColourTraceSnapshot>();
        foreach (var line in File.ReadLines(traces[0]))
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("name", out var name)
                || !name.ValueEquals("audio_display_snapshot")
                || !root.TryGetProperty("payload", out var payload)
                || !payload.TryGetProperty("renderer", out var renderer)
                || !renderer.ValueEquals("skia"))
            {
                continue;
            }

            snapshots.Add(new FocusColourTraceSnapshot(
                payload.GetProperty("frame_id").GetInt64(),
                payload.GetProperty("content_kind").GetString() ?? string.Empty,
                payload.GetProperty("focused").GetBoolean(),
                payload.GetProperty("chrome_revision").GetInt64(),
                payload.GetProperty("presentation_revision").GetInt64(),
                payload.GetProperty("analysis_generation").GetInt64(),
                payload.GetProperty("complete_content_viewport").GetBoolean(),
                payload.GetProperty("gpu_tile_uploads_this_frame").GetInt64(),
                payload.GetProperty("gpu_tile_uploads").GetInt64(),
                payload.GetProperty("gpu_palette_uploads").GetInt64(),
                payload.GetProperty("cpu_tile_misses").GetInt64(),
                payload.GetProperty("fft_misses").GetInt64(),
                payload.GetProperty("fft_visible_builds").GetInt64(),
                payload.GetProperty("worker_builds_started").GetInt64(),
                payload.GetProperty("worker_payload_builds_started").GetInt64()));
        }

        var expectedKind = spectrum ? "spectrum" : "waveform";
        var target = snapshots.Where(snapshot => snapshot.Kind == expectedKind).ToArray();
        if (target.Length < 6)
            throw new InvalidDataException($"Focus/colour trace did not contain enough {expectedKind} snapshots");

        var firstFocused = FindFocusColourSnapshot(target, 0, snapshot => snapshot.Focused);
        if (firstFocused == 0)
            throw new InvalidDataException("Focus/colour trace did not contain a settled unfocused baseline");
        var firstUnfocused = FindFocusColourSnapshot(target, firstFocused + 1, snapshot => !snapshot.Focused);
        var secondFocused = FindFocusColourSnapshot(target, firstUnfocused + 1, snapshot => snapshot.Focused);
        var secondUnfocused = FindFocusColourSnapshot(target, secondFocused + 1, snapshot => !snapshot.Focused);

        var baseline = target[firstFocused - 1];
        var focusSequence = new[]
        {
            target[firstFocused],
            target[firstUnfocused],
            target[secondFocused],
            target[secondUnfocused]
        };
        if (!baseline.CompleteContentViewport)
            throw new InvalidDataException("Focus/colour baseline did not have a complete audio viewport");
        if (focusSequence.Any(snapshot => snapshot.PresentationRevision != baseline.PresentationRevision
            || snapshot.AnalysisGeneration != baseline.AnalysisGeneration
            || snapshot.GpuUploadsThisFrame != 0))
        {
            throw new InvalidDataException("Focus switching invalidated presentation/analysis or uploaded a GPU content tile");
        }
        if (!(focusSequence[0].ChromeRevision > baseline.ChromeRevision
            && focusSequence[1].ChromeRevision > focusSequence[0].ChromeRevision
            && focusSequence[2].ChromeRevision > focusSequence[1].ChromeRevision
            && focusSequence[3].ChromeRevision > focusSequence[2].ChromeRevision))
        {
            throw new InvalidDataException("Focus switching did not advance chrome revisions for every state transition");
        }
        if (!AudioContentCountersEqual(baseline, focusSequence[^1]))
            throw new InvalidDataException("Focus switching started audio analysis/worker work or uploaded content tiles");

        var colourIndex = FindFocusColourSnapshot(
            target,
            secondUnfocused + 1,
            snapshot => snapshot.PresentationRevision > baseline.PresentationRevision);
        var colour = target[colourIndex];
        if (colour.AnalysisGeneration != baseline.AnalysisGeneration
            || colour.ChromeRevision != focusSequence[^1].ChromeRevision
            || colour.GpuUploadsThisFrame != 0
            || !AudioContentCountersEqual(focusSequence[^1], colour))
        {
            throw new InvalidDataException("Colour scheme apply rebuilt audio analysis/content or uploaded a GPU content tile");
        }

        var paletteDelta = colour.GpuPaletteUploads - focusSequence[^1].GpuPaletteUploads;
        if (!spectrum && paletteDelta != 0)
            throw new InvalidDataException("Waveform colour apply unexpectedly uploaded a spectrum palette texture");
        if (spectrum && paletteDelta is < 1 or > 4)
            throw new InvalidDataException($"Spectrum colour apply uploaded an unexpected number of palettes: {paletteDelta}");

        Console.WriteLine($"uia.focus_colour.kind={expectedKind}");
        Console.WriteLine($"uia.focus_colour.focus_frames={string.Join(',', focusSequence.Select(snapshot => snapshot.FrameId))}");
        Console.WriteLine($"uia.focus_colour.chrome_revisions={baseline.ChromeRevision}->{string.Join(',', focusSequence.Select(snapshot => snapshot.ChromeRevision))}");
        Console.WriteLine($"uia.focus_colour.presentation_revision={baseline.PresentationRevision}->{colour.PresentationRevision}");
        Console.WriteLine($"uia.focus_colour.palette_upload_delta={paletteDelta}");
        Console.WriteLine("uia.focus_colour.content_rebuild_delta=0");
        Console.WriteLine("uia.focus_colour.trace_validation=ok");
    }

    private static bool AudioContentCountersEqual(
        FocusColourTraceSnapshot left,
        FocusColourTraceSnapshot right) =>
        left.GpuTileUploads == right.GpuTileUploads
        && left.CpuTileMisses == right.CpuTileMisses
        && left.FftMisses == right.FftMisses
        && left.FftVisibleBuilds == right.FftVisibleBuilds
        && left.WorkerBuildsStarted == right.WorkerBuildsStarted
        && left.WorkerPayloadBuildsStarted == right.WorkerPayloadBuildsStarted;

    private static int FindFocusColourSnapshot(
        IReadOnlyList<FocusColourTraceSnapshot> snapshots,
        int start,
        Func<FocusColourTraceSnapshot, bool> predicate)
    {
        for (var index = Math.Max(0, start); index < snapshots.Count; ++index)
        {
            if (predicate(snapshots[index]))
                return index;
        }
        throw new InvalidDataException($"Focus/colour trace sequence was incomplete after snapshot {start}");
    }

    private void ValidateDpiTransitionTrace()
    {
        var traceSelection = Environment.GetEnvironmentVariable("AEGISUB_PERF_TRACE");
        if (string.IsNullOrWhiteSpace(traceSelection)
            || traceSelection.Equals("0", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("false", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("uia.dpi_transition.trace_validation=skipped:not-enabled");
            return;
        }
        if (string.IsNullOrWhiteSpace(automationProfileDirectory)
            || dpiTransitionInitialScale is not double initialScale
            || dpiTransitionTargetScale is not double targetScale)
        {
            throw new InvalidOperationException("DPI transition trace state was not initialized");
        }

        var sessionRoot = Path.Combine(automationProfileDirectory, "user", "perf-sessions");
        var traces = Directory.Exists(sessionRoot)
            ? Directory.EnumerateFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories).ToArray()
            : Array.Empty<string>();
        if (traces.Length != 1)
            throw new InvalidDataException($"DPI transition expected exactly one perf trace, found {traces.Length}");

        var snapshots = new List<DpiTransitionTraceSnapshot>();
        foreach (var line in File.ReadLines(traces[0]))
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("name", out var name)
                || !name.ValueEquals("audio_display_snapshot")
                || !root.TryGetProperty("payload", out var payload)
                || !payload.TryGetProperty("renderer", out var renderer)
                || !renderer.ValueEquals("skia"))
            {
                continue;
            }

            snapshots.Add(new DpiTransitionTraceSnapshot(
                payload.GetProperty("frame_id").GetInt64(),
                payload.GetProperty("content_scale").GetDouble(),
                payload.GetProperty("analysis_generation").GetInt64(),
                payload.GetProperty("chrome_revision").GetInt64(),
                payload.GetProperty("presentation_revision").GetInt64(),
                payload.GetProperty("complete_content_viewport").GetBoolean(),
                payload.GetProperty("target_width").GetInt32(),
                payload.GetProperty("target_height").GetInt32(),
                payload.GetProperty("gpu_tile_uploads").GetInt64(),
                payload.GetProperty("worker_builds_started").GetInt64(),
                payload.GetProperty("worker_payload_builds_started").GetInt64()));
        }
        if (snapshots.Count < 3)
            throw new InvalidDataException("DPI transition trace did not contain enough Skia Audio Display snapshots");

        var targetIndex = FindDpiTransitionSnapshot(
            snapshots,
            0,
            snapshot => snapshot.CompleteContentViewport
                && SameContentScale(snapshot.ContentScale, targetScale));
        var baselineIndex = -1;
        for (var index = targetIndex - 1; index >= 0; --index)
        {
            if (snapshots[index].CompleteContentViewport
                && SameContentScale(snapshots[index].ContentScale, initialScale))
            {
                baselineIndex = index;
                break;
            }
        }
        if (baselineIndex < 0)
            throw new InvalidDataException("DPI transition trace did not contain a complete initial-scale baseline");
        var restoredIndex = FindDpiTransitionSnapshot(
            snapshots,
            targetIndex + 1,
            snapshot => snapshot.CompleteContentViewport
                && SameContentScale(snapshot.ContentScale, initialScale));

        var baseline = snapshots[baselineIndex];
        var target = snapshots[targetIndex];
        var restored = snapshots[restoredIndex];
        if (!(target.AnalysisGeneration > baseline.AnalysisGeneration
            && restored.AnalysisGeneration > target.AnalysisGeneration))
        {
            throw new InvalidDataException("DPI transitions did not rebuild scale-dependent audio analysis generations");
        }
        if (!(target.PresentationRevision > baseline.PresentationRevision
            && restored.PresentationRevision > target.PresentationRevision
            && target.ChromeRevision > baseline.ChromeRevision
            && restored.ChromeRevision > target.ChromeRevision))
        {
            throw new InvalidDataException("DPI transitions did not invalidate presentation and device-space Chrome layers");
        }
        if (!(target.WorkerBuildsStarted > baseline.WorkerBuildsStarted
            && restored.WorkerBuildsStarted > target.WorkerBuildsStarted
            && target.WorkerPayloadBuildsStarted > baseline.WorkerPayloadBuildsStarted
            && restored.WorkerPayloadBuildsStarted > target.WorkerPayloadBuildsStarted
            && target.GpuTileUploads > baseline.GpuTileUploads
            && restored.GpuTileUploads > target.GpuTileUploads))
        {
            throw new InvalidDataException("DPI transitions reused stale scale-dependent content or did not publish replacement tiles");
        }
        if (target.TargetWidth <= 0 || target.TargetHeight <= 0
            || restored.TargetWidth <= 0 || restored.TargetHeight <= 0)
        {
            throw new InvalidDataException("DPI transition produced an invalid Skia render target");
        }

        Console.WriteLine($"uia.dpi_transition.frames={baseline.FrameId},{target.FrameId},{restored.FrameId}");
        Console.WriteLine($"uia.dpi_transition.content_scale={baseline.ContentScale}->{target.ContentScale}->{restored.ContentScale}");
        Console.WriteLine($"uia.dpi_transition.analysis_generation={baseline.AnalysisGeneration}->{target.AnalysisGeneration}->{restored.AnalysisGeneration}");
        Console.WriteLine($"uia.dpi_transition.presentation_revision={baseline.PresentationRevision}->{target.PresentationRevision}->{restored.PresentationRevision}");
        Console.WriteLine($"uia.dpi_transition.chrome_revision={baseline.ChromeRevision}->{target.ChromeRevision}->{restored.ChromeRevision}");
        Console.WriteLine($"uia.dpi_transition.gpu_tile_uploads={baseline.GpuTileUploads}->{target.GpuTileUploads}->{restored.GpuTileUploads}");
        Console.WriteLine("uia.dpi_transition.trace_validation=ok");
    }

    private static int FindDpiTransitionSnapshot(
        IReadOnlyList<DpiTransitionTraceSnapshot> snapshots,
        int start,
        Func<DpiTransitionTraceSnapshot, bool> predicate)
    {
        for (var index = Math.Max(0, start); index < snapshots.Count; ++index)
        {
            if (predicate(snapshots[index]))
                return index;
        }
        throw new InvalidDataException($"DPI transition trace sequence was incomplete after snapshot {start}");
    }

    private static bool SameContentScale(double left, double right) =>
        Math.Abs(left - right) <= 0.01;

    private void ValidateRuntimeFallbackTrace(bool spectrum)
    {
        var traceSelection = Environment.GetEnvironmentVariable("AEGISUB_PERF_TRACE");
        if (string.IsNullOrWhiteSpace(traceSelection)
            || traceSelection.Equals("0", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("false", StringComparison.OrdinalIgnoreCase)
            || traceSelection.Equals("off", StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("uia.runtime_fallback.trace_validation=skipped:not-enabled");
            return;
        }
        if (string.IsNullOrWhiteSpace(automationProfileDirectory)
            || runtimeFallbackRequiredSkiaFrames <= 0)
        {
            throw new InvalidOperationException("Runtime fallback trace state was not initialized");
        }

        var sessionRoot = Path.Combine(automationProfileDirectory, "user", "perf-sessions");
        var traces = Directory.Exists(sessionRoot)
            ? Directory.EnumerateFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories).ToArray()
            : Array.Empty<string>();
        if (traces.Length != 1)
            throw new InvalidDataException($"Runtime fallback expected exactly one perf trace, found {traces.Length}");

        var snapshots = new List<RuntimeFallbackTraceSnapshot>();
        foreach (var line in File.ReadLines(traces[0]))
        {
            if (string.IsNullOrWhiteSpace(line))
                continue;
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("name", out var name)
                || !name.ValueEquals("audio_display_snapshot")
                || !root.TryGetProperty("payload", out var payload))
            {
                continue;
            }
            var renderer = payload.GetProperty("renderer").GetString() ?? string.Empty;
            if (renderer is not ("skia" or "wx"))
                continue;
            snapshots.Add(new RuntimeFallbackTraceSnapshot(
                renderer,
                payload.GetProperty("content_kind").GetString() ?? string.Empty,
                payload.GetProperty("frame_id").GetInt64(),
                payload.GetProperty("content_scale").GetDouble(),
                payload.GetProperty("viewport_first_column").GetInt64(),
                payload.GetProperty("complete_content_viewport").GetBoolean(),
                payload.GetProperty("target_width").GetInt32(),
                payload.GetProperty("target_height").GetInt32(),
                payload.GetProperty("swap_attempted").GetBoolean(),
                payload.GetProperty("bitmap_cache_hits").GetInt64(),
                payload.GetProperty("bitmap_cache_misses").GetInt64()));
        }

        var firstWxIndex = snapshots.FindIndex(snapshot => snapshot.Renderer == "wx");
        if (firstWxIndex <= 0)
            throw new InvalidDataException("Runtime fallback trace did not contain a Skia -> wx renderer transition");
        var skiaSnapshots = snapshots.Take(firstWxIndex)
            .Where(snapshot => snapshot.Renderer == "skia")
            .ToArray();
        if (skiaSnapshots.Length < runtimeFallbackRequiredSkiaFrames)
        {
            throw new InvalidDataException(
                $"Runtime fallback occurred before the configured successful-frame threshold: {skiaSnapshots.Length}/{runtimeFallbackRequiredSkiaFrames}");
        }
        if (snapshots.Skip(firstWxIndex + 1).Any(snapshot => snapshot.Renderer == "skia"))
            throw new InvalidDataException("A failed Skia canvas rendered again after the wx fallback became active");

        var expectedKind = spectrum ? "spectrum" : "waveform";
        var lastSkia = skiaSnapshots.LastOrDefault(snapshot =>
            snapshot.Kind == expectedKind && snapshot.CompleteContentViewport);
        if (lastSkia.Renderer != "skia")
            throw new InvalidDataException($"Runtime fallback lacked a complete {expectedKind} Skia baseline");
        var firstWx = snapshots[firstWxIndex];
        if (firstWx.Kind != expectedKind
            || firstWx.FrameId != 1
            || firstWx.SwapAttempted
            || firstWx.TargetWidth <= 0
            || firstWx.TargetHeight <= 0
            || firstWx.BitmapCacheHits + firstWx.BitmapCacheMisses <= 0)
        {
            throw new InvalidDataException("The first wx fallback frame did not render the expected audio content");
        }
        if (!SameContentScale(lastSkia.ContentScale, firstWx.ContentScale))
            throw new InvalidDataException("The wx fallback did not preserve the active display scale");
        if (lastSkia.ViewportFirstColumn <= 0)
            throw new InvalidDataException("Runtime fallback did not establish a non-zero direct Skia viewport before failure");
        if (firstWx.ViewportFirstColumn <= 0)
            throw new InvalidDataException("The wx fallback lost the non-zero Skia viewport");
        var lastSkiaLogicalScroll = lastSkia.ViewportFirstColumn / lastSkia.ContentScale;
        if (Math.Abs(lastSkiaLogicalScroll - firstWx.ViewportFirstColumn) > 1.0)
        {
            throw new InvalidDataException(
                $"The wx fallback did not preserve the exact Skia viewport: {lastSkiaLogicalScroll:F2} -> {firstWx.ViewportFirstColumn}");
        }

        Console.WriteLine($"uia.runtime_fallback.skia_successful_snapshots={skiaSnapshots.Length}");
        Console.WriteLine($"uia.runtime_fallback.last_skia_frame={lastSkia.FrameId}");
        Console.WriteLine($"uia.runtime_fallback.first_wx_frame={firstWx.FrameId}");
        Console.WriteLine($"uia.runtime_fallback.first_wx_bitmap_cache={firstWx.BitmapCacheHits}/{firstWx.BitmapCacheMisses}");
        Console.WriteLine($"uia.runtime_fallback.viewport={lastSkiaLogicalScroll:F2}/{firstWx.ViewportFirstColumn}");
        Console.WriteLine($"uia.runtime_fallback.content_kind={firstWx.Kind}");
        Console.WriteLine("uia.runtime_fallback.trace_validation=ok");
    }

    private void PreservePerfSessions()
    {
        if (string.IsNullOrWhiteSpace(automationProfileDirectory)
            || string.IsNullOrWhiteSpace(automationArtifactsDirectory)
            || process is null
            || !process.HasExited)
            return;

        var source = Path.Combine(automationProfileDirectory, "user", "perf-sessions");
        if (!Directory.Exists(source))
        {
            Console.WriteLine("uia.perf_sessions=none");
            return;
        }

        var destination = Path.Combine(automationArtifactsDirectory, "perf-sessions");
        Directory.CreateDirectory(destination);
        var copied = 0;
        foreach (var session in Directory.EnumerateDirectories(source))
        {
            var target = Path.Combine(destination, Path.GetFileName(session));
            CopyDirectory(session, target);
            ++copied;
        }

        Console.WriteLine($"uia.perf_sessions.count={copied}");
        Console.WriteLine($"uia.perf_sessions.artifacts={destination}");
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var file in Directory.EnumerateFiles(source))
            File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), overwrite: true);
        foreach (var directory in Directory.EnumerateDirectories(source))
            CopyDirectory(directory, Path.Combine(destination, Path.GetFileName(directory)));
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

    private void MiddleDrag(WinPoint start, WinPoint end, int steps, int delayMs)
    {
        MovePointer(start);
        SendMiddleMouseButton(true);
        for (var i = 1; i <= steps; ++i)
        {
            MovePointer(new WinPoint(start.X + (end.X - start.X) * i / steps,
                start.Y + (end.Y - start.Y) * i / steps));
            Thread.Sleep(delayMs);
        }
        SendMiddleMouseButton(false);
    }

    private static bool ContainsAny(string value, params string[] needles) =>
        needles.Any(needle => value.Contains(needle, StringComparison.OrdinalIgnoreCase));

    private static bool IsRuntimeOptIn(string? value) =>
        value is not null && (value.Equals("1", StringComparison.OrdinalIgnoreCase)
            || value.Equals("true", StringComparison.OrdinalIgnoreCase)
            || value.Equals("on", StringComparison.OrdinalIgnoreCase));

    private static void SendMouseWheelToWindow(nint hwnd, WinPoint screenPoint, int delta)
    {
        var wheel = (long)(ushort)(short)delta << 16;
        var coordinates = ((long)(ushort)(short)screenPoint.Y << 16)
            | (ushort)(short)screenPoint.X;
        SendWindowMessage(hwnd, WindowMessage.MouseWheel, new nint(wheel), new nint(coordinates));
    }

    private static void SendMouseToWindow(
        nint hwnd,
        WindowMessage message,
        int clientX,
        int clientY,
        int keyState)
    {
        var coordinates = ((long)(ushort)(short)clientY << 16)
            | (ushort)(short)clientX;
        SendWindowMessage(hwnd, message, new nint(keyState), new nint(coordinates));
    }

    private static void SendKeyToWindow(nint hwnd, ushort virtualKey)
    {
        SendWindowMessage(hwnd, WindowMessage.KeyDown, new nint(virtualKey), new nint(1));
        var keyUpState = unchecked((long)0xC0000001);
        SendWindowMessage(hwnd, WindowMessage.KeyUp, new nint(virtualKey), new nint(keyUpState));
    }

    private static void SendWindowMessage(nint hwnd, WindowMessage message, nint wParam, nint lParam)
    {
        var sent = SendMessageTimeout(hwnd, (uint)message, wParam, lParam,
            SendMessageTimeoutFlags.AbortIfHung | SendMessageTimeoutFlags.Block,
            2000, out _);
        if (sent == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"SendMessageTimeout failed for {message}");
    }

    private static bool TryClickButtonInBackground(AutomationElement button)
    {
        var bounds = button.Current.BoundingRectangle;
        if (bounds.IsEmpty || bounds.Width <= 0 || bounds.Height <= 0)
            return false;

        var screen = new NativePoint(
            checked((int)Math.Round(bounds.Left + bounds.Width / 2)),
            checked((int)Math.Round(bounds.Top + bounds.Height / 2)));
        var hwnd = WindowFromPoint(screen);
        if (hwnd == 0 || !ScreenToClient(hwnd, ref screen))
            return false;

        var coordinates = new nint(((long)(ushort)(short)screen.Y << 16)
            | (ushort)(short)screen.X);
        SendWindowMessage(hwnd, WindowMessage.MouseMove, 0, coordinates);
        SendWindowMessage(hwnd, WindowMessage.LeftButtonDown, 1, coordinates);
        SendWindowMessage(hwnd, WindowMessage.LeftButtonUp, 0, coordinates);
        return true;
    }

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
                // Absolute SendInput coordinates use the primary display unless
                // VirtualDesk is set, while the normalization above deliberately
                // uses the full virtual-screen bounds.
                Flags = MouseEventFlags.Move | MouseEventFlags.Absolute | MouseEventFlags.VirtualDesk,
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

    private static void SendMiddleMouseButton(bool down)
    {
        SendMouseInput(down ? MouseEventFlags.MiddleDown : MouseEventFlags.MiddleUp, 0);
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

    private static IEnumerable<nint> EnumerateTopLevelWindows(int processId)
    {
        var result = new List<nint>();
        EnumWindows((hwnd, _) =>
        {
            GetWindowThreadProcessId(hwnd, out var ownerProcessId);
			if (ownerProcessId == (uint)processId)
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

    private static string GetWindowText(nint hwnd)
    {
        var buffer = new StringBuilder(512);
        return GetWindowText(hwnd, buffer, buffer.Capacity) == 0 ? string.Empty : buffer.ToString();
    }

    private static WinRect GetWindowRect(nint hwnd)
    {
        GetWindowRect(hwnd, out var rect);
        return new WinRect(rect.Left, rect.Top, rect.Right - rect.Left, rect.Bottom - rect.Top);
    }

    public void Dispose()
    {
        try
        {
            if (!options.KeepOpen && process is not null && !process.HasExited)
            {
                // Preserve the original scenario/startup exception instead of
                // replacing it with a cleanup error from Close().
                if (scenarioSucceeded)
                    Close();
                else
                    TerminateAutomationProcess("scenario-failed");
            }
        }
        finally
        {
            PreservePerfSessions();
            DeleteAutomationProject();
            process?.Dispose();
        }
    }

    private const string BlankAutomationProject = """
        [Script Info]
        Title: Aegisub Skia Audio UIA
        ScriptType: v4.00+
        WrapStyle: 0
        PlayResX: 640
        PlayResY: 480
        ScaledBorderAndShadow: yes

        [V4+ Styles]
        Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
        Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1

        [Events]
        Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
        Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0000,0000,0000,,
        """;

    private const uint InputMouse = 0;
    private const uint InputKeyboard = 1;
    private const uint MonitorDefaultToNearest = 2;
    private const ushort VirtualKeyHome = 0x24;
    private const ushort VirtualKeyUp = 0x26;
    private const ushort VirtualKeyDown = 0x28;

    [Flags]
    private enum SetWindowPosFlags : uint { NoActivate = 0x0010, ShowWindow = 0x0040 }
    [Flags]
    private enum MouseEventFlags : uint { Move = 0x0001, LeftDown = 0x0002, LeftUp = 0x0004, MiddleDown = 0x0020, MiddleUp = 0x0040, Wheel = 0x0800, VirtualDesk = 0x4000, Absolute = 0x8000 }
    [Flags]
    private enum KeyboardEventFlags : uint { KeyUp = 0x0002 }
    [Flags]
    private enum SendMessageTimeoutFlags : uint { Block = 0x0001, AbortIfHung = 0x0002 }
    private enum WindowMessage : uint
    {
        Close = 0x0010,
        KeyDown = 0x0100,
        KeyUp = 0x0101,
        MouseMove = 0x0200,
        LeftButtonDown = 0x0201,
        LeftButtonUp = 0x0202,
        MouseWheel = 0x020A
    }
    private enum SystemMetric { VirtualScreenLeft = 76, VirtualScreenTop = 77, VirtualScreenWidth = 78, VirtualScreenHeight = 79 }
    private enum MonitorDpiType { Effective = 0 }

    private readonly record struct WinPoint(int X, int Y);
    private readonly record struct CursorTraceSnapshot(
        string Source,
        int PositionMs,
        double DeviceX,
        bool LabelVisible,
        bool CursorOnly,
        int TargetWidth,
        bool VisibleRequest,
        bool ContentLookup,
        long TilesDrawn,
        long GpuUploads);
    private readonly record struct PlaybackMarkerTraceSnapshot(
        long MarkerRevision,
        string Source,
        int PositionMs,
        bool LabelVisible,
        bool CursorOnly,
        bool VisibleRequest,
        bool ContentLookup,
        long TilesDrawn,
        long GpuUploads,
        long WorkerBuildsStarted,
        long WorkerPayloadBuildsStarted);
    private readonly record struct MiddleSeekTraceSnapshot(
        bool Active,
        string Source,
        int PositionMs,
        bool LabelVisible,
        bool CursorOnly,
        bool RetainedLayersReused,
        bool VisibleRequest,
        bool ContentLookup,
        long TilesDrawn,
        long GpuUploadsThisFrame,
        long GpuTileUploads,
        long WorkerBuildsStarted,
        long WorkerPayloadBuildsStarted);
    private readonly record struct MiddleSeekTraceEvent(string Phase, int TimeMs, int Frame);
    private readonly record struct FocusColourTraceSnapshot(
        long FrameId,
        string Kind,
        bool Focused,
        long ChromeRevision,
        long PresentationRevision,
        long AnalysisGeneration,
        bool CompleteContentViewport,
        long GpuUploadsThisFrame,
        long GpuTileUploads,
        long GpuPaletteUploads,
        long CpuTileMisses,
        long FftMisses,
        long FftVisibleBuilds,
        long WorkerBuildsStarted,
        long WorkerPayloadBuildsStarted);
    private readonly record struct DpiTransitionTraceSnapshot(
        long FrameId,
        double ContentScale,
        long AnalysisGeneration,
        long ChromeRevision,
        long PresentationRevision,
        bool CompleteContentViewport,
        int TargetWidth,
        int TargetHeight,
        long GpuTileUploads,
        long WorkerBuildsStarted,
        long WorkerPayloadBuildsStarted);
    private readonly record struct RuntimeFallbackTraceSnapshot(
        string Renderer,
        string Kind,
        long FrameId,
        double ContentScale,
        long ViewportFirstColumn,
        bool CompleteContentViewport,
        int TargetWidth,
        int TargetHeight,
        bool SwapAttempted,
        long BitmapCacheHits,
        long BitmapCacheMisses);
    private readonly record struct MonitorDescriptor(
        nint Handle,
        WinRect Work,
        uint DpiX,
        uint DpiY);
    private readonly record struct ColourSchemeSelection(string Before, string After);
    private readonly record struct WinRect(int Left, int Top, int Width, int Height)
    {
        public int Right => Left + Width;
        public int Bottom => Top + Height;
        public long Area => (long)Width * Height;
        public WinPoint Center => new(Left + Width / 2, Top + Height / 2);
    }

    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct NativePoint { public int X, Y; public NativePoint(int x, int y) { X = x; Y = y; } }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo
    {
        public int Size;
        public NativeRect Monitor;
        public NativeRect Work;
        public uint Flags;
    }
    [StructLayout(LayoutKind.Sequential)] private struct GuiThreadInfo
    {
        public int Size;
        public uint Flags;
        public nint Active;
        public nint Focus;
        public nint Capture;
        public nint MenuOwner;
        public nint MoveSize;
        public nint Caret;
        public NativeRect CaretRect;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public InputUnion Union; public MouseInput Mouse { get => Union.Mouse; set => Union.Mouse = value; } public KeyboardInput Keyboard { get => Union.Keyboard; set => Union.Keyboard = value; } }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public MouseInput Mouse; [FieldOffset(0)] public KeyboardInput Keyboard; }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput { public int X, Y; public uint Data; public MouseEventFlags Flags; public uint Time; public nint ExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] private struct KeyboardInput { public ushort VirtualKey, ScanCode; public KeyboardEventFlags Flags; public uint Time; public nint ExtraInfo; }

    private delegate bool EnumWindowsProc(nint hwnd, nint lParam);
    private delegate bool MonitorEnumProc(nint monitor, nint hdc, ref NativeRect bounds, nint data);

    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint inputCount, Input[] inputs, int size);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool PostMessage(nint hwnd, uint message, nint wParam, nint lParam);
    [DllImport("user32.dll", SetLastError = true)] private static extern nint SendMessageTimeout(nint hwnd, uint message, nint wParam, nint lParam, SendMessageTimeoutFlags flags, uint timeoutMs, out nint result);
    [DllImport("user32.dll")] private static extern nint WindowFromPoint(NativePoint point);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetCursorPos(out NativePoint point);
    [DllImport("user32.dll")] private static extern bool ScreenToClient(nint hwnd, ref NativePoint point);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(nint parent, EnumWindowsProc callback, nint lParam);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, nint lParam);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool EnumDisplayMonitors(nint hdc, nint clip, MonitorEnumProc callback, nint data);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")] private static extern bool IsWindowEnabled(nint hwnd);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetGUIThreadInfo(uint threadId, ref GuiThreadInfo info);
    [DllImport("user32.dll")] private static extern nint GetParent(nint hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(nint hwnd, StringBuilder className, int maxCount);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(nint hwnd, StringBuilder text, int maxCount);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(nint hwnd, out NativeRect rect);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint hwnd);
    [DllImport("user32.dll")] private static extern nint MonitorFromWindow(nint hwnd, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW", SetLastError = true)] private static extern bool GetMonitorInfo(nint monitor, ref MonitorInfo info);
    [DllImport("shcore.dll")] private static extern int GetDpiForMonitor(nint monitor, MonitorDpiType dpiType, out uint dpiX, out uint dpiY);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint hwnd);
    [DllImport("user32.dll")] private static extern bool ShowWindow(nint hwnd, int command);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint hwnd, nint insertAfter, int x, int y, int width, int height, SetWindowPosFlags flags);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(SystemMetric index);
}
