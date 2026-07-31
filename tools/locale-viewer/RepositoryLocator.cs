namespace Aegisub.LocaleViewer;

internal static class RepositoryLocator
{
    public static string Find(string startDirectory)
    {
        var current = new DirectoryInfo(startDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "CMakeLists.txt"))
                && Directory.Exists(Path.Combine(current.FullName, "po")))
                return current.FullName;

            current = current.Parent;
        }

        throw new InvalidOperationException("Could not locate the Aegisub repository root.");
    }
}
