using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Aegisub.Managed.Contracts;

namespace Aegisub.CoreClr.Adapter;

public static unsafe class BridgeEntryPoints
{
    private const uint BridgeAbiVersion = 1;

    private enum BridgeStatus
    {
        Success = 0,
        InvalidArgument = -1,
        IncompatibleAbi = -2,
        BufferTooSmall = -3,
        AlreadyInitialized = -4,
        NotInitialized = -5,
        NotFound = -6,
        ExtensionLoadFailed = -7,
        InvalidHandle = -8,
        InvocationFailed = -9,
        UnloadFailed = -10,
        Cancelled = -11,
        HostServiceFailed = -12,
        EventDispatchFailed = -13,
        AdapterException = -100
    }

    // The native host owns and initializes this versioned ABI table.
#pragma warning disable CS0649
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeHostApi
    {
        public uint AbiVersion;
        public uint Size;
        public delegate* unmanaged<byte*, int, void> LogUtf8;
        public delegate* unmanaged<long, int> IsCancellationRequested;
        public delegate* unmanaged<long, long, long, byte*, int, int> ReportProgress;
        public delegate* unmanaged<ulong, long, byte*, int, byte*, int, byte*, int, int*, int>
            InvokeHostServiceUtf8;
        public delegate* unmanaged<ulong, byte*, int, byte*, int, int> EmitPluginEventUtf8;
    }
#pragma warning restore CS0649

    [StructLayout(LayoutKind.Sequential)]
    private struct AdapterApi
    {
        public uint AbiVersion;
        public uint Size;
        public delegate* unmanaged<byte*, int, int*, int> GetRuntimeInfoUtf8;
        public delegate* unmanaged<byte*, int, byte*, int, ulong*, int> LoadPluginUtf8;
        public delegate* unmanaged<ulong, byte*, int, int*, int> GetPluginMetadataUtf8;
        public delegate* unmanaged<ulong, byte*, int, byte*, int, byte*, int, byte*, int, int*, int>
            InvokeContributionUtf8;
        public delegate* unmanaged<ulong, byte*, int, byte*, int, int> DispatchPluginEventUtf8;
        public delegate* unmanaged<ulong, int> UnloadPlugin;
        public delegate* unmanaged<byte*, int, int*, int> GetLastErrorUtf8;
        public delegate* unmanaged<int> Shutdown;
    }

    private sealed record PendingInvocation(
        ulong Handle,
        string ContributionId,
        string OperationId,
        string RequestJson,
        string Result);

