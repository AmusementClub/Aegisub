using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Aegisub.DependencyControl.Plugin;
using Aegisub.Managed.Contracts;

namespace Aegisub.DependencyControl.InstallSmokeHost;

internal static class Program
{
    private const string MacroName = "DependencyControl install transaction smoke";

    public static async Task<int> Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "--batch-update-atomicity")
        {
            try
            {
                string batchWorkDirectory = Path.GetFullPath(args[1]);
                RecreateDirectory(batchWorkDirectory);
                await RunBatchUpdateAtomicitySmokeAsync(batchWorkDirectory)
                    .ConfigureAwait(false);
                Console.WriteLine("DependencyControl batch update atomicity smoke passed.");
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine(
                    $"DependencyControl batch update atomicity smoke failed: {error}");
                return 1;
            }
        }
        if (args.Length == 2 && args[0] == "--managed-tool-view")
        {
            try
            {
                string toolViewWorkDirectory = Path.GetFullPath(args[1]);
                RecreateDirectory(toolViewWorkDirectory);
                await RunManagedToolViewSmokeAsync(toolViewWorkDirectory)
                    .ConfigureAwait(false);
                Console.WriteLine("DependencyControl managed ToolView smoke passed.");
                return 0;
            }
            catch (Exception error)
            {
                Console.Error.WriteLine(
                    $"DependencyControl managed ToolView smoke failed: {error}");
                return 1;
            }
        }
        if (args.Length != 3)
        {
            Console.Error.WriteLine(
                "Usage: install-smoke-host <aegisub-executable> <lua-fixture> <work-directory>");
            return 2;
        }

        string executable = Path.GetFullPath(args[0]);
        string script = Path.GetFullPath(args[1]);
        string workDirectory = Path.GetFullPath(args[2]);
        if (!File.Exists(executable) || !File.Exists(script))
        {
            Console.Error.WriteLine("The Aegisub executable or Lua fixture does not exist.");
            return 2;
        }

        try
        {
            RecreateDirectory(workDirectory);
            await RunManagedToolViewSmokeAsync(workDirectory).ConfigureAwait(false);
            await RunBatchUpdateAtomicitySmokeAsync(workDirectory).ConfigureAwait(false);
            await RunSuccessAsync(executable, script, workDirectory).ConfigureAwait(false);
            await RunHashFailureAsync(executable, script, workDirectory).ConfigureAwait(false);
            await RunRollbackAsync(executable, script, workDirectory).ConfigureAwait(false);
            await RunUpdateRemovalRollbackAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunSharedOwnershipPreservationAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunSharedOwnershipConflictAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunUninstallAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunUninstallRollbackAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunUninstallRecoveryAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunJournalRecoveryAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunUpdateJournalRecoveryAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunConfigurationPersistenceAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunKnownFeedDiscoveryAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunSystemProxyAsync(executable, script, workDirectory).ConfigureAwait(false);
            await RunManualProxyAsync(executable, script, workDirectory).ConfigureAwait(false);
            await RunProxyAuthenticationFailureAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunSocks5ProxyAsync(executable, script, workDirectory).ConfigureAwait(false);
            await RunManualBypassAsync(executable, script, workDirectory).ConfigureAwait(false);
            await RunDirectProxyBypassAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            await RunTruncatedResponseAsync(executable, script, workDirectory)
                .ConfigureAwait(false);
            Console.WriteLine(
                "DependencyControl transaction, failure-path, known-feed, and HTTP/SOCKS5 proxy smoke passed.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"DependencyControl install smoke failed: {error}");
            return 1;
        }
    }

    private static async Task RunSuccessAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.installed";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"installed\" }\n");
        byte[] legacy = Encoding.UTF8.GetBytes("return { legacy = true }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/legacy.lua", legacy);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [
                new FeedFile(".lua", "/main.lua", Sha1(main)),
                new FeedFile("/legacy.lua", "/legacy.lua", Sha1(legacy))
            ]));

        string root = ScenarioRoot(workDirectory, "success-\U0001F600");
        string target = Path.Combine(root, "include", "fixture", "installed.lua");
        string legacyTarget = Path.Combine(
            root, "include", "fixture", "installed", "legacy.lua");
        string[] originalTargets =
        [
            "include/fixture/installed.lua",
            "include/fixture/installed/legacy.lua"
        ];
        string stateRoot = StateRoot(root);
        Directory.CreateDirectory(stateRoot);
        File.WriteAllText(Path.Combine(stateRoot, "installed.json"), "{ invalid");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "installed",
            null,
            installedStateExpectation: "corrupted",
            installedExpectedFileCount: 2)
            .ConfigureAwait(false);
        if (result.ExitCode != 0)
            throw ProcessFailure("success scenario", result);
        if (!File.Exists(target) || !File.ReadAllBytes(target).SequenceEqual(main))
            throw new InvalidOperationException(
                "The successful transaction did not install the expected module bytes.");
        if (!File.Exists(legacyTarget) ||
            !File.ReadAllBytes(legacyTarget).SequenceEqual(legacy))
            throw new InvalidOperationException(
                "The successful transaction did not install the legacy fixture bytes.");
        if (!File.Exists(result.MarkerPath) ||
            File.ReadAllText(result.MarkerPath) != "ok:installed")
            throw new InvalidOperationException(
                "The current Lua State did not load the newly installed module.");
        File.Delete(result.MarkerPath);
        if (origin.RequestCount("/feed.json") == 0 || origin.RequestCount("/main.lua") == 0)
            throw new InvalidOperationException(
                "The successful scenario did not use the loopback HTTP transport.");
        long installedRevision = RequireInstalledState(
            result.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedTargets: originalTargets);
        int feedRequests = origin.RequestCount("/feed.json");
        int fileRequests = origin.RequestCount("/main.lua");
        ProcessResult restarted = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "installed",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            installedExpectedFileCount: 2).ConfigureAwait(false);
        RequireSuccessfulModule(restarted, "installed");
        if (origin.RequestCount("/feed.json") != feedRequests ||
            origin.RequestCount("/main.lua") != fileRequests)
            throw new InvalidOperationException(
                "DependencyControl re-downloaded an already installed module after restart.");
        if (RequireInstalledState(
                restarted.StateRoot,
                moduleName,
                origin.Url("/feed.json"),
                expectedTargets: originalTargets) != installedRevision)
            throw new InvalidOperationException(
                "DependencyControl rewrote unchanged installed state after restart.");
        byte[] updatedMain = Encoding.UTF8.GetBytes(
            "return { value = \"updated\" }\n");
        origin.Set("/main.lua", updatedMain);
        origin.Set("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(updatedMain))],
            version: "2.0.0"));
        ProcessResult catalog = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "installed",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            catalogExpectedVersion: "2.0.0",
            installedExpectedFileCount: 2).ConfigureAwait(false);
        RequireSuccessfulModule(catalog, "installed");
        if (origin.RequestCount("/feed.json") <= feedRequests ||
            origin.RequestCount("/main.lua") != fileRequests)
            throw new InvalidOperationException(
                "DependencyControl catalog/update check did not remain metadata-only.");
        if (RequireInstalledState(
                catalog.StateRoot,
                moduleName,
                origin.Url("/feed.json"),
                expectedTargets: originalTargets) != installedRevision)
            throw new InvalidOperationException(
                "DependencyControl metadata-only catalog operations rewrote installed state.");

        int metadataFeedRequests = origin.RequestCount("/feed.json");
        ProcessResult updated = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "installed",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            catalogExpectedVersion: "2.0.0",
            applyVersion: "2.0.0",
            installedExpectedFileCount: 2).ConfigureAwait(false);
        RequireSuccessfulModule(updated, "installed");
        if (!File.ReadAllBytes(target).SequenceEqual(updatedMain))
            throw new InvalidOperationException(
                "DependencyControl updates.apply did not replace the installed payload.");
        if (File.Exists(legacyTarget))
            throw new InvalidOperationException(
                "DependencyControl updates.apply did not remove obsolete owned content.");
        if (origin.RequestCount("/feed.json") <= metadataFeedRequests ||
            origin.RequestCount("/main.lua") != fileRequests + 1)
            throw new InvalidOperationException(
                "DependencyControl updates.apply did not download exactly one updated payload.");
        long updatedRevision = RequireInstalledState(
            updated.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 1,
            expectedVersion: "2.0.0");
        if (updatedRevision <= installedRevision)
            throw new InvalidOperationException(
                "DependencyControl updates.apply did not advance installed-state revision.");
        if (File.Exists(Path.Combine(updated.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "DependencyControl updates.apply left a pending install journal behind.");

        int updatedFeedRequests = origin.RequestCount("/feed.json");
        int updatedFileRequests = origin.RequestCount("/main.lua");
        ProcessResult updateRestarted = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "updated",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            installedExpectedVersion: "2.0.0").ConfigureAwait(false);
        RequireSuccessfulModule(updateRestarted, "updated");
        if (origin.RequestCount("/feed.json") != updatedFeedRequests ||
            origin.RequestCount("/main.lua") != updatedFileRequests)
            throw new InvalidOperationException(
                "DependencyControl re-downloaded an updated module after restart.");
        if (RequireInstalledState(
                updateRestarted.StateRoot,
                moduleName,
                origin.Url("/feed.json"),
                expectedCorruptBackups: 1,
                expectedVersion: "2.0.0") != updatedRevision)
            throw new InvalidOperationException(
                "DependencyControl rewrote unchanged updated state after restart.");
        RequireCleanStaging(root);
    }

    private static async Task RunHashFailureAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.hashfail";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"unexpected\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", new string('0', 40))]));

        string root = ScenarioRoot(workDirectory, "hash-failure");
        ProcessResult result = await RunAegisubAsync(
            executable, script, root, origin.Url("/feed.json"), moduleName, "unused", null)
            .ConfigureAwait(false);
        string target = Path.Combine(root, "include", "fixture", "hashfail.lua");
        if (File.Exists(target))
            throw new InvalidOperationException(
                "A hash failure left an installed target behind.");
        if (origin.RequestCount("/feed.json") == 0 || origin.RequestCount("/main.lua") == 0)
            throw new InvalidOperationException(
                "The hash failure scenario did not download its fixture payload.");
        if (!File.Exists(result.MarkerPath) ||
            !File.ReadAllText(result.MarkerPath).StartsWith("error:", StringComparison.Ordinal))
            throw new InvalidOperationException(
                "The hash failure scenario did not report a module-resolution error.");
        File.Delete(result.MarkerPath);
        RequireCleanStaging(root);
    }

    private static async Task RunRollbackAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.rollback";
        byte[] first = Encoding.UTF8.GetBytes("return { value = \"new\" }\n");
        byte[] second = Encoding.UTF8.GetBytes("return true\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", first);
        origin.Add("/second.lua", second);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [
                new FeedFile(".lua", "/main.lua", Sha1(first)),
                new FeedFile("/Second.lua", "/second.lua", Sha1(second))
            ]));

        string root = ScenarioRoot(workDirectory, "rollback");
        string firstTarget = Path.Combine(root, "include", "fixture", "rollback.lua");
        string secondTarget = Path.Combine(
            root, "include", "fixture", "rollback", "Second.lua");
        Directory.CreateDirectory(Path.GetDirectoryName(firstTarget)!);
        byte[] old = Encoding.UTF8.GetBytes("return { value = \"old\" }\n");
        File.WriteAllBytes(firstTarget, old);

        ProcessResult result = await RunAegisubAsync(
            executable, script, root, origin.Url("/feed.json"), moduleName, "unused", "1")
            .ConfigureAwait(false);
        if (!File.ReadAllBytes(firstTarget).SequenceEqual(old))
            throw new InvalidOperationException(
                "Rollback did not restore the previous module bytes.");
        if (File.Exists(secondTarget))
            throw new InvalidOperationException(
                "Rollback left a newly installed file behind.");
        if (origin.RequestCount("/feed.json") == 0 ||
            origin.RequestCount("/main.lua") == 0 ||
            origin.RequestCount("/second.lua") == 0)
            throw new InvalidOperationException(
                "The rollback scenario did not stage the complete HTTP payload set.");
        if (!File.Exists(result.MarkerPath) ||
            !File.ReadAllText(result.MarkerPath).StartsWith("error:", StringComparison.Ordinal))
            throw new InvalidOperationException(
                "The injected commit failure was not reported to the Lua facade.");
        File.Delete(result.MarkerPath);
        RequireCleanStaging(root);
    }

    private static async Task RunUpdateRemovalRollbackAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.updaterollback";
        byte[] original = Encoding.UTF8.GetBytes(
            "return { value = \"rollback-old\" }\n");
        byte[] updated = Encoding.UTF8.GetBytes(
            "return { value = \"rollback-new\" }\n");
        byte[] legacy = Encoding.UTF8.GetBytes("return { legacy = true }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", original);
        origin.Add("/legacy.lua", legacy);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [
                new FeedFile(".lua", "/main.lua", Sha1(original)),
                new FeedFile("/legacy.lua", "/legacy.lua", Sha1(legacy))
            ]));

        string root = ScenarioRoot(workDirectory, "update-removal-rollback");
        string target = Path.Combine(
            root, "include", "fixture", "updaterollback.lua");
        string legacyTarget = Path.Combine(
            root, "include", "fixture", "updaterollback", "legacy.lua");
        string[] originalTargets =
        [
            "include/fixture/updaterollback.lua",
            "include/fixture/updaterollback/legacy.lua"
        ];
        ProcessResult installed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "rollback-old",
            null).ConfigureAwait(false);
        RequireSuccessfulModule(installed, "rollback-old");
        long installedRevision = RequireInstalledState(
            installed.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0,
            expectedTargets: originalTargets);

        origin.Set("/main.lua", updated);
        origin.Set("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(updated))],
            version: "2.0.0"));
        ProcessResult failed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "rollback-old",
            "2",
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            catalogExpectedVersion: "2.0.0",
            applyVersion: "2.0.0",
            installedExpectedFileCount: 2,
            applyExpectedFailure: true).ConfigureAwait(false);
        RequireSuccessfulModule(failed, "rollback-old");
        if (!File.ReadAllBytes(target).SequenceEqual(original) ||
            !File.ReadAllBytes(legacyTarget).SequenceEqual(legacy))
            throw new InvalidOperationException(
                "DependencyControl update rollback did not restore replacement and deletion.");
        if (RequireInstalledState(
                failed.StateRoot,
                moduleName,
                origin.Url("/feed.json"),
                expectedCorruptBackups: 0,
                expectedTargets: originalTargets) != installedRevision)
            throw new InvalidOperationException(
                "DependencyControl failed update changed installed ownership state.");
        if (File.Exists(Path.Combine(failed.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "DependencyControl failed update left a pending journal behind.");
        RequireCleanStaging(root);
    }

    private static async Task RunSharedOwnershipPreservationAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.sharedpreserve";
        const string otherNamespace = "fixture.sharedowner";
        const string mainOwnerNamespace = "fixture.sharedmainowner";
        const string mainRelative = "include/fixture/sharedpreserve.lua";
        const string sharedRelative = "include/fixture/sharedpreserve/shared.lua";
        byte[] original = Encoding.UTF8.GetBytes(
            "return { value = \"shared-old\" }\n");
        byte[] updated = Encoding.UTF8.GetBytes(
            "return { value = \"shared-new\" }\n");
        byte[] shared = Encoding.UTF8.GetBytes("return { shared = true }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", original);
        origin.Add("/shared.lua", shared);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [
                new FeedFile(".lua", "/main.lua", Sha1(original)),
                new FeedFile("/shared.lua", "/shared.lua", Sha1(shared))
            ]));

        string root = ScenarioRoot(workDirectory, "shared-ownership-preservation");
        string target = Path.Combine(root, "include", "fixture", "sharedpreserve.lua");
        string sharedTarget = Path.Combine(
            root, "include", "fixture", "sharedpreserve", "shared.lua");
        ProcessResult installed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "shared-old",
            null).ConfigureAwait(false);
        RequireSuccessfulModule(installed, "shared-old");
        AddInstalledOwner(
            installed.StateRoot,
            otherNamespace,
            origin.Url("/feed.json"),
            sharedRelative,
            Sha1(shared));

        origin.Set("/main.lua", updated);
        origin.Set("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(updated))],
            version: "2.0.0"));
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "shared-old",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            catalogExpectedVersion: "2.0.0",
            applyVersion: "2.0.0",
            installedExpectedFileCount: 2).ConfigureAwait(false);
        RequireSuccessfulModule(result, "shared-old");
        if (!File.ReadAllBytes(target).SequenceEqual(updated) ||
            !File.ReadAllBytes(sharedTarget).SequenceEqual(shared))
            throw new InvalidOperationException(
                "DependencyControl did not preserve content with a remaining owner.");
        RequireInstalledState(
            result.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0,
            expectedVersion: "2.0.0");
        RequirePackageOwnsTarget(result.StateRoot, otherNamespace, sharedRelative);
        AddInstalledOwner(
            result.StateRoot,
            mainOwnerNamespace,
            origin.Url("/feed.json"),
            mainRelative,
            Sha1(updated));
        int feedRequests = origin.RequestCount("/feed.json");
        int fileRequests = origin.RequestCount("/main.lua");
        ProcessResult removed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "shared-new",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            installedExpectedVersion: "2.0.0",
            installedExpectedFileCount: 1,
            uninstallTest: true).ConfigureAwait(false);
        RequireSuccessfulModule(removed, "shared-new");
        if (!File.Exists(target) || !File.Exists(sharedTarget))
            throw new InvalidOperationException(
                "DependencyControl uninstall removed content still owned by another package.");
        RequirePackageAbsent(removed.StateRoot, moduleName);
        RequirePackageOwnsTarget(removed.StateRoot, mainOwnerNamespace, mainRelative);
        RequirePackageOwnsTarget(removed.StateRoot, otherNamespace, sharedRelative);
        if (origin.RequestCount("/feed.json") != feedRequests ||
            origin.RequestCount("/main.lua") != fileRequests)
            throw new InvalidOperationException(
                "DependencyControl shared-owner uninstall performed an unexpected download.");
        RequireCleanStaging(root);
    }

    private static async Task RunSharedOwnershipConflictAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.sharedconflict";
        const string otherNamespace = "fixture.conflictowner";
        const string targetRelative = "include/fixture/sharedconflict.lua";
        byte[] original = Encoding.UTF8.GetBytes(
            "return { value = \"conflict-old\" }\n");
        byte[] updated = Encoding.UTF8.GetBytes(
            "return { value = \"conflict-new\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", original);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(original))]));

        string root = ScenarioRoot(workDirectory, "shared-ownership-conflict");
        string target = Path.Combine(root, "include", "fixture", "sharedconflict.lua");
        ProcessResult installed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "conflict-old",
            null).ConfigureAwait(false);
        RequireSuccessfulModule(installed, "conflict-old");
        AddInstalledOwner(
            installed.StateRoot,
            otherNamespace,
            origin.Url("/feed.json"),
            targetRelative,
            Sha1(original));

        origin.Set("/main.lua", updated);
        origin.Set("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(updated))],
            version: "2.0.0"));
        int payloadRequests = origin.RequestCount("/main.lua");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "conflict-old",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            catalogExpectedVersion: "2.0.0",
            applyVersion: "2.0.0",
            applyExpectedFailure: true).ConfigureAwait(false);
        RequireSuccessfulModule(result, "conflict-old");
        if (!File.ReadAllBytes(target).SequenceEqual(original))
            throw new InvalidOperationException(
                "DependencyControl changed a target with conflicting ownership.");
        if (origin.RequestCount("/main.lua") != payloadRequests)
            throw new InvalidOperationException(
                "DependencyControl downloaded a payload before ownership rejection.");
        RequireInstalledState(
            result.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0);
        RequirePackageOwnsTarget(result.StateRoot, otherNamespace, targetRelative);
        if (File.Exists(Path.Combine(result.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "DependencyControl ownership rejection created a pending journal.");
        RequireCleanStaging(root);
    }

    private static async Task RunUninstallAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.uninstall";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"uninstall\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]));

        string root = ScenarioRoot(workDirectory, "uninstall");
        ProcessResult installed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall",
            null).ConfigureAwait(false);
        RequireSuccessfulModule(installed, "uninstall");
        string configPath = Path.Combine(
            installed.StateRoot, "config", moduleName + ".json");
        Directory.CreateDirectory(Path.GetDirectoryName(configPath)!);
        File.WriteAllText(configPath, "{\"keep\":true}", Encoding.UTF8);
        int feedRequests = origin.RequestCount("/feed.json");
        int fileRequests = origin.RequestCount("/main.lua");

        ProcessResult removed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            installedExpectedFileCount: 1,
            uninstallTest: true,
            uninstallRemoveConfig: false).ConfigureAwait(false);
        RequireSuccessfulModule(removed, "uninstall");
        string target = Path.Combine(root, "include", "fixture", "uninstall.lua");
        if (File.Exists(target) || !File.Exists(configPath))
            throw new InvalidOperationException(
                "DependencyControl uninstall did not remove the target while preserving configuration.");
        RequirePackageAbsent(removed.StateRoot, moduleName);
        if (origin.RequestCount("/feed.json") != feedRequests ||
            origin.RequestCount("/main.lua") != fileRequests)
            throw new InvalidOperationException(
                "DependencyControl uninstall performed an unexpected download.");
        if (File.Exists(Path.Combine(removed.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "DependencyControl uninstall left a pending journal behind.");

        ProcessResult reinstalled = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall",
            null,
            useIsolatedAutomationScript: true).ConfigureAwait(false);
        RequireSuccessfulModule(reinstalled, "uninstall");
        if (!File.Exists(target) || !File.Exists(configPath))
            throw new InvalidOperationException(
                "DependencyControl could not reinstall a package after uninstall.");
        ProcessResult configRemoved = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            uninstallTest: true).ConfigureAwait(false);
        RequireSuccessfulModule(configRemoved, "uninstall");
        if (File.Exists(target) || File.Exists(configPath))
            throw new InvalidOperationException(
                "DependencyControl uninstall did not remove configuration by default.");
        RequirePackageAbsent(configRemoved.StateRoot, moduleName);
        RequireCleanStaging(root);
    }

    private static async Task RunUninstallRollbackAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.uninstallrollback";
        byte[] main = Encoding.UTF8.GetBytes(
            "return { value = \"uninstall-rollback\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]));
        string root = ScenarioRoot(workDirectory, "uninstall-rollback");
        ProcessResult installed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall-rollback",
            null).ConfigureAwait(false);
        RequireSuccessfulModule(installed, "uninstall-rollback");
        long revision = RequireInstalledState(
            installed.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0);
        ProcessResult failed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall-rollback",
            "1",
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            installedExpectedFileCount: 1,
            uninstallTest: true,
            uninstallExpectedFailure: true).ConfigureAwait(false);
        RequireSuccessfulModule(failed, "uninstall-rollback");
        string target = Path.Combine(
            root, "include", "fixture", "uninstallrollback.lua");
        if (!File.Exists(target))
            throw new InvalidOperationException(
                "DependencyControl uninstall rollback did not restore the target.");
        if (RequireInstalledState(
                failed.StateRoot,
                moduleName,
                origin.Url("/feed.json"),
                expectedCorruptBackups: 0) != revision)
            throw new InvalidOperationException(
                "DependencyControl uninstall rollback changed installed state.");
        if (File.Exists(Path.Combine(failed.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "DependencyControl uninstall rollback left a pending journal.");
        RequireCleanStaging(root);
    }

    private static async Task RunUninstallRecoveryAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.uninstallrecovery";
        byte[] main = Encoding.UTF8.GetBytes(
            "return { value = \"uninstall-recovery\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]));

        string root = ScenarioRoot(workDirectory, "uninstall-recovery");
        ProcessResult installed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall-recovery",
            null).ConfigureAwait(false);
        RequireSuccessfulModule(installed, "uninstall-recovery");
        int feedRequests = origin.RequestCount("/feed.json");
        int fileRequests = origin.RequestCount("/main.lua");
        ProcessResult interrupted = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall-recovery",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            installedExpectedFileCount: 1,
            uninstallTest: true,
            exitAfterCommit: true).ConfigureAwait(false);
        if (interrupted.ExitCode != 86)
            throw ProcessFailure("uninstall commit-to-state interruption scenario", interrupted);
        string target = Path.Combine(
            root, "include", "fixture", "uninstallrecovery.lua");
        if (File.Exists(target) ||
            !File.Exists(Path.Combine(interrupted.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "DependencyControl uninstall interruption did not leave the expected pending state.");
        if (File.Exists(interrupted.MarkerPath))
            File.Delete(interrupted.MarkerPath);
        RequireInstalledState(
            interrupted.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0);

        ProcessResult recovered = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "uninstall-recovered",
            null,
            useIsolatedAutomationScript: true,
            uninstallRecovery: true).ConfigureAwait(false);
        RequireUninstallRecovery(recovered);
        RequirePackageAbsent(recovered.StateRoot, moduleName);
        if (origin.RequestCount("/feed.json") != feedRequests ||
            origin.RequestCount("/main.lua") != fileRequests)
            throw new InvalidOperationException(
                "DependencyControl uninstall recovery re-downloaded the removed package.");
        if (File.Exists(Path.Combine(recovered.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "DependencyControl uninstall recovery did not clear its pending journal.");
        RequireCleanStaging(root);
    }

    private static async Task RunKnownFeedDiscoveryAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.knownfeed";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"known\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/catalog.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]));
        origin.Add("/feed.json", BuildKnownFeedIndex(origin.Url("/catalog.json")));

        string root = ScenarioRoot(workDirectory, "known-feed");
        ProcessResult result = await RunAegisubAsync(
            executable, script, root, origin.Url("/feed.json"), moduleName, "known", null)
            .ConfigureAwait(false);
        RequireSuccessfulModule(result, "known");
        if (origin.RequestCount("/feed.json") == 0 ||
            origin.RequestCount("/catalog.json") == 0 ||
            origin.RequestCount("/main.lua") == 0)
            throw new InvalidOperationException(
                "DependencyControl did not discover the module through knownFeeds.");
        RequireCleanStaging(root);
    }

    private static async Task RunJournalRecoveryAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.journal";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"journal\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]));

        string root = ScenarioRoot(workDirectory, "journal-recovery-\U0001F600");
        ProcessResult interrupted = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "journal",
            null,
            exitAfterCommit: true).ConfigureAwait(false);
        if (interrupted.ExitCode != 86)
            throw ProcessFailure("commit-to-state interruption scenario", interrupted);
        string target = Path.Combine(root, "include", "fixture", "journal.lua");
        if (!File.Exists(target) || !File.ReadAllBytes(target).SequenceEqual(main))
            throw new InvalidOperationException(
                "DependencyControl did not complete native commit before interruption.");
        string pending = Path.Combine(interrupted.StateRoot, "pending-install.json");
        if (!File.Exists(pending))
            throw new InvalidOperationException(
                "DependencyControl did not retain its pending install journal.");
        int feedRequests = origin.RequestCount("/feed.json");
        int fileRequests = origin.RequestCount("/main.lua");

        ProcessResult recovered = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "journal",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true).ConfigureAwait(false);
        RequireSuccessfulModule(recovered, "journal");
        if (origin.RequestCount("/feed.json") != feedRequests ||
            origin.RequestCount("/main.lua") != fileRequests)
            throw new InvalidOperationException(
                "DependencyControl recovery re-downloaded a committed package.");
        RequireInstalledState(
            recovered.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0);
        if (File.Exists(pending) ||
            Directory.EnumerateFiles(recovered.StateRoot, "pending-install-*.tmp").Any() ||
            Directory.EnumerateFiles(
                recovered.StateRoot, "pending-install.json.abandoned-*").Any())
            throw new InvalidOperationException(
                "DependencyControl did not finalize its pending install journal.");
        RequireCleanStaging(root);
    }

    private static async Task RunUpdateJournalRecoveryAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.updatejournal";
        byte[] original = Encoding.UTF8.GetBytes(
            "return { value = \"update-old\" }\n");
        byte[] updated = Encoding.UTF8.GetBytes(
            "return { value = \"update-recovered\" }\n");
        byte[] legacy = Encoding.UTF8.GetBytes("return { legacy = true }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", original);
        origin.Add("/legacy.lua", legacy);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [
                new FeedFile(".lua", "/main.lua", Sha1(original)),
                new FeedFile("/legacy.lua", "/legacy.lua", Sha1(legacy))
            ]));

        string root = ScenarioRoot(workDirectory, "update-journal-recovery");
        string legacyTarget = Path.Combine(
            root, "include", "fixture", "updatejournal", "legacy.lua");
        string[] originalTargets =
        [
            "include/fixture/updatejournal.lua",
            "include/fixture/updatejournal/legacy.lua"
        ];
        ProcessResult installed = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "update-old",
            null).ConfigureAwait(false);
        RequireSuccessfulModule(installed, "update-old");
        long installedRevision = RequireInstalledState(
            installed.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0,
            expectedTargets: originalTargets);
        if (!File.Exists(legacyTarget) ||
            !File.ReadAllBytes(legacyTarget).SequenceEqual(legacy))
            throw new InvalidOperationException(
                "DependencyControl update recovery fixture did not install legacy content.");

        origin.Set("/main.lua", updated);
        origin.Set("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(updated))],
            version: "2.0.0"));
        int payloadRequests = origin.RequestCount("/main.lua");
        ProcessResult interrupted = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "update-old",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            catalogExpectedVersion: "2.0.0",
            applyVersion: "2.0.0",
            installedExpectedFileCount: 2,
            exitAfterCommit: true).ConfigureAwait(false);
        if (interrupted.ExitCode != 86)
            throw ProcessFailure("update commit-to-state interruption scenario", interrupted);
        string target = Path.Combine(
            root, "include", "fixture", "updatejournal.lua");
        if (!File.Exists(target) || !File.ReadAllBytes(target).SequenceEqual(updated))
            throw new InvalidOperationException(
                "DependencyControl did not commit the updated payload before interruption.");
        if (File.Exists(legacyTarget))
            throw new InvalidOperationException(
                "DependencyControl did not commit obsolete-file removal before interruption.");
        if (origin.RequestCount("/main.lua") != payloadRequests + 1)
            throw new InvalidOperationException(
                "DependencyControl interrupted update did not download one new payload.");
        string pending = Path.Combine(interrupted.StateRoot, "pending-install.json");
        if (!File.Exists(pending))
            throw new InvalidOperationException(
                "DependencyControl interrupted update did not retain its pending journal.");
        if (RequireInstalledState(
                interrupted.StateRoot,
                moduleName,
                origin.Url("/feed.json"),
                expectedCorruptBackups: 0,
                expectedTargets: originalTargets) != installedRevision)
            throw new InvalidOperationException(
                "DependencyControl persisted update state before the injected interruption.");
        if (File.Exists(interrupted.MarkerPath))
            File.Delete(interrupted.MarkerPath);

        int feedRequestsAfterCommit = origin.RequestCount("/feed.json");
        int fileRequestsAfterCommit = origin.RequestCount("/main.lua");
        ProcessResult recovered = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "update-recovered",
            null,
            installedStateExpectation: "persisted",
            useIsolatedAutomationScript: true,
            installedExpectedVersion: "2.0.0").ConfigureAwait(false);
        RequireSuccessfulModule(recovered, "update-recovered");
        if (origin.RequestCount("/feed.json") != feedRequestsAfterCommit ||
            origin.RequestCount("/main.lua") != fileRequestsAfterCommit)
            throw new InvalidOperationException(
                "DependencyControl update recovery re-downloaded a committed package.");
        long recoveredRevision = RequireInstalledState(
            recovered.StateRoot,
            moduleName,
            origin.Url("/feed.json"),
            expectedCorruptBackups: 0,
            expectedVersion: "2.0.0");
        if (recoveredRevision <= installedRevision)
            throw new InvalidOperationException(
                "DependencyControl update recovery did not advance installed state.");
        if (File.Exists(pending) ||
            Directory.EnumerateFiles(recovered.StateRoot, "pending-install-*.tmp").Any() ||
            Directory.EnumerateFiles(
                recovered.StateRoot, "pending-install.json.abandoned-*").Any())
            throw new InvalidOperationException(
                "DependencyControl did not finalize its interrupted update journal.");
        RequireCleanStaging(root);
    }

    private static async Task RunConfigurationPersistenceAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.configuration";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"configuration\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/main.lua", main);
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]));

        string root = ScenarioRoot(workDirectory, "configuration-\U0001F600");
        string stateRoot = Path.Combine(Path.GetDirectoryName(root)!, "state");
        string configRoot = Path.Combine(stateRoot, "config");
        string legacyConfigRoot = Path.Combine(
            Path.GetDirectoryName(root)!, "legacy-config");
        Directory.CreateDirectory(configRoot);
        Directory.CreateDirectory(legacyConfigRoot);
        File.WriteAllText(
            Path.Combine(legacyConfigRoot, "legacy.json"),
            "{\"legacy\":\"from-legacy\"}",
            Encoding.UTF8);
        File.WriteAllText(
            Path.Combine(configRoot, "corrupt.json"),
            "{ invalid-json",
            Encoding.UTF8);

        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "configuration",
            null,
            networkOptions: null,
            testConfiguration: true,
            legacyConfigRoot: legacyConfigRoot).ConfigureAwait(false);
        RequireSuccessfulModule(result, "configuration");

        string compatibilityPath = Path.Combine(configRoot, "compatibility.json");
        using (JsonDocument document = JsonDocument.Parse(
            File.ReadAllBytes(compatibilityPath)))
        {
            JsonElement profile = document.RootElement
                .GetProperty("profiles")
                .GetProperty("fixture");
            if (profile.GetProperty("theme").GetString() != "dark" ||
                profile.GetProperty("nested").GetProperty("count").GetInt32() != 2 ||
                profile.GetProperty("nested").GetProperty("enabled").GetBoolean() != true ||
                profile.GetProperty("imported").GetInt32() != 7 ||
                profile.TryGetProperty("_private", out _) ||
                document.RootElement.GetProperty("profiles").TryGetProperty(
                    "sibling", out _))
                throw new InvalidOperationException(
                    "DependencyControl configuration sections were not persisted correctly.");
        }
        using (JsonDocument document = JsonDocument.Parse(
            File.ReadAllBytes(Path.Combine(configRoot, "corrupt.json"))))
        {
            if (!document.RootElement.GetProperty("recovered").GetBoolean())
                throw new InvalidOperationException(
                    "DependencyControl did not recover from a corrupted configuration.");
        }
        if (Directory.EnumerateFiles(
                configRoot,
                "corrupt.json.corrupted-*",
                SearchOption.TopDirectoryOnly).Count() != 1)
            throw new InvalidOperationException(
                "DependencyControl did not retain exactly one corrupted configuration backup.");
        if (Directory.EnumerateFiles(
                configRoot,
                "*.tmp",
                SearchOption.TopDirectoryOnly).Any())
            throw new InvalidOperationException(
                "DependencyControl left a configuration temporary file behind.");
        if (File.Exists(Path.Combine(stateRoot, "escape.json")))
            throw new InvalidOperationException(
                "DependencyControl configuration escaped its bounded root.");
        RequireCleanStaging(root);
    }

    private static async Task RunSystemProxyAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.systemproxy";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"system\" }\n");
        await using LoopbackOrigin origin = new();
        await using LoopbackOrigin proxy = new();
        byte[] feed = BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]);
        AddNetworkRoutes(origin, feed, main);
        AddNetworkRoutes(proxy, feed, main);

        string root = ScenarioRoot(workDirectory, "system-proxy");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "system",
            null,
            new(
                ExpectedMode: "System",
                UpdateMode: null,
                ManualProxyUri: null,
                Bypass: null,
                EnvironmentProxyUri: proxy.BaseUrl,
                NetworkTestUrl: origin.Url("/probe")))
            .ConfigureAwait(false);
        RequireSuccessfulModule(result, "system");
        if (proxy.RequestCount("/feed.json") == 0 ||
            proxy.RequestCount("/main.lua") == 0 ||
            proxy.RequestCount("/probe") == 0)
            throw new InvalidOperationException(
                "System proxy mode did not use the process system-proxy configuration.");
        RequireCleanStaging(root);
    }

    private static async Task RunManualProxyAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.manualproxy";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"manual\" }\n");
        await using LoopbackOrigin origin = new();
        await using LoopbackOrigin proxy = new();
        byte[] feed = BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]);
        AddNetworkRoutes(origin, feed, main);
        AddNetworkRoutes(proxy, feed, main);

        string root = ScenarioRoot(workDirectory, "manual-proxy");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "manual",
            null,
            new(
                ExpectedMode: "Manual",
                UpdateMode: "Manual",
                ManualProxyUri: proxy.BaseUrl,
                Bypass: null,
                EnvironmentProxyUri: null,
                NetworkTestUrl: origin.Url("/probe")))
            .ConfigureAwait(false);
        RequireSuccessfulModule(result, "manual");
        RequirePersistedMode(result.StateRoot, "Manual");
        if (proxy.RequestCount("/feed.json") == 0 ||
            proxy.RequestCount("/main.lua") == 0 ||
            proxy.RequestCount("/probe") == 0 ||
            origin.TotalRequestCount != 0)
            throw new InvalidOperationException(
                "Manual proxy mode did not route all HTTP requests through its proxy.");
        RequireCleanStaging(root);
    }

    private static async Task RunManualBypassAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.manualbypass";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"bypass\" }\n");
        await using LoopbackOrigin origin = new();
        await using LoopbackOrigin proxy = new();
        byte[] feed = BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]);
        AddNetworkRoutes(origin, feed, main);
        AddNetworkRoutes(proxy, feed, main);

        string root = ScenarioRoot(workDirectory, "manual-bypass");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "bypass",
            null,
            new(
                ExpectedMode: "Manual",
                UpdateMode: "Manual",
                ManualProxyUri: proxy.BaseUrl,
                Bypass: "127.0.0.1",
                EnvironmentProxyUri: null,
                NetworkTestUrl: origin.Url("/probe")))
            .ConfigureAwait(false);
        RequireSuccessfulModule(result, "bypass");
        RequirePersistedMode(result.StateRoot, "Manual");
        if (origin.RequestCount("/feed.json") == 0 ||
            origin.RequestCount("/main.lua") == 0 ||
            origin.RequestCount("/probe") == 0 ||
            proxy.TotalRequestCount != 0)
            throw new InvalidOperationException(
                "Manual proxy bypass did not send matching hosts directly.");
        RequireCleanStaging(root);
    }

    private static async Task RunProxyAuthenticationFailureAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.proxyauthfailure";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"unexpected\" }\n");
        await using LoopbackOrigin origin = new();
        await using LoopbackOrigin proxy = new();
        byte[] feed = BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]);
        AddNetworkRoutes(origin, feed, main);
        proxy.AddProxyAuthenticationRequired("/feed.json");

        string root = ScenarioRoot(workDirectory, "proxy-authentication-failure");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "unused",
            null,
            new(
                ExpectedMode: "Manual",
                UpdateMode: "Manual",
                ManualProxyUri: proxy.BaseUrl,
                Bypass: null,
                EnvironmentProxyUri: null,
                NetworkTestUrl: null))
            .ConfigureAwait(false);
        RequireFailedModule(result, "407");
        RequirePersistedMode(result.StateRoot, "Manual");
        if (proxy.RequestCount("/feed.json") == 0 || origin.TotalRequestCount != 0)
            throw new InvalidOperationException(
                "Proxy authentication failure did not remain on the configured proxy path.");
        if (File.Exists(Path.Combine(root, "include", "fixture", "proxyauthfailure.lua")))
            throw new InvalidOperationException(
                "Proxy authentication failure left an installed payload behind.");
        RequireCleanStaging(root);
    }

    private static async Task RunSocks5ProxyAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.socks5proxy";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"socks5\" }\n");
        await using LoopbackOrigin origin = new();
        await using LoopbackSocks5Proxy proxy = new();
        byte[] feed = BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]);
        AddNetworkRoutes(origin, feed, main);

        string root = ScenarioRoot(workDirectory, "socks5-proxy");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "socks5",
            null,
            new(
                ExpectedMode: "Manual",
                UpdateMode: "Manual",
                ManualProxyUri: proxy.BaseUrl,
                Bypass: null,
                EnvironmentProxyUri: null,
                NetworkTestUrl: origin.Url("/probe")))
            .ConfigureAwait(false);
        RequireSuccessfulModule(result, "socks5");
        RequirePersistedMode(result.StateRoot, "Manual");
        proxy.RequireHealthy();
        if (proxy.ConnectionCount == 0 ||
            origin.RequestCount("/feed.json") == 0 ||
            origin.RequestCount("/main.lua") == 0 ||
            origin.RequestCount("/probe") == 0)
            throw new InvalidOperationException(
                "Manual SOCKS5 mode did not tunnel all expected HTTP requests.");
        RequireCleanStaging(root);
    }

    private static async Task RunDirectProxyBypassAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.directproxy";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"direct\" }\n");
        await using LoopbackOrigin origin = new();
        await using LoopbackOrigin proxy = new();
        byte[] feed = BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]);
        AddNetworkRoutes(origin, feed, main);
        AddNetworkRoutes(proxy, feed, main);

        string root = ScenarioRoot(workDirectory, "direct-proxy");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "direct",
            null,
            new(
                ExpectedMode: "Direct",
                UpdateMode: "Direct",
                ManualProxyUri: null,
                Bypass: null,
                EnvironmentProxyUri: proxy.BaseUrl,
                NetworkTestUrl: origin.Url("/probe")))
            .ConfigureAwait(false);
        RequireSuccessfulModule(result, "direct");
        RequirePersistedMode(result.StateRoot, "Direct");
        if (origin.RequestCount("/feed.json") == 0 ||
            origin.RequestCount("/main.lua") == 0 ||
            origin.RequestCount("/probe") == 0 ||
            proxy.TotalRequestCount != 0)
            throw new InvalidOperationException(
                "Direct mode did not bypass the configured system proxy.");
        RequireCleanStaging(root);
    }

    private static async Task RunTruncatedResponseAsync(
        string executable,
        string script,
        string workDirectory)
    {
        const string moduleName = "fixture.truncatedresponse";
        byte[] main = Encoding.UTF8.GetBytes("return { value = \"truncated\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            moduleName,
            [new FeedFile(".lua", "/main.lua", Sha1(main))]));
        origin.AddTruncated("/main.lua", main, main.Length + 32);

        string root = ScenarioRoot(workDirectory, "truncated-response");
        ProcessResult result = await RunAegisubAsync(
            executable,
            script,
            root,
            origin.Url("/feed.json"),
            moduleName,
            "unused",
            null,
            new(
                ExpectedMode: "Direct",
                UpdateMode: "Direct",
                ManualProxyUri: null,
                Bypass: null,
                EnvironmentProxyUri: null,
                NetworkTestUrl: null))
            .ConfigureAwait(false);
        RequireFailedModule(result);
        if (origin.RequestCount("/feed.json") == 0 ||
            origin.RequestCount("/main.lua") == 0)
            throw new InvalidOperationException(
                "Truncated-response scenario did not reach the payload response.");
        if (File.Exists(Path.Combine(root, "include", "fixture", "truncatedresponse.lua")))
            throw new InvalidOperationException(
                "Truncated HTTP response left an installed payload behind.");
        if (File.Exists(Path.Combine(result.StateRoot, "pending-install.json")))
            throw new InvalidOperationException(
                "Truncated HTTP response left a pending install journal behind.");
        RequireCleanStaging(root);
    }

    private static async Task<ProcessResult> RunAegisubAsync(
        string executable,
        string script,
        string automationRoot,
        string feedUrl,
        string moduleName,
        string expectedValue,
        string? failCommitAfter,
        NetworkOptions? networkOptions = null,
        bool testConfiguration = false,
        string? installedStateExpectation = null,
        bool useIsolatedAutomationScript = false,
        string? catalogExpectedVersion = null,
        string? applyVersion = null,
        string? installedExpectedVersion = null,
        int? installedExpectedFileCount = null,
        bool applyExpectedFailure = false,
        bool uninstallTest = false,
        bool uninstallRemoveConfig = true,
        bool uninstallExpectedFailure = false,
        bool uninstallRecovery = false,
        bool exitAfterCommit = false,
        string? legacyConfigRoot = null)
    {
        Directory.CreateDirectory(automationRoot);
        string selectedScript = script;
        if (useIsolatedAutomationScript)
        {
            string autoloadRoot = Path.Combine(automationRoot, "autoload");
            Directory.CreateDirectory(autoloadRoot);
            selectedScript = Path.Combine(autoloadRoot, "dependency-control-install.lua");
            File.Copy(script, selectedScript, overwrite: true);
        }
        string traceDirectory = Path.Combine(automationRoot, "trace");
        string stateRoot = Path.Combine(
            Path.GetDirectoryName(automationRoot)!, "state");
        Directory.CreateDirectory(traceDirectory);
        string scenarioPath = Path.Combine(automationRoot, "automation-scenario.json");
        var scenario = new JsonObject
        {
            ["version"] = 1,
            ["name"] = "dependency-control-install",
            ["hosts"] = new JsonArray("headless"),
            ["resources"] = new JsonObject { ["script"] = "" },
            ["steps"] = new JsonArray
            {
                new JsonObject
                {
                    ["action"] = "run_automation",
                    ["script"] = "script",
                    ["macro"] = MacroName
                }
            }
        };
        File.WriteAllText(scenarioPath, scenario.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true
        }));
        ProcessStartInfo start = new(executable)
        {
            WorkingDirectory = Path.GetDirectoryName(executable)!,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };
        start.ArgumentList.Add("--headless");
        start.ArgumentList.Add("run");
        start.ArgumentList.Add("--scenario");
        start.ArgumentList.Add(scenarioPath);
        start.ArgumentList.Add("--input");
        start.ArgumentList.Add($"script={selectedScript}");
        start.ArgumentList.Add("--artifacts");
        start.ArgumentList.Add(traceDirectory);
        start.Environment["AEGISUB_DEPENDENCY_CONTROL_AUTOMATION_ROOT"] = automationRoot;
        start.Environment["AEGISUB_DEPENDENCY_CONTROL_STATE_ROOT"] = stateRoot;
        start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_FEED_URL"] = feedUrl;
        start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_MODULE"] = moduleName;
        start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_EXPECTED"] = expectedValue;
        string markerPath = Path.Combine(
            Path.GetTempPath(), $"aegisub-dc-{Guid.NewGuid():N}.marker");
        start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_MARKER"] = markerPath;
        if (failCommitAfter is null)
            start.Environment.Remove("AEGISUB_DEPENDENCY_CONTROL_TEST_FAIL_COMMIT_AFTER");
        else
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_TEST_FAIL_COMMIT_AFTER"] =
                failCommitAfter;

        foreach (string variable in new[]
        {
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_EXPECTED_PROXY_MODE",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UPDATE_PROXY_MODE",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_MANUAL_PROXY",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_PROXY_BYPASS",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_NETWORK_TEST_URL",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_CONFIG_TEST",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_STATE_TEST",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_VERSION",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_FILE_COUNT",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_CATALOG_VERSION",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_APPLY_VERSION",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_APPLY_EXPECT_FAILURE",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_TEST",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_REMOVE_CONFIG",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_EXPECT_FAILURE",
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_RECOVERY",
            "AEGISUB_DEPENDENCY_CONTROL_TEST_EXIT_AFTER_COMMIT",
            "AEGISUB_DEPENDENCY_CONTROL_LEGACY_CONFIG_ROOT",
            "HTTP_PROXY",
            "HTTPS_PROXY",
            "ALL_PROXY",
            "NO_PROXY",
            "http_proxy",
            "https_proxy",
            "all_proxy",
            "no_proxy"
        })
        {
            start.Environment.Remove(variable);
        }
        if (networkOptions is not null)
        {
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_EXPECTED_PROXY_MODE"] =
                networkOptions.ExpectedMode;
            SetOptionalEnvironment(
                start,
                "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UPDATE_PROXY_MODE",
                networkOptions.UpdateMode);
            SetOptionalEnvironment(
                start,
                "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_MANUAL_PROXY",
                networkOptions.ManualProxyUri);
            SetOptionalEnvironment(
                start,
                "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_PROXY_BYPASS",
                networkOptions.Bypass);
            SetOptionalEnvironment(
                start,
                "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_NETWORK_TEST_URL",
                networkOptions.NetworkTestUrl);
            if (networkOptions.EnvironmentProxyUri is not null)
            {
                start.Environment["HTTP_PROXY"] = networkOptions.EnvironmentProxyUri;
                start.Environment["HTTPS_PROXY"] = networkOptions.EnvironmentProxyUri;
                start.Environment["ALL_PROXY"] = networkOptions.EnvironmentProxyUri;
            }
        }
        if (testConfiguration)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_CONFIG_TEST"] = "1";
        if (legacyConfigRoot is not null)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_LEGACY_CONFIG_ROOT"] =
                legacyConfigRoot;
        SetOptionalEnvironment(
            start,
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_STATE_TEST",
            installedStateExpectation);
        SetOptionalEnvironment(
            start,
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_CATALOG_VERSION",
            catalogExpectedVersion);
        SetOptionalEnvironment(
            start,
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_APPLY_VERSION",
            applyVersion);
        SetOptionalEnvironment(
            start,
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_VERSION",
            installedExpectedVersion);
        SetOptionalEnvironment(
            start,
            "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_FILE_COUNT",
            installedExpectedFileCount?.ToString());
        if (applyExpectedFailure)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_APPLY_EXPECT_FAILURE"] = "1";
        if (uninstallTest)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_TEST"] = "1";
        if (!uninstallRemoveConfig)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_REMOVE_CONFIG"] = "0";
        if (uninstallExpectedFailure)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_EXPECT_FAILURE"] = "1";
        if (uninstallRecovery)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_RECOVERY"] = "1";
        if (exitAfterCommit)
            start.Environment["AEGISUB_DEPENDENCY_CONTROL_TEST_EXIT_AFTER_COMMIT"] = "1";

        using Process process = Process.Start(start)
            ?? throw new InvalidOperationException("Could not start the Aegisub smoke process.");
        Task<string> stdout = process.StandardOutput.ReadToEndAsync();
        Task<string> stderr = process.StandardError.ReadToEndAsync();
        using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(45));
        await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
        return new(
            process.ExitCode,
            await stdout.ConfigureAwait(false),
            await stderr.ConfigureAwait(false),
            markerPath,
            stateRoot);
    }

    private static void SetOptionalEnvironment(
        ProcessStartInfo start,
        string name,
        string? value)
    {
        if (!string.IsNullOrEmpty(value))
            start.Environment[name] = value;
    }

    private static byte[] BuildFeed(
        string baseUrl,
        string moduleName,
        IReadOnlyList<FeedFile> files,
        string version = "1.0.0")
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream))
        {
            writer.WriteStartObject();
            writer.WriteString("dependencyControlFeedFormatVersion", "0.3.0");
            writer.WriteString("name", "Aegisub loopback transaction fixture");
            writer.WritePropertyName("modules");
            writer.WriteStartObject();
            writer.WritePropertyName(moduleName);
            writer.WriteStartObject();
            writer.WriteString("name", moduleName);
            writer.WritePropertyName("channels");
            writer.WriteStartObject();
            writer.WritePropertyName("release");
            writer.WriteStartObject();
            writer.WriteString("version", version);
            writer.WriteBoolean("default", true);
            writer.WritePropertyName("files");
            writer.WriteStartArray();
            foreach (FeedFile file in files)
            {
                writer.WriteStartObject();
                writer.WriteString("name", file.Name);
                writer.WriteString("url", baseUrl + file.Route);
                writer.WriteString("sha1", file.Sha1);
                writer.WriteString("type", "script");
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
            writer.WriteEndObject();
            writer.WriteEndObject();
            writer.WriteEndObject();
            writer.WriteEndObject();
        }
        return stream.ToArray();
    }

    private static byte[] BuildKnownFeedIndex(string knownFeedUrl)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream))
        {
            writer.WriteStartObject();
            writer.WriteString("dependencyControlFeedFormatVersion", "0.3.0");
            writer.WriteString("name", "Aegisub loopback known-feed fixture");
            writer.WritePropertyName("knownFeeds");
            writer.WriteStartObject();
            writer.WriteString("catalog", knownFeedUrl);
            writer.WriteEndObject();
            writer.WriteEndObject();
        }
        return stream.ToArray();
    }

    private static string Sha1(byte[] payload) => Convert.ToHexString(SHA1.HashData(payload));

    private static string ScenarioRoot(string workDirectory, string name) =>
        Path.Combine(workDirectory, name, "automation");

    private static string StateRoot(string automationRoot) =>
        Path.Combine(Path.GetDirectoryName(automationRoot)!, "state");

    private static void RequireCleanStaging(string automationRoot)
    {
        string staging = Path.Combine(automationRoot, ".dependency-control", "staging");
        if (Directory.Exists(staging) && Directory.EnumerateFileSystemEntries(staging).Any())
            throw new InvalidOperationException(
                "DependencyControl left a transaction staging directory behind.");
    }

    private static void RequireSuccessfulModule(ProcessResult result, string expected)
    {
        if (result.ExitCode != 0)
            throw ProcessFailure("module installation scenario", result);
        string marker = File.Exists(result.MarkerPath)
            ? File.ReadAllText(result.MarkerPath)
            : "<missing>";
        if (marker != $"ok:{expected}")
            throw new InvalidOperationException(
                "The current Lua State did not load the expected installed module.\n" +
                $"marker: {marker}\nstdout:\n{result.StandardOutput}\n" +
                $"stderr:\n{result.StandardError}");
        File.Delete(result.MarkerPath);
    }

    private static void RequireFailedModule(
        ProcessResult result,
        string? expectedMessage = null)
    {
        if (result.ExitCode != 0)
            throw ProcessFailure("expected module installation failure scenario", result);
        string marker = File.Exists(result.MarkerPath)
            ? File.ReadAllText(result.MarkerPath)
            : "<missing>";
        if (!marker.StartsWith("error:", StringComparison.Ordinal) ||
            expectedMessage is not null &&
            !marker.Contains(expectedMessage, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(
                "DependencyControl did not report the expected installation failure.\n" +
                $"marker: {marker}\nstdout:\n{result.StandardOutput}\n" +
                $"stderr:\n{result.StandardError}");
        File.Delete(result.MarkerPath);
    }

    private static void RequireUninstallRecovery(ProcessResult result)
    {
        if (result.ExitCode != 0)
            throw ProcessFailure("uninstall recovery scenario", result);
        string marker = File.Exists(result.MarkerPath)
            ? File.ReadAllText(result.MarkerPath)
            : "<missing>";
        if (marker != "ok:uninstall-recovered")
            throw new InvalidOperationException(
                "DependencyControl uninstall recovery did not complete.\n" +
                $"marker: {marker}\nstdout:\n{result.StandardOutput}\n" +
                $"stderr:\n{result.StandardError}");
        File.Delete(result.MarkerPath);
    }

    private static void RequirePersistedMode(string stateRoot, string expectedMode)
    {
        string path = Path.Combine(stateRoot, "network.json");
        if (!File.Exists(path))
            throw new InvalidOperationException(
                "DependencyControl did not persist its network settings.");
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        JsonElement root = document.RootElement;
        if (!root.TryGetProperty("mode", out JsonElement mode) ||
            mode.GetString() != expectedMode)
            throw new InvalidOperationException(
                "DependencyControl persisted an unexpected proxy mode.");
        if (expectedMode == "Manual" &&
            root.TryGetProperty("manualProxyUri", out JsonElement proxyValue) &&
            Uri.TryCreate(proxyValue.GetString(), UriKind.Absolute, out Uri? proxyUri) &&
            proxyUri.UserInfo.Length > 0)
            throw new InvalidOperationException(
                "DependencyControl persisted credentials in its manual proxy URI.");
    }

    private static long RequireInstalledState(
        string stateRoot,
        string moduleName,
        string feedUrl,
        int expectedCorruptBackups = 1,
        string expectedVersion = "1.0.0",
        IReadOnlyList<string>? expectedTargets = null)
    {
        string path = Path.Combine(stateRoot, "installed.json");
        if (!File.Exists(path))
            throw new InvalidOperationException(
                "DependencyControl did not persist installed package state.");
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        JsonElement root = document.RootElement;
        if (root.GetProperty("schemaVersion").GetInt32() != 1 ||
            root.GetProperty("revision").GetInt64() < 2)
            throw new InvalidOperationException(
                "DependencyControl persisted an invalid installed-state header.");
        JsonElement? installed = null;
        foreach (JsonElement package in root.GetProperty("packages").EnumerateArray())
        {
            if (package.GetProperty("recordType").GetString() == "module" &&
                package.GetProperty("namespace").GetString() == moduleName)
            {
                installed = package;
                break;
            }
        }
        if (installed is null ||
            installed.Value.GetProperty("version").GetString() != expectedVersion ||
            installed.Value.GetProperty("channel").GetString() != "release" ||
            installed.Value.GetProperty("feed").GetString() != feedUrl ||
            installed.Value.GetProperty("source").GetString() != "transaction")
            throw new InvalidOperationException(
                "DependencyControl persisted an unexpected installed package record.");
        JsonElement files = installed.Value.GetProperty("files");
        expectedTargets ??= ["include/" + moduleName.Replace('.', '/') + ".lua"];
        string[] actualTargets = files.EnumerateArray()
            .Select(file => file.GetProperty("target").GetString()!)
            .Order(StringComparer.Ordinal)
            .ToArray();
        if (!actualTargets.SequenceEqual(expectedTargets.Order(StringComparer.Ordinal)))
            throw new InvalidOperationException(
                "DependencyControl did not persist installed file ownership.");
        if (Directory.EnumerateFiles(stateRoot, "installed-*.tmp").Any())
            throw new InvalidOperationException(
                "DependencyControl left an installed-state temporary file behind.");
        if (Directory.EnumerateFiles(
                stateRoot, "installed.json.corrupted-*").Count() != expectedCorruptBackups)
            throw new InvalidOperationException(
                "DependencyControl retained an unexpected corrupted state backup count.");
        return root.GetProperty("revision").GetInt64();
    }

    private static void AddInstalledOwner(
        string stateRoot,
        string packageNamespace,
        string feedUrl,
        string target,
        string sha1)
    {
        string path = Path.Combine(stateRoot, "installed.json");
        JsonObject root = JsonNode.Parse(File.ReadAllText(path, Encoding.UTF8))?.AsObject()
            ?? throw new InvalidOperationException(
                "DependencyControl installed-state fixture is not an object.");
        JsonArray packages = root["packages"]?.AsArray()
            ?? throw new InvalidOperationException(
                "DependencyControl installed-state fixture has no package array.");
        long revision = root["revision"]?.GetValue<long>()
            ?? throw new InvalidOperationException(
                "DependencyControl installed-state fixture has no revision.");
        root["revision"] = checked(revision + 1);
        packages.Add(new JsonObject
        {
            ["recordType"] = "module",
            ["namespace"] = packageNamespace,
            ["name"] = packageNamespace,
            ["version"] = "1.0.0",
            ["description"] = "",
            ["author"] = "",
            ["feed"] = feedUrl,
            ["channel"] = "release",
            ["configFile"] = $"{packageNamespace}.json",
            ["source"] = "transaction",
            ["requiredModules"] = new JsonArray(),
            ["files"] = new JsonArray(new JsonObject
            {
                ["target"] = target,
                ["sha1"] = sha1
            })
        });
        File.WriteAllText(
            path,
            root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }),
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    private static void RequirePackageOwnsTarget(
        string stateRoot,
        string packageNamespace,
        string expectedTarget)
    {
        string path = Path.Combine(stateRoot, "installed.json");
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        foreach (JsonElement package in
                 document.RootElement.GetProperty("packages").EnumerateArray())
        {
            if (package.GetProperty("recordType").GetString() != "module" ||
                package.GetProperty("namespace").GetString() != packageNamespace)
                continue;
            if (package.GetProperty("files").EnumerateArray().Any(
                    file => file.GetProperty("target").GetString() == expectedTarget))
                return;
            break;
        }
        throw new InvalidOperationException(
            "DependencyControl did not preserve the expected package owner.");
    }

    private static void RequirePackageAbsent(
        string stateRoot,
        string packageNamespace)
    {
        string path = Path.Combine(stateRoot, "installed.json");
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        bool exists = document.RootElement.GetProperty("packages").EnumerateArray().Any(
            package => package.GetProperty("recordType").GetString() == "module" &&
                package.GetProperty("namespace").GetString() == packageNamespace);
        if (exists)
            throw new InvalidOperationException(
                "DependencyControl retained an uninstalled package record.");
    }

    private static void AddNetworkRoutes(
        LoopbackOrigin origin,
        byte[] feed,
        byte[] main)
    {
        origin.Add("/feed.json", feed);
        origin.Add("/main.lua", main);
        origin.Add("/probe", Encoding.UTF8.GetBytes("ok\n"));
    }

    private static async Task RunManagedToolViewSmokeAsync(string workDirectory)
    {
        const string packageNamespace = "fixture.ui";
        const string availableNamespace = "fixture.available";
        byte[] availablePayload = Encoding.UTF8.GetBytes(
            "return { value = \"available\" }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/available.lua", availablePayload);
        origin.AddDelayed(
            "/slow", Encoding.UTF8.GetBytes("slow\n"), TimeSpan.FromSeconds(10));
        origin.Add("/feed.json", BuildFeed(
            origin.BaseUrl,
            availableNamespace,
            [new FeedFile(".lua", "/available.lua", Sha1(availablePayload))]));
        string root = Path.Combine(workDirectory, "managed-tool-view");
        string stateRoot = Path.Combine(root, "state");
        string automationRoot = Path.Combine(root, "automation");
        Directory.CreateDirectory(stateRoot);
        Directory.CreateDirectory(automationRoot);
        JsonObject state = new()
        {
            ["schemaVersion"] = 1,
            ["revision"] = 1,
            ["packages"] = new JsonArray(new JsonObject
            {
                ["recordType"] = "module",
                ["namespace"] = packageNamespace,
                ["name"] = "ToolView fixture",
                ["version"] = "1.0.0",
                ["description"] = "",
                ["author"] = "",
                ["feed"] = "",
                ["channel"] = "release",
                ["configFile"] = packageNamespace + ".json",
                ["source"] = "transaction",
                ["requiredModules"] = new JsonArray(),
                ["files"] = new JsonArray()
            })
        };
        File.WriteAllText(
            Path.Combine(stateRoot, "installed.json"),
            state.ToJsonString(),
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

        ManagedToolViewHost host = new(
            stateRoot, automationRoot, origin.Url("/feed.json"));
        DependencyControlPlugin plugin = new();
        await plugin.ActivateAsync(host, CancellationToken.None).ConfigureAwait(false);
        try
        {
            IAegisubServiceProviderContribution service = plugin.Contributions
                .OfType<IAegisubServiceProviderContribution>()
                .Single();
            await service.InvokeAsync(
                "logs.append",
                "{\"level\":3,\"source\":\"ToolView smoke\",\"message\":\"Log row\"}",
                CancellationToken.None).ConfigureAwait(false);
            IAegisubAutomationContribution automation = plugin.Contributions
                .OfType<IAegisubAutomationContribution>()
                .Single(contribution => contribution.Metadata.Id ==
                    "aegisub.dependency-control.automation");
            IAegisubMacro macro = automation.Macros.Single(item => item.Metadata.Id ==
                "aegisub.dependency-control.package-manager");
            MacroResult result = await macro.ExecuteAsync(
                new ManagedToolViewMacroContext(), CancellationToken.None)
                .ConfigureAwait(false);
            if (!result.StatusMessage.StartsWith("Opened ", StringComparison.Ordinal) ||
                host.OpenedView is null ||
                host.OpenedView.Tabs?.SelectMany(tab => tab.Tables)
                    .Single(table => table.Id == "packages")
                    .Rows.Single().Id != "module:" + packageNamespace)
                throw new InvalidOperationException(
                    "DependencyControl managed ToolView did not open with installed packages.");
            IReadOnlyList<ToolViewTabDefinition> tabs = host.OpenedView.Tabs ??
                throw new InvalidOperationException(
                    "DependencyControl managed ToolView did not expose tabs.");
            if (tabs.Count != 5 || tabs[0].Id != "installed" ||
                tabs[1].Id != "available" || tabs[2].Id != "feeds" ||
                tabs[3].Id != "logs" || tabs[4].Id != "network" ||
                tabs[3].Tables.Single(table => table.Id == "log-list").Rows.Count != 1)
                throw new InvalidOperationException(
                    "DependencyControl managed ToolView did not expose its package-manager tabs.");
            UiActionDefinition[] allActions =
            [
                .. host.OpenedView.Actions,
                .. tabs.SelectMany(tab => tab.Actions)
            ];
            if (allActions.Count(action => action.IsDefault) > 1 ||
                allActions.Count(action => action.IsCancel) > 1)
                throw new InvalidOperationException(
                    "DependencyControl ToolView exceeds the native default/cancel action limit.");

            IAegisubPluginEventHandler eventHandler = plugin;
            int operationPatchStart = host.Patches.Count;
            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "change",
                    sourceId: "search",
                    values: ToolViewValues(search: "missing"),
                    selectedRows: [],
                    activeTabId: "installed"),
                CancellationToken.None).ConfigureAwait(false);
            if (host.Patches.Last().Tables.Single().Rows.Count != 0)
                throw new InvalidOperationException(
                    "DependencyControl ToolView search did not filter package rows.");

            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "logs-clear",
                    values: ToolViewValues(search: "fixture"),
                    selectedRows: [],
                    activeTabId: "logs"),
                CancellationToken.None).ConfigureAwait(false);
            if (host.Patches.Last().Tables.Single(table => table.Id == "log-list").Rows.Count != 0)
                throw new InvalidOperationException(
                    "DependencyControl ToolView did not clear persisted logs.");

            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "save-network",
                    values: ToolViewValues(search: "fixture", proxyMode: "Direct"),
                    selectedRows: ["module:" + packageNamespace],
                    activeTabId: "network"),
                CancellationToken.None).ConfigureAwait(false);
            using (JsonDocument network = JsonDocument.Parse(File.ReadAllBytes(
                       Path.Combine(stateRoot, "network.json"))))
            {
                if (network.RootElement.GetProperty("mode").GetString() != "Direct")
                    throw new InvalidOperationException(
                        "DependencyControl ToolView did not persist proxy settings.");
            }

            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "feed-add",
                    values: ToolViewValues(search: "fixture"),
                    selectedRows: [],
                    activeTabId: "feeds"),
                CancellationToken.None).ConfigureAwait(false);
            using JsonDocument feedState = JsonDocument.Parse(File.ReadAllBytes(
                Path.Combine(stateRoot, "feeds.json")));
            JsonElement configuredFeed = feedState.RootElement.GetProperty("feeds")[0];
            string feedId = configuredFeed.GetProperty("id").GetString()!;
            string feedRowId = "feed:" + feedId;
            if (configuredFeed.GetProperty("url").GetString() != origin.Url("/feed.json") ||
                host.Patches.Last().Tables.Single(table => table.Id == "feed-list").Rows.Count != 1)
                throw new InvalidOperationException(
                    "DependencyControl ToolView did not persist and render an added feed.");
            using (JsonDocument inspected = JsonDocument.Parse(await service.InvokeAsync(
                       "feeds.inspect",
                       JsonSerializer.Serialize(new Dictionary<string, string>
                       {
                           ["url"] = origin.Url("/feed.json")
                       }),
                       CancellationToken.None).ConfigureAwait(false)))
            {
                if (inspected.RootElement.GetProperty("moduleCount").GetInt32() != 1)
                    throw new InvalidOperationException(
                        "DependencyControl feed inspection did not expose package metadata.");
            }

            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "feed-edit",
                    values: ToolViewValues(search: "fixture"),
                    selectedRows: [feedRowId],
                    activeTabId: "feeds"),
                CancellationToken.None).ConfigureAwait(false);
            using (JsonDocument editedFeedState = JsonDocument.Parse(File.ReadAllBytes(
                       Path.Combine(stateRoot, "feeds.json"))))
            {
                if (editedFeedState.RootElement.GetProperty("feeds")[0]
                        .GetProperty("label").GetString() != "Fixture feed edited")
                    throw new InvalidOperationException(
                        "DependencyControl ToolView did not edit its configured feed.");
            }

            operationPatchStart = host.Patches.Count;
            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "feed-test",
                    values: ToolViewValues(search: "fixture"),
                    selectedRows: [feedRowId],
                    activeTabId: "feeds"),
                CancellationToken.None).ConfigureAwait(false);
            await WaitForToolViewOperationAsync(
                host, operationPatchStart, "feed test").ConfigureAwait(false);
            if (!host.Patches.Skip(operationPatchStart).Any(patch =>
                    patch.Controls.Any(control =>
                    control.Id == "status" && control.Text?.Contains(
                        "1 modules", StringComparison.Ordinal) == true)))
                throw new InvalidOperationException(
                    "DependencyControl ToolView did not test the configured feed.");

            operationPatchStart = host.Patches.Count;
            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "available-refresh",
                    values: ToolViewValues(search: "fixture"),
                    selectedRows: [],
                    activeTabId: "available"),
                CancellationToken.None).ConfigureAwait(false);
            await WaitForToolViewOperationAsync(
                host, operationPatchStart, "available refresh").ConfigureAwait(false);
            string availableRowId = "available:module:" + availableNamespace;
            if (host.Patches.Skip(operationPatchStart)
                    .Last(patch => patch.Tables.Any(
                        table => table.Id == "available-packages")).Tables
                    .Single(table => table.Id == "available-packages")
                    .Rows.Single().Id != availableRowId)
                throw new InvalidOperationException(
                    "DependencyControl ToolView did not browse the available catalog.");

            operationPatchStart = host.Patches.Count;
            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "available-install",
                    values: ToolViewValues(search: "fixture"),
                    selectedRows: [availableRowId],
                    activeTabId: "available"),
                CancellationToken.None).ConfigureAwait(false);
            await WaitForToolViewOperationAsync(
                host, operationPatchStart, "available install").ConfigureAwait(false);
            string installedTarget = Path.Combine(
                automationRoot, "include", "fixture", "available.lua");
            if (!File.Exists(installedTarget) ||
                host.Patches.Skip(operationPatchStart)
                    .Last(patch => patch.Tables.Any(
                        table => table.Id == "available-packages")).Tables
                    .Single(table => table.Id == "available-packages")
                    .Rows.Single().Cells.Single(cell => cell.Id == "status")
                    .JsonValue != "\"Installed 1.0.0\"")
                throw new InvalidOperationException(
                    "DependencyControl ToolView did not install the available package.");

            byte[] updatedAvailablePayload = Encoding.UTF8.GetBytes(
                "return { value = \"available-2\" }\n");
            origin.Set("/available.lua", updatedAvailablePayload);
            origin.Set("/feed.json", BuildFeed(
                origin.BaseUrl,
                availableNamespace,
                [new FeedFile(
                    ".lua", "/available.lua", Sha1(updatedAvailablePayload))],
                version: "2.0.0"));
            operationPatchStart = host.Patches.Count;
            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "apply-all",
                    values: ToolViewValues(search: "fixture", proxyMode: "Direct"),
                    selectedRows: [],
                    activeTabId: "installed"),
                CancellationToken.None).ConfigureAwait(false);
            await WaitForToolViewOperationAsync(
                host, operationPatchStart, "atomic Update All").ConfigureAwait(false);
            if (!File.ReadAllBytes(installedTarget).SequenceEqual(updatedAvailablePayload) ||
                !host.Patches.Skip(operationPatchStart).Any(patch =>
                    patch.Controls.Any(control => control.Id == "status" &&
                        control.Text?.StartsWith(
                            "Updated all packages in one transaction",
                            StringComparison.Ordinal) == true)))
                throw new InvalidOperationException(
                    "DependencyControl ToolView Update All was not globally atomic.");
            using (JsonDocument updatedInstalled = JsonDocument.Parse(File.ReadAllBytes(
                       Path.Combine(stateRoot, "installed.json"))))
            {
                JsonElement updatedPackage = updatedInstalled.RootElement
                    .GetProperty("packages").EnumerateArray().Single(package =>
                        package.GetProperty("namespace").GetString() == availableNamespace);
                if (updatedPackage.GetProperty("version").GetString() != "2.0.0")
                    throw new InvalidOperationException(
                        "DependencyControl ToolView Update All did not persist its package version.");
            }

            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "feed-remove",
                    values: ToolViewValues(search: "fixture"),
                    selectedRows: [feedRowId],
                    activeTabId: "feeds"),
                CancellationToken.None).ConfigureAwait(false);
            if (host.Patches.Last().Tables
                    .Single(table => table.Id == "feed-list").Rows.Count != 0)
                throw new InvalidOperationException(
                    "DependencyControl ToolView did not remove the configured feed.");

            operationPatchStart = host.Patches.Count;
            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "test-network",
                    values: ToolViewValues(
                        search: "fixture",
                        proxyMode: "Direct",
                        testUrl: origin.Url("/slow")),
                    selectedRows: [],
                    activeTabId: "network"),
                CancellationToken.None).ConfigureAwait(false);
            using (CancellationTokenSource requestTimeout = new(TimeSpan.FromSeconds(5)))
            {
                while (origin.RequestCount("/slow") == 0)
                    await Task.Delay(10, requestTimeout.Token).ConfigureAwait(false);
            }
            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "cancel-operation",
                    values: ToolViewValues(
                        search: "fixture",
                        proxyMode: "Direct",
                        testUrl: origin.Url("/slow")),
                    selectedRows: [],
                    activeTabId: "network"),
                CancellationToken.None).ConfigureAwait(false);
            await WaitForToolViewOperationAsync(
                host, operationPatchStart, "network cancellation").ConfigureAwait(false);
            if (!host.Patches.Any(patch => patch.Controls.Any(control =>
                    control.Id == "status" && control.Text?.StartsWith(
                        "Operation cancelled.", StringComparison.Ordinal) == true)))
                throw new InvalidOperationException(
                    "DependencyControl ToolView did not cancel its active network operation.");

            await eventHandler.HandleEventAsync(
                ToolViewPluginEvent(
                    eventId: "action",
                    sourceId: "uninstall",
                    values: ToolViewValues(search: "fixture", proxyMode: "Direct"),
                    selectedRows: ["module:" + packageNamespace],
                    activeTabId: "installed"),
                CancellationToken.None).ConfigureAwait(false);
            if (host.LastForm?.OwnerViewId !=
                    "aegisub.dependency-control.package-manager" ||
                host.Patches.Last().Tables.Single(table => table.Id == "packages")
                    .Rows.Any(row => row.Id == "module:" + packageNamespace))
                throw new InvalidOperationException(
                    "DependencyControl ToolView uninstall did not use its owner form or refresh rows.");
            using JsonDocument installed = JsonDocument.Parse(File.ReadAllBytes(
                Path.Combine(stateRoot, "installed.json")));
            if (installed.RootElement.GetProperty("packages").GetArrayLength() != 1 ||
                installed.RootElement.GetProperty("packages")[0]
                    .GetProperty("namespace").GetString() != availableNamespace)
                throw new InvalidOperationException(
                    "DependencyControl ToolView uninstall changed the wrong installed package.");
        }
        finally
        {
            await plugin.DeactivateAsync(CancellationToken.None).ConfigureAwait(false);
        }
    }

    private static async Task RunBatchUpdateAtomicitySmokeAsync(string workDirectory)
    {
        const string firstNamespace = "fixture.batch.first";
        const string secondNamespace = "fixture.batch.second";
        byte[] firstOriginal = Encoding.UTF8.GetBytes("return { value = 'first-1' }\n");
        byte[] secondOriginal = Encoding.UTF8.GetBytes("return { value = 'second-1' }\n");
        byte[] firstUpdated = Encoding.UTF8.GetBytes("return { value = 'first-2' }\n");
        byte[] secondUpdated = Encoding.UTF8.GetBytes("return { value = 'second-2' }\n");
        await using LoopbackOrigin origin = new();
        origin.Add("/first.lua", firstUpdated);
        origin.Add("/second.lua", secondUpdated);
        origin.Add("/first-feed.json", BuildFeed(
            origin.BaseUrl,
            firstNamespace,
            [new FeedFile(".lua", "/first.lua", Sha1(firstUpdated))],
            version: "2.0.0"));
        origin.Add("/second-feed.json", BuildFeed(
            origin.BaseUrl,
            secondNamespace,
            [new FeedFile(".lua", "/second.lua", new string('0', 40))],
            version: "2.0.0"));

        string root = Path.Combine(workDirectory, "batch-update-atomicity");
        string stateRoot = Path.Combine(root, "state");
        string automationRoot = Path.Combine(root, "automation");
        string firstTarget = Path.Combine(automationRoot, "include", "fixture", "batch", "first.lua");
        string secondTarget = Path.Combine(automationRoot, "include", "fixture", "batch", "second.lua");
        Directory.CreateDirectory(Path.GetDirectoryName(firstTarget)!);
        File.WriteAllBytes(firstTarget, firstOriginal);
        File.WriteAllBytes(secondTarget, secondOriginal);
        Directory.CreateDirectory(stateRoot);
        File.WriteAllText(
            Path.Combine(stateRoot, "installed.json"),
            new JsonObject
            {
                ["schemaVersion"] = 1,
                ["revision"] = 7,
                ["packages"] = new JsonArray(
                    BatchInstalledPackage(
                        firstNamespace,
                        origin.Url("/first-feed.json"),
                        "include/fixture/batch/first.lua",
                        Sha1(firstOriginal)),
                    BatchInstalledPackage(
                        secondNamespace,
                        origin.Url("/second-feed.json"),
                        "include/fixture/batch/second.lua",
                        Sha1(secondOriginal)))
            }.ToJsonString());

        ManagedToolViewHost host = new(stateRoot, automationRoot, origin.Url("/first-feed.json"));
        DependencyControlPlugin plugin = new();
        await plugin.ActivateAsync(host, CancellationToken.None).ConfigureAwait(false);
        try
        {
            IAegisubServiceProviderContribution service = plugin.Contributions
                .OfType<IAegisubServiceProviderContribution>().Single();
            string request = JsonSerializer.Serialize(new Dictionary<string, object>
            {
                ["packages"] = new object[]
                {
                    new Dictionary<string, string>
                    {
                        ["recordType"] = "module",
                        ["namespace"] = firstNamespace
                    },
                    new Dictionary<string, string>
                    {
                        ["recordType"] = "module",
                        ["namespace"] = secondNamespace
                    }
                }
            });
            bool failed = false;
            try
            {
                await service.InvokeAsync("updates.apply", request, CancellationToken.None)
                    .ConfigureAwait(false);
            }
            catch (InvalidOperationException)
            {
                failed = true;
            }
            if (!failed)
                throw new InvalidOperationException(
                    "DependencyControl batch update accepted the bad second package.");
            if (host.BeginTransactionCount != 1 || host.CommitTransactionCount != 0)
                throw new InvalidOperationException(
                    "DependencyControl failed batch update crossed its transaction boundary.");
            if (!File.ReadAllBytes(firstTarget).SequenceEqual(firstOriginal) ||
                !File.ReadAllBytes(secondTarget).SequenceEqual(secondOriginal))
                throw new InvalidOperationException(
                    "DependencyControl batch update leaked a partial package commit.");
            using JsonDocument state = JsonDocument.Parse(File.ReadAllBytes(
                Path.Combine(stateRoot, "installed.json")));
            if (state.RootElement.GetProperty("revision").GetInt64() != 7 ||
                state.RootElement.GetProperty("packages").GetArrayLength() != 2)
                throw new InvalidOperationException(
                    "DependencyControl failed batch update changed installed state.");
            string fakeStaging = Path.Combine(stateRoot, "staging");
            string nativeStaging = Path.Combine(
                automationRoot, ".dependency-control", "staging");
            if (File.Exists(Path.Combine(stateRoot, "pending-install.json")) ||
                Directory.Exists(fakeStaging) &&
                Directory.EnumerateFileSystemEntries(fakeStaging).Any() ||
                Directory.Exists(nativeStaging) &&
                Directory.EnumerateFileSystemEntries(nativeStaging).Any())
                throw new InvalidOperationException(
                    "DependencyControl failed batch update left transaction state behind.");

            origin.Set("/second-feed.json", BuildFeed(
                origin.BaseUrl,
                secondNamespace,
                [new FeedFile(".lua", "/second.lua", Sha1(secondUpdated))],
                version: "2.0.0"));
            using JsonDocument response = JsonDocument.Parse(await service.InvokeAsync(
                "updates.apply", request, CancellationToken.None).ConfigureAwait(false));
            if (!response.RootElement.GetProperty("atomic").GetBoolean() ||
                !response.RootElement.GetProperty("committed").GetBoolean() ||
                response.RootElement.GetProperty("updatedCount").GetInt32() != 2 ||
                response.RootElement.GetProperty("packages").GetArrayLength() != 2 ||
                host.BeginTransactionCount != 2 || host.CommitTransactionCount != 1)
                throw new InvalidOperationException(
                    "DependencyControl successful batch update did not use one global transaction.");
            if (!File.ReadAllBytes(firstTarget).SequenceEqual(firstUpdated) ||
                !File.ReadAllBytes(secondTarget).SequenceEqual(secondUpdated))
                throw new InvalidOperationException(
                    "DependencyControl successful batch update did not commit both packages.");
            using JsonDocument updatedState = JsonDocument.Parse(File.ReadAllBytes(
                Path.Combine(stateRoot, "installed.json")));
            if (updatedState.RootElement.GetProperty("revision").GetInt64() != 8 ||
                updatedState.RootElement.GetProperty("packages")
                    .EnumerateArray().Any(package =>
                        package.GetProperty("version").GetString() != "2.0.0"))
                throw new InvalidOperationException(
                    "DependencyControl successful batch update did not persist atomically.");

            string singleBatchRequest = JsonSerializer.Serialize(
                new Dictionary<string, object>
                {
                    ["packages"] = new object[]
                    {
                        new Dictionary<string, string>
                        {
                            ["recordType"] = "module",
                            ["namespace"] = firstNamespace
                        }
                    }
                });
            using JsonDocument singleBatch = JsonDocument.Parse(await service.InvokeAsync(
                "updates.apply", singleBatchRequest, CancellationToken.None)
                .ConfigureAwait(false));
            if (!singleBatch.RootElement.GetProperty("atomic").GetBoolean() ||
                singleBatch.RootElement.GetProperty("packages").GetArrayLength() != 1 ||
                singleBatch.RootElement.GetProperty("committed").GetBoolean())
                throw new InvalidOperationException(
                    "DependencyControl one-item batch lost the batch response shape.");
        }
        finally
        {
            await plugin.DeactivateAsync(CancellationToken.None).ConfigureAwait(false);
        }
    }

    private static JsonObject BatchInstalledPackage(
        string packageNamespace,
        string feed,
        string target,
        string sha1) => new()
        {
            ["recordType"] = "module",
            ["namespace"] = packageNamespace,
            ["name"] = packageNamespace,
            ["version"] = "1.0.0",
            ["description"] = "",
            ["author"] = "",
            ["feed"] = feed,
            ["channel"] = "release",
            ["configFile"] = packageNamespace + ".json",
            ["source"] = "transaction",
            ["requiredModules"] = new JsonArray(),
            ["files"] = new JsonArray(new JsonObject
            {
                ["target"] = target,
                ["sha1"] = sha1
            })
        };

    private static PluginEvent ToolViewPluginEvent(
        string eventId,
        string sourceId,
        IReadOnlyList<UiValue> values,
        IReadOnlyList<string> selectedRows,
        string activeTabId = "installed")
    {
        ToolViewEvent toolEvent = new(
            "aegisub.dependency-control.package-manager",
            eventId,
            sourceId,
            values,
            selectedRows,
            Revision: 0,
            ActiveTabId: activeTabId);
        return new PluginEvent(
            "aegisub.ui.toolViewEvent",
            JsonSerializer.Serialize(
                toolEvent,
                DeclarativeUiJsonContext.Default.ToolViewEvent));
    }

    private static IReadOnlyList<UiValue> ToolViewValues(
        string search,
        string proxyMode = "System",
        string testUrl = "") =>
        [
            JsonValue("search", search),
            JsonValue("available-search", ""),
            JsonValue("scope", "all"),
            JsonValue("proxy-mode", proxyMode),
            JsonValue("proxy-uri", ""),
            JsonValue("proxy-bypass", ""),
            JsonValue("proxy-default-credentials", false),
            JsonValue("test-url", testUrl),
            JsonValue("status", "")
        ];

    private static async Task WaitForToolViewOperationAsync(
        ManagedToolViewHost host,
        int initialPatchCount,
        string description)
    {
        using CancellationTokenSource timeout = new(TimeSpan.FromSeconds(10));
        while (true)
        {
            IReadOnlyList<ToolViewPatch> patches = host.Patches;
            if (patches.Skip(initialPatchCount).Any(patch => patch.Controls.Any(control =>
                    control.Id == "operation-progress" && control.Visible == false)))
            {
                if (!patches.Skip(initialPatchCount).Any(patch => patch.Controls.Any(control =>
                        control.Id == "operation-progress" && control.Visible == true)))
                    throw new InvalidOperationException(
                        $"DependencyControl ToolView {description} did not show progress.");
                await Task.Delay(20, timeout.Token).ConfigureAwait(false);
                return;
            }
            try
            {
                await Task.Delay(10, timeout.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                throw new InvalidOperationException(
                    $"DependencyControl ToolView {description} did not finish.");
            }
        }
    }

    private static UiValue JsonValue<T>(string id, T value) =>
        new(id, JsonSerializer.Serialize(value));

    private static void RecreateDirectory(string path)
    {
        if (Directory.Exists(path))
            Directory.Delete(path, recursive: true);
        Directory.CreateDirectory(path);
    }

    private static Exception ProcessFailure(string scenario, ProcessResult result) =>
        new InvalidOperationException(
            $"The {scenario} failed with exit code {result.ExitCode}.\n" +
            $"stdout:\n{result.StandardOutput}\nstderr:\n{result.StandardError}");

    private sealed record FeedFile(string Name, string Route, string Sha1);
    private sealed record NetworkOptions(
        string ExpectedMode,
        string? UpdateMode,
        string? ManualProxyUri,
        string? Bypass,
        string? EnvironmentProxyUri,
        string? NetworkTestUrl);
    private sealed record ProcessResult(
        int ExitCode,
        string StandardOutput,
        string StandardError,
        string MarkerPath,
        string StateRoot);
}

