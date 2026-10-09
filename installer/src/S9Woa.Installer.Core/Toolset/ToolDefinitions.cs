// SPDX-License-Identifier: BSD-2-Clause-Patent
namespace S9Woa.Installer.Core.Toolset;

public enum ToolKind
{
    /// <summary>An executable the installer runs (adb, Heimdall, Zadig).</summary>
    Program,

    /// <summary>A Windows driver installed on this PC.</summary>
    Driver,

    /// <summary>A file written to the phone (UEFI, TWRP) or injected into the image (drivers).</summary>
    Payload,
}

public enum ToolState
{
    Missing,
    Ready,

    /// <summary>Can only be completed later in the install (for example, needs the phone in Download mode).</summary>
    Deferred,
    Error,
}

/// <summary>Ways a tool can be provided. The Setup page shows one action per entry.</summary>
[Flags]
public enum ToolSource
{
    None = 0,
    Winget = 1,
    PickFile = 2,
    OpenPage = 4,
    Release = 8,
    BuildFolder = 16,
    Launch = 32,
}

public sealed record ToolDefinition(
    string Id,
    string Name,
    string Purpose,
    ToolKind Kind,
    bool Required,
    ToolSource Sources,
    string? WingetId = null,
    string? PageUrl = null,
    string? FilePattern = null);

public sealed record ToolStatus(ToolState State, string Detail, string? Path = null)
{
    public static ToolStatus Missing(string detail) => new(ToolState.Missing, detail);
    public static ToolStatus Ready(string path, string? detail = null) => new(ToolState.Ready, detail ?? path, path);
}

/// <summary>Well-known ids and the catalogue of everything the installer needs.</summary>
public static class Tools
{
    public const string Adb = "adb";
    public const string Heimdall = "heimdall";
    public const string Zadig = "zadig";
    public const string SamsungUsb = "samsung-usb";
    public const string DownloadModeDriver = "download-mode-driver";
    public const string Twrp = "twrp";
    public const string Uefi = "uefi";
    public const string Drivers = "drivers";

    /// <summary>Fallback page used before a phone is identified; once one is, the
    /// row opens <c>TwrpProfiles</c>'s page for that phone instead (see
    /// <c>SetupPage.xaml.cs</c>).</summary>
    public const string TwrpPage = "https://twrp.me/samsung/samsunggalaxys9plus.html";
    public const string SamsungDriverPage = "https://developer.samsung.com/android-usb-driver";

    public static IReadOnlyList<ToolDefinition> All { get; } =
    [
        new(Adb, "Android platform tools (adb)",
            "Talks to the phone in Android and in TWRP: identify, back up, and write Windows.",
            ToolKind.Program, true, ToolSource.Winget | ToolSource.PickFile, WingetId: "Google.PlatformTools", FilePattern: "adb.exe"),
        new(SamsungUsb, "Samsung USB driver",
            "Lets Windows see the phone over USB in Android and Download mode.",
            ToolKind.Driver, true, ToolSource.OpenPage | ToolSource.PickFile, PageUrl: SamsungDriverPage, FilePattern: "*.exe"),
        new(Heimdall, "Heimdall",
            "Fallback TWRP flasher. The installer flashes TWRP itself through the Samsung USB driver; Heimdall is used only if that fails.",
            ToolKind.Program, false, ToolSource.Winget | ToolSource.PickFile, WingetId: "BenjaminDobell.Heimdall", FilePattern: "heimdall.exe"),
        new(Zadig, "Zadig",
            "Only for the Heimdall fallback: switches the Download-mode USB interface to WinUSB (Odin and Smart Switch then stop seeing it).",
            ToolKind.Program, false, ToolSource.Winget | ToolSource.PickFile, WingetId: "akeo.ie.Zadig", FilePattern: "zadig*.exe"),
        new(DownloadModeDriver, "Download-mode USB driver for Heimdall",
            "Only for the Heimdall fallback: a one-time Zadig step while the phone is in Download mode.",
            ToolKind.Driver, false, ToolSource.Launch),
        new(Twrp, "TWRP recovery for the connected phone",
            "The recovery the installer boots to back up the phone and write Windows. Choose the official image for the phone in front of you; where this installer knows how, it turns it into a Windows Recovery-style recovery on this PC.",
            ToolKind.Payload, true, ToolSource.OpenPage | ToolSource.PickFile, PageUrl: TwrpPage, FilePattern: "*.img"),
        new(Uefi, "UEFI firmware image",
            "The open-source UEFI that boots Windows, written to the BOOT partition.",
            ToolKind.Payload, true, ToolSource.Release | ToolSource.BuildFolder | ToolSource.PickFile, FilePattern: "*.img"),
        new(Drivers, "Phone drivers (UFS storage, touch)",
            "Injected into the Windows image so it can boot from the phone's storage and use the screen.",
            ToolKind.Payload, true, ToolSource.Release | ToolSource.BuildFolder),
    ];

    public static ToolDefinition Get(string id) => All.First(t => t.Id == id);
}
