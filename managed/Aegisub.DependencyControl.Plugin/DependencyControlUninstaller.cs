using System.Text.Json;
using Aegisub.Managed.Contracts;

namespace Aegisub.DependencyControl.Plugin;

internal sealed class DependencyControlUninstaller(IAegisubPluginContext context)
{
    private const string BeginService =
        "aegisub.dependency-control.begin-transaction";
    private const string CommitService =
        "aegisub.dependency-control.commit-transaction";
    private const string AbortService =
        "aegisub.dependency-control.abort-transaction";

    public async Task<DependencyControlUninstallResult> UninstallAsync(
        DependencyControlUninstallPlan plan,
        DependencyControlInstallJournal journal,
        DependencyControlInstalledStateStore installedState,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(plan);
        ArgumentNullException.ThrowIfNull(journal);
        ArgumentNullException.ThrowIfNull(installedState);
        if (plan.Actions.Count == 0)
            return new("", "", plan.Removal, []);

        string transactionId = "";
        bool committed = false;
        bool journalPrepared = false;
        string automationRoot = "";
        try
        {
            string beginResponse = await context.InvokeHostServiceAsync(
                BeginService, "{}", cancellationToken).ConfigureAwait(false);
            using (JsonDocument begin = JsonDocument.Parse(beginResponse))
            {
                transactionId = ReadRequiredString(
                    begin.RootElement, "transactionId");
                automationRoot = ReadRequiredString(
                    begin.RootElement, "automationRoot");
            }
            journal.Prepare(
                transactionId,
                automationRoot,
                installedState.Revision,
                [],
                plan.Actions,
                [plan.Removal]);
            journalPrepared = true;
            string commitResponse = await context.InvokeHostServiceAsync(
                CommitService,
                BuildCommitRequest(transactionId, plan.Actions),
                cancellationToken).ConfigureAwait(false);
            using (JsonDocument commit = JsonDocument.Parse(commitResponse))
            {
                if (!commit.RootElement.TryGetProperty(
                        "committed", out JsonElement value) ||
                    value.ValueKind != JsonValueKind.True)
                    throw new InvalidOperationException(
                        "DependencyControl native host did not commit the uninstall transaction.");
            }
            committed = true;
            return new(
                transactionId,
                automationRoot,
                plan.Removal,
                plan.Actions.Select(action => action.Target).ToArray());
        }
        finally
        {
            if (!committed && transactionId.Length > 0)
            {
                try
                {
                    await context.InvokeHostServiceAsync(
                        AbortService,
                        BuildAbortRequest(transactionId),
                        CancellationToken.None).ConfigureAwait(false);
                }
                catch
                {
                    // Native commit rollback is authoritative; cleanup failure
                    // must not replace the original uninstall error.
                }
            }
            if (!committed && journalPrepared)
            {
                try
                {
                    journal.Cancel(transactionId);
                }
                catch
                {
                    // Startup reconciliation isolates an unresolved journal.
                }
            }
        }
    }

    private static string BuildCommitRequest(
        string transactionId,
        IReadOnlyList<DependencyControlResolvedFile> actions) =>
        DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("transactionId", transactionId);
            writer.WritePropertyName("files");
            writer.WriteStartArray();
            foreach (DependencyControlResolvedFile action in actions)
            {
                writer.WriteStartObject();
                writer.WriteString("target", action.Target);
                writer.WriteBoolean("delete", true);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        });

    private static string BuildAbortRequest(string transactionId) =>
        DependencyControlServiceContribution.BuildJson(writer =>
        {
            writer.WriteStartObject();
            writer.WriteString("transactionId", transactionId);
            writer.WriteEndObject();
        });

    private static string ReadRequiredString(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out JsonElement value) ||
            value.ValueKind != JsonValueKind.String ||
            string.IsNullOrEmpty(value.GetString()))
            throw new InvalidOperationException(
                $"DependencyControl native host response requires string '{property}'.");
        return value.GetString()!;
    }
}

internal sealed record DependencyControlUninstallResult(
    string TransactionId,
    string AutomationRoot,
    DependencyControlPackageIdentity Removal,
    IReadOnlyList<string> RemovedFiles);
