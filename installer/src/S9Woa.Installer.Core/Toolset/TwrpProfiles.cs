// SPDX-License-Identifier: BSD-2-Clause-Patent
namespace S9Woa.Installer.Core.Toolset;

/// <summary>
/// The payload identity of one supported phone: the official TWRP image the user
/// downloads, the page Team Win publishes it on, the names the installer keeps
/// its payloads under, the UFS driver package it expects, and whether the WinRE
/// skin can be rebuilt for it at all.
///
/// Both official names were read, not inferred: the star2lte one is the file the
/// validated flow uses, and the beyond0lte one is the current build on Team Win's
/// own index for this device (53.9 MB, dated 2023-02-07), which is the same build
/// the phone reports running (<c>ro.twrp.version = 3.7.0_9-2</c>, read over adb on
/// RF8M21HYDSZ). See notes/m9-install-attempt.md §15.
/// </summary>
public sealed record TwrpProfile(string Codename, string OfficialImage, string PageUrl,
    string TwrpPayloadStem, string WinrePayloadStem, string DriverInf, bool WinReSkinSupported);

public static class TwrpProfiles
{
    /// <summary>
    /// The validated Galaxy S9+ flow. Everything here was observed end to end.
    /// </summary>
    public static readonly TwrpProfile Star2Lte = new("star2lte",
        "twrp-3.7.0_9-0-star2lte.img", "https://twrp.me/samsung/samsunggalaxys9plus.html",
        "twrp-star2lte", "twrp-winre-star2lte", "Exynos9810Ufs.inf", true);

    /// <summary>
    /// Beyond0lte (SM-G970F). The official image is the current build on Team Win's
    /// index for this device and matches what the phone reports running. The WinRE
    /// skin is NOT supported: the kernel patch offsets and the known-good base hash
    /// in <c>PowerOffRoutePatch</c> / <c>WinReTwrpBuilder</c> are the star2lte TWRP
    /// 3.7.0_9-0 kernel's, and nothing equivalent has been derived for this phone's
    /// kernel, so its stock TWRP is flashed as it is.
    /// </summary>
    public static readonly TwrpProfile Beyond0Lte = new("beyond0lte",
        "twrp-3.7.0_9-2-beyond0lte.img", "https://twrp.me/samsung/samsunggalaxys10e.html",
        "twrp-beyond0lte", "twrp-winre-beyond0lte", "Exynos9820Ufs.inf", false);

    public static TwrpProfile For(string? codename) =>
        codename?.ToLowerInvariant() switch
        {
            "star2lte" => Star2Lte,
            "beyond0lte" => Beyond0Lte,
            _ => throw new ArgumentOutOfRangeException(nameof(codename),
                $"No payload profile is recorded for {(codename is null ? "an unidentified phone" : codename)}."),
        };

    /// <summary>True for a codename this class carries a profile for.</summary>
    public static bool IsKnown(string? codename) =>
        codename?.ToLowerInvariant() is "star2lte" or "beyond0lte";

    /// <summary>What the user-facing text calls the official image.</summary>
    public static string OfficialImageText(string? codename) => For(codename).OfficialImage;
}
