// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Text.RegularExpressions;

namespace S9Woa.Installer.Core.Image;

/// <summary>One Windows edition inside a WIM/ESD, as reported by <c>dism /Get-ImageInfo</c>.</summary>
public sealed record WindowsImageEdition(int Index, string Name);

/// <summary>Locates the install image inside user media and lists its editions.</summary>
public sealed partial class WindowsMedia
{
    private static readonly TimeSpan Timeout = TimeSpan.FromMinutes(2);
    private readonly Processes.IProcessRunner _runner;
    private readonly string _dism;
    private readonly string _powershell;

    public WindowsMedia(Processes.IProcessRunner runner, string? system32 = null)
    {
        _runner = runner;
        system32 ??= Environment.GetFolderPath(Environment.SpecialFolder.System);
        _dism = Path.Combine(system32, "dism.exe");
        _powershell = Path.Combine(system32, @"WindowsPowerShell\v1.0\powershell.exe");
    }

    public static bool IsIso(string path) => Path.GetExtension(path).Equals(".iso", StringComparison.OrdinalIgnoreCase);

    private static string Quote(string path) => $"'{path.Replace("'", "''", StringComparison.Ordinal)}'";

    /// <summary>
    /// Attaches an ISO read-only (or finds it already attached) and prints its drive letter and
    /// whether this call attached it, e.g. <c>E 1</c>.
    /// </summary>
    internal static string MountIsoScript(string iso) => string.Join("\n",
        "$ErrorActionPreference = 'Stop'",
        $"$img = Get-DiskImage -ImagePath {Quote(iso)}",
        "$mine = 0",
        "if (-not $img.Attached) { $img = Mount-DiskImage -InputObject $img -Access ReadOnly -PassThru; $mine = 1 }",
        "$letter = ($img | Get-Volume).DriveLetter",
        "if (-not $letter) { throw 'The ISO has no drive letter.' }",
        "Write-Output \"$letter $mine\"");

    internal static string DismountIsoScript(string iso) => $"Dismount-DiskImage -ImagePath {Quote(iso)} | Out-Null";

