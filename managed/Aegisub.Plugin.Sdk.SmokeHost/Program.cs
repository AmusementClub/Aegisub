using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text.Json;
using Aegisub.Plugin.Generated;

namespace Aegisub.Plugin.Sdk.SmokeHost;

internal static class Program
{
    private const string PluginId = "aegisub.plugin-sdk.smoke";

    public static int Main(string[] args)
    {
        if (args.Length != 3)
        {
            Console.Error.WriteLine(
                "Usage: sdk-smoke-host <coreclr-manifest> <coreclr-nupkg> <native-library>");
            return 2;
        }
        try
        {
            VerifyGeneratedRegistration();
            VerifyManifest(args[0]);
            VerifyPackage(args[1]);
            VerifyNativeExport(args[2]);
            Console.WriteLine(
                "Aegisub Plugin SDK generator, manifest, NuGet, and NativeAOT export smoke passed.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"Aegisub Plugin SDK smoke failed: {error}");
            return 1;
        }
    }

    private static void VerifyGeneratedRegistration()
    {
        Aegisub.Managed.Contracts.IAegisubPlugin plugin =
            Registration_global__Aegisub_Plugin_Sdk_Smoke_SdkSmokePlugin.CreatePlugin();
        if (plugin.Metadata.Id != PluginId ||
            Registration_global__Aegisub_Plugin_Sdk_Smoke_SdkSmokePlugin.PluginId != PluginId ||
            Registration_global__Aegisub_Plugin_Sdk_Smoke_SdkSmokePlugin.Contributions.Automation !=
                PluginId + ".automation" ||
            Registration_global__Aegisub_Plugin_Sdk_Smoke_SdkSmokePlugin.Ui.MainToolView !=
                PluginId + ".tool-view" ||
            Registration_global__Aegisub_Plugin_Sdk_Smoke_SdkSmokePlugin.Ui.RunEvent !=
                PluginId + ".run")
            throw new InvalidOperationException(
                "Generated plugin registration or stable IDs do not match metadata.");
        string payload = JsonSerializer.Serialize(
            new Aegisub.Plugin.Sdk.Smoke.SmokePayload("generated"),
            AegisubGeneratedJsonContexts.Plugin.SmokePayload);
        if (payload != "{\"message\":\"generated\"}")
            throw new InvalidOperationException(
                "Generated plugin JSON context did not apply the shared JSON policy.");
    }

    private static void VerifyManifest(string path)
    {
        using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(path));
        JsonElement root = document.RootElement;
        if (root.GetProperty("manifestVersion").GetInt32() != 2 ||
            root.GetProperty("id").GetString() != PluginId ||
            root.GetProperty("runtime").GetProperty("kind").GetString() != "coreclr" ||
            root.GetProperty("runtime").GetProperty("entryType").GetString() !=
                "Aegisub.Plugin.Sdk.Smoke.SdkSmokePlugin" ||
            root.GetProperty("contributions").GetArrayLength() != 1)
            throw new InvalidOperationException(
                "BuildTasks generated an unexpected CoreCLR manifest.");
    }

    private static void VerifyPackage(string path)
    {
        using ZipArchive package = ZipFile.OpenRead(path);
        HashSet<string> names = package.Entries.Select(entry =>
            entry.FullName.Replace('\\', '/')).ToHashSet(StringComparer.Ordinal);
        if (!names.Contains($"aegisub/{PluginId}.aegisub-plugin.json") ||
            !names.Contains($"aegisub/{PluginId}/Aegisub.Plugin.Sdk.Smoke.dll"))
            throw new InvalidOperationException(
                "CoreCLR plugin NuGet package does not contain the Aegisub layout.");
    }

    private static void VerifyNativeExport(string path)
    {
        nint library = NativeLibrary.Load(Path.GetFullPath(path));
        try
        {
            if (!NativeLibrary.TryGetExport(
                    library,
                    "aegisub_plugin_init_v1",
                    out nint export) || export == 0)
                throw new InvalidOperationException(
                    "Generated NativeAOT library does not export aegisub_plugin_init_v1.");
        }
        finally
        {
            NativeLibrary.Free(library);
        }
    }
}
