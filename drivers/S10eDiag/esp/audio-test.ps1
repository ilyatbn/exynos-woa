# D8 Windows audio test (runs in the logged-on user's session via a one-shot scheduled task from extra-audio.cmd).
# Plays 2 s tones on the default render endpoint and changes the volume with injected volume keys in between,
# so the PC microphone (tools/audio-listen) can see playback and the level changes. Log: C:\S10eTmp\audiotest.txt
$log = 'C:\S10eTmp\audiotest.txt'
function L($m) { "{0:HH:mm:ss.fff} {1}" -f (Get-Date), $m | Out-File -Append -Encoding ascii $log }
Add-Type -Namespace W -Name K -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte v, byte s, uint f, System.UIntPtr e);'
function Key($vk, $n) { for ($i = 0; $i -lt $n; $i++) { [W.K]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); [W.K]::keybd_event($vk, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 } }
function Wav($hz, $sec, $path) {
  $rate = 48000; $n = [int]($rate * $sec); $ms = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter($ms)
  $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $n * 4)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
  $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]2); $w.Write([int]$rate); $w.Write([int]($rate * 4)); $w.Write([int16]4); $w.Write([int16]16)
  $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]($n * 4))
  for ($i = 0; $i -lt $n; $i++) { $v = [int16](8000 * [Math]::Sin(2 * [Math]::PI * $hz * $i / $rate)); $w.Write($v); $w.Write($v) }
  $w.Flush(); [IO.File]::WriteAllBytes($path, $ms.ToArray())
}
try {
  L "start user=$env:USERNAME"
  Wav 1500 2 'C:\S10eTmp\t1500.wav'; Wav 2000 2 'C:\S10eTmp\t2000.wav'
  $p1 = New-Object Media.SoundPlayer 'C:\S10eTmp\t1500.wav'; $p2 = New-Object Media.SoundPlayer 'C:\S10eTmp\t2000.wav'
  Key 0xAE 50; Key 0xAF 25; L 'volume keys: 0 then +25 steps (~50%)'
  Start-Sleep 1; L 'play 1500 Hz A (50%)'; $p1.PlaySync(); L 'done A'
  Start-Sleep 1; Key 0xAE 10; L 'volume down x10 (~30%)'
  Start-Sleep 1; L 'play 1500 Hz B (30%)'; $p1.PlaySync(); L 'done B'
  Start-Sleep 1; Key 0xAF 20; L 'volume up x20 (~70%)'
  Start-Sleep 1; L 'play 2000 Hz C (70%)'; $p2.PlaySync(); L 'done C'
  Start-Sleep 1; Key 0xAD 1; L 'mute key'
  Start-Sleep 1; L 'play 2000 Hz D (muted)'; $p2.PlaySync(); L 'done D'
  Key 0xAD 1; Key 0xAE 20; L 'unmuted, volume back to ~30%'
} catch { L "ERROR $_" }
L 'end'
