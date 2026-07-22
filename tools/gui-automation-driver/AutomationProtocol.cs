using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Threading;

namespace Aegisub.GuiAutomation.Driver;

public sealed record ReadyArtifact(
    int Version,
    string Host,
    string State,
    int ProcessId,
    string Artifacts);

public static class AutomationProtocol
{
    public static ReadyArtifact WaitForReadyArtifact(
        string path,
        Process process,
        TimeSpan timeout)
    {
        var deadline = Stopwatch.GetTimestamp() +
            (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (TryReadReadyArtifact(path, process.Id, out var ready))
                return ready!;
            if (process.HasExited)
                throw new InvalidOperationException(
                    "Aegisub exited before GUI-test host became ready");
            Thread.Sleep(100);
        }
        throw new TimeoutException("GUI-test host did not produce ready.json");
    }

    public static bool TryReadReadyArtifact(
        string path,
        int expectedProcessId,
        out ReadyArtifact? ready)
    {
        ready = null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            var candidate = new ReadyArtifact(
                root.GetProperty("version").GetInt32(),
                root.GetProperty("host").GetString() ?? string.Empty,
                root.GetProperty("state").GetString() ?? string.Empty,
                root.GetProperty("process_id").GetInt32(),
                root.GetProperty("artifacts").GetString() ?? string.Empty);
            if (candidate.Version != 1
                || candidate.Host != "gui-test"
                || candidate.State != "ready"
                || candidate.ProcessId != expectedProcessId)
                return false;
            ready = candidate;
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
        catch (KeyNotFoundException)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
        catch (IOException)
        {
            // The worker publishes ready.json by rename; a reader can observe
            // a short share/lock window while that transaction is in flight.
            return false;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }
}