    private static readonly JsonSerializerOptions ErrorJsonOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase
    };
    private static readonly BridgeJsonContext ErrorJson = new(ErrorJsonOptions);

    private static delegate* unmanaged<byte*, int, void> s_logUtf8;
    private static delegate* unmanaged<long, int> s_isCancellationRequested;
    private static delegate* unmanaged<long, long, long, byte*, int, int> s_reportProgress;
    private static delegate* unmanaged<ulong, long, byte*, int, byte*, int, byte*, int, int*, int>
        s_invokeHostServiceUtf8;
    private static delegate* unmanaged<ulong, byte*, int, byte*, int, int> s_emitPluginEventUtf8;
    private static PluginManager? s_pluginManager;
    private static int s_initialized;
    private static bool s_nativeAot;

    [ThreadStatic]
    private static string? s_lastErrorEnvelope;

    [ThreadStatic]
    private static PendingInvocation? s_pendingInvocation;

    [UnmanagedCallersOnly]
    public static int Initialize(void* nativeApiPointer, void* adapterApiPointer)
        => InitializeCore(nativeApiPointer, adapterApiPointer, null, 0);

    public static int InitializeNativeAot(
        IAegisubPlugin plugin,
        ulong pluginHandle,
        void* nativeApiPointer,
        void* adapterApiPointer) =>
        InitializeCore(nativeApiPointer, adapterApiPointer, plugin, pluginHandle);

    private static int InitializeCore(
        void* nativeApiPointer,
        void* adapterApiPointer,
        IAegisubPlugin? staticPlugin,
        ulong staticPluginHandle)
    {
        try
        {
            if (nativeApiPointer == null || adapterApiPointer == null)
                return (int)BridgeStatus.InvalidArgument;

            NativeHostApi* nativeApi = (NativeHostApi*)nativeApiPointer;
            AdapterApi* adapterApi = (AdapterApi*)adapterApiPointer;
            *adapterApi = default;

            if (nativeApi->AbiVersion != BridgeAbiVersion || nativeApi->Size != sizeof(NativeHostApi))
                return (int)BridgeStatus.IncompatibleAbi;
            if (Interlocked.CompareExchange(ref s_initialized, 1, 0) != 0)
                return (int)BridgeStatus.AlreadyInitialized;

            s_logUtf8 = nativeApi->LogUtf8;
            s_isCancellationRequested = nativeApi->IsCancellationRequested;
            s_reportProgress = nativeApi->ReportProgress;
            s_invokeHostServiceUtf8 = nativeApi->InvokeHostServiceUtf8;
            s_emitPluginEventUtf8 = nativeApi->EmitPluginEventUtf8;
            s_pluginManager = new PluginManager(
                LogUtf8,
                s_isCancellationRequested == null ? null : IsCancellationRequested,
                s_reportProgress == null ? null : ReportProgress,
                s_invokeHostServiceUtf8 == null ? null : InvokeHostService,
                s_emitPluginEventUtf8 == null ? null : EmitPluginEvent);
            if (staticPlugin is not null)
            {
                if (staticPluginHandle == 0)
                    throw new ArgumentOutOfRangeException(nameof(staticPluginHandle));
                s_pluginManager.LoadStatic(staticPluginHandle, staticPlugin);
                s_nativeAot = true;
            }
            adapterApi->AbiVersion = BridgeAbiVersion;
            adapterApi->Size = (uint)sizeof(AdapterApi);
            adapterApi->GetRuntimeInfoUtf8 = &GetRuntimeInfoUtf8;
#if AEGISUB_NATIVEAOT
            adapterApi->LoadPluginUtf8 = null;
#else
            adapterApi->LoadPluginUtf8 = staticPlugin is null ? &LoadPluginUtf8 : null;
#endif
            adapterApi->GetPluginMetadataUtf8 = &GetPluginMetadataUtf8;
            adapterApi->InvokeContributionUtf8 = &InvokeContributionUtf8;
            adapterApi->DispatchPluginEventUtf8 = &DispatchPluginEventUtf8;
            adapterApi->UnloadPlugin = &UnloadPlugin;
            adapterApi->GetLastErrorUtf8 = &GetLastErrorUtf8;
            adapterApi->Shutdown = &Shutdown;
            LogUtf8(staticPlugin is null
                ? "Aegisub CoreCLR Adapter initialized"
                : "Aegisub NativeAOT plugin adapter initialized");
            return (int)BridgeStatus.Success;
        }
        catch (Exception error)
        {
            SetError(
                "bridge.initialization_failed",
                "bridge",
                error.Message,
                error,
                retryable: false);
            try { LogUtf8($"AdapterException [bridge.initialization_failed]: {error.Message}"); }
            catch { }
            try { s_pluginManager?.Shutdown(); }
            catch { }
            s_pluginManager = null;
            s_nativeAot = false;
            s_logUtf8 = null;
            s_isCancellationRequested = null;
            s_reportProgress = null;
            s_invokeHostServiceUtf8 = null;
            s_emitPluginEventUtf8 = null;
            Volatile.Write(ref s_initialized, 0);
            return (int)BridgeStatus.AdapterException;
        }
    }

#if !AEGISUB_NATIVEAOT
    [UnmanagedCallersOnly]
    private static int LoadPluginUtf8(
        byte* assemblyPath,
        int assemblyPathLength,
        byte* entryType,
        int entryTypeLength,
        ulong* pluginHandle)
    {
        ClearError();
        try
        {
            PluginManager? manager = GetManager();
            if (manager is null)
                return (int)BridgeStatus.NotInitialized;
            if (assemblyPath == null || assemblyPathLength <= 0 ||
                entryType == null || entryTypeLength <= 0 || pluginHandle == null)
                return (int)BridgeStatus.InvalidArgument;

            *pluginHandle = manager.Load(
                DecodeUtf8(assemblyPath, assemblyPathLength),
                DecodeUtf8(entryType, entryTypeLength));
            return (int)BridgeStatus.Success;
        }
        catch (Exception error)
        {
            return FailExtensionLoad(error);
        }
    }
