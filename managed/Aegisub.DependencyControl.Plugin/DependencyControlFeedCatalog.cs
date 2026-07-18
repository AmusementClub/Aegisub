using System.Text;
using Aegisub.DependencyControl;

namespace Aegisub.DependencyControl.Plugin;

internal sealed class DependencyControlFeedCatalog(DependencyControlHttpTransport transport)
{
    private const int MaximumFeedBytes = 4 * 1024 * 1024;
    private const int MaximumFeeds = 32;
    private readonly Dictionary<string, DependencyControlFeed> _feeds =
        new(StringComparer.Ordinal);
    private readonly HashSet<string> _requestedFeeds = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Exception> _feedFailures = new(StringComparer.Ordinal);

    public async Task<DependencyControlCatalogSnapshot> BuildSnapshotAsync(
        IReadOnlyList<string> rootUrls,
        CancellationToken cancellationToken)
    {
        Queue<(string Url, bool IsRoot)> pending = new();
        HashSet<string> enqueued = new(StringComparer.OrdinalIgnoreCase);
        foreach (string rootUrl in rootUrls)
        {
            if (enqueued.Add(rootUrl))
                pending.Enqueue((rootUrl, true));
        }

        List<DependencyControlFeed> feeds = [];
        List<DependencyControlFeedFailure> failures = [];
        Dictionary<string, DependencyControlCatalogPackage> packages =
            new(StringComparer.Ordinal);
        while (pending.Count > 0)
        {
            (string url, bool isRoot) = pending.Dequeue();
            DependencyControlFeed feed;
            try
            {
                feed = await GetFeedAsync(url, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error) when (error is not OperationCanceledException)
            {
                failures.Add(new(url, isRoot, error.Message));
                continue;
            }
            feeds.Add(feed);
            AddPackages(packages, "automation", feed, feed.Macros.Values);
            AddPackages(packages, "module", feed, feed.Modules.Values);
            foreach (string knownFeed in feed.KnownFeeds.Values)
            {
                if (!enqueued.Add(knownFeed))
                    continue;
                if (enqueued.Count > MaximumFeeds)
                    throw new InvalidOperationException(
                        "DependencyControl known-feed discovery exceeds the feed limit.");
                pending.Enqueue((knownFeed, false));
            }
        }
        if (packages.Count > 4096)
            throw new InvalidOperationException(
                "DependencyControl catalog exceeds the package limit.");
        return new(
            feeds,
            packages.Values
                .OrderBy(package => package.RecordType, StringComparer.Ordinal)
                .ThenBy(package => package.Package.Namespace, StringComparer.Ordinal)
                .ToArray(),
            failures);
    }

    public Task<DependencyControlFeed> LoadFeedAsync(
        string url,
        CancellationToken cancellationToken) => GetFeedAsync(url, cancellationToken);

    public async Task<(DependencyControlFeed Feed, DependencyControlPackage Package)>
        FindPackageAsync(
            string rootUrl,
            DependencyControlPackageKind kind,
            string packageNamespace,
            CancellationToken cancellationToken)
    {
        Queue<(string Url, bool IsRoot)> pending = new();
        HashSet<string> enqueued = new(StringComparer.Ordinal);
        pending.Enqueue((rootUrl, true));
        enqueued.Add(rootUrl);
        while (pending.Count > 0)
        {
            (string url, bool isRoot) = pending.Dequeue();
            DependencyControlFeed feed;
            try
            {
                feed = await GetFeedAsync(url, cancellationToken).ConfigureAwait(false);
            }
            catch (Exception error) when (
                !isRoot && error is not OperationCanceledException)
            {
                continue;
            }
            IReadOnlyDictionary<string, DependencyControlPackage> packages =
                kind == DependencyControlPackageKind.Module ? feed.Modules : feed.Macros;
            if (packages.TryGetValue(packageNamespace, out DependencyControlPackage? package))
                return (feed, package);
            foreach (string knownFeed in feed.KnownFeeds.Values)
            {
                if (!enqueued.Add(knownFeed))
                    continue;
                if (enqueued.Count > MaximumFeeds)
                    throw new InvalidOperationException(
                        "DependencyControl known-feed discovery exceeds the feed limit.");
                pending.Enqueue((knownFeed, false));
            }
        }
        throw new InvalidOperationException(
            $"DependencyControl known feeds do not contain {kind.ToString().ToLowerInvariant()} " +
            $"'{packageNamespace}'.");
    }

    private async Task<DependencyControlFeed> GetFeedAsync(
        string url,
        CancellationToken cancellationToken)
    {
        if (_feeds.TryGetValue(url, out DependencyControlFeed? cached))
            return cached;
        if (_feedFailures.TryGetValue(url, out Exception? previousFailure))
            throw new InvalidOperationException(
                "DependencyControl feed request previously failed.", previousFailure);
        if (!_requestedFeeds.Add(url))
            throw new InvalidOperationException(
                "DependencyControl feed request is already in progress.");
        if (_requestedFeeds.Count > MaximumFeeds)
            throw new InvalidOperationException(
                "DependencyControl resolution exceeds the feed request limit.");
        try
        {
            byte[] payload = await transport.GetBytesAsync(
                url, MaximumFeedBytes, cancellationToken).ConfigureAwait(false);
            string json;
            try
            {
                json = new UTF8Encoding(
                    encoderShouldEmitUTF8Identifier: false,
                    throwOnInvalidBytes: true).GetString(payload);
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidOperationException(
                    "DependencyControl feed is not valid UTF-8.", error);
            }
            DependencyControlFeed feed = DependencyControlFeedParser.Parse(json, url);
            _feeds.Add(url, feed);
            return feed;
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            _feedFailures[url] = error;
            throw;
        }
    }

    private static void AddPackages(
        Dictionary<string, DependencyControlCatalogPackage> target,
        string recordType,
        DependencyControlFeed feed,
        IEnumerable<DependencyControlPackage> packages)
    {
        foreach (DependencyControlPackage package in packages)
        {
            string key = $"{recordType}\n{package.Namespace}";
            target.TryAdd(key, new(recordType, feed, package));
        }
    }
}

internal sealed record DependencyControlCatalogPackage(
    string RecordType,
    DependencyControlFeed Feed,
    DependencyControlPackage Package);

internal sealed record DependencyControlFeedFailure(
    string Url,
    bool IsRoot,
    string Error);

internal sealed record DependencyControlCatalogSnapshot(
    IReadOnlyList<DependencyControlFeed> Feeds,
    IReadOnlyList<DependencyControlCatalogPackage> Packages,
    IReadOnlyList<DependencyControlFeedFailure> Failures);
