using System.Runtime.InteropServices;
using Aegisub.CoreClr.Adapter;
using Aegisub.DependencyControl.Plugin;

namespace Aegisub.DependencyControl.NativeAot;

public static unsafe class NativeAotEntryPoint
{
    [UnmanagedCallersOnly(EntryPoint = "aegisub_plugin_init_v1")]
    public static int Initialize(
        void* nativeHostApi,
        ulong pluginHandle,
        void* adapterApi)
    {
        try
        {
            return BridgeEntryPoints.InitializeNativeAot(
                new DependencyControlPlugin(),
                pluginHandle,
                nativeHostApi,
                adapterApi);
        }
        catch
        {
            return -100;
        }
    }
}