    /// <summary>Parses <see cref="MountIsoScript"/>'s last line; null when there is no drive letter.</summary>
    internal static (char Letter, bool AttachedHere)? ParseMount(string stdout)
    {
        var last = stdout.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).LastOrDefault();
        if (last is null || last.Length == 0 || !char.IsAsciiLetter(last[0]) || (last.Length > 1 && last[1] != ' '))
        {
            return null;
        }
        return (char.ToUpperInvariant(last[0]), last.EndsWith(" 1", StringComparison.Ordinal));
    }

    private async Task<(char Letter, bool AttachedHere)> AttachIsoAsync(string iso, CancellationToken ct)
    {
        var r = await _runner.RunAsync(_powershell, ["-NoProfile", "-NonInteractive", "-Command", MountIsoScript(iso)], Timeout, ct)
            .ConfigureAwait(false);
        return (r.Succeeded ? ParseMount(r.StdOut) : null)
            ?? throw new InvalidOperationException($"Could not open {Path.GetFileName(iso)}: {(r.StdErr + r.StdOut).Trim()}");
    }

    /// <summary>
    /// The install image inside <paramref name="mediaPath"/>: a .wim/.esd as is, or for an .iso the
    /// <c>sources\install.*</c> of the disc, attached read-only (it stays attached for the image build).
    /// </summary>
    public async Task<string?> ResolveInstallImageAsync(string mediaPath, CancellationToken ct = default)
    {
        if (!IsIso(mediaPath))
        {
            return FindInstallImage(mediaPath);
        }
        var (letter, _) = await AttachIsoAsync(mediaPath, ct).ConfigureAwait(false);
        return FindInstallImage($"{letter}:\\");
    }

    /// <summary>
    /// Copies <c>sources\install.wim</c> (or <c>.esd</c>) out of an ISO into <paramref name="destinationDirectory"/>
    /// and returns its path. The ISO is attached read-only and detached again if this call attached it.
    /// A finished copy of the same size is reused; a partial one is never mistaken for it.
    /// </summary>
    public async Task<string> ExtractInstallImageAsync(string iso, string destinationDirectory, IProgress<double>? progress = null,
        CancellationToken ct = default)
    {
        var (letter, attachedHere) = await AttachIsoAsync(iso, ct).ConfigureAwait(false);
        try
        {
            var source = FindInstallImage($"{letter}:\\")
                ?? throw new InvalidOperationException($"{Path.GetFileName(iso)} has no sources\\install.wim or install.esd. Is it a Windows installation ISO?");
            var target = Path.Combine(destinationDirectory, Path.GetFileName(source));
            await CopyWithProgressAsync(source, target, progress, ct).ConfigureAwait(false);
            return target;
        }
        finally
        {
            if (attachedHere)
            {
                await _runner.RunAsync(_powershell, ["-NoProfile", "-NonInteractive", "-Command", DismountIsoScript(iso)], Timeout,
                    CancellationToken.None).ConfigureAwait(false);
            }
        }
    }

    internal static async Task CopyWithProgressAsync(string source, string target, IProgress<double>? progress, CancellationToken ct)
    {
        var length = new FileInfo(source).Length;
        if (File.Exists(target) && new FileInfo(target).Length == length)
        {
            progress?.Report(1);
            return;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        var partial = target + ".partial";
        await using (var src = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan | FileOptions.Asynchronous))
        await using (var dst = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, FileOptions.Asynchronous))
        {
            var buffer = new byte[4 << 20];
            long done = 0;
            var lastPermille = -1L;
            int n;
            while ((n = await src.ReadAsync(buffer, ct).ConfigureAwait(false)) > 0)
            {
                await dst.WriteAsync(buffer.AsMemory(0, n), ct).ConfigureAwait(false);
                done += n;
                var permille = length == 0 ? 1000 : done * 1000 / length;
                if (permille != lastPermille)
                {
                    lastPermille = permille;
                    progress?.Report(permille / 1000.0);
                }
            }
        }
        File.Move(partial, target, overwrite: true);
    }

    /// <summary>An .esd/.wim is the image itself; inside a mounted ISO it is under <c>sources\install.*</c>.</summary>
    public static string? FindInstallImage(string mediaPathOrMountRoot)
    {
        var ext = Path.GetExtension(mediaPathOrMountRoot);
        if (ext.Equals(".wim", StringComparison.OrdinalIgnoreCase) || ext.Equals(".esd", StringComparison.OrdinalIgnoreCase))
        {
            return File.Exists(mediaPathOrMountRoot) ? mediaPathOrMountRoot : null;
        }
        foreach (var name in new[] { "install.wim", "install.esd" })
        {
            var candidate = Path.Combine(mediaPathOrMountRoot, "sources", name);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }
        return null;
    }

    [GeneratedRegex(@"Index\s*:\s*(\d+)\s*[\r\n]+\s*Name\s*:\s*(.+?)\s*[\r\n]", RegexOptions.Multiline)]
    private static partial Regex IndexNameRegex();

    internal static IReadOnlyList<WindowsImageEdition> ParseImageInfo(string dismOutput) =>
        IndexNameRegex().Matches(dismOutput)
            .Select(m => new WindowsImageEdition(int.Parse(m.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture), m.Groups[2].Value.Trim()))
            .ToList();

    [GeneratedRegex(@"^\s*Version\s*:\s*\d+\.\d+\.(\d+)\s*$", RegexOptions.Multiline)]
    private static partial Regex VersionRegex();

    [GeneratedRegex(@"^\s*ServicePack Build\s*:\s*(\d+)\s*$", RegexOptions.Multiline)]
    private static partial Regex ServicePackBuildRegex();

    /// <summary>
    /// "build.revision" of one edition from <c>dism /Get-ImageInfo /Index:n</c>, e.g. <c>22631.2428</c>.
    /// The line "Version: 10.0.26100..." DISM prints about itself has no space before the colon and is skipped.
    /// </summary>
    internal static string? ParseBuild(string dismIndexOutput)
    {
        var version = VersionRegex().Matches(dismIndexOutput).LastOrDefault();
        var sp = ServicePackBuildRegex().Match(dismIndexOutput);
        return version is null || !sp.Success ? null : $"{version.Groups[1].Value}.{sp.Groups[1].Value}";
    }

    public async Task<string?> GetBuildAsync(string installImage, int index, CancellationToken ct = default)
    {
        var r = await _runner.RunAsync(_dism, ["/Get-ImageInfo", $"/ImageFile:{installImage}", $"/Index:{index}", "/English"], Timeout, ct)
            .ConfigureAwait(false);
        return r.Succeeded ? ParseBuild(r.StdOut) : null;
    }

    public async Task<IReadOnlyList<WindowsImageEdition>> GetEditionsAsync(string installImage, CancellationToken ct = default)
    {
        var r = await _runner.RunAsync(_dism, ["/Get-ImageInfo", $"/ImageFile:{installImage}", "/English"], Timeout, ct).ConfigureAwait(false);
        if (!r.Succeeded)
        {
            throw new InvalidOperationException($"Could not read {Path.GetFileName(installImage)}: {r.StdOut.Trim()}");
        }
        return ParseImageInfo(r.StdOut);
    }

    /// <summary>
    /// Prefer IoT Enterprise (the edition validated on the phone), then Pro, then Home, else the first.
    /// </summary>
    public static WindowsImageEdition ChooseEdition(IReadOnlyList<WindowsImageEdition> editions)
    {
        if (editions.Count == 0)
        {
            throw new InvalidOperationException("The media contains no Windows editions.");
        }
        return editions.FirstOrDefault(e => e.Name.Contains("IoT Enterprise", StringComparison.OrdinalIgnoreCase)
                                            && !e.Name.Contains("Subscription", StringComparison.OrdinalIgnoreCase))
            ?? editions.FirstOrDefault(e => e.Name.Contains("Pro", StringComparison.OrdinalIgnoreCase) && !e.Name.Contains("Education", StringComparison.OrdinalIgnoreCase))
            ?? editions.FirstOrDefault(e => e.Name.Contains("Home", StringComparison.OrdinalIgnoreCase))
            ?? editions[0];
    }
}
