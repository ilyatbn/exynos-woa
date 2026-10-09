// SPDX-License-Identifier: BSD-2-Clause-Patent
namespace S9Woa.Installer.Core.Toolset;

/// <summary>
/// The payload identity of one supported phone: the official TWRP image the user
/// downloads, the names the installer keeps its payloads under, the UFS driver
/// package it expects, and whether the WinRE-look recovery can be rebuilt for it
/// at all. These were previously constants of the star2lte flow applied to
/// whichever phone was connected, which is what put an S9+ recovery image and an
/// S9+ driver package on an S10e.
///
/// A null field means "not read here yet", never "same as the reference phone".
/// The reads that close them are on the phone (its own TWRP build and recovery
/// partition) or on the vendor's release page; guessing them writes the wrong
/// thing to the wrong device.
/// </summary>
public sealed record TwrpProfile(string Codename, string? OfficialImage, string TwrpPayloadStem,
    string WinrePayloadStem, string DriverInf, bool WinReSkinSupported);

public static class TwrpProfiles
{
    /// <summary>
    /// The validated Galaxy S9+ flow. Everything here was observed end to end.
    /// </summary>
    public static readonly TwrpProfile Star2Lte = new("star2lte",
        "twrp-3.7.0_9-0-star2lte.img", "twrp-star2lte", "twrp-winre-star2lte",
        "Exynos9810Ufs.inf", true);

    /// <summary>
    /// Beyond0lte (SM-G970F). The official image name is UNVERIFIED here: it has
    /// not been read from the phone or from the release page in this session, and
    /// naming the wrong one offers the user a download that will not boot this
    /// phone. The WinRE skin is NOT supported because the kernel patch offsets
    /// and the known-good base hash in <c>PowerOffRoutePatch</c> /
    /// <c>WinReTwrpBuilder</c> are the star2lte TWRP's; nothing equivalent has
    /// been derived for this phone, so the recovery is flashed as it is.
    /// </summary>
    public static readonly TwrpProfile Beyond0Lte = new("beyond0lte",
        null, "twrp-beyond0lte", "twrp-winre-beyond0lte",
        "Exynos9820Ufs.inf", false);

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

    /// <summary>What the user-facing text says about the official image.</summary>
    public static string OfficialImageText(string? codename) =>
        For(codename).OfficialImage is string name ? name
            : "the official TWRP build for this phone (its exact file name has not been read here yet)";
}
