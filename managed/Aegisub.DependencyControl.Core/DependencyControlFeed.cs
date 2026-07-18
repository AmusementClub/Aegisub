namespace Aegisub.DependencyControl;

public enum DependencyControlPackageKind
{
    Macro,
    Module
}

public sealed record DependencyControlFeed(
    string FormatVersion,
    string Name,
    string Description,
    string Maintainer,
    string SourceUrl,
    IReadOnlyDictionary<string, string> KnownFeeds,
    IReadOnlyDictionary<string, DependencyControlPackage> Macros,
    IReadOnlyDictionary<string, DependencyControlPackage> Modules);

public sealed record DependencyControlPackage(
    DependencyControlPackageKind Kind,
    string Namespace,
    string Name,
    string Description,
    string Author,
    string Url,
    IReadOnlyDictionary<string, DependencyControlChannel> Channels);

public sealed record DependencyControlChannel(
    string Name,
    string Version,
    string Released,
    bool IsDefault,
    IReadOnlyList<string> Platforms,
    IReadOnlyList<DependencyControlFile> Files,
    IReadOnlyList<DependencyControlRequirement> RequiredModules);

public sealed record DependencyControlFile(
    string Name,
    string Url,
    string Sha1,
    string Type,
    string Platform,
    bool Delete,
    string RelativeTargetPath);

public sealed record DependencyControlRequirement(
    string ModuleName,
    string Version,
    string Feed,
    string Channel,
    bool Optional);

public sealed class DependencyControlFeedException(string message) :
    Exception(message)
{
}
