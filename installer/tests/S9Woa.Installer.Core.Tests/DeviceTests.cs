// SPDX-License-Identifier: BSD-2-Clause-Patent
using S9Woa.Installer.Core;
using S9Woa.Installer.Core.Device;
using S9Woa.Installer.Core.Stages;

namespace S9Woa.Installer.Core.Tests;

public class DeviceTests
{
    [Fact]
    public void ParsesSamsungBuild()
    {
        var b = SamsungBuild.TryParse("G965FXXUHFVG4", "G965F")!;
        Assert.Equal("XX", b.Csc);
        Assert.Equal('U', b.UpdateType);
        Assert.Equal(17, b.BinaryRevision);
        Assert.Equal(('V', 'G', '4'), b.ReleaseKey);
        Assert.Null(SamsungBuild.TryParse("G960FXXUHFVG4", "G965F"));
        Assert.Null(SamsungBuild.TryParse("garbage", "G965F"));
    }

    [Fact]
    public void ParsesAdbDevicesAndGetprop()
    {
        var devices = AdbClient.ParseDevices(
            "* daemon started successfully\r\nList of devices attached\r\n" +
            "ABC123 device product:star2ltexx model:SM_G965F device:star2lte transport_id:1\r\n" +
            "DEF456 unauthorized transport_id:2\r\nGHI789 recovery\r\n");
        Assert.Collection(devices,
            d => { Assert.Equal(AdbState.Device, d.State); Assert.Equal("SM_G965F", d.Model); Assert.Equal("star2ltexx", d.Product); },
            d => Assert.Equal(AdbState.Unauthorized, d.State),
            d => Assert.Equal(AdbState.Recovery, d.State));

        var props = AdbClient.ParseGetprop("[ro.product.model]: [SM-G965F]\r\n[empty]: []\nnoise\n");
        Assert.Equal("SM-G965F", props["ro.product.model"]);
        Assert.Equal("", props["empty"]);
        Assert.Equal(2, props.Count);
    }

    [Fact]
    public void DownloadModeContinuesAsThePhoneIdentifiedEarlier()
    {
        Assert.Null(DeviceSnapshot.InDownloadMode(null, "G965FXXUHFVG4"));          // never identified
        Assert.Null(DeviceSnapshot.InDownloadMode("aa11bb22cc33dd44", "G960FXXU9FVB1")); // not a supported model
        var phone = DeviceSnapshot.InDownloadMode("aa11bb22cc33dd44", "G965FXXUHFVG4")!;
        Assert.Equal(DeviceMode.Download, phone.Mode);
        Assert.Equal("SM-G965F", phone.Model);

        var checks = DeviceEligibility.Evaluate(phone);
        Assert.False(checks.HasBlockers());
        Assert.DoesNotContain(checks, c => c.Severity == CheckSeverity.Warning);
        Assert.Equal(CheckSeverity.Info, checks.Single(c => c.Id == "unlock").Severity);
    }

    [Fact]
    public void PrefersBootloaderModelInRecovery()
    {
        var props = AdbClient.ParseGetprop("[ro.product.model]: [Galaxy S9+]\n[ro.boot.em.model]: [SM-G965F]\n[ro.twrp.version]: [3.7.0_9-0]\n");
        var snap = DeviceSnapshot.FromAdb(AdbClient.ParseDevices(
            "List of devices attached\naa11bb22cc33dd44 recovery product:omni_star2lte model:Galaxy_S9_ device:star2lte\n")[0], props);
        Assert.Equal("SM-G965F", snap.Model);
        Assert.Equal(DeviceMode.Recovery, snap.Mode);
    }

    private static DeviceSnapshot Snap(string? bootloader = "G965FXXUHFVG4", string model = "SM-G965F",
        DeviceMode mode = DeviceMode.Android, bool? locked = true, bool? oem = true) =>
        new("S", mode, model, "star2lte", "exynos9810", bootloader, "10", null, oem, locked, "green", false);

    [Theory]
    [InlineData("G965FXXUHFVG4", CheckSeverity.Pass)]
    [InlineData("G965FXXUGFVB1", CheckSeverity.Blocker)]
    [InlineData("G965FXXUHFVH1", CheckSeverity.Warning)]
    [InlineData(null, CheckSeverity.Warning)]
    public void GatesBootloaderVersion(string? bootloader, CheckSeverity expected)
    {
        var r = DeviceEligibility.Evaluate(Snap(bootloader)).Single(c => c.Id == "bootloader");
        Assert.Equal(expected, r.Severity);
    }

    [Fact]
    public void RejectsOtherModelsAndUnauthorized()
    {
        Assert.True(DeviceEligibility.Evaluate(Snap(model: "SM-G960F")).HasBlockers());
        Assert.True(DeviceEligibility.Evaluate(Snap(model: "SM-G965U")).HasBlockers());
        Assert.True(DeviceEligibility.Evaluate(Snap(mode: DeviceMode.Unauthorized)).HasBlockers());
        Assert.False(DeviceEligibility.Evaluate(Snap()).HasBlockers());
    }

    [Fact]
    public void RecognisesTheS10eButKeepsItUnvalidated()
    {
        var s10e = new DeviceSnapshot(
            Serial: "aa11bb22cc33dd44", Mode: DeviceMode.Android, Model: "SM-G970F",
            Codename: "beyond0lte", Hardware: "exynos9820", Bootloader: "G970FXXSGHWC1",
            AndroidVersion: "12", RecoveryVersion: null, OemUnlockAllowed: true,
            FlashLocked: true, VerifiedBootState: "green", WarrantyTripped: false);

        var checks = DeviceEligibility.Evaluate(s10e);
        Assert.False(checks.HasBlockers());
        Assert.Equal(CheckSeverity.Warning, checks.Single(c => c.Id == "model").Severity);
        Assert.Equal(CheckSeverity.Pass, checks.Single(c => c.Id == "bootloader").Severity);

        Assert.False(StageCatalog.Runnable("transfer", "beyond0lte"));
        Assert.True(StageCatalog.Runnable("transfer", "star2lte"));
        Assert.Equal(StageAvailability.NotImplemented, StageCatalog.Resolve("uefi", "beyond0lte"));
        Assert.Equal(StageAvailability.Experimental, StageCatalog.Resolve("image", "beyond0lte"));
        Assert.Equal(StageAvailability.Ready, StageCatalog.Resolve("identify", "beyond0lte"));
    }

    [Theory]
    [InlineData(RebootTarget.System, "-s S reboot")]
    [InlineData(RebootTarget.Recovery, "-s S reboot recovery")]
    [InlineData(RebootTarget.Download, "-s S reboot download")]
    public void BuildsRebootArguments(RebootTarget target, string expected) =>
        Assert.Equal(expected, string.Join(' ', DeviceActions.RebootArguments("S", target)));
}
