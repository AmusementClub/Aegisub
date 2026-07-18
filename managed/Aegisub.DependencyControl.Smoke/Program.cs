using Aegisub.DependencyControl;

namespace Aegisub.DependencyControl.Smoke;

internal static class Program
{
    private const string SourceUrl = "https://example.invalid/DependencyControl.json";

    public static int Main(string[] args)
    {
        try
        {
            if (args.Length > 0)
                return InspectExternalFeed(args);
            VerifyVersionTwoFeed();
            VerifyVersionThreeFeed();
            VerifyUnsafeInputsAreRejected();
            Console.WriteLine("feed_0_2_compatibility=true");
            Console.WriteLine("feed_0_3_compatibility=true");
            Console.WriteLine("rolling_templates=true");
            Console.WriteLine("namespace_targets=true");
            Console.WriteLine("unsafe_feed_rejection=true");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"DependencyControl feed smoke failed: {error}");
            return 1;
        }
    }

    private static int InspectExternalFeed(string[] args)
    {
        if (args.Length != 3 || args[0] != "--feed")
            throw new ArgumentException("Usage: --feed PATH SOURCE_URL");
        DependencyControlFeed feed = DependencyControlFeedParser.Parse(
            File.ReadAllText(args[1]), args[2]);
        Console.WriteLine($"feed_format={feed.FormatVersion}");
        Console.WriteLine($"feed_name={feed.Name}");
        Console.WriteLine($"known_feeds={feed.KnownFeeds.Count}");
        Console.WriteLine($"macros={feed.Macros.Count}");
        Console.WriteLine($"modules={feed.Modules.Count}");
        return 0;
    }

    private static void VerifyVersionTwoFeed()
    {
        DependencyControlFeed feed = ParseFixture("feed-0.2.json");
        Equal("0.2.0", feed.FormatVersion, "feed 0.2 version");
        Equal(1, feed.Macros.Count, "feed 0.2 Macro count");
        Equal(2, feed.Modules.Count, "feed 0.2 module count");
        DependencyControlChannel macro = feed.Macros["sample.tool"].Channels["release"];
        True(macro.IsDefault, "feed 0.2 default channel");
        Equal(
            "https://packages.invalid/release/sample.tool.moon",
            macro.Files[0].Url,
            "feed 0.2 rolling URL");
        Equal(
            "autoload/sample.tool.moon",
            macro.Files[0].RelativeTargetPath,
            "feed 0.2 Macro target");
        True(macro.Files[1].Delete, "feed 0.2 delete record");
        Equal(
            "https://feeds.invalid/modules.json",
            macro.RequiredModules[1].Feed,
            "feed 0.2 named feed expansion");
        True(macro.RequiredModules[1].Optional, "feed 0.2 optional dependency");

        DependencyControlChannel core = feed.Modules["sample.core"].Channels["release"];
        Equal(
            "include/sample/core.lua",
            core.Files[0].RelativeTargetPath,
            "feed 0.2 module target");
        Equal(
            "include/sample/core/Config.lua",
            core.Files[1].RelativeTargetPath,
            "feed 0.2 module child target");
        Equal("sample.other", core.RequiredModules[0].ModuleName, "feed 0.2 cycle edge");
    }

    private static void VerifyVersionThreeFeed()
    {
        DependencyControlFeed feed = ParseFixture("feed-0.3.json");
        Equal("0.3.0", feed.FormatVersion, "feed 0.3 version");
        DependencyControlChannel channel =
            feed.Modules["sample.platform"].Channels["alpha"];
        Equal("win64", channel.Files[0].Platform, "platform file selector");
        Equal(
            "https://packages.invalid/v2.1.0-alpha/modules/sample/platform.lua",
            channel.Files[0].Url,
            "package/channel rolling fileBaseUrl");
        Equal(
            "tests/DepUnit/modules/sample/platform/Spec.lua",
            channel.Files[1].RelativeTargetPath,
            "test target");
    }

    private static void VerifyUnsafeInputsAreRejected()
    {
        ExpectRejected(
            """
            {"dependencyControlFeedFormatVersion":"0.3.0","dependencyControlFeedFormatVersion":"0.3.0","name":"duplicate"}
            """,
            "duplicate JSON property");
        ExpectRejected(BuildFeed("badnamespace", ".lua", FortyHexDigits), "invalid namespace");
        ExpectRejected(BuildFeed("sample.bad", "/../escape.lua", FortyHexDigits), "path traversal");
        ExpectRejected(BuildFeed("sample.bad", "C:/escape.lua", FortyHexDigits), "drive path");
        ExpectRejected(BuildFeed("sample.bad", ".lua", "1234"), "invalid SHA-1");
        ExpectRejected(
            BuildFeed("sample.bad", ".lua", FortyHexDigits, "@{missing}/file.lua"),
            "unresolved template");
    }

    private static DependencyControlFeed ParseFixture(string name)
    {
        string path = Path.Combine(AppContext.BaseDirectory, "fixtures", name);
        return DependencyControlFeedParser.Parse(File.ReadAllText(path), SourceUrl);
    }

    private static string BuildFeed(
        string packageNamespace,
        string fileName,
        string sha1,
        string url = "https://packages.invalid/file.lua") =>
        $$"""
        {
          "dependencyControlFeedFormatVersion":"0.3.0",
          "name":"negative fixture",
          "modules":{
            "{{packageNamespace}}":{
              "name":"Negative",
              "channels":{
                "release":{
                  "version":"1.0.0",
                  "default":true,
                  "files":[{"name":"{{fileName}}","url":"{{url}}","sha1":"{{sha1}}"}]
                }
              }
            }
          }
        }
        """;

    private static void ExpectRejected(string json, string scenario)
    {
        try
        {
            DependencyControlFeedParser.Parse(json, SourceUrl);
        }
        catch (DependencyControlFeedException)
        {
            return;
        }
        throw new InvalidOperationException($"Expected rejection for {scenario}.");
    }

    private static void Equal<T>(T expected, T actual, string scenario)
        where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            throw new InvalidOperationException(
                $"{scenario}: expected '{expected}', got '{actual}'.");
    }

    private static void True(bool value, string scenario)
    {
        if (!value) throw new InvalidOperationException($"{scenario}: expected true.");
    }

    private const string FortyHexDigits = "0123456789abcdef0123456789abcdef01234567";
}