#endif

    [UnmanagedCallersOnly]
    private static int GetPluginMetadataUtf8(
        ulong pluginHandle,
        byte* buffer,
        int capacity,
        int* payloadLength)
    {
        ClearError();
        try
        {
            PluginManager? manager = GetManager();
            if (manager is null)
                return (int)BridgeStatus.NotInitialized;
            if (pluginHandle == 0 || payloadLength == null || capacity < 0)
                return (int)BridgeStatus.InvalidArgument;

            return WriteUtf8(manager.GetMetadata(pluginHandle), buffer, capacity, payloadLength);
        }
        catch (InvalidPluginHandleException error)
        {
            return Fail(
                BridgeStatus.InvalidHandle,
                "bridge.invalid_handle",
                "bridge",
                error,
                retryable: false);
        }
        catch (Exception error)
        {
            return Fail(
                BridgeStatus.AdapterException,
                "bridge.metadata_failed",
                "bridge",
                error,
                retryable: false);
        }
    }

    [UnmanagedCallersOnly]
    private static int InvokeContributionUtf8(
        ulong pluginHandle,
        byte* contributionId,
        int contributionIdLength,
        byte* operationId,
        int operationIdLength,
        byte* requestJson,
        int requestJsonLength,
        byte* buffer,
        int capacity,
        int* payloadLength)
    {
        ClearError();
        try
        {
            PluginManager? manager = GetManager();
            if (manager is null)
                return (int)BridgeStatus.NotInitialized;
            if (pluginHandle == 0 || contributionIdLength < 0 ||
                (contributionIdLength > 0 && contributionId == null) ||
                operationId == null || operationIdLength <= 0 ||
                requestJson == null || requestJsonLength <= 0 ||
                payloadLength == null || capacity < 0)
                return (int)BridgeStatus.InvalidArgument;

            string contribution = contributionIdLength == 0
                ? string.Empty
                : DecodeUtf8(contributionId, contributionIdLength);
            string operation = DecodeUtf8(operationId, operationIdLength);
            string request = DecodeUtf8(requestJson, requestJsonLength);
            PendingInvocation? pending = s_pendingInvocation;
            string result;
            if (pending is not null && pending.Handle == pluginHandle &&
                string.Equals(pending.ContributionId, contribution, StringComparison.Ordinal) &&
                string.Equals(pending.OperationId, operation, StringComparison.Ordinal) &&
                string.Equals(pending.RequestJson, request, StringComparison.Ordinal))
            {
                result = pending.Result;
            }
            else
            {
                s_pendingInvocation = null;
                result = manager.InvokeContribution(
                    pluginHandle,
                    contribution,
                    operation,
                    request);
                s_pendingInvocation = new PendingInvocation(
                    pluginHandle,
                    contribution,
                    operation,
                    request,
                    result);
            }

            int status = WriteUtf8(result, buffer, capacity, payloadLength);
            if (status == (int)BridgeStatus.Success)
            {
                s_pendingInvocation = null;
                LogUtf8(
                    $"Executed CLR plugin contribution '{contribution}' operation '{operation}'");
            }
            return status;
        }
        catch (InvalidPluginHandleException error)
        {
            s_pendingInvocation = null;
            return Fail(
                BridgeStatus.InvalidHandle,
                "bridge.invalid_handle",
                "bridge",
                error,
                retryable: false);
        }
        catch (ContributionNotFoundException error)
        {
            s_pendingInvocation = null;
            return Fail(
                BridgeStatus.NotFound,
                "contract.contribution_not_found",
                "contract",
                error,
                retryable: false);
        }
        catch (ContributionOperationNotFoundException error)
        {
            s_pendingInvocation = null;
            return Fail(
                BridgeStatus.NotFound,
                "contract.operation_not_found",
                "contract",
                error,
                retryable: false);
        }
        catch (OperationCanceledException error)
        {
            s_pendingInvocation = null;
            return Fail(
                BridgeStatus.Cancelled,
                "invocation.cancelled",
                "invocation",
                error,
                retryable: true);
        }
        catch (Exception error)
        {
            s_pendingInvocation = null;
            return FailInvocation(error);
        }
    }

    [UnmanagedCallersOnly]
    private static int DispatchPluginEventUtf8(
        ulong pluginHandle,
        byte* eventId,
        int eventIdLength,
        byte* payloadJson,
        int payloadJsonLength)
    {
        ClearError();
        try
        {
            PluginManager? manager = GetManager();
            if (manager is null)
                return (int)BridgeStatus.NotInitialized;
            if (pluginHandle == 0 || eventId == null || eventIdLength <= 0 ||
                payloadJson == null || payloadJsonLength <= 0)
                return (int)BridgeStatus.InvalidArgument;

            manager.DispatchEvent(
                pluginHandle,
                DecodeUtf8(eventId, eventIdLength),
                DecodeUtf8(payloadJson, payloadJsonLength));
            return (int)BridgeStatus.Success;
        }
        catch (InvalidPluginHandleException error)
        {
            return Fail(
                BridgeStatus.InvalidHandle,
                "bridge.invalid_handle",
                "bridge",
                error,
                retryable: false);
        }
        catch (PluginEventHandlerNotFoundException error)
        {
            return Fail(
                BridgeStatus.NotFound,
                "contract.event_handler_not_found",
                "contract",
                error,
                retryable: false);
        }
        catch (HostContractException error)
        {
            return Fail(
                BridgeStatus.EventDispatchFailed,
                "host.invalid_event_payload",
                "host",
                error,
                retryable: false);
        }
        catch (OperationCanceledException error)
        {
            return Fail(
                BridgeStatus.Cancelled,
                "invocation.cancelled",
                "invocation",
                error,
                retryable: true);
        }
        catch (Exception error)
        {
            return Fail(
                BridgeStatus.EventDispatchFailed,
                "extension.event_dispatch_failed",
                "extension",
                error,
                retryable: false);
        }
    }

    [UnmanagedCallersOnly]
    private static int UnloadPlugin(ulong pluginHandle)
    {
        ClearError();
        try
        {
            PluginManager? manager = GetManager();
            if (manager is null)
                return (int)BridgeStatus.NotInitialized;
            if (pluginHandle == 0)
                return (int)BridgeStatus.InvalidArgument;

            manager.Unload(pluginHandle);
            s_pendingInvocation = null;
            return (int)BridgeStatus.Success;
        }
        catch (InvalidPluginHandleException error)
        {
            return Fail(
                BridgeStatus.InvalidHandle,
                "bridge.invalid_handle",
                "bridge",
                error,
                retryable: false);
        }
        catch (Exception error)
        {
            return Fail(
                BridgeStatus.UnloadFailed,
                "extension.unload_failed",
                "extension",
                error,
                retryable: true);
        }
    }

    [UnmanagedCallersOnly]
    private static int GetRuntimeInfoUtf8(byte* buffer, int capacity, int* payloadLength)
    {
        ClearError();
        try
        {
            if (Volatile.Read(ref s_initialized) == 0)
                return (int)BridgeStatus.NotInitialized;
            if (payloadLength == null || capacity < 0)
                return (int)BridgeStatus.InvalidArgument;

            string message =
                $"{{\"bridge\":\"Aegisub.CoreClr.Adapter\"," +
                $"\"bridgeAbi\":{BridgeAbiVersion}," +
                $"\"runtimeKind\":\"{(s_nativeAot ? "native" : "coreclr")}\"," +
                $"\"framework\":\"{RuntimeInformation.FrameworkDescription}\"," +
                $"\"runtimeVersion\":\"{Environment.Version}\"," +
                "\"message\":\"C++ → C# 桥接成功\"}";

            int status = WriteUtf8(message, buffer, capacity, payloadLength);
            if (status == (int)BridgeStatus.Success)
                LogUtf8("C# → C++ 反向回调成功");
            return status;
        }
        catch (Exception error)
        {
            return Fail(
                BridgeStatus.AdapterException,
                "runtime.info_failed",
                "runtime",
                error,
                retryable: false);
        }
    }

    [UnmanagedCallersOnly]
    private static int GetLastErrorUtf8(byte* buffer, int capacity, int* payloadLength)
    {
        try
        {
            if (payloadLength == null || capacity < 0)
                return (int)BridgeStatus.InvalidArgument;
            return WriteUtf8(s_lastErrorEnvelope ?? string.Empty, buffer, capacity, payloadLength);
        }
        catch
        {
            return (int)BridgeStatus.AdapterException;
        }
    }

    [UnmanagedCallersOnly]
    private static int Shutdown()
    {
        ClearError();
        if (Interlocked.Exchange(ref s_initialized, 0) == 0)
            return (int)BridgeStatus.NotInitialized;

        try
        {
            s_pluginManager?.Shutdown();
            LogUtf8(s_nativeAot
                ? "Aegisub NativeAOT plugin adapter shutdown"
                : "Aegisub CoreCLR Adapter shutdown");
            return (int)BridgeStatus.Success;
        }
        catch (Exception error)
        {
            return Fail(
                BridgeStatus.UnloadFailed,
                "bridge.shutdown_failed",
                "bridge",
                error,
                retryable: false);
        }
        finally
        {
            s_pendingInvocation = null;
            s_pluginManager = null;
            s_nativeAot = false;
            s_logUtf8 = null;
            s_isCancellationRequested = null;
            s_reportProgress = null;
            s_invokeHostServiceUtf8 = null;
            s_emitPluginEventUtf8 = null;
        }
    }

    private static PluginManager? GetManager() =>
        Volatile.Read(ref s_initialized) == 0 ? null : s_pluginManager;

    private static string DecodeUtf8(byte* source, int length) =>
        Encoding.UTF8.GetString(new ReadOnlySpan<byte>(source, length));

    private static int WriteUtf8(string value, byte* buffer, int capacity, int* payloadLength)
    {
        int required = Encoding.UTF8.GetByteCount(value);
        *payloadLength = required;
        if (buffer == null || capacity <= required)
            return (int)BridgeStatus.BufferTooSmall;

        Span<byte> destination = new(buffer, capacity);
        int written = Encoding.UTF8.GetBytes(value, destination);
        destination[written] = 0;
        return (int)BridgeStatus.Success;
    }

    private static void ClearError() => s_lastErrorEnvelope = null;

    private static int FailExtensionLoad(Exception error)
    {
        return error switch
        {
            ExtensionContractException => Fail(
                BridgeStatus.ExtensionLoadFailed,
                "contract.invalid_extension",
                "contract",
                error,
                retryable: false),
            FileNotFoundException => Fail(
                BridgeStatus.ExtensionLoadFailed,
                "extension.assembly_not_found",
                "extension",
                error,
                retryable: false),
            BadImageFormatException => Fail(
                BridgeStatus.ExtensionLoadFailed,
                "extension.invalid_assembly",
                "extension",
                error,
                retryable: false),
            FileLoadException => Fail(
                BridgeStatus.ExtensionLoadFailed,
                "extension.dependency_load_failed",
                "extension",
                error,
                retryable: true),
            TypeLoadException => Fail(
                BridgeStatus.ExtensionLoadFailed,
                "contract.entry_type_not_found",
                "contract",
                error,
                retryable: false),
            _ => Fail(
                BridgeStatus.ExtensionLoadFailed,
                "extension.load_failed",
                "extension",
                error,
                retryable: false)
        };
    }

    private static int FailInvocation(Exception error)
    {
        return error switch
        {
            AutomationFailureException failure => Fail(
                BridgeStatus.InvocationFailed,
                failure.Code,
                "extension",
                failure,
                failure.IsRetryable),
            HostContractException => Fail(
                BridgeStatus.InvocationFailed,
                "host.invalid_context",
                "host",
                error,
                retryable: false),
            ExtensionContractException => Fail(
                BridgeStatus.InvocationFailed,
                "contract.invalid_macro_result",
                "contract",
                error,
                retryable: false),
            _ => Fail(
                BridgeStatus.InvocationFailed,
                "extension.unhandled_exception",
                "extension",
                error,
                retryable: false)
        };
    }

    private static int Fail(
        BridgeStatus status,
        string code,
        string category,
        Exception error,
        bool retryable)
    {
        SetError(code, category, error.Message, error, retryable);
        LogUtf8($"{status} [{code}]: {error.Message}");
        return (int)status;
    }

    private static void SetError(
        string code,
        string category,
        string message,
        Exception error,
        bool retryable)
    {
        s_lastErrorEnvelope = JsonSerializer.Serialize(
            new BridgeErrorEnvelope(
                SchemaVersion: 1,
                Code: code,
                Category: category,
                Message: message,
                Details: error.ToString(),
                ExceptionType: error.GetType().FullName ?? error.GetType().Name,
                Retryable: retryable),
            ErrorJson.BridgeErrorEnvelope);
    }

    internal static void LogUtf8(string message)
    {
        if (s_logUtf8 == null)
            return;

        int length = Encoding.UTF8.GetByteCount(message);
        byte[]? rented = null;
        Span<byte> buffer = length <= 1024
            ? stackalloc byte[length]
            : (rented = ArrayPool<byte>.Shared.Rent(length)).AsSpan(0, length);
        try
        {
            Encoding.UTF8.GetBytes(message, buffer);
            fixed (byte* pointer = buffer)
                s_logUtf8(pointer, length);
        }
        finally
        {
            if (rented is not null)
                ArrayPool<byte>.Shared.Return(rented);
        }
    }

    private static bool IsCancellationRequested(long invocationToken) =>
        s_isCancellationRequested != null && s_isCancellationRequested(invocationToken) > 0;

    private static void ReportProgress(
        long invocationToken,
        long current,
        long maximum,
        string message)
    {
        if (s_reportProgress == null)
            return;

        int length = Encoding.UTF8.GetByteCount(message);
        byte[]? rented = null;
        Span<byte> buffer = length <= 1024
            ? stackalloc byte[length]
            : (rented = ArrayPool<byte>.Shared.Rent(length)).AsSpan(0, length);
        try
        {
            Encoding.UTF8.GetBytes(message, buffer);
            fixed (byte* pointer = buffer)
            {
                int status = s_reportProgress(
                    invocationToken,
                    current,
                    maximum,
                    pointer,
                    length);
                if (status < 0)
                    throw new InvalidOperationException(
                        $"Native progress callback failed with Bridge status {status}.");
            }
        }
        finally
        {
            if (rented is not null)
                ArrayPool<byte>.Shared.Return(rented);
        }
    }

    private static string InvokeHostService(
        ulong pluginHandle,
        long invocationToken,
        string serviceId,
        string requestJson)
    {
        if (s_invokeHostServiceUtf8 == null)
            throw new InvalidOperationException("The native host-service Bridge is unavailable.");

        byte[] serviceBytes = Encoding.UTF8.GetBytes(serviceId);
        byte[] requestBytes = Encoding.UTF8.GetBytes(requestJson);
        fixed (byte* servicePointer = serviceBytes)
        fixed (byte* requestPointer = requestBytes)
        {
            int payloadLength = 0;
            int status = s_invokeHostServiceUtf8(
                pluginHandle,
                invocationToken,
                servicePointer,
                serviceBytes.Length,
                requestPointer,
                requestBytes.Length,
                null,
                0,
                &payloadLength);
            if (status != (int)BridgeStatus.BufferTooSmall || payloadLength < 0)
                throw new InvalidOperationException(
                    $"Native host service '{serviceId}' size query failed with Bridge status {status}.");
            if (payloadLength == int.MaxValue)
                throw new InvalidOperationException(
                    $"Native host service '{serviceId}' returned an oversized payload.");

            byte[] resultBytes = new byte[payloadLength + 1];
            fixed (byte* resultPointer = resultBytes)
            {
                status = s_invokeHostServiceUtf8(
                    pluginHandle,
                    invocationToken,
                    servicePointer,
                    serviceBytes.Length,
                    requestPointer,
                    requestBytes.Length,
                    resultPointer,
                    resultBytes.Length,
                    &payloadLength);
            }
            if (status != (int)BridgeStatus.Success || payloadLength < 0 ||
                payloadLength >= resultBytes.Length)
                throw new InvalidOperationException(
                    $"Native host service '{serviceId}' failed with Bridge status {status}.");
            return Encoding.UTF8.GetString(resultBytes, 0, payloadLength);
        }
    }

    private static void EmitPluginEvent(
        ulong pluginHandle,
        string eventId,
        string payloadJson)
    {
        if (s_emitPluginEventUtf8 == null)
            throw new InvalidOperationException("The native plugin-event Bridge is unavailable.");

        byte[] eventBytes = Encoding.UTF8.GetBytes(eventId);
        byte[] payloadBytes = Encoding.UTF8.GetBytes(payloadJson);
        fixed (byte* eventPointer = eventBytes)
        fixed (byte* payloadPointer = payloadBytes)
        {
            int status = s_emitPluginEventUtf8(
                pluginHandle,
                eventPointer,
                eventBytes.Length,
                payloadPointer,
                payloadBytes.Length);
            if (status != (int)BridgeStatus.Success)
                throw new InvalidOperationException(
                    $"Native plugin event '{eventId}' failed with Bridge status {status}.");
        }
    }
}
