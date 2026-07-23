#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

try
{
    return Run(args);
}
catch (Exception error)
{
    Console.Error.WriteLine($"uia.correctness.error={error.GetType().Name}:{error.Message}");
    return 1;
}

static int Run(string[] args)
{
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
        var readyPath = Path.Combine(output, "ready.json");
        var ready = AutomationProtocol.WaitForReadyArtifact(
            readyPath, process, TimeSpan.FromSeconds(timeoutSeconds));
        if (File.GetLastWriteTimeUtc(readyPath) < processStartedAtUtc.AddSeconds(-1))
            throw new InvalidOperationException("ready.json predates this UIA run");
        if (!PathsEqual(ready.Artifacts, output))
            throw new InvalidOperationException("ready.json points at a different artifacts directory");
        Console.WriteLine($"uia.correctness.ready_pid={ready.ProcessId}");
        var window = UiaDriver.WaitForMainWindow(process, TimeSpan.FromSeconds(timeoutSeconds));
        UiaDriver.ThrowIfFatalDialog(process);
        UiaDriver.FocusAndVerify(window, process, TimeSpan.FromSeconds(5));

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
        UiaDriver.ThrowIfFatalDialog(process);

        var screenshot = Path.Combine(output, "correctness-main-window.png");
        var capture = ScreenCapture.SaveWindowPng(window, screenshot);
        Console.WriteLine($"uia.correctness.capture={screenshot}");
        Console.WriteLine(
            $"uia.correctness.capture_evidence={capture.Width}x{capture.Height};" +
            $"non_black_pixels={capture.NonBlackPixelCount};" +
            $"minimum={capture.MinimumNonBlackPixelCount}");
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

static void PrepareRunArtifacts(string output, string profile)
{
    Directory.CreateDirectory(output);
    foreach (var name in new[]
    {
        "ready.json",
        "result.json",
        "correctness-main-window.png",
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
