using System.Reflection;
using System.Runtime.Loader;
using Aegisub.Managed.Contracts;

namespace Aegisub.Managed.DevHost;

internal sealed class DevHostExtensionLoadContext : AssemblyLoadContext
{
    private static readonly Assembly ContractsAssembly =
        typeof(IAegisubPlugin).Assembly;

    private readonly AssemblyDependencyResolver _resolver;
    private readonly string _assemblyDirectory;

    public DevHostExtensionLoadContext(string entryAssemblyPath)
        : base($"Aegisub.DevHost:{Path.GetFileNameWithoutExtension(entryAssemblyPath)}")
    {
        _resolver = new AssemblyDependencyResolver(entryAssemblyPath);
        _assemblyDirectory = Path.GetDirectoryName(entryAssemblyPath)
            ?? throw new ArgumentException(
                "The entry assembly must have a parent directory.",
                nameof(entryAssemblyPath));
    }

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        if (string.Equals(
                assemblyName.Name,
                ContractsAssembly.GetName().Name,
                StringComparison.Ordinal))
        {
            return ContractsAssembly;
        }

        string? resolvedPath = _resolver.ResolveAssemblyToPath(assemblyName);
        if (resolvedPath is null && assemblyName.Name is not null)
        {
            string siblingPath = Path.Combine(
                _assemblyDirectory,
                assemblyName.Name + ".dll");
            if (File.Exists(siblingPath))
                resolvedPath = siblingPath;
        }

        return resolvedPath is null ? null : LoadFromAssemblyPath(resolvedPath);
    }

    protected override nint LoadUnmanagedDll(string unmanagedDllName)
    {
        string? resolvedPath = _resolver.ResolveUnmanagedDllToPath(unmanagedDllName);
        return resolvedPath is null ? nint.Zero : LoadUnmanagedDllFromPath(resolvedPath);
    }
}
