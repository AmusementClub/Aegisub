using Aegisub.LocaleViewer;
using Avalonia;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;

[assembly: AvaloniaTestApplication(typeof(Aegisub.LocaleViewer.Tests.HeadlessApp))]

namespace Aegisub.LocaleViewer.Tests;

public static class HeadlessApp
{
    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions());
}