internal sealed class ManagedToolViewHost(
    string stateRoot,
    string automationRoot,
    string fixtureFeedUrl) : IAegisubPluginContext
{
    private readonly object _patchGate = new();
    private readonly List<ToolViewPatch> _patches = [];
    private string _transactionId = "";
    private string _stagingRoot = "";

    public ToolViewDefinition? OpenedView { get; private set; }
    public OpenFormRequest? LastForm { get; private set; }
    public int BeginTransactionCount { get; private set; }
    public int CommitTransactionCount { get; private set; }
    public IReadOnlyList<ToolViewPatch> Patches
    {
        get
        {
            lock (_patchGate)
                return _patches.ToArray();
        }
    }

    public void Log(string message)
    {
    }

    public ValueTask<string> InvokeHostServiceAsync(
        string serviceId,
        string requestJson,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(serviceId switch
        {
            "aegisub.dependency-control.get-state-root" => JsonSerializer.Serialize(
                new Dictionary<string, string> { ["stateRoot"] = stateRoot }),
            "aegisub.dependency-control.reconcile-transactions" => JsonSerializer.Serialize(
                new Dictionary<string, object>
                {
                    ["automationRoot"] = automationRoot,
                    ["recoveredTransactions"] = 0
                }),
            "aegisub.ui.openToolView" => OpenToolView(requestJson),
            "aegisub.ui.patchToolView" => PatchToolView(requestJson),
            "aegisub.ui.openForm" => OpenForm(requestJson),
            "aegisub.dependency-control.begin-transaction" => BeginTransaction(),
            "aegisub.dependency-control.commit-transaction" =>
                CommitTransaction(requestJson),
            "aegisub.dependency-control.abort-transaction" => AbortTransaction(),
            _ => throw new InvalidOperationException(
                $"Unexpected managed ToolView host service '{serviceId}'.")
        });
    }

    public void EmitEvent(string eventId, string payloadJson)
    {
    }

    private string OpenToolView(string requestJson)
    {
        OpenToolViewRequest request = JsonSerializer.Deserialize(
            requestJson,
            DeclarativeUiJsonContext.Default.OpenToolViewRequest)
            ?? throw new InvalidOperationException("Managed ToolView request was empty.");
        OpenedView = request.Definition;
        return JsonSerializer.Serialize(
            new ToolViewOperationResult(request.Definition.Id, true, "opened"),
            DeclarativeUiJsonContext.Default.ToolViewOperationResult);
    }

    private string PatchToolView(string requestJson)
    {
        PatchToolViewRequest request = JsonSerializer.Deserialize(
            requestJson,
            DeclarativeUiJsonContext.Default.PatchToolViewRequest)
            ?? throw new InvalidOperationException("Managed ToolView patch was empty.");
        lock (_patchGate)
            _patches.Add(request.Patch);
        return JsonSerializer.Serialize(
            new ToolViewOperationResult(request.Patch.ViewId, true),
            DeclarativeUiJsonContext.Default.ToolViewOperationResult);
    }

    private string OpenForm(string requestJson)
    {
        LastForm = JsonSerializer.Deserialize(
            requestJson,
            DeclarativeUiJsonContext.Default.OpenFormRequest)
            ?? throw new InvalidOperationException("Managed ToolView form was empty.");
        FormResult result = LastForm.Definition.Id switch
        {
            string id when id.EndsWith(".feed", StringComparison.Ordinal) => new(
                id,
                "save",
                [
                    new UiValue(
                        "feed-label",
                        JsonSerializer.Serialize(
                            LastForm.Definition.Title == "Edit feed"
                                ? "Fixture feed edited"
                                : "Fixture feed")),
                    new UiValue("feed-url", JsonSerializer.Serialize(fixtureFeedUrl)),
                    new UiValue("feed-enabled", "true")
                ],
                Cancelled: false),
            string id when id.EndsWith(".install", StringComparison.Ordinal) => new(
                id,
                "install",
                [new UiValue("install-channel", "\"release\"")],
                Cancelled: false),
            string id when id.EndsWith(".remove-feed", StringComparison.Ordinal) => new(
                id,
                "remove",
                [],
                Cancelled: false),
            _ => new(
                LastForm.Definition.Id,
                "uninstall",
                [new UiValue("remove-config", "true")],
                Cancelled: false)
        };
        return JsonSerializer.Serialize(
            result,
            DeclarativeUiJsonContext.Default.FormResult);
    }

    private string BeginTransaction()
    {
        if (_transactionId.Length > 0)
            throw new InvalidOperationException(
                "Managed ToolView host already has a transaction.");
        _transactionId = Guid.NewGuid().ToString("N");
        ++BeginTransactionCount;
        _stagingRoot = Path.Combine(stateRoot, "staging", _transactionId);
        Directory.CreateDirectory(_stagingRoot);
        return JsonSerializer.Serialize(new Dictionary<string, object>
        {
            ["transactionId"] = _transactionId,
            ["stagingRoot"] = _stagingRoot,
            ["automationRoot"] = automationRoot
        });
    }

    private string CommitTransaction(string requestJson)
    {
        using JsonDocument document = JsonDocument.Parse(requestJson);
        JsonElement root = document.RootElement;
        if (root.GetProperty("transactionId").GetString() != _transactionId)
            throw new InvalidOperationException(
                "Managed ToolView host received the wrong transaction ID.");
        ++CommitTransactionCount;
        foreach (JsonElement file in root.GetProperty("files").EnumerateArray())
        {
            string target = file.GetProperty("target").GetString()!;
            string destination = Path.GetFullPath(Path.Combine(
                automationRoot,
                target.Replace('/', Path.DirectorySeparatorChar)));
            string rootPrefix = Path.GetFullPath(automationRoot) + Path.DirectorySeparatorChar;
            if (!destination.StartsWith(
                    rootPrefix,
                    OperatingSystem.IsWindows()
                        ? StringComparison.OrdinalIgnoreCase
                        : StringComparison.Ordinal))
                throw new InvalidOperationException(
                    "Managed ToolView host transaction escaped the Automation root.");
            if (file.TryGetProperty("delete", out JsonElement delete) &&
                delete.ValueKind == JsonValueKind.True)
            {
                File.Delete(destination);
                continue;
            }
            string stagedName = file.GetProperty("stagedName").GetString()!;
            string source = Path.Combine(_stagingRoot, stagedName);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Move(source, destination, overwrite: true);
        }
        Directory.Delete(_stagingRoot, recursive: true);
        _transactionId = "";
        _stagingRoot = "";
        return "{\"committed\":true}";
    }

    private string AbortTransaction()
    {
        if (_stagingRoot.Length > 0 && Directory.Exists(_stagingRoot))
            Directory.Delete(_stagingRoot, recursive: true);
        _transactionId = "";
        _stagingRoot = "";
        return "{\"aborted\":true}";
    }
}

