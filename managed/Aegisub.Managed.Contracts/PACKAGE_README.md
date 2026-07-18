# Aegisub.Managed.Contracts

Framework-neutral contracts for experimental Aegisub CLR plugins.

Version `0.x` is not a stable compatibility promise. Plugin projects should
pin the exact Contracts version declared by their `*.aegisub-plugin.json`
manifest. The Aegisub host supplies and shares the Contracts assembly from its
default `AssemblyLoadContext`; extension packages must not deploy a private
runtime copy.

The first supported target is .NET 10. Version `0.1.0` introduces
`IAegisubPlugin`, plugin lifecycle, and contribution metadata. Automation is
the first executable contribution kind; command, settings, tool-view, and
service-provider kinds are reserved for subsequent host capabilities. Existing
`IAegisubAutomationModule` entry points remain loadable through a compatibility
adapter.

`IAegisubPluginContext` supplies the transport-neutral lifecycle boundary for
logging, JSON host-service calls, and managed-to-native plugin events.
`IAegisubPluginEventHandler` receives native-to-managed events. These generic
primitives are the internal foundation for typed capability APIs; plugins
should prefer typed contracts as they become available rather than inventing
private host-service IDs.

Automation APIs cover Macro metadata, declarative QueryState, owned subtitle
snapshots, batch mutations, progress, cancellation, structured extension
failures, selection-scoped snapshots, document Extradata, and optional i18n
resource keys with fallback text.
