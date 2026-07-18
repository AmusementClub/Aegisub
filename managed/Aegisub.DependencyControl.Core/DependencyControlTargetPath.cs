namespace Aegisub.DependencyControl;

public static class DependencyControlTargetPath
{
    public static string Build(
        DependencyControlPackageKind kind,
        string packageNamespace,
        string fileName,
        string fileType)
    {
        DependencyControlFeedParser.ValidateNamespace(packageNamespace);
        ArgumentException.ThrowIfNullOrWhiteSpace(fileName);
        ArgumentException.ThrowIfNullOrWhiteSpace(fileType);

        string suffix = fileName.Replace('\\', '/');
        if (suffix.StartsWith("//", StringComparison.Ordinal) ||
            suffix.Contains(':', StringComparison.Ordinal) ||
            suffix.Any(char.IsControl))
            throw new DependencyControlFeedException(
                $"DependencyControl file '{fileName}' is not a safe namespace suffix.");

        suffix = suffix.TrimStart('/');
        if (suffix.Length == 0)
            throw new DependencyControlFeedException(
                "DependencyControl file suffix cannot be empty.");
        foreach (string component in suffix.Split('/'))
        {
            if (component.Length == 0 || component is "." or "..")
                throw new DependencyControlFeedException(
                    $"DependencyControl file '{fileName}' contains an invalid path component.");
        }

        string namespacePath = kind == DependencyControlPackageKind.Module
            ? packageNamespace.Replace('.', '/')
            : packageNamespace;
        string basePath = fileType switch
        {
            "script" when kind == DependencyControlPackageKind.Macro => "autoload",
            "script" => "include",
            "test" when kind == DependencyControlPackageKind.Macro =>
                "tests/DepUnit/macros",
            "test" => "tests/DepUnit/modules",
            _ => throw new DependencyControlFeedException(
                $"DependencyControl file type '{fileType}' is not supported.")
        };

        string separator = fileName.StartsWith('/') || fileName.StartsWith('\\')
            ? "/"
            : string.Empty;
        return $"{basePath}/{namespacePath}{separator}{suffix}";
    }
}