internal sealed class ManagedToolViewMacroContext : IAegisubMacroContext
{
    public SubtitleDocumentSnapshot? Subtitles => null;

    public void Log(string message)
    {
    }

    public void ReportProgress(long current, long maximum, string message = "")
    {
    }

    public void ReportIndeterminate(string message = "")
    {
    }
}

internal sealed class LoopbackSocks5Proxy : IAsyncDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource _stop = new();
    private readonly object _gate = new();
    private readonly HashSet<Task> _connections = [];
    private readonly Task _acceptLoop;
    private Exception? _failure;
    private int _connectionCount;

    public LoopbackSocks5Proxy()
    {
        _listener.Start();
        int port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        BaseUrl = $"socks5://127.0.0.1:{port}";
        _acceptLoop = AcceptLoopAsync();
    }

    public string BaseUrl { get; }
    public int ConnectionCount => Volatile.Read(ref _connectionCount);

    public void RequireHealthy()
    {
        lock (_gate)
        {
            if (_failure is not null)
                throw new InvalidOperationException(
                    "The loopback SOCKS5 proxy failed.", _failure);
        }
    }

    private async Task AcceptLoopAsync()
    {
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                TcpClient client = await _listener.AcceptTcpClientAsync(_stop.Token)
                    .ConfigureAwait(false);
                Task connection = ServeSafelyAsync(client);
                lock (_gate)
                    _connections.Add(connection);
                _ = RemoveWhenCompleteAsync(connection);
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException) when (_stop.IsCancellationRequested)
        {
        }
    }

    private async Task RemoveWhenCompleteAsync(Task connection)
    {
        await connection.ConfigureAwait(false);
        lock (_gate)
            _connections.Remove(connection);
    }

    private async Task ServeSafelyAsync(TcpClient client)
    {
        try
        {
            await ServeAsync(client).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException) when (_stop.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            lock (_gate)
                _failure ??= error;
        }
        finally
        {
            client.Dispose();
        }
    }

    private async Task ServeAsync(TcpClient client)
    {
        NetworkStream downstream = client.GetStream();
        byte[] greeting = new byte[2];
        await downstream.ReadExactlyAsync(greeting, _stop.Token).ConfigureAwait(false);
        if (greeting[0] != 5 || greeting[1] == 0)
            throw new InvalidDataException("The SOCKS5 greeting is invalid.");
        byte[] methods = new byte[greeting[1]];
        await downstream.ReadExactlyAsync(methods, _stop.Token).ConfigureAwait(false);
        if (!methods.Contains((byte)0))
        {
            await downstream.WriteAsync(new byte[] { 5, 0xFF }, _stop.Token)
                .ConfigureAwait(false);
            return;
        }
        await downstream.WriteAsync(new byte[] { 5, 0 }, _stop.Token)
            .ConfigureAwait(false);

        byte[] request = new byte[4];
        await downstream.ReadExactlyAsync(request, _stop.Token).ConfigureAwait(false);
        if (request[0] != 5 || request[1] != 1 || request[2] != 0)
            throw new InvalidDataException("The SOCKS5 CONNECT request is invalid.");
        string host = request[3] switch
        {
            1 => new IPAddress(await ReadBytesAsync(downstream, 4).ConfigureAwait(false))
                .ToString(),
            3 => Encoding.ASCII.GetString(await ReadDomainAsync(downstream)
                .ConfigureAwait(false)),
            4 => new IPAddress(await ReadBytesAsync(downstream, 16).ConfigureAwait(false))
                .ToString(),
            _ => throw new InvalidDataException(
                "The SOCKS5 CONNECT address type is unsupported.")
        };
        byte[] rawPort = await ReadBytesAsync(downstream, 2).ConfigureAwait(false);
        int port = (rawPort[0] << 8) | rawPort[1];
        if (!IPAddress.TryParse(host, out IPAddress? address) ||
            !IPAddress.IsLoopback(address))
        {
            await WriteReplyAsync(downstream, status: 2).ConfigureAwait(false);
            return;
        }

        using TcpClient upstream = new(address.AddressFamily);
        try
        {
            await upstream.ConnectAsync(address, port, _stop.Token).ConfigureAwait(false);
        }
        catch
        {
            await WriteReplyAsync(downstream, status: 5).ConfigureAwait(false);
            throw;
        }
        await WriteReplyAsync(downstream, status: 0).ConfigureAwait(false);
        Interlocked.Increment(ref _connectionCount);

        NetworkStream upstreamStream = upstream.GetStream();
        Task upload = downstream.CopyToAsync(
            upstreamStream, 64 * 1024, _stop.Token);
        Task download = upstreamStream.CopyToAsync(
            downstream, 64 * 1024, _stop.Token);
        await Task.WhenAny(upload, download).ConfigureAwait(false);
        client.Close();
        upstream.Close();
        try
        {
            await Task.WhenAll(upload, download).ConfigureAwait(false);
        }
        catch (Exception error) when (
            error is IOException or ObjectDisposedException or OperationCanceledException)
        {
        }
    }

    private async Task<byte[]> ReadBytesAsync(NetworkStream stream, int count)
    {
        byte[] result = new byte[count];
        await stream.ReadExactlyAsync(result, _stop.Token).ConfigureAwait(false);
        return result;
    }

    private async Task<byte[]> ReadDomainAsync(NetworkStream stream)
    {
        byte[] length = await ReadBytesAsync(stream, 1).ConfigureAwait(false);
        if (length[0] == 0)
            throw new InvalidDataException("The SOCKS5 domain is empty.");
        return await ReadBytesAsync(stream, length[0]).ConfigureAwait(false);
    }

    private Task WriteReplyAsync(NetworkStream stream, byte status) =>
        stream.WriteAsync(
            new byte[] { 5, status, 0, 1, 0, 0, 0, 0, 0, 0 },
            _stop.Token).AsTask();

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _listener.Stop();
        await _acceptLoop.ConfigureAwait(false);
        Task[] connections;
        lock (_gate)
            connections = [.. _connections];
        await Task.WhenAll(connections).ConfigureAwait(false);
        _stop.Dispose();
    }
}

