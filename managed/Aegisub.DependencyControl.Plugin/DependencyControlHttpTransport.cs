using System.Net;

namespace Aegisub.DependencyControl.Plugin;

internal sealed class DependencyControlHttpTransport : IDisposable
{
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);
    private readonly object _gate = new();
    private HttpClient _client;
    private DependencyControlNetworkSettings _settings;
    private bool _disposed;

    public DependencyControlHttpTransport()
    {
        _settings = DependencyControlNetworkSettings.Default;
        _client = CreateClient(_settings);
    }

    public DependencyControlNetworkSettings Settings
    {
        get
        {
            lock (_gate)
                return _settings;
        }
    }

    public void ApplySettings(DependencyControlNetworkSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        HttpClient replacement;
        HttpClient previous;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            replacement = CreateClient(settings);
            previous = _client;
            _client = replacement;
            _settings = settings;
        }
        previous.Dispose();
    }

    private static HttpClient CreateClient(DependencyControlNetworkSettings settings)
    {
        SocketsHttpHandler handler = new()
        {
            AllowAutoRedirect = true,
            MaxAutomaticRedirections = 5,
            AutomaticDecompression = DecompressionMethods.None,
            UseCookies = false,
            ConnectTimeout = TimeSpan.FromSeconds(10),
            PooledConnectionLifetime = TimeSpan.FromMinutes(10)
        };
        switch (settings.Mode)
        {
            case DependencyControlProxyMode.System:
                handler.UseProxy = true;
                handler.Proxy = null;
                if (settings.UseDefaultCredentials)
                    handler.DefaultProxyCredentials = CredentialCache.DefaultCredentials;
                break;
            case DependencyControlProxyMode.Direct:
                handler.UseProxy = false;
                break;
            case DependencyControlProxyMode.Manual:
            {
                (bool bypassOnLocal, string[] bypassPatterns) =
                    settings.BuildBypassPatterns();
                WebProxy proxy = new(
                    new Uri(settings.ManualProxyUri),
                    bypassOnLocal,
                    bypassPatterns,
                    settings.UseDefaultCredentials
                        ? CredentialCache.DefaultCredentials
                        : null);
                handler.UseProxy = true;
                handler.Proxy = proxy;
                break;
            }
            default:
                throw new InvalidOperationException(
                    "DependencyControl proxy mode is not supported.");
        }
        HttpClient client = new(handler, disposeHandler: true)
        {
            Timeout = RequestTimeout
        };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("Aegisub-DependencyControl/0.1");
        return client;
    }

    public async Task<byte[]> GetBytesAsync(
        string url,
        int maximumBytes,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(url);
        if (maximumBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(maximumBytes));
        if (!Uri.TryCreate(url, UriKind.Absolute, out Uri? uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps) ||
            string.IsNullOrEmpty(uri.Host))
            throw new InvalidOperationException(
                "DependencyControl network requests require an HTTP or HTTPS URL.");

        HttpClient client;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            client = _client;
        }
        using HttpRequestMessage request = new(HttpMethod.Get, uri);
        using HttpResponseMessage response = await client.SendAsync(
            request,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is long length && length > maximumBytes)
            throw new InvalidOperationException(
                "DependencyControl HTTP response exceeds the configured size limit.");

        await using Stream input = await response.Content.ReadAsStreamAsync(cancellationToken)
            .ConfigureAwait(false);
        using MemoryStream output = new(
            response.Content.Headers.ContentLength is long knownLength
                ? checked((int)Math.Min(knownLength, maximumBytes))
                : 0);
        byte[] buffer = new byte[64 * 1024];
        while (true)
        {
            int read = await input.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read == 0)
                break;
            if (output.Length + read > maximumBytes)
                throw new InvalidOperationException(
                    "DependencyControl HTTP response exceeds the configured size limit.");
            output.Write(buffer, 0, read);
        }
        return output.ToArray();
    }

    public void Dispose()
    {
        HttpClient client;
        lock (_gate)
        {
            if (_disposed)
                return;
            _disposed = true;
            client = _client;
        }
        client.Dispose();
    }
}
