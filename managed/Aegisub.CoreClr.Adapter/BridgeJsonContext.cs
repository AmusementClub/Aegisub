using System.Text.Json.Serialization;
using Aegisub.Managed.Contracts;

namespace Aegisub.CoreClr.Adapter;

internal sealed record MacroInvocationRequest(
    long InvocationToken,
    SubtitleDocumentSnapshot? Subtitles);

[JsonSerializable(typeof(MacroInvocationRequest))]
[JsonSerializable(typeof(MacroResult))]
[JsonSerializable(typeof(BridgeErrorEnvelope))]
[JsonSerializable(typeof(PluginManager.MetadataDocument))]
internal sealed partial class BridgeJsonContext : JsonSerializerContext
{
}
