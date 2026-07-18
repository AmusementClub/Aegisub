using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Aegisub.Plugin.Generators;

[Generator(LanguageNames.CSharp)]
public sealed class AegisubPluginGenerator : IIncrementalGenerator
{
    private const string PluginAttributeName =
        "Aegisub.Plugin.Sdk.AegisubPluginAttribute";
    private const string NativeEntryAttributeName =
        "Aegisub.Plugin.Sdk.AegisubNativeAotEntryPointAttribute";
    private const string JsonContextAttributeName =
        "Aegisub.Plugin.Sdk.AegisubJsonContextAttribute";
    private const string ContributionAttributeName =
        "Aegisub.Plugin.Sdk.AegisubContributionIdAttribute";
    private const string UiAttributeName =
        "Aegisub.Plugin.Sdk.AegisubUiIdAttribute";
    private const string PluginInterfaceName =
        "Aegisub.Managed.Contracts.IAegisubPlugin";

    private static readonly DiagnosticDescriptor InvalidId = new(
        "AEGISUBSDK001",
        "Invalid Aegisub plugin ID",
        "Aegisub ID '{0}' must be a stable lowercase dotted identifier",
        "Aegisub.Plugin.Sdk",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidPluginType = new(
        "AEGISUBSDK002",
        "Invalid Aegisub plugin type",
        "Type '{0}' must be a non-abstract IAegisubPlugin with a public parameterless constructor",
        "Aegisub.Plugin.Sdk",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor DuplicatePluginId = new(
        "AEGISUBSDK003",
        "Duplicate Aegisub plugin ID",
        "Aegisub plugin ID '{0}' is declared more than once in this compilation",
        "Aegisub.Plugin.Sdk",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor DuplicateSymbolicName = new(
        "AEGISUBSDK004",
        "Duplicate generated Aegisub ID name",
        "Generated Aegisub ID name '{0}' is declared more than once on '{1}'",
        "Aegisub.Plugin.Sdk",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor NativeAdapterMissing = new(
        "AEGISUBSDK005",
        "NativeAOT adapter reference is missing",
        "AegisubNativeAotEntryPoint requires a reference to Aegisub.CoreClr.Adapter",
        "Aegisub.Plugin.Sdk",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor InvalidJsonContext = new(
        "AEGISUBSDK006",
        "Invalid Aegisub JSON context",
        "Aegisub JSON context '{0}' must derive from JsonSerializerContext",
        "Aegisub.Plugin.Sdk",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        IncrementalValuesProvider<PluginCandidate> plugins = context.SyntaxProvider
            .ForAttributeWithMetadataName(
                PluginAttributeName,
                static (node, _) => node is ClassDeclarationSyntax,
                static (attributeContext, _) => CreateCandidate(attributeContext))
            .Where(static candidate => candidate is not null)
            .Select(static (candidate, _) => candidate!);

        context.RegisterSourceOutput(
            plugins.Collect(),
            static (productionContext, candidates) =>
                EmitPluginRegistrations(productionContext, candidates));

        IncrementalValueProvider<NativeEntryCandidate?> nativeEntry =
            context.CompilationProvider.Select(
                static (compilation, _) => CreateNativeEntryCandidate(compilation));
        context.RegisterSourceOutput(
            nativeEntry,
            static (productionContext, candidate) =>
                EmitNativeEntryPoint(productionContext, candidate));

        IncrementalValueProvider<ImmutableArray<JsonContextCandidate>> jsonContexts =
            context.CompilationProvider.Select(
                static (compilation, _) => ReadJsonContexts(compilation));
        context.RegisterSourceOutput(
            jsonContexts,
            static (productionContext, candidates) =>
                EmitJsonContextRegistry(productionContext, candidates));
    }

    private static PluginCandidate? CreateCandidate(
        GeneratorAttributeSyntaxContext context)
    {
        if (context.TargetSymbol is not INamedTypeSymbol type ||
            context.Attributes.Length != 1)
            return null;
        AttributeData attribute = context.Attributes[0];
        if (attribute.ConstructorArguments.Length != 5)
            return null;
        return new PluginCandidate(
            type,
            attribute,
            GetString(attribute, 0),
            GetString(attribute, 1),
            GetString(attribute, 2),
            GetString(attribute, 3),
            GetString(attribute, 4));
    }

    private static NativeEntryCandidate? CreateNativeEntryCandidate(Compilation compilation)
    {
        AttributeData? attribute = compilation.Assembly.GetAttributes().FirstOrDefault(
            static value => value.AttributeClass?.ToDisplayString() ==
                NativeEntryAttributeName);
        if (attribute is null || attribute.ConstructorArguments.Length != 1 ||
            attribute.ConstructorArguments[0].Value is not INamedTypeSymbol pluginType)
            return null;
        bool hasAdapter = compilation.GetTypeByMetadataName(
            "Aegisub.CoreClr.Adapter.BridgeEntryPoints") is not null;
        return new NativeEntryCandidate(pluginType, attribute, hasAdapter);
    }

    private static ImmutableArray<JsonContextCandidate> ReadJsonContexts(
        Compilation compilation)
    {
        ImmutableArray<JsonContextCandidate>.Builder result =
            ImmutableArray.CreateBuilder<JsonContextCandidate>();
        HashSet<string> names = new(StringComparer.Ordinal);
        foreach (AttributeData attribute in compilation.Assembly.GetAttributes().Where(
            static value => value.AttributeClass?.ToDisplayString() ==
                JsonContextAttributeName))
        {
            if (attribute.ConstructorArguments.Length != 2 ||
                attribute.ConstructorArguments[0].Value is not string name ||
                attribute.ConstructorArguments[1].Value is not INamedTypeSymbol type ||
                !names.Add(name))
                continue;
            result.Add(new JsonContextCandidate(name, type, attribute));
        }
        return result.ToImmutable();
    }

    private static void EmitPluginRegistrations(
        SourceProductionContext context,
        ImmutableArray<PluginCandidate> candidates)
    {
        foreach (IGrouping<string, PluginCandidate> duplicate in candidates
            .GroupBy(static candidate => candidate.Id, StringComparer.Ordinal)
            .Where(static group => group.Count() > 1))
        {
            foreach (PluginCandidate candidate in duplicate)
                context.ReportDiagnostic(Diagnostic.Create(
                    DuplicatePluginId,
                    candidate.Attribute.ApplicationSyntaxReference?.GetSyntax().GetLocation(),
                    duplicate.Key));
        }

        foreach (PluginCandidate candidate in candidates)
        {
            Location? location = candidate.Attribute.ApplicationSyntaxReference?
                .GetSyntax().GetLocation();
            if (!IsValidId(candidate.Id))
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    InvalidId, location, candidate.Id));
                continue;
            }
            if (!IsValidPluginType(candidate.Type))
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    InvalidPluginType,
                    location,
                    candidate.Type.ToDisplayString()));
                continue;
            }

            List<GeneratedId> contributionIds = ReadGeneratedIds(
                context, candidate, ContributionAttributeName, "Contributions");
            List<GeneratedId> uiIds = ReadGeneratedIds(
                context, candidate, UiAttributeName, "Ui");
            string typeName = candidate.Type.ToDisplayString(
                SymbolDisplayFormat.FullyQualifiedFormat);
            string generatedName = "Registration_" + SanitizeIdentifier(
                candidate.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat));
            StringBuilder source = new();
            source.AppendLine("// <auto-generated />");
            source.AppendLine("#nullable enable");
            source.AppendLine("namespace Aegisub.Plugin.Generated");
            source.AppendLine("{");
            source.Append("    public static class ").Append(generatedName).AppendLine();
            source.AppendLine("    {");
            source.Append("        public const string PluginId = ")
                .Append(SymbolDisplay.FormatLiteral(candidate.Id, true)).AppendLine(";");
            source.Append("        public static global::Aegisub.Plugin.Sdk.AegisubGeneratedPluginDescriptor Descriptor { get; } = new(")
                .Append(SymbolDisplay.FormatLiteral(candidate.Id, true)).Append(", ")
                .Append(SymbolDisplay.FormatLiteral(candidate.Name, true)).Append(", ")
                .Append(SymbolDisplay.FormatLiteral(candidate.Description, true)).Append(", ")
                .Append(SymbolDisplay.FormatLiteral(candidate.Author, true)).Append(", ")
                .Append(SymbolDisplay.FormatLiteral(candidate.Version, true)).Append(", typeof(")
                .Append(typeName).AppendLine("));");
            source.Append("        public static global::Aegisub.Managed.Contracts.IAegisubPlugin CreatePlugin() => new ")
                .Append(typeName).AppendLine("();");
            AppendIds(source, "Contributions", contributionIds);
            AppendIds(source, "Ui", uiIds);
            source.AppendLine("    }");
            source.AppendLine("}");
            context.AddSource(
                generatedName + ".g.cs",
                SourceText.From(source.ToString(), Encoding.UTF8));
        }
    }

    private static List<GeneratedId> ReadGeneratedIds(
        SourceProductionContext context,
        PluginCandidate candidate,
        string attributeName,
        string groupName)
    {
        List<GeneratedId> result = new();
        HashSet<string> names = new(StringComparer.Ordinal);
        foreach (AttributeData attribute in candidate.Type.GetAttributes().Where(
            value => value.AttributeClass?.ToDisplayString() == attributeName))
        {
            if (attribute.ConstructorArguments.Length < 2)
                continue;
            string name = GetString(attribute, 0);
            string id = GetString(attribute, 1);
            Location? location = attribute.ApplicationSyntaxReference?.GetSyntax().GetLocation();
            if (!SyntaxFacts.IsValidIdentifier(name) || !names.Add(name))
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    DuplicateSymbolicName,
                    location,
                    name,
                    candidate.Type.ToDisplayString()));
                continue;
            }
            if (!IsValidId(id))
            {
                context.ReportDiagnostic(Diagnostic.Create(InvalidId, location, id));
                continue;
            }
            result.Add(new GeneratedId(name, id, groupName));
        }
        return result;
    }

    private static void AppendIds(
        StringBuilder source,
        string className,
        IReadOnlyList<GeneratedId> ids)
    {
        if (ids.Count == 0)
            return;
        source.Append("        public static class ").Append(className).AppendLine();
        source.AppendLine("        {");
        foreach (GeneratedId id in ids)
            source.Append("            public const string ").Append(id.Name).Append(" = ")
                .Append(SymbolDisplay.FormatLiteral(id.Id, true)).AppendLine(";");
        source.AppendLine("        }");
    }

    private static void EmitNativeEntryPoint(
        SourceProductionContext context,
        NativeEntryCandidate? candidate)
    {
        if (candidate is null)
            return;
        Location? location = candidate.Attribute.ApplicationSyntaxReference?
            .GetSyntax().GetLocation();
        if (!candidate.HasAdapter)
        {
            context.ReportDiagnostic(Diagnostic.Create(NativeAdapterMissing, location));
            return;
        }
        if (!IsValidPluginType(candidate.PluginType))
        {
            context.ReportDiagnostic(Diagnostic.Create(
                InvalidPluginType,
                location,
                candidate.PluginType.ToDisplayString()));
            return;
        }
        string pluginType = candidate.PluginType.ToDisplayString(
            SymbolDisplayFormat.FullyQualifiedFormat);
        string source = """
            // <auto-generated />
            #nullable enable
            namespace Aegisub.Plugin.Generated
            {
                public static unsafe class NativeEntryPoint
                {
                    [global::System.Runtime.InteropServices.UnmanagedCallersOnly(
                        EntryPoint = "aegisub_plugin_init_v1")]
                    public static int Initialize(
                        void* nativeHostApi,
                        ulong pluginHandle,
                        void* adapterApi)
                    {
                        try
                        {
                            return global::Aegisub.CoreClr.Adapter.BridgeEntryPoints.InitializeNativeAot(
                                new PLUGIN_TYPE(), pluginHandle, nativeHostApi, adapterApi);
                        }
                        catch
                        {
                            return -100;
                        }
                    }
                }
            }
            """.Replace("PLUGIN_TYPE", pluginType);
        context.AddSource(
            "AegisubGeneratedNativeEntryPoint.g.cs",
            SourceText.From(source, Encoding.UTF8));
    }

    private static void EmitJsonContextRegistry(
        SourceProductionContext context,
        ImmutableArray<JsonContextCandidate> candidates)
    {
        if (candidates.IsDefaultOrEmpty)
            return;
        StringBuilder source = new();
        source.AppendLine("// <auto-generated />");
        source.AppendLine("#nullable enable");
        source.AppendLine("namespace Aegisub.Plugin.Generated");
        source.AppendLine("{");
        source.AppendLine("    public static class AegisubGeneratedJsonContexts");
        source.AppendLine("    {");
        foreach (JsonContextCandidate candidate in candidates)
        {
            Location? location = candidate.Attribute.ApplicationSyntaxReference?
                .GetSyntax().GetLocation();
            if (!SyntaxFacts.IsValidIdentifier(candidate.Name) ||
                !DerivesFromJsonSerializerContext(candidate.Type))
            {
                context.ReportDiagnostic(Diagnostic.Create(
                    InvalidJsonContext,
                    location,
                    candidate.Type.ToDisplayString()));
                continue;
            }
            string typeName = candidate.Type.ToDisplayString(
                SymbolDisplayFormat.FullyQualifiedFormat);
            source.Append("        public static ").Append(typeName).Append(' ')
                .Append(candidate.Name).Append(" => ").Append(typeName)
                .AppendLine(".Default;");
        }
        source.AppendLine("    }");
        source.AppendLine("}");
        context.AddSource(
            "AegisubGeneratedJsonContexts.g.cs",
            SourceText.From(source.ToString(), Encoding.UTF8));
    }

    private static bool DerivesFromJsonSerializerContext(INamedTypeSymbol type)
    {
        for (INamedTypeSymbol? current = type.BaseType;
            current is not null;
            current = current.BaseType)
        {
            if (current.ToDisplayString() ==
                "System.Text.Json.Serialization.JsonSerializerContext")
                return true;
        }
        return false;
    }

    private static bool IsValidPluginType(INamedTypeSymbol type) =>
        !type.IsAbstract &&
        type.TypeKind == TypeKind.Class &&
        type.AllInterfaces.Any(static value =>
            value.ToDisplayString() == PluginInterfaceName) &&
        type.InstanceConstructors.Any(static constructor =>
            constructor.DeclaredAccessibility == Accessibility.Public &&
            constructor.Parameters.Length == 0);

    private static bool IsValidId(string value)
    {
        if (value.Length is < 1 or > 128 ||
            value[0] is not (>= 'a' and <= 'z') and not (>= '0' and <= '9') ||
            value[value.Length - 1] is '.' or '-')
            return false;
        bool previousDot = false;
        foreach (char character in value)
        {
            if (character is not (>= 'a' and <= 'z') and
                not (>= '0' and <= '9') and not '.' and not '-')
                return false;
            if (character == '.' && previousDot)
                return false;
            previousDot = character == '.';
        }
        return true;
    }

    private static string GetString(AttributeData attribute, int index) =>
        attribute.ConstructorArguments[index].Value as string ?? "";

    private static string SanitizeIdentifier(string value)
    {
        StringBuilder result = new(value.Length);
        foreach (char character in value)
            result.Append(char.IsLetterOrDigit(character) ? character : '_');
        return result.ToString();
    }

    private sealed class PluginCandidate
    {
        public PluginCandidate(
            INamedTypeSymbol type,
            AttributeData attribute,
            string id,
            string name,
            string description,
            string author,
            string version)
        {
            Type = type;
            Attribute = attribute;
            Id = id;
            Name = name;
            Description = description;
            Author = author;
            Version = version;
        }

        public INamedTypeSymbol Type { get; }
        public AttributeData Attribute { get; }
        public string Id { get; }
        public string Name { get; }
        public string Description { get; }
        public string Author { get; }
        public string Version { get; }
    }

    private sealed class NativeEntryCandidate
    {
        public NativeEntryCandidate(
            INamedTypeSymbol pluginType,
            AttributeData attribute,
            bool hasAdapter)
        {
            PluginType = pluginType;
            Attribute = attribute;
            HasAdapter = hasAdapter;
        }

        public INamedTypeSymbol PluginType { get; }
        public AttributeData Attribute { get; }
        public bool HasAdapter { get; }
    }

    private sealed class GeneratedId
    {
        public GeneratedId(string name, string id, string group)
        {
            Name = name;
            Id = id;
            Group = group;
        }

        public string Name { get; }
        public string Id { get; }
        public string Group { get; }
    }

    private sealed class JsonContextCandidate
    {
        public JsonContextCandidate(
            string name,
            INamedTypeSymbol type,
            AttributeData attribute)
        {
            Name = name;
            Type = type;
            Attribute = attribute;
        }

        public string Name { get; }
        public INamedTypeSymbol Type { get; }
        public AttributeData Attribute { get; }
    }
}
