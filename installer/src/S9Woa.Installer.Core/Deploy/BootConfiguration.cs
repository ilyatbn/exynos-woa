// SPDX-License-Identifier: BSD-2-Clause-Patent
using S9Woa.Installer.Core.Processes;

namespace S9Woa.Installer.Core.Deploy;

/// <summary>
/// Turns the BCD store bcdboot wrote on the PC into one that boots Windows on the phone.
/// The boot manager's device becomes the phone's EFI system partition and the loader's
/// device and OS device become USERDATA, each as a GPT partition qualified by the phone's
/// disk and partition GUIDs (Samsung's fixed identifiers, see <see cref="PartitionMap"/>).
/// Those devices are written through the Windows BCD WMI provider, which accepts a disk that
/// is not attached; the plain settings use bcdedit.
/// </summary>
public sealed class BootConfiguration
{
    public const string BootManagerId = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}";
    private static readonly TimeSpan Timeout = TimeSpan.FromMinutes(2);
    private readonly IProcessRunner _runner;
    private readonly string _bcdedit;
    private readonly string _powershell;

    public BootConfiguration(IProcessRunner runner, string? system32 = null)
    {
        _runner = runner;
        system32 ??= Environment.GetFolderPath(Environment.SpecialFolder.System);
        _bcdedit = Path.Combine(system32, "bcdedit.exe");
        _powershell = Path.Combine(system32, @"WindowsPowerShell\v1.0\powershell.exe");
    }

    /// <summary>
    /// Loader settings of the phone's working full-OS entry: test signing (the phone drivers are
    /// test-signed), integrity checks kept, four processors, no VSM or hypervisor, HAL detection,
    /// and ignoring boot-status failures so an unclean shutdown does not stop at a recovery prompt.
    /// </summary>
    internal static IReadOnlyList<IReadOnlyList<string>> SettingCommands(string storePath) =>
    [
        ["/store", storePath, "/set", "{default}", "path", "\\Windows\\System32\\winload.efi"],
        ["/store", storePath, "/set", "{default}", "systemroot", "\\Windows"],
        ["/store", storePath, "/set", "{default}", "testsigning", "on"],
        ["/store", storePath, "/set", "{default}", "nointegritychecks", "off"],
        ["/store", storePath, "/set", "{default}", "numproc", "4"],
        ["/store", storePath, "/set", "{default}", "vsmlaunchtype", "off"],
        ["/store", storePath, "/set", "{default}", "hypervisorlaunchtype", "off"],
        ["/store", storePath, "/set", "{default}", "detecthal", "on"],
        ["/store", storePath, "/set", "{default}", "bootstatuspolicy", "IgnoreAllFailures"],
    ];

    /// <summary>
    /// PowerShell that sets the phone devices, turns isolated context off (0x16000060) and the
    /// boot-manager timeout to zero (0x25000004) through the BcdStore WMI provider.
    /// </summary>
    internal static string DeviceScript(string storePath, GeometryType g) => string.Join("\n",
        "$ErrorActionPreference = 'Stop'",
        "function Call($o, [string]$m, [hashtable]$a) {",
        "  $r = Invoke-CimMethod -InputObject $o -MethodName $m -Arguments $a",
        "  if (-not $r.ReturnValue) { throw \"BCD rejected $m\" }",
        "  $r",
        "}",
        $"$open = Invoke-CimMethod -Namespace root\\WMI -ClassName BcdStore -MethodName OpenStore -Arguments @{{ File = '{storePath.Replace("'", "''", StringComparison.Ordinal)}' }}",
        "if (-not $open.ReturnValue) { throw 'Cannot open the BCD store' }",
        "$store = $open.Store",
        $"$mgr = (Call $store 'OpenObject' @{{ Id = '{BootManagerId}' }}).Object",
        "$loaderId = (Call $mgr 'GetElement' @{ Type = [uint32]0x23000003 }).Element.Id",
        "$loader = (Call $store 'OpenObject' @{ Id = $loaderId }).Object",
        $"$null = Call $mgr 'SetQualifiedPartitionDeviceElement' @{{ Type = [uint32]0x11000001; PartitionStyle = [uint32]1; DiskSignature = '{g.DiskGuid}'; PartitionIdentifier = '{g.EfiPartitionGuid}' }}",
        "foreach ($t in [uint32]0x11000001, [uint32]0x21000001) {",
        $"  $null = Call $loader 'SetQualifiedPartitionDeviceElement' @{{ Type = $t; PartitionStyle = [uint32]1; DiskSignature = '{g.DiskGuid}'; PartitionIdentifier = '{g.WindowsPartitionGuid}' }}",
        "}",
        "$null = Call $loader 'SetBooleanElement' @{ Type = [uint32]0x16000060; Boolean = $false }",
        "$null = Call $mgr 'SetIntegerElement' @{ Type = [uint32]0x25000004; Integer = [uint64]0 }",
        "'S9WOA_BCD_PHONE_DEVICES_SET'");

    public async Task ConfigureForPhoneAsync(string storePath, GeometryType g,
        IProgress<string>? log = null, CancellationToken ct = default)
    {
        if (!File.Exists(storePath))
        {
            throw new FileNotFoundException("BCD store not found.", storePath);
        }
        log?.Report("Pointing the boot configuration at the phone's partitions...");
        foreach (var args in SettingCommands(storePath))
        {
            ct.ThrowIfCancellationRequested();
            var r = await _runner.RunAsync(_bcdedit, args, Timeout, ct).ConfigureAwait(false);
            if (!r.Succeeded)
            {
                throw new InvalidOperationException($"bcdedit {string.Join(' ', args)} failed: {(r.StdErr + r.StdOut).Trim()}");
            }
        }

        var script = Path.Combine(Path.GetTempPath(), $"s9woa-bcd-{Guid.NewGuid():N}.ps1");
        await File.WriteAllTextAsync(script, DeviceScript(storePath, g), ct).ConfigureAwait(false);
        try
        {
            var r = await _runner.RunAsync(_powershell,
                ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script], Timeout, ct).ConfigureAwait(false);
            if (!r.Succeeded || !r.StdOut.Contains("S9WOA_BCD_PHONE_DEVICES_SET", StringComparison.Ordinal))
            {
                throw new InvalidOperationException($"Setting the phone boot devices failed: {(r.StdErr + r.StdOut).Trim()}");
            }
        }
        finally
        {
            try { File.Delete(script); } catch (IOException) { }
        }
    }
}