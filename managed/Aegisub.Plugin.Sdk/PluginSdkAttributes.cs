using Aegisub.Managed.Contracts;

namespace Aegisub.Plugin.Sdk;

[AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = false)]
public sealed class AegisubPluginAttribute(
    string id,
    string name,
    string description,
    string author,
    string version) : Attribute
{
    public string Id { get; } = id;
    public string Name { get; } = name;
    public string Description { get; } = description;
    public string Author { get; } = author;
    public string Version { get; } = version;
}

[AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = false)]
public sealed class AegisubContributionIdAttribute(
    string name,
    string id,
    ContributionKind kind) : Attribute
{
    public string Name { get; } = name;
    public string Id { get; } = id;
    public ContributionKind Kind { get; } = kind;
}

public enum AegisubUiIdKind
{
    Form,
    ToolView,
    Event
}

[AttributeUsage(AttributeTargets.Class, AllowMultiple = true, Inherited = false)]
public sealed class AegisubUiIdAttribute(
    string name,
    string id,
    AegisubUiIdKind kind) : Attribute
{
    public string Name { get; } = name;
    public string Id { get; } = id;
    public AegisubUiIdKind Kind { get; } = kind;
}

[AttributeUsage(AttributeTargets.Assembly, AllowMultiple = false)]
public sealed class AegisubNativeAotEntryPointAttribute(Type pluginType) : Attribute
{
    public Type PluginType { get; } = pluginType;
}

[AttributeUsage(AttributeTargets.Assembly, AllowMultiple = true)]
public sealed class AegisubJsonContextAttribute(
    string name,
    Type contextType) : Attribute
{
    public string Name { get; } = name;
    public Type ContextType { get; } = contextType;
}

public sealed record AegisubGeneratedPluginDescriptor(
    string Id,
    string Name,
    string Description,
    string Author,
    string Version,
    Type PluginType);