internal sealed class LoopbackOrigin : IAsyncDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly Dictionary<string, byte[]> _routes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> _requestCounts = new(StringComparer.Ordinal);
    private readonly Dictionary<string, TimeSpan> _delays = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> _declaredLengths = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _statusLines = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _extraHeaders = new(StringComparer.Ordinal);
    private readonly HashSet<Task> _connections = [];
    private readonly object _gate = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly Task _acceptLoop;

    public LoopbackOrigin()
    {
        _listener.Start();
        int port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        BaseUrl = $"http://127.0.0.1:{port}";
        _acceptLoop = AcceptLoopAsync();
    }

    public string BaseUrl { get; }
    public string Url(string route) => BaseUrl + route;

    public void Add(string route, byte[] content) => _routes.Add(route, content);

    public void AddDelayed(string route, byte[] content, TimeSpan delay)
    {
        _routes.Add(route, content);
        _delays.Add(route, delay);
    }

    public void AddProxyAuthenticationRequired(string route)
    {
        byte[] content = Encoding.UTF8.GetBytes("proxy authentication required");
        _routes.Add(route, content);
        _statusLines.Add(route, "407 Proxy Authentication Required");
        _extraHeaders.Add(route, "Proxy-Authenticate: Basic realm=\"fixture\"\r\n");
    }

    public void AddTruncated(string route, byte[] content, int declaredLength)
    {
        if (declaredLength <= content.Length)
            throw new ArgumentOutOfRangeException(nameof(declaredLength));
        _routes.Add(route, content);
        _declaredLengths.Add(route, declaredLength);
    }

    public void Set(string route, byte[] content) => _routes[route] = content;

    public int RequestCount(string route)
    {
        lock (_gate)
            return _requestCounts.GetValueOrDefault(route);
    }

    public int TotalRequestCount
    {
        get
        {
            lock (_gate)
                return _requestCounts.Values.Sum();
        }
    }

    private async Task AcceptLoopAsync()
    {
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                TcpClient client = await _listener.AcceptTcpClientAsync(_stop.Token)
                    .ConfigureAwait(false);
                Task connection = ServeSafelyAsync(client);
                lock (_gate)
                    _connections.Add(connection);
                _ = RemoveWhenCompleteAsync(connection);
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException) when (_stop.IsCancellationRequested)
        {
        }
    }

    private async Task RemoveWhenCompleteAsync(Task connection)
    {
        await connection.ConfigureAwait(false);
        lock (_gate)
            _connections.Remove(connection);
    }

    private async Task ServeSafelyAsync(TcpClient client)
    {
        try
        {
            await ServeAsync(client).ConfigureAwait(false);
        }
        catch (Exception error) when (
            error is IOException or ObjectDisposedException ||
            _stop.IsCancellationRequested && error is OperationCanceledException)
        {
        }
    }

    private async Task ServeAsync(TcpClient client)
    {
        using (client)
        await using (NetworkStream stream = client.GetStream())
        using (StreamReader reader = new(
            stream,
            Encoding.ASCII,
            detectEncodingFromByteOrderMarks: false,
            leaveOpen: true))
        {
            string requestLine = await reader.ReadLineAsync().ConfigureAwait(false) ?? "";
            string? line;
            do
            {
                line = await reader.ReadLineAsync().ConfigureAwait(false);
            }
            while (!string.IsNullOrEmpty(line));

            string[] parts = requestLine.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            string rawTarget = parts.Length >= 2 ? parts[1] : "";
            string route = Uri.TryCreate(rawTarget, UriKind.Absolute, out Uri? targetUri)
                ? targetUri.PathAndQuery
                : rawTarget;
            lock (_gate)
                _requestCounts[route] = _requestCounts.GetValueOrDefault(route) + 1;
            bool found;
            byte[]? body;
            TimeSpan delay;
            int declaredLength;
            string statusLine;
            string extraHeaders;
            lock (_gate)
            {
                found = _routes.TryGetValue(route, out body);
                delay = _delays.GetValueOrDefault(route);
                declaredLength = _declaredLengths.GetValueOrDefault(
                    route, body?.Length ?? 0);
                statusLine = _statusLines.GetValueOrDefault(route, "200 OK");
                extraHeaders = _extraHeaders.GetValueOrDefault(route) ?? "";
            }
            body ??= Encoding.UTF8.GetBytes("not found");
            if (delay > TimeSpan.Zero)
                await Task.Delay(delay, _stop.Token).ConfigureAwait(false);
            string header = found
                ? $"HTTP/1.1 {statusLine}\r\nContent-Length: {declaredLength}\r\n" +
                  $"{extraHeaders}Connection: close\r\n\r\n"
                : $"HTTP/1.1 404 Not Found\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n";
            await stream.WriteAsync(Encoding.ASCII.GetBytes(header)).ConfigureAwait(false);
            await stream.WriteAsync(body).ConfigureAwait(false);
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _listener.Stop();
        try
        {
            await _acceptLoop.ConfigureAwait(false);
        }
        finally
        {
            Task[] connections;
            lock (_gate)
                connections = [.. _connections];
            await Task.WhenAll(connections).ConfigureAwait(false);
            _stop.Dispose();
        }
    }
}
